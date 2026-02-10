#!/usr/bin/env python3
"""Extract room geometry + textures from OoT ROM into PS1-native PRM v2 format.

Usage:
  python3 extract_room.py <rom_path> <room_name> --prm output.prm [--obj output.obj]

Auto-detects scene file from room name (strips _room_N → scene DMA entry).
Scene provides shared TLUT palette at segment 2.
"""

import struct, csv, sys, math, re
from pathlib import Path

# ── Configuration ────────────────────────────────────────────────────────────

DECOMP_ROOT = Path(__file__).resolve().parent.parent.parent / "third_party" / "oot"
SEGMENTS_CSV = DECOMP_ROOT / "baseroms" / "ntsc-1.2" / "segments.csv"
DMA_TABLE_OFFSET = 0x7960  # NTSC 1.2

# F3DEX2 opcodes
G_VTX = 0x01; G_TRI1 = 0x05; G_TRI2 = 0x06
G_DL = 0xDE; G_ENDDL = 0xDF
G_SETTIMG = 0xFD; G_SETTILE = 0xF5; G_SETTILESIZE = 0xF2
G_LOADBLOCK = 0xF3; G_LOADTILE = 0xF4; G_LOADTLUT = 0xF0
G_SETPRIMCOLOR = 0xFA; G_SETENVCOLOR = 0xFB; G_SETCOMBINE = 0xFC
G_GEOMETRYMODE = 0xD9; G_TEXTURE = 0xD7
G_SETOTHERMODE_L = 0xE2; G_SETOTHERMODE_H = 0xE3
G_RDPPIPESYNC = 0xE7; G_RDPLOADSYNC = 0xE6; G_RDPTILESYNC = 0xE5
CMD_ROOM_SHAPE = 0x0A; CMD_END = 0x14
MAX_CHUNK_VERTS = 255

# N64 texture formats
FMT_RGBA = 0; FMT_CI = 2; FMT_IA = 3; FMT_I = 4
SIZ_4b = 0; SIZ_8b = 1; SIZ_16b = 2

# Opcodes to silently skip during DL walk
SKIP_OPCODES = {
    G_RDPPIPESYNC, G_RDPLOADSYNC, G_RDPTILESYNC, G_SETCOMBINE,
    G_GEOMETRYMODE, G_SETPRIMCOLOR, G_SETENVCOLOR, G_TEXTURE,
    G_SETOTHERMODE_L, G_SETOTHERMODE_H, G_LOADBLOCK, G_LOADTILE,
    0xED, 0xF6, 0xF7, 0xF8, 0xF9, 0xFF, 0xFE,
}

# ── Yaz0 Decompression ──────────────────────────────────────────────────────

def yaz0_decompress(data: bytes) -> bytes:
    if data[:4] != b"Yaz0":
        raise ValueError("Not Yaz0 data")
    sz = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(sz); src = 16; dst = 0
    while dst < sz:
        if src >= len(data): break
        cb = data[src]; src += 1
        for bit in range(7, -1, -1):
            if dst >= sz: break
            if cb & (1 << bit):
                out[dst] = data[src]; src += 1; dst += 1
            else:
                b1 = data[src]; b2 = data[src+1]; src += 2
                d = ((b1 & 0x0F) << 8) | b2; sp = dst - d - 1
                ln = b1 >> 4
                if ln == 0: ln = data[src] + 0x12; src += 1
                else: ln += 2
                for _ in range(ln):
                    if dst >= sz: break
                    out[dst] = out[sp]; sp += 1; dst += 1
    return bytes(out)

# ── ROM / DMA ────────────────────────────────────────────────────────────────

def read_u32(data, off): return struct.unpack_from(">I", data, off)[0]
def read_s16(data, off): return struct.unpack_from(">h", data, off)[0]

class DMAEntry:
    __slots__ = ("name", "vrom_start", "vrom_end", "rom_start", "rom_end")
    def __init__(self, name, vs, ve, rs, re):
        self.name = name; self.vrom_start = vs; self.vrom_end = ve
        self.rom_start = rs; self.rom_end = re
    @property
    def vrom_size(self): return self.vrom_end - self.vrom_start

def load_dma_table(rom: bytes) -> dict:
    names = []
    with open(SEGMENTS_CSV) as f:
        reader = csv.reader(f); next(reader)
        for row in reader: names.append(row[0] if row else "")
    entries = {}; off = DMA_TABLE_OFFSET; idx = 0
    while True:
        vs, ve, rs, re = struct.unpack_from(">IIII", rom, off)
        if vs == 0 and ve == 0 and rs == 0 and re == 0: break
        n = names[idx] if idx < len(names) else f"seg_{idx}"
        entries[n] = DMAEntry(n, vs, ve, rs, re)
        off += 16; idx += 1
    return entries

