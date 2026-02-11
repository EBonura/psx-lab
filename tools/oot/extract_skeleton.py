#!/usr/bin/env python3
"""Extract Link's skeleton + animations from OoT ROM into PS1-native SKM format.

Usage:
  python3 extract_skeleton.py <rom_path> --skm output.skm

Extracts adult Link's FlexSkeleton (21 LOD limbs, far DLists) from object_link_boy
and selected animations from link_animetion.
"""

import struct, csv, sys, math
from pathlib import Path

# ── Configuration ────────────────────────────────────────────────────────────

DECOMP_ROOT = Path(__file__).resolve().parent.parent.parent / "third_party" / "oot"
SEGMENTS_CSV = DECOMP_ROOT / "baseroms" / "ntsc-1.2" / "segments.csv"
DMA_TABLE_OFFSET = 0x7960  # NTSC 1.2

# object_link_boy skeleton offsets (segment 6)
SKEL_HEADER_OFF = 0x377F4   # FlexSkeletonHeader
NUM_LIMBS       = 21
FRAME_SIZE      = 134        # 22×Vec3s + u16 face = 134 bytes per anim frame

# OoT actor scale factor. Kept at 1 so extraction preserves full model-space
# precision (int16). The PS1 renderer applies the 1/100 scale at runtime via
# GTE translation, avoiding the catastrophic integer quantization that occurs
# when dividing small limb-local vertex coords by 100.
MODEL_SCALE     = 1

# Animations: (name, frame_count, offset_in_segment, loop)
ANIMATIONS = [
    ("idle", 89, 0x1C3030, True),
    ("walk", 29, 0x1F5050, True),
    ("run",  20, 0x1B3600, True),
]

LIMB_NAMES = [
    "ROOT", "WAIST", "LOWER", "R_THIGH", "R_SHIN", "R_FOOT",
    "L_THIGH", "L_SHIN", "L_FOOT", "UPPER", "HEAD", "HAT",
    "COLLAR", "L_SHOULDER", "L_FOREARM", "L_HAND",
    "R_SHOULDER", "R_FOREARM", "R_HAND", "SHEATH", "TORSO",
]

# F3DEX2 opcodes
G_VTX = 0x01; G_TRI1 = 0x05; G_TRI2 = 0x06
G_DL = 0xDE; G_ENDDL = 0xDF
G_SETTIMG = 0xFD; G_SETTILE = 0xF5; G_SETTILESIZE = 0xF2
G_LOADBLOCK = 0xF3; G_LOADTILE = 0xF4; G_LOADTLUT = 0xF0
G_SETPRIMCOLOR = 0xFA; G_SETENVCOLOR = 0xFB; G_SETCOMBINE = 0xFC
G_GEOMETRYMODE = 0xD9; G_TEXTURE = 0xD7
G_SETOTHERMODE_L = 0xE2; G_SETOTHERMODE_H = 0xE3
G_RDPPIPESYNC = 0xE7; G_RDPLOADSYNC = 0xE6; G_RDPTILESYNC = 0xE5

FMT_RGBA = 0; FMT_CI = 2; FMT_IA = 3; FMT_I = 4
SIZ_4b = 0; SIZ_8b = 1; SIZ_16b = 2

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

# ── Limb Mesh ────────────────────────────────────────────────────────────────

class LimbMesh:
    __slots__ = ("verts", "colors", "uvs", "tris")
    def __init__(self):
        self.verts = []; self.colors = []; self.uvs = []; self.tris = []

# ── Skeleton Extractor ───────────────────────────────────────────────────────