def load_file(rom: bytes, entry: DMAEntry) -> bytes:
    rs = entry.rom_start or entry.vrom_start
    re = entry.rom_end or (rs + entry.vrom_size)
    raw = rom[rs:re]
    return yaz0_decompress(raw) if raw[:4] == b"Yaz0" else raw

# ── N64→PS1 Color Conversion ────────────────────────────────────────────────

def n64_rgba5551_to_ps1(c16):
    """N64 RGBA5551 big-endian → PS1 ABGR1555 little-endian uint16."""
    r = (c16 >> 11) & 0x1F
    g = (c16 >> 6) & 0x1F
    b = (c16 >> 1) & 0x1F
    a = c16 & 1
    return (a << 15) | (b << 10) | (g << 5) | r

# ── Texture Info ─────────────────────────────────────────────────────────────

class TextureInfo:
    __slots__ = ("timg_addr", "fmt", "siz", "width", "height", "tlut_addr",
                 "ps1_pixels", "ps1_clut", "ps1_4bit", "num_clut_colors")
    def __init__(self, timg_addr, fmt, siz, width, height, tlut_addr=0):
        self.timg_addr = timg_addr; self.fmt = fmt; self.siz = siz
        self.width = width; self.height = height; self.tlut_addr = tlut_addr
        self.ps1_pixels = None; self.ps1_clut = None
        self.ps1_4bit = False; self.num_clut_colors = 0

# ── Chunk ────────────────────────────────────────────────────────────────────

class Chunk:
    __slots__ = ("verts", "colors", "uvs", "tris", "cx", "cy", "cz", "radius")
    def __init__(self):
        self.verts = []; self.colors = []; self.uvs = []; self.tris = []
        self.cx = self.cy = self.cz = 0; self.radius = 0

    def compute_bounds(self):
        if not self.verts: return
        xs = [v[0] for v in self.verts]
        ys = [v[1] for v in self.verts]
        zs = [v[2] for v in self.verts]
        self.cx = (min(xs) + max(xs)) // 2
        self.cy = (min(ys) + max(ys)) // 2
        self.cz = (min(zs) + max(zs)) // 2
        max_r2 = 0
        for x, y, z in self.verts:
            dx, dy, dz = x - self.cx, y - self.cy, z - self.cz
            r2 = dx*dx + dy*dy + dz*dz
            if r2 > max_r2: max_r2 = r2
        self.radius = min(int(math.isqrt(max_r2)) + 1, 32767)

# ── Room Extractor ───────────────────────────────────────────────────────────

class RoomExtractor:
    def __init__(self, room_data: bytes, scene_data: bytes = None):
        self.data = room_data
        self.segments = {3: room_data}
        if scene_data:
            self.segments[2] = scene_data
        self.chunks = []
        self._cur_chunk = None
        self._vtx_buf = [None] * 64
        # Texture state machine
        self._timg_addr = 0
        self._timg_fmt = 0
        self._timg_siz = 0
        self._tlut_addr = 0
        self._tile0_fmt = 0
        self._tile0_siz = 0
        self._cur_tex_id = 0xFF
        self.textures = []
        self._tex_dedup = {}
        self.stats = {"vtx_cmds": 0, "tri1_cmds": 0, "tri2_cmds": 0,
                      "dl_calls": 0, "unknown_cmds": set()}

    def resolve(self, addr):
        seg = (addr >> 24) & 0x0F; off = addr & 0x00FFFFFF
        return off if seg == 3 and off < len(self.data) else None

    def resolve_any(self, addr):
        """Resolve segment address across all loaded segments → (buffer, offset)."""
        seg = (addr >> 24) & 0x0F; off = addr & 0x00FFFFFF
        buf = self.segments.get(seg)
        if buf is not None and off < len(buf):
            return buf, off
        return None, None

    def extract(self):
        off = 0
        while off + 8 <= len(self.data):
            cmd = self.data[off]
            if cmd == CMD_END: break
            if cmd == CMD_ROOM_SHAPE:
                self._parse_room_shape(read_u32(self.data, off + 4))
            off += 8
        for c in self.chunks:
            c.compute_bounds()
        return self.chunks

    def _parse_room_shape(self, ptr):
        off = self.resolve(ptr)
        if off is None: return
        shape_type = self.data[off]
        num_entries = self.data[off + 1]
        entries_ptr = read_u32(self.data, off + 4)
        eoff = self.resolve(entries_ptr)
        if eoff is None: return

        if shape_type == 2:
            for i in range(num_entries):
                eo = eoff + i * 16
                cx = read_s16(self.data, eo)
                cy = read_s16(self.data, eo + 2)
                cz = read_s16(self.data, eo + 4)
                radius = read_s16(self.data, eo + 6)
                opa = read_u32(self.data, eo + 8)
                xlu = read_u32(self.data, eo + 12)
                self._begin_chunk()
                self._cur_chunk.cx = cx
                self._cur_chunk.cy = cy
                self._cur_chunk.cz = cz
                self._cur_chunk.radius = radius
                if opa: self._walk_dl(opa)
                if xlu: self._walk_dl(xlu)
                self._end_chunk(use_authored_bounds=True)
        elif shape_type == 0:
            for i in range(num_entries):
                eo = eoff + i * 8
                opa = read_u32(self.data, eo)
                xlu = read_u32(self.data, eo + 4)
                self._begin_chunk()
                if opa: self._walk_dl(opa)
                if xlu: self._walk_dl(xlu)
                self._end_chunk(use_authored_bounds=False)
        else:
            print(f"  [WARN] Unsupported room shape type {shape_type}")

    def _begin_chunk(self):
        self._cur_chunk = Chunk()
        self._vtx_buf = [None] * 64

    def _end_chunk(self, use_authored_bounds):
        c = self._cur_chunk
        if not c or (not c.verts and not c.tris):
            self._cur_chunk = None; return
        if len(c.verts) > MAX_CHUNK_VERTS:
            self._split_and_append(c, use_authored_bounds)
        else:
            if not use_authored_bounds: c.compute_bounds()
            self.chunks.append(c)
        self._cur_chunk = None

    def _split_and_append(self, big, use_authored_bounds):
        verts, colors, uvs, tris = big.verts, big.colors, big.uvs, big.tris
        assigned = [False] * len(tris)
        while True:
            sub = Chunk(); local_map = {}
            for ti, (a, b, c, tid) in enumerate(tris):
                if assigned[ti]: continue
                needed = set()
                for v in (a, b, c):
                    if v not in local_map: needed.add(v)
                if len(sub.verts) + len(needed) > MAX_CHUNK_VERTS: continue
                for vi in needed:
                    local_map[vi] = len(sub.verts)
                    sub.verts.append(verts[vi])
                    sub.colors.append(colors[vi])
                    sub.uvs.append(uvs[vi])
                sub.tris.append((local_map[a], local_map[b], local_map[c], tid))
                assigned[ti] = True
            if not sub.tris: break
            sub.compute_bounds()
            self.chunks.append(sub)
            if all(assigned): break

    def _walk_dl(self, ptr, depth=0):
        off = self.resolve(ptr)
        if off is None or depth > 16: return
        while off + 8 <= len(self.data):
            w0 = read_u32(self.data, off); w1 = read_u32(self.data, off+4); off += 8
            cmd = (w0 >> 24) & 0xFF
            if cmd == G_VTX:
                n = (w0 >> 12) & 0xFF; v0 = ((w0 >> 1) & 0x7F) - n
                self._load_verts(w1, n, v0); self.stats["vtx_cmds"] += 1
            elif cmd == G_TRI1:
                self._emit_tri(((w0>>16)&0xFF)//2, ((w0>>8)&0xFF)//2, (w0&0xFF)//2)
                self.stats["tri1_cmds"] += 1
            elif cmd == G_TRI2:
                self._emit_tri(((w0>>16)&0xFF)//2, ((w0>>8)&0xFF)//2, (w0&0xFF)//2)
                self._emit_tri(((w1>>16)&0xFF)//2, ((w1>>8)&0xFF)//2, (w1&0xFF)//2)
                self.stats["tri2_cmds"] += 1
            elif cmd == G_SETTIMG:
                self._timg_addr = w1
                self._timg_fmt = (w0 >> 21) & 0x07
                self._timg_siz = (w0 >> 19) & 0x03
            elif cmd == G_LOADTLUT:
                self._tlut_addr = self._timg_addr
            elif cmd == G_SETTILE:
                tile = (w1 >> 24) & 0x07
                if tile == 0:
                    self._tile0_fmt = (w0 >> 21) & 0x07
                    self._tile0_siz = (w0 >> 19) & 0x03
            elif cmd == G_SETTILESIZE:
                self._on_settilesize(w0, w1)
            elif cmd == G_DL:
                if (w0 >> 16) & 0xFF == 0:
                    self._walk_dl(w1, depth+1); self.stats["dl_calls"] += 1
                else:
                    new_off = self.resolve(w1)
                    if new_off is not None: off = new_off
            elif cmd == G_ENDDL:
                break
            elif cmd not in SKIP_OPCODES:
                self.stats["unknown_cmds"].add(cmd)

    def _on_settilesize(self, w0, w1):
        tile = (w1 >> 24) & 0x07
        if tile != 0: return
        uls = (w0 >> 12) & 0xFFF; ult = w0 & 0xFFF
        lrs = (w1 >> 12) & 0xFFF; lrt = w1 & 0xFFF
        width = (lrs >> 2) - (uls >> 2) + 1
        height = (lrt >> 2) - (ult >> 2) + 1
        if width <= 0 or height <= 0: return

        key = (self._timg_addr, width, height)
        if key in self._tex_dedup:
            self._cur_tex_id = self._tex_dedup[key]
            return

        tex_id = len(self.textures)
        seg = (self._timg_addr >> 24) & 0x0F
        if seg == 8:
            ti = TextureInfo(self._timg_addr, FMT_CI, SIZ_8b, 2, 2, 0)
        else:
            ti = TextureInfo(self._timg_addr, self._tile0_fmt, self._tile0_siz,
                           width, height, self._tlut_addr)
        self.textures.append(ti)
        self._tex_dedup[key] = tex_id
        self._cur_tex_id = tex_id

    def _load_verts(self, ptr, count, v0):
        off = self.resolve(ptr)
        if off is None or self._cur_chunk is None: return
        c = self._cur_chunk
        for i in range(count):
            vo = off + i * 16
            if vo + 16 > len(self.data): break
            x = read_s16(self.data, vo)
            y = read_s16(self.data, vo + 2)
            z = read_s16(self.data, vo + 4)
            # UV: bytes 8-11, int16 BE S10.5 fixed-point → texel coords
            s_raw = read_s16(self.data, vo + 8)
            t_raw = read_s16(self.data, vo + 10)
            u = (s_raw >> 5) & 0xFF
            v = (t_raw >> 5) & 0xFF
            r = self.data[vo + 12]; g = self.data[vo + 13]
            b = self.data[vo + 14]; a = self.data[vo + 15]
            local_idx = len(c.verts)
            c.verts.append((x, y, z))
            c.colors.append((r, g, b, a))
            c.uvs.append((u, v))
            slot = v0 + i
            if slot < 64: self._vtx_buf[slot] = local_idx

    def _emit_tri(self, i0, i1, i2):
        c = self._cur_chunk
        if c is None: return
        g0 = self._vtx_buf[i0] if i0 < 64 else None
        g1 = self._vtx_buf[i1] if i1 < 64 else None
        g2 = self._vtx_buf[i2] if i2 < 64 else None
        if g0 is not None and g1 is not None and g2 is not None:
            c.tris.append((g0, g1, g2, self._cur_tex_id))

    # ── Texture Conversion ───────────────────────────────────────────────────

    def finalize_textures(self):
        """Convert all tracked N64 textures to PS1 indexed format."""
        for i, tex in enumerate(self.textures):
            seg = (tex.timg_addr >> 24) & 0x0F
            if seg == 8:
                self._make_fallback(tex); continue
            if tex.fmt == FMT_CI and tex.siz == SIZ_8b:
                self._convert_ci8(tex)
            elif tex.fmt == FMT_CI and tex.siz == SIZ_4b:
                self._convert_ci4(tex)
            elif tex.fmt == FMT_RGBA and tex.siz == SIZ_16b:
                self._convert_rgba16(tex)
            elif tex.fmt == FMT_IA and tex.siz == SIZ_16b:
                self._convert_direct16(tex, is_ia=True)
            elif tex.fmt == FMT_IA and tex.siz == SIZ_8b:
                self._convert_ia8(tex)
            elif tex.fmt == FMT_I and tex.siz == SIZ_8b:
                self._convert_i8(tex)
            else:
                print(f"  [WARN] Tex {i}: unsupported fmt={tex.fmt} siz={tex.siz}")
                self._make_fallback(tex)
            bpp = "4bit" if tex.ps1_4bit else "8bit"
            print(f"  Tex {i:2d}: {tex.width:3d}x{tex.height:<3d} "
                  f"fmt={tex.fmt} siz={tex.siz} → {bpp} "
                  f"{tex.num_clut_colors} colors, "
                  f"{len(tex.ps1_pixels)} px bytes")

    def _make_fallback(self, tex):
        tex.width = 2; tex.height = 2
        tex.ps1_4bit = True; tex.num_clut_colors = 1
        tex.ps1_pixels = bytes([0x00, 0x00])
        grey = (1 << 15) | (16 << 10) | (16 << 5) | 16
        tex.ps1_clut = struct.pack("<H", grey)

    def _convert_ci8(self, tex):
        pix_buf, pix_off = self.resolve_any(tex.timg_addr)
        if pix_buf is None: self._make_fallback(tex); return
        npix = tex.width * tex.height
        if pix_off + npix > len(pix_buf): self._make_fallback(tex); return
        raw = pix_buf[pix_off:pix_off + npix]

        tlut_buf, tlut_off = self.resolve_any(tex.tlut_addr)
        if tlut_buf is None: self._make_fallback(tex); return

        used = set(raw)
        if len(used) <= 16:
            sorted_used = sorted(used)
            remap = {old: new for new, old in enumerate(sorted_used)}
            packed = bytearray(npix // 2)
            for j in range(0, npix, 2):
                lo = remap[raw[j]]
                hi = remap[raw[j+1]] if j+1 < npix else 0
                packed[j//2] = lo | (hi << 4)
            tex.ps1_pixels = bytes(packed)
            tex.ps1_4bit = True
            tex.num_clut_colors = len(sorted_used)
            clut = bytearray(len(sorted_used) * 2)
            for ci, old_idx in enumerate(sorted_used):
                n64c = struct.unpack_from(">H", tlut_buf, tlut_off + old_idx * 2)[0]
                struct.pack_into("<H", clut, ci * 2, n64_rgba5551_to_ps1(n64c))
            tex.ps1_clut = bytes(clut)
        else:
            tex.ps1_pixels = bytes(raw)
            tex.ps1_4bit = False
            tex.num_clut_colors = 256
            clut = bytearray(256 * 2)
            for ci in range(256):
                if tlut_off + ci * 2 + 2 <= len(tlut_buf):
                    n64c = struct.unpack_from(">H", tlut_buf, tlut_off + ci * 2)[0]
                    struct.pack_into("<H", clut, ci * 2, n64_rgba5551_to_ps1(n64c))
            tex.ps1_clut = bytes(clut)

    def _convert_ci4(self, tex):
        pix_buf, pix_off = self.resolve_any(tex.timg_addr)
        if pix_buf is None: self._make_fallback(tex); return
        npix = tex.width * tex.height
        nbytes = (npix + 1) // 2
        if pix_off + nbytes > len(pix_buf): self._make_fallback(tex); return

        tex.ps1_pixels = bytes(pix_buf[pix_off:pix_off + nbytes])
        tex.ps1_4bit = True
        tex.num_clut_colors = 16

        tlut_buf, tlut_off = self.resolve_any(tex.tlut_addr)
        if tlut_buf is None: self._make_fallback(tex); return
        clut = bytearray(16 * 2)
        for ci in range(16):
            if tlut_off + ci * 2 + 2 <= len(tlut_buf):
                n64c = struct.unpack_from(">H", tlut_buf, tlut_off + ci * 2)[0]
                struct.pack_into("<H", clut, ci * 2, n64_rgba5551_to_ps1(n64c))
        tex.ps1_clut = bytes(clut)

    def _convert_rgba16(self, tex):
        pix_buf, pix_off = self.resolve_any(tex.timg_addr)
        if pix_buf is None: self._make_fallback(tex); return
        npix = tex.width * tex.height
        if pix_off + npix * 2 > len(pix_buf): self._make_fallback(tex); return

        ps1c = []
        for j in range(npix):
            n64c = struct.unpack_from(">H", pix_buf, pix_off + j * 2)[0]
            ps1c.append(n64_rgba5551_to_ps1(n64c))
        self._build_indexed_from_colors(tex, ps1c)

    def _convert_direct16(self, tex, is_ia=False):
        pix_buf, pix_off = self.resolve_any(tex.timg_addr)
        if pix_buf is None: self._make_fallback(tex); return
        npix = tex.width * tex.height
        if pix_off + npix * 2 > len(pix_buf): self._make_fallback(tex); return

        ps1c = []
        for j in range(npix):
            if is_ia:
                i_val = pix_buf[pix_off + j * 2]
                a_val = pix_buf[pix_off + j * 2 + 1]
                g5 = i_val >> 3
                a = 1 if a_val >= 128 else 0
                ps1c.append((a << 15) | (g5 << 10) | (g5 << 5) | g5)
            else:
                n64c = struct.unpack_from(">H", pix_buf, pix_off + j * 2)[0]
                ps1c.append(n64_rgba5551_to_ps1(n64c))
        self._build_indexed_from_colors(tex, ps1c)

    def _convert_ia8(self, tex):
        pix_buf, pix_off = self.resolve_any(tex.timg_addr)
        if pix_buf is None: self._make_fallback(tex); return
        npix = tex.width * tex.height
        if pix_off + npix > len(pix_buf): self._make_fallback(tex); return
        ps1c = []
        for j in range(npix):
            byte = pix_buf[pix_off + j]
            i4 = (byte >> 4) & 0xF; a4 = byte & 0xF
            g5 = (i4 << 1) | (i4 >> 3)
            a = 1 if a4 >= 8 else 0
            ps1c.append((a << 15) | (g5 << 10) | (g5 << 5) | g5)
        self._build_indexed_from_colors(tex, ps1c)

    def _convert_i8(self, tex):
        pix_buf, pix_off = self.resolve_any(tex.timg_addr)
        if pix_buf is None: self._make_fallback(tex); return
        npix = tex.width * tex.height
        if pix_off + npix > len(pix_buf): self._make_fallback(tex); return
        ps1c = []
        for j in range(npix):
            g5 = pix_buf[pix_off + j] >> 3
            ps1c.append((1 << 15) | (g5 << 10) | (g5 << 5) | g5)
        self._build_indexed_from_colors(tex, ps1c)

    def _build_indexed_from_colors(self, tex, ps1_colors):
        """Auto-select 4-bit or 8-bit indexed format from a list of PS1 colors."""
        unique = list(dict.fromkeys(ps1_colors))  # preserve order, deduplicate
        npix = len(ps1_colors)

        if len(unique) > 256:
            # Quantize: sort by luminance, subsample
            unique.sort(key=lambda c: (c & 0x1F)*3 + ((c>>5)&0x1F)*6 + ((c>>10)&0x1F))
            step = len(unique) / 256
            unique = [unique[int(i * step)] for i in range(256)]

        color_to_idx = {c: i for i, c in enumerate(unique)}
        use_4bit = len(unique) <= 16

        # For colors that were quantized away, find nearest
        def nearest(c):
            if c in color_to_idx: return color_to_idx[c]
            r, g, b = c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F
            best_i = 0; best_d = 99999
            for pi, pc in enumerate(unique):
                pr, pg, pb = pc & 0x1F, (pc >> 5) & 0x1F, (pc >> 10) & 0x1F
                d = abs(r-pr) + abs(g-pg) + abs(b-pb)
                if d < best_d: best_d = d; best_i = pi
            color_to_idx[c] = best_i
            return best_i

        if use_4bit:
            packed = bytearray(npix // 2)
            for j in range(0, npix, 2):
                lo = nearest(ps1_colors[j])
                hi = nearest(ps1_colors[j+1]) if j+1 < npix else 0
                packed[j//2] = lo | (hi << 4)
            tex.ps1_pixels = bytes(packed)
            tex.ps1_4bit = True
        else:
            tex.ps1_pixels = bytes(nearest(c) for c in ps1_colors)
            tex.ps1_4bit = False

        tex.num_clut_colors = len(unique)
        clut = bytearray(len(unique) * 2)
        for ci, c in enumerate(unique):
            struct.pack_into("<H", clut, ci * 2, c)
        tex.ps1_clut = bytes(clut)

# ── PRM v2 Binary Export ─────────────────────────────────────────────────────
#
# HEADER (20 bytes):
#   0x00  u8[4]  magic        "PRM\x02"
#   0x04  u16    num_chunks
#   0x06  u16    num_verts     (total)
#   0x08  u16    num_tris      (total)
#   0x0A  u16    num_textures
#   0x0C  u32    data_start    (offset to first chunk data)
#   0x10  u32    tex_start     (offset to texture section)
#
# CHUNK TABLE (num_chunks * 16): same as v1
#
# CHUNK DATA per chunk:
#   positions[nv * 8]
#   colors[nv * 4]
#   uvs[nv * 2]  + padding to 4-byte align
#   indices[nt * 4]   (v0, v1, v2, tex_id)
#
# TEXTURE SECTION (at tex_start):
#   TexDesc[num_textures * 12]
#   Per-texture data blocks (pixel data then CLUT, contiguous per texture)

def export_prm(chunks, textures, path):
    total_v = sum(len(c.verts) for c in chunks)
    total_t = sum(len(c.tris) for c in chunks)
    num_tex = len(textures)

    header_size = 20
    chunk_table_size = len(chunks) * 16
    data_start = (header_size + chunk_table_size + 3) & ~3

    # Chunk data offsets
    data_offset = 0; chunk_offsets = []
    for c in chunks:
        chunk_offsets.append(data_offset)
        pos_sz = len(c.verts) * 8
        col_sz = len(c.verts) * 4
        uv_sz = (len(c.verts) * 2 + 3) & ~3
        idx_sz = len(c.tris) * 4
        data_offset += pos_sz + col_sz + uv_sz + idx_sz

    tex_start = (data_start + data_offset + 3) & ~3
    tex_desc_size = num_tex * 12

    # Compute per-texture data offsets (pixel + clut blocks, contiguous)
    tex_data_off = 0; tex_offsets = []
    for tex in textures:
        tex_offsets.append(tex_data_off)
        pix_sz = len(tex.ps1_pixels) if tex.ps1_pixels else 0
        clut_sz = len(tex.ps1_clut) if tex.ps1_clut else 0
        tex_data_off += pix_sz + clut_sz
    tex_data_off = (tex_data_off + 3) & ~3

    total_size = tex_start + tex_desc_size + tex_data_off
    buf = bytearray(total_size)

    # Header
    buf[0:4] = b"PRM\x02"
    struct.pack_into("<HHHH II", buf, 4,
                     len(chunks), total_v, total_t, num_tex,
                     data_start, tex_start)

    # Chunk table
    for i, c in enumerate(chunks):
        off = header_size + i * 16
        struct.pack_into("<hhhh HH I", buf, off,
                         c.cx, c.cy, c.cz, c.radius,
                         len(c.verts), len(c.tris), chunk_offsets[i])

    # Chunk data
    for i, c in enumerate(chunks):
        base = data_start + chunk_offsets[i]
        for j, (x, y, z) in enumerate(c.verts):
            struct.pack_into("<hhhh", buf, base + j * 8, x, y, z, 0)
        col_base = base + len(c.verts) * 8
        for j, (r, g, b, a) in enumerate(c.colors):
            struct.pack_into("<BBBB", buf, col_base + j * 4, r, g, b, a)
        uv_base = col_base + len(c.verts) * 4
        for j, (u, v) in enumerate(c.uvs):
            struct.pack_into("<BB", buf, uv_base + j * 2, u, v)
        uv_sz = (len(c.verts) * 2 + 3) & ~3
        idx_base = uv_base + uv_sz
        for j, tri in enumerate(c.tris):
            v0, v1, v2 = tri[0], tri[1], tri[2]
            tid = tri[3] if len(tri) > 3 else 0xFF
            struct.pack_into("<BBBB", buf, idx_base + j * 4, v0, v1, v2, tid)

    # Texture descriptors
    for i, tex in enumerate(textures):
        off = tex_start + i * 12
        fmt = 0 if tex.ps1_4bit else 1
        nc = tex.num_clut_colors & 0xFF  # 256 → 0
        struct.pack_into("<HH BB H I", buf, off,
                         tex.width, tex.height, fmt, nc, 0, tex_offsets[i])

    # Texture data (pixel + clut per texture)
    tex_data_base = tex_start + tex_desc_size
    for i, tex in enumerate(textures):
        off = tex_data_base + tex_offsets[i]
        if tex.ps1_pixels:
            buf[off:off + len(tex.ps1_pixels)] = tex.ps1_pixels
            off += len(tex.ps1_pixels)
        if tex.ps1_clut:
            buf[off:off + len(tex.ps1_clut)] = tex.ps1_clut

    with open(path, "wb") as f:
        f.write(buf)

    print(f"  PRM v2: {total_size} bytes ({total_size/1024:.1f} KB)")
    print(f"    {len(chunks)} chunks, {total_v} verts, {total_t} tris, {num_tex} textures")
    print(f"    chunk data: {data_offset} B  tex section: {total_size - tex_start} B")
    max_cv = max((len(c.verts) for c in chunks), default=0)
    max_ct = max((len(c.tris) for c in chunks), default=0)
    print(f"    max chunk: {max_cv} verts, {max_ct} tris")

# ── OBJ Export (debug) ───────────────────────────────────────────────────────

def export_obj(chunks, path):
    with open(path, "w") as f:
        total_v = sum(len(c.verts) for c in chunks)
        total_t = sum(len(c.tris) for c in chunks)
        f.write(f"# OoT Room — {total_v} verts, {total_t} tris, {len(chunks)} chunks\n\n")
        vert_offset = 0
        for ci, c in enumerate(chunks):
            f.write(f"g chunk_{ci}\n")
            for j, ((x, y, z), (r, g, b, a)) in enumerate(zip(c.verts, c.colors)):
                f.write(f"v {x} {y} {z} {r/255:.4f} {g/255:.4f} {b/255:.4f}\n")
            for tri in c.tris:
                v0, v1, v2 = tri[0], tri[1], tri[2]
                f.write(f"f {vert_offset+v0+1} {vert_offset+v1+1} {vert_offset+v2+1}\n")
            vert_offset += len(c.verts)
    print(f"  OBJ: {path} ({total_v} verts, {total_t} tris)")

# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 extract_room.py <rom> <room_name> --prm out.prm "
              "[--obj out.obj] [--scene scene_name]")
        sys.exit(1)

    rom_path = sys.argv[1]; room_name = sys.argv[2]
    prm_path = obj_path = scene_name = None
    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == "--prm" and i + 1 < len(sys.argv):
            prm_path = sys.argv[i+1]; i += 2
        elif sys.argv[i] == "--obj" and i + 1 < len(sys.argv):
            obj_path = sys.argv[i+1]; i += 2
        elif sys.argv[i] == "--scene" and i + 1 < len(sys.argv):
            scene_name = sys.argv[i+1]; i += 2
        else:
            i += 1

    # Auto-detect scene from room name
    if scene_name is None:
        m = re.match(r"(.+)_room_\d+", room_name)
        if m:
            scene_name = m.group(1) + "_scene"
            print(f"Auto-detected scene: {scene_name}")

    print(f"Loading ROM: {rom_path}")
    with open(rom_path, "rb") as f: rom = f.read()
    print(f"  {len(rom)/1024/1024:.1f} MB")

    dma = load_dma_table(rom)

    if room_name not in dma:
        matches = sorted(n for n in dma if room_name.split("_")[0] in n)[:10]
        print(f"ERROR: '{room_name}' not found. Similar: {', '.join(matches)}")
        sys.exit(1)

    entry = dma[room_name]
    print(f"Loading {room_name} (vrom {entry.vrom_size} bytes)...")
    room_data = load_file(rom, entry)
    print(f"  Decompressed: {len(room_data)} bytes")

    # Load scene for TLUT
    scene_data = None
    if scene_name and scene_name in dma:
        scene_entry = dma[scene_name]
        print(f"Loading {scene_name} (vrom {scene_entry.vrom_size} bytes)...")
        scene_data = load_file(rom, scene_entry)
        print(f"  Decompressed: {len(scene_data)} bytes")
    elif scene_name:
        print(f"  [WARN] Scene '{scene_name}' not found in DMA table")

    extractor = RoomExtractor(room_data, scene_data)
    chunks = extractor.extract()

    total_v = sum(len(c.verts) for c in chunks)
    total_t = sum(len(c.tris) for c in chunks)
    print(f"\nExtracted: {total_v} verts, {total_t} tris in {len(chunks)} chunks")
    print(f"  DL commands: {extractor.stats['vtx_cmds']} VTX, "
          f"{extractor.stats['tri1_cmds']} TRI1, {extractor.stats['tri2_cmds']} TRI2")
    print(f"  Textures found: {len(extractor.textures)}")
    if extractor.stats["unknown_cmds"]:
        print(f"  Skipped: {', '.join(f'0x{c:02x}' for c in sorted(extractor.stats['unknown_cmds']))}")

    for i, c in enumerate(chunks):
        print(f"  chunk {i:2d}: {len(c.verts):4d}v {len(c.tris):4d}t  "
              f"center=({c.cx},{c.cy},{c.cz}) r={c.radius}")

    # Convert textures
    if extractor.textures:
        print(f"\nConverting {len(extractor.textures)} textures...")
        extractor.finalize_textures()

    if prm_path:
        print(f"\nExporting PRM v2...")
        export_prm(chunks, extractor.textures, prm_path)
    if obj_path:
        export_obj(chunks, obj_path)

if __name__ == "__main__":
    main()