class SkeletonExtractor:
    def __init__(self, obj_data: bytes):
        self.data = obj_data
        self.segments = {6: obj_data}
        self.limbs = []       # [(joint_pos, child, sibling, dl_far_addr), ...]
        self.limb_meshes = []
        self._vtx_buf = [None] * 64
        self._cur_mesh = None
        self._timg_addr = 0
        self._timg_fmt = 0
        self._timg_siz = 0
        self._tlut_addr = 0
        self._tile0_fmt = 0
        self._tile0_siz = 0
        self._cur_tex_id = 0xFF
        self.textures = []
        self._tex_dedup = {}

    def resolve(self, addr):
        seg = (addr >> 24) & 0x0F; off = addr & 0x00FFFFFF
        return off if seg == 6 and off < len(self.data) else None

    def resolve_any(self, addr):
        seg = (addr >> 24) & 0x0F; off = addr & 0x00FFFFFF
        buf = self.segments.get(seg)
        if buf is not None and off < len(buf):
            return buf, off
        return None, None

    def parse_skeleton(self):
        # Read FlexSkeletonHeader at SKEL_HEADER_OFF
        limb_table_ptr = read_u32(self.data, SKEL_HEADER_OFF)
        limb_count = self.data[SKEL_HEADER_OFF + 4]
        dlist_count = self.data[SKEL_HEADER_OFF + 8]

        limb_table_off = self.resolve(limb_table_ptr)
        if limb_table_off is None:
            print(f"ERROR: Cannot resolve limb table pointer {limb_table_ptr:#010x}")
            sys.exit(1)

        print(f"  FlexSkeletonHeader @ {SKEL_HEADER_OFF:#x}:")
        print(f"    limbCount={limb_count}, dListCount={dlist_count}")
        print(f"    limb table @ {limb_table_off:#x}")

        if limb_count != NUM_LIMBS:
            print(f"  [WARN] Expected {NUM_LIMBS} limbs, got {limb_count}")

        for i in range(limb_count):
            ptr = read_u32(self.data, limb_table_off + i * 4)
            limb_off = self.resolve(ptr)
            if limb_off is None:
                self.limbs.append(((0, 0, 0), 0xFF, 0xFF, 0))
                continue
            jx = round(read_s16(self.data, limb_off) / MODEL_SCALE)
            jy = round(read_s16(self.data, limb_off + 2) / MODEL_SCALE)
            jz = round(read_s16(self.data, limb_off + 4) / MODEL_SCALE)
            child = self.data[limb_off + 6]
            sibling = self.data[limb_off + 7]
            # LodLimb: dLists[0]=near, dLists[1]=far
            dl_far = read_u32(self.data, limb_off + 12)
            self.limbs.append(((jx, jy, jz), child, sibling, dl_far))

    def extract_meshes(self):
        total_v = total_t = 0
        for i, (joint, child, sib, dl_far) in enumerate(self.limbs):
            mesh = LimbMesh()
            self._cur_mesh = mesh
            self._vtx_buf = [None] * 64
            if dl_far != 0:
                self._walk_dl(dl_far)
            self.limb_meshes.append(mesh)
            nv, nt = len(mesh.verts), len(mesh.tris)
            total_v += nv; total_t += nt
            name = LIMB_NAMES[i] if i < len(LIMB_NAMES) else f"limb_{i}"
            dl_str = f"{dl_far:#010x}" if dl_far else "      null"
            print(f"  {i:2d} {name:12s}  {nv:3d}v {nt:3d}t  "
                  f"joint=({joint[0]:5d},{joint[1]:5d},{joint[2]:5d}) "
                  f"ch={child:3d} sib={sib:3d}  dl={dl_str}")
        print(f"  Total: {total_v} verts, {total_t} tris")

    # ── DL Walker ────────────────────────────────────────────────────────────

    def _walk_dl(self, ptr, depth=0):
        off = self.resolve(ptr)
        if off is None or depth > 16: return
        while off + 8 <= len(self.data):
            w0 = read_u32(self.data, off); w1 = read_u32(self.data, off + 4); off += 8
            cmd = (w0 >> 24) & 0xFF
            if cmd == G_VTX:
                n = (w0 >> 12) & 0xFF; v0 = ((w0 >> 1) & 0x7F) - n
                self._load_verts(w1, n, v0)
            elif cmd == G_TRI1:
                self._emit_tri(((w0 >> 16) & 0xFF) // 2,
                               ((w0 >> 8) & 0xFF) // 2,
                               (w0 & 0xFF) // 2)
            elif cmd == G_TRI2:
                self._emit_tri(((w0 >> 16) & 0xFF) // 2,
                               ((w0 >> 8) & 0xFF) // 2,
                               (w0 & 0xFF) // 2)
                self._emit_tri(((w1 >> 16) & 0xFF) // 2,
                               ((w1 >> 8) & 0xFF) // 2,
                               (w1 & 0xFF) // 2)
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
                    self._walk_dl(w1, depth + 1)
                else:
                    new_off = self.resolve(w1)
                    if new_off is not None: off = new_off
            elif cmd == G_ENDDL:
                break

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
        if seg not in self.segments:
            ti = TextureInfo(self._timg_addr, FMT_CI, SIZ_8b, 2, 2, 0)
        else:
            ti = TextureInfo(self._timg_addr, self._tile0_fmt, self._tile0_siz,
                             width, height, self._tlut_addr)
        self.textures.append(ti)
        self._tex_dedup[key] = tex_id
        self._cur_tex_id = tex_id

    def _load_verts(self, ptr, count, v0):
        off = self.resolve(ptr)
        if off is None or self._cur_mesh is None: return
        m = self._cur_mesh
        for i in range(count):
            vo = off + i * 16
            if vo + 16 > len(self.data): break
            x = round(read_s16(self.data, vo) / MODEL_SCALE)
            y = round(read_s16(self.data, vo + 2) / MODEL_SCALE)
            z = round(read_s16(self.data, vo + 4) / MODEL_SCALE)
            s_raw = read_s16(self.data, vo + 8)
            t_raw = read_s16(self.data, vo + 10)
            u = (s_raw >> 5) & 0xFF
            v = (t_raw >> 5) & 0xFF
            r = self.data[vo + 12]; g = self.data[vo + 13]
            b = self.data[vo + 14]; a = self.data[vo + 15]
            local_idx = len(m.verts)
            m.verts.append((x, y, z))
            m.colors.append((r, g, b, a))
            m.uvs.append((u, v))
            slot = v0 + i
            if 0 <= slot < 64:
                self._vtx_buf[slot] = local_idx

    def _emit_tri(self, i0, i1, i2):
        m = self._cur_mesh
        if m is None: return
        g0 = self._vtx_buf[i0] if 0 <= i0 < 64 else None
        g1 = self._vtx_buf[i1] if 0 <= i1 < 64 else None
        g2 = self._vtx_buf[i2] if 0 <= i2 < 64 else None
        if g0 is not None and g1 is not None and g2 is not None:
            m.tris.append((g0, g1, g2, self._cur_tex_id))

    # ── Texture Conversion ───────────────────────────────────────────────────

    def finalize_textures(self):
        for i, tex in enumerate(self.textures):
            seg = (tex.timg_addr >> 24) & 0x0F
            if seg not in self.segments:
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
                  f"fmt={tex.fmt} siz={tex.siz} -> {bpp} "
                  f"{tex.num_clut_colors} colors, "
                  f"{len(tex.ps1_pixels)} px bytes")

    def _make_fallback(self, tex):
        tex.width = 4; tex.height = 4
        tex.ps1_4bit = True; tex.num_clut_colors = 1
        tex.ps1_pixels = bytes(8)
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
                hi = remap[raw[j + 1]] if j + 1 < npix else 0
                packed[j // 2] = lo | (hi << 4)
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
        # Swap nibbles: N64 CI4 = high nibble first, PS1 4-bit = low nibble first
        raw = pix_buf[pix_off:pix_off + nbytes]
        tex.ps1_pixels = bytes(((b >> 4) | ((b & 0x0F) << 4)) for b in raw)
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
        unique = list(dict.fromkeys(ps1_colors))
        npix = len(ps1_colors)
        if len(unique) > 256:
            unique.sort(key=lambda c: (c & 0x1F) * 3 + ((c >> 5) & 0x1F) * 6 + ((c >> 10) & 0x1F))
            step = len(unique) / 256
            unique = [unique[int(i * step)] for i in range(256)]
        color_to_idx = {c: i for i, c in enumerate(unique)}
        use_4bit = len(unique) <= 16

        def nearest(c):
            if c in color_to_idx: return color_to_idx[c]
            r, g, b = c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F
            best_i = 0; best_d = 99999
            for pi, pc in enumerate(unique):
                pr, pg, pb = pc & 0x1F, (pc >> 5) & 0x1F, (pc >> 10) & 0x1F
                d = abs(r - pr) + abs(g - pg) + abs(b - pb)
                if d < best_d: best_d = d; best_i = pi
            color_to_idx[c] = best_i
            return best_i

        if use_4bit:
            packed = bytearray(npix // 2)
            for j in range(0, npix, 2):
                lo = nearest(ps1_colors[j])
                hi = nearest(ps1_colors[j + 1]) if j + 1 < npix else 0
                packed[j // 2] = lo | (hi << 4)
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


# ── Animation Extraction ─────────────────────────────────────────────────────

def extract_animations(anim_data: bytes):
    anims = []
    for name, frame_count, offset, loop in ANIMATIONS:
        frames = []
        for f in range(frame_count):
            frame_off = offset + f * FRAME_SIZE
            if frame_off + FRAME_SIZE > len(anim_data):
                print(f"  [WARN] Anim '{name}' frame {f} truncated")
                break
            # Byte-swap 67 big-endian s16 values to little-endian
            # First 3 values are root position (scale to world space);
            # remaining 63 s16 are limb rotations + face (no scaling).
            frame_bytes = bytearray(FRAME_SIZE)
            for j in range(67):  # 22 Vec3s (66 s16) + 1 face u16 = 67
                val = struct.unpack_from(">h", anim_data, frame_off + j * 2)[0]
                if j < 3:  # root_x, root_y, root_z
                    val = int(round(val / MODEL_SCALE))
                struct.pack_into("<h", frame_bytes, j * 2, val)
            frames.append(bytes(frame_bytes))
        anims.append((name, frame_count, loop, frames))
        print(f"  '{name}': {len(frames)} frames {'(loop)' if loop else ''}")
    return anims


# ── SKM Binary Export ─────────────────────────────────────────────────────────
#
# HEADER (20 bytes):
#   magic[4]       "SKM\x01"
#   num_limbs      u8
#   num_anims      u8
#   num_textures   u16
#   mesh_start     u32
#   anim_start     u32
#   tex_start      u32
#
# LIMB_DESC[num_limbs] (12 bytes each):
#   joint_x/y/z    s16*3
#   child          u8
#   sibling        u8
#   num_verts      u16
#   num_tris       u16
#
# MESH DATA (at mesh_start, per limb sequential):
#   Pos[nv*8]  Color[nv*4]  UV[nv*2 padded]  Tri[nt*4]
#
# ANIM SECTION (at anim_start):
#   AnimDesc[num_anims] (8 bytes each):
#     frame_count u16, flags u8, reserved u8, data_offset u32
#   Frame data (134 bytes each):
#     root_pos s16*3, rotations s16*3*21, face u16

def export_skm(limbs, limb_meshes, anims, textures, path):
    num_limbs = len(limbs)
    num_anims = len(anims)
    num_tex = len(textures)

    header_size = 20
    limb_desc_size = num_limbs * 12

    # Mesh section
    mesh_start = (header_size + limb_desc_size + 3) & ~3
    mesh_data_size = 0
    mesh_offsets = []
    for m in limb_meshes:
        mesh_offsets.append(mesh_data_size)
        nv = len(m.verts); nt = len(m.tris)
        mesh_data_size += nv * 8 + nv * 4 + ((nv * 2 + 3) & ~3) + nt * 4

    # Animation section
    anim_start = (mesh_start + mesh_data_size + 3) & ~3
    anim_desc_size = num_anims * 8
    anim_data_size = 0
    anim_offsets = []
    for _, fc, _, frames in anims:
        anim_offsets.append(anim_data_size)
        anim_data_size += len(frames) * FRAME_SIZE

    # Texture section
    tex_start = (anim_start + anim_desc_size + anim_data_size + 3) & ~3
    tex_desc_size = num_tex * 12
    tex_data_off = 0
    tex_offsets = []
    for tex in textures:
        tex_offsets.append(tex_data_off)
        pix_sz = len(tex.ps1_pixels) if tex.ps1_pixels else 0
        clut_sz = len(tex.ps1_clut) if tex.ps1_clut else 0
        tex_data_off += pix_sz + clut_sz
        tex_data_off = (tex_data_off + 3) & ~3

    total_size = tex_start + tex_desc_size + tex_data_off
    buf = bytearray(total_size)

    # Header
    buf[0:4] = b"SKM\x01"
    buf[4] = num_limbs
    buf[5] = num_anims
    struct.pack_into("<H", buf, 6, num_tex)
    struct.pack_into("<III", buf, 8, mesh_start, anim_start, tex_start)

    # LimbDesc[]
    for i, ((joint, child, sib, _), mesh) in enumerate(zip(limbs, limb_meshes)):
        off = header_size + i * 12
        struct.pack_into("<hhhBBHH", buf, off,
                         joint[0], joint[1], joint[2],
                         child, sib,
                         len(mesh.verts), len(mesh.tris))

    # Mesh data per limb
    for i, mesh in enumerate(limb_meshes):
        base = mesh_start + mesh_offsets[i]
        nv = len(mesh.verts)
        for j, (x, y, z) in enumerate(mesh.verts):
            struct.pack_into("<hhhh", buf, base + j * 8, x, y, z, 0)
        col_base = base + nv * 8
        for j, (r, g, b, a) in enumerate(mesh.colors):
            struct.pack_into("<BBBB", buf, col_base + j * 4, r, g, b, a)
        uv_base = col_base + nv * 4
        for j, (u, v) in enumerate(mesh.uvs):
            struct.pack_into("<BB", buf, uv_base + j * 2, u, v)
        uv_sz = (nv * 2 + 3) & ~3
        tri_base = uv_base + uv_sz
        for j, (v0, v1, v2, tid) in enumerate(mesh.tris):
            struct.pack_into("<BBBB", buf, tri_base + j * 4, v0, v1, v2, tid)

    # AnimDesc[]
    for i, (name, fc, loop, frames) in enumerate(anims):
        off = anim_start + i * 8
        flags = 1 if loop else 0
        struct.pack_into("<HBxI", buf, off, fc, flags, anim_offsets[i])

    # Animation frame data
    anim_data_base = anim_start + anim_desc_size
    for i, (_, _, _, frames) in enumerate(anims):
        off = anim_data_base + anim_offsets[i]
        for frame in frames:
            buf[off:off + FRAME_SIZE] = frame
            off += FRAME_SIZE

    # TexDesc[]
    for i, tex in enumerate(textures):
        off = tex_start + i * 12
        fmt = 0 if tex.ps1_4bit else 1
        nc = tex.num_clut_colors & 0xFF
        struct.pack_into("<HH BB H I", buf, off,
                         tex.width, tex.height, fmt, nc, 0, tex_offsets[i])

    # Texture pixel + CLUT data
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

    total_v = sum(len(m.verts) for m in limb_meshes)
    total_t = sum(len(m.tris) for m in limb_meshes)
    total_frames = sum(len(frames) for _, _, _, frames in anims)
    print(f"\n  SKM: {total_size} bytes ({total_size / 1024:.1f} KB)")
    print(f"    {num_limbs} limbs, {total_v} verts, {total_t} tris")
    print(f"    {num_anims} anims, {total_frames} frames")
    print(f"    {num_tex} textures")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 extract_skeleton.py <rom> --skm output.skm")
        sys.exit(1)

    rom_path = sys.argv[1]
    skm_path = None
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--skm" and i + 1 < len(sys.argv):
            skm_path = sys.argv[i + 1]; i += 2
        else:
            i += 1

    if not skm_path:
        print("ERROR: --skm output path required")
        sys.exit(1)

    print(f"Loading ROM: {rom_path}")
    with open(rom_path, "rb") as f:
        rom = f.read()
    print(f"  {len(rom) / 1024 / 1024:.1f} MB")

    dma = load_dma_table(rom)

    # Load object_link_boy (segment 6)
    obj_entry = dma.get("object_link_boy")
    if not obj_entry:
        print("ERROR: object_link_boy not found in DMA table")
        sys.exit(1)
    print(f"\nLoading object_link_boy ({obj_entry.vrom_size} bytes)...")
    obj_data = load_file(rom, obj_entry)
    print(f"  Decompressed: {len(obj_data)} bytes")

    # Load link_animetion (segment 7)
    anim_entry = dma.get("link_animetion")
    if not anim_entry:
        print("ERROR: link_animetion not found in DMA table")
        sys.exit(1)
    print(f"Loading link_animetion ({anim_entry.vrom_size} bytes)...")
    anim_data = load_file(rom, anim_entry)
    print(f"  Decompressed: {len(anim_data)} bytes")

    # Parse skeleton
    print(f"\nParsing skeleton...")
    ext = SkeletonExtractor(obj_data)
    ext.parse_skeleton()
    ext.extract_meshes()

    # Convert textures
    if ext.textures:
        print(f"\nConverting {len(ext.textures)} textures...")
        ext.finalize_textures()

    # Extract animations
    print(f"\nExtracting {len(ANIMATIONS)} animations...")
    anims = extract_animations(anim_data)

    # Export SKM
    print(f"\nExporting SKM...")
    export_skm(ext.limbs, ext.limb_meshes, anims, ext.textures, skm_path)

    print("\nDone!")


if __name__ == "__main__":
    main()
