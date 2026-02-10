#!/usr/bin/env python3
"""Extract room geometry from an OoT ROM into PS1-native .prm format.

Usage:
  python3 extract_room.py <rom_path> <room_name> --prm output.prm [--obj output.obj]

Reads the DMA table and segments.csv for NTSC 1.2 to locate a room file,
decompresses it (Yaz0), parses the room header, walks F3DEX2 display lists,
and outputs a .prm binary optimized for PS1 GTE rendering.

PRM format (all little-endian, 4-byte aligned):
  Header       16 bytes
  ChunkDesc[]  num_chunks * 16 bytes
  --- chunk data (contiguous, 4-byte aligned per section) ---
  Per chunk:
    positions[]  num_verts * 8   (int16 x,y,z,pad — GTE SVector)
    colors[]     num_verts * 4   (uint8 r,g,b,a)
    indices[]    num_tris  * 4   (uint8 v0,v1,v2,pad — local to chunk)
"""

import struct
import csv
import sys
import math
from pathlib import Path

# ── Configuration ──────────────────────────────────────────────────────────

DECOMP_ROOT = Path(__file__).resolve().parent.parent.parent / "third_party" / "oot"
SEGMENTS_CSV = DECOMP_ROOT / "baseroms" / "ntsc-1.2" / "segments.csv"
DMA_TABLE_OFFSET = 0x7960  # NTSC 1.2

# F3DEX2 opcodes
G_VTX   = 0x01; G_TRI1 = 0x05; G_TRI2 = 0x06
G_DL    = 0xDE; G_ENDDL = 0xDF
G_SETPRIMCOLOR = 0xFA; G_SETENVCOLOR = 0xFB

CMD_ROOM_SHAPE = 0x0A; CMD_END = 0x14

# Chunk vertex limit — enforces uint8 indices, fits scratchpad for batch xform
MAX_CHUNK_VERTS = 255

# ── Yaz0 Decompression ────────────────────────────────────────────────────

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

# ── ROM / DMA ─────────────────────────────────────────────────────────────

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

# ── Chunk-aware extraction ────────────────────────────────────────────────

class Chunk:
    """One spatial group of geometry (maps to one OoT cullable/normal entry)."""
    __slots__ = ("verts", "colors", "tris", "cx", "cy", "cz", "radius")
    def __init__(self):
        self.verts = []    # [(x,y,z), ...]
        self.colors = []   # [(r,g,b,a), ...]
        self.tris = []     # [(v0,v1,v2), ...] local indices
        self.cx = self.cy = self.cz = 0
        self.radius = 0

    def compute_bounds(self):
        if not self.verts:
            return
        xs = [v[0] for v in self.verts]
        ys = [v[1] for v in self.verts]
        zs = [v[2] for v in self.verts]
        self.cx = (min(xs) + max(xs)) // 2
        self.cy = (min(ys) + max(ys)) // 2
        self.cz = (min(zs) + max(zs)) // 2
        # Bounding sphere radius from center
        max_r2 = 0
        for x, y, z in self.verts:
            dx = x - self.cx; dy = y - self.cy; dz = z - self.cz
            r2 = dx*dx + dy*dy + dz*dz
            if r2 > max_r2: max_r2 = r2
        self.radius = min(int(math.isqrt(max_r2)) + 1, 32767)

class RoomExtractor:
    def __init__(self, room_data: bytes):
        self.data = room_data
        self.chunks = []         # [Chunk, ...]
        self._cur_chunk = None   # Active chunk during DL walk
        self._vtx_buf = [None] * 64   # RSP vtx buf slot → (local_idx, chunk)
        self._remap = {}         # (chunk_id, global_n64_vtx_key) → local_idx
        self.stats = {"vtx_cmds": 0, "tri1_cmds": 0, "tri2_cmds": 0,
                      "dl_calls": 0, "unknown_cmds": set()}

    def resolve(self, addr):
        seg = (addr >> 24) & 0x0F; off = addr & 0x00FFFFFF
        return off if seg == 3 and off < len(self.data) else None

    def extract(self):
        off = 0
        while off + 8 <= len(self.data):
            cmd = self.data[off]
            if cmd == CMD_END: break
            if cmd == CMD_ROOM_SHAPE:
                self._parse_room_shape(read_u32(self.data, off + 4))
            off += 8
        # Compute bounding spheres
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

        if shape_type == 2:   # CULLABLE (16-byte entries with bounds)
            for i in range(num_entries):
                eo = eoff + i * 16
                cx = read_s16(self.data, eo)
                cy = read_s16(self.data, eo + 2)
                cz = read_s16(self.data, eo + 4)
                radius = read_s16(self.data, eo + 6)
                opa = read_u32(self.data, eo + 8)
                xlu = read_u32(self.data, eo + 12)
                self._begin_chunk()
                # Use OoT's authored bounds if available
                self._cur_chunk.cx = cx
                self._cur_chunk.cy = cy
                self._cur_chunk.cz = cz
                self._cur_chunk.radius = radius
                if opa: self._walk_dl(opa)
                if xlu: self._walk_dl(xlu)
                self._end_chunk(use_authored_bounds=True)
        elif shape_type == 0:  # NORMAL (8-byte entries)
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
            self._cur_chunk = None
            return
        # Split if over MAX_CHUNK_VERTS
        if len(c.verts) > MAX_CHUNK_VERTS:
            self._split_and_append(c, use_authored_bounds)
        else:
            if not use_authored_bounds:
                c.compute_bounds()
            self.chunks.append(c)
        self._cur_chunk = None

    def _split_and_append(self, big_chunk, use_authored_bounds):
        """Split a chunk with >255 verts into sub-chunks that fit uint8 indices."""
        verts = big_chunk.verts
        colors = big_chunk.colors
        tris = big_chunk.tris

        # Greedy: assign triangles to sub-chunks, each capped at MAX_CHUNK_VERTS
        assigned = [False] * len(tris)
        while True:
            sub = Chunk()
            local_map = {}  # old_local_idx → new_local_idx
            remaining = []
            for ti, (a, b, c) in enumerate(tris):
                if assigned[ti]: continue
                needed = set()
                if a not in local_map: needed.add(a)
                if b not in local_map: needed.add(b)
                if c not in local_map: needed.add(c)
                if len(sub.verts) + len(needed) > MAX_CHUNK_VERTS:
                    continue  # Skip, try later
                for vi in needed:
                    local_map[vi] = len(sub.verts)
                    sub.verts.append(verts[vi])
                    sub.colors.append(colors[vi])
                sub.tris.append((local_map[a], local_map[b], local_map[c]))
                assigned[ti] = True

            if not sub.tris:
                break  # All assigned
            sub.compute_bounds()
            self.chunks.append(sub)
            if all(assigned):
                break

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
            elif cmd == G_DL:
                if (w0 >> 16) & 0xFF == 0:  # call
                    self._walk_dl(w1, depth+1); self.stats["dl_calls"] += 1
                else:  # branch
                    new_off = self.resolve(w1)
                    if new_off is not None: off = new_off
            elif cmd == G_ENDDL:
                break
            else:
                self.stats["unknown_cmds"].add(cmd)

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
            r = self.data[vo + 12]; g = self.data[vo + 13]
            b = self.data[vo + 14]; a = self.data[vo + 15]
            local_idx = len(c.verts)
            c.verts.append((x, y, z))
            c.colors.append((r, g, b, a))
            slot = v0 + i
            if slot < 64:
                self._vtx_buf[slot] = local_idx

    def _emit_tri(self, i0, i1, i2):
        c = self._cur_chunk
        if c is None: return
        g0 = self._vtx_buf[i0] if i0 < 64 else None
        g1 = self._vtx_buf[i1] if i1 < 64 else None
        g2 = self._vtx_buf[i2] if i2 < 64 else None
        if g0 is not None and g1 is not None and g2 is not None:
            c.tris.append((g0, g1, g2))

# ── PRM Binary Export ─────────────────────────────────────────────────────
#
# All little-endian. Every section 4-byte aligned.
#
# HEADER (16 bytes):
#   0x00  u8[4]  magic     "PRM\x01"
#   0x04  u16    num_chunks
#   0x06  u16    num_verts  (total, for stats)
#   0x08  u16    num_tris   (total, for stats)
#   0x0A  u16    flags      (reserved, 0)
#   0x0C  u32    data_start (byte offset to first chunk's data)
#
# CHUNK TABLE (num_chunks * 16 bytes):
#   0x00  s16    cx, cy, cz    bounding sphere center
#   0x06  s16    radius        bounding sphere radius
#   0x08  u16    num_verts
#   0x0A  u16    num_tris
#   0x0C  u32    data_offset   byte offset from data_start to this chunk's positions[]
#
# CHUNK DATA (per chunk, contiguous):
#   positions[num_verts]   — 8 bytes each: s16 x, s16 y, s16 z, s16 pad(0)
#   colors[num_verts]      — 4 bytes each: u8 r, u8 g, u8 b, u8 a
#   indices[num_tris]      — 4 bytes each: u8 v0, u8 v1, u8 v2, u8 pad(0)

def export_prm(chunks, path):
    total_v = sum(len(c.verts) for c in chunks)
    total_t = sum(len(c.tris) for c in chunks)

    header_size = 16
    chunk_table_size = len(chunks) * 16
    data_start = header_size + chunk_table_size
    # Align data_start to 4 bytes
    data_start = (data_start + 3) & ~3

    # Pre-compute data offsets for each chunk
    data_offset = 0
    chunk_offsets = []
    for c in chunks:
        chunk_offsets.append(data_offset)
        pos_sz = len(c.verts) * 8
        col_sz = len(c.verts) * 4
        idx_sz = len(c.tris) * 4
        data_offset += pos_sz + col_sz + idx_sz
        # Align next chunk to 4 bytes (already aligned since all sizes are *4)

    total_size = data_start + data_offset
    buf = bytearray(total_size)

    # Write header
    buf[0:4] = b"PRM\x01"
    struct.pack_into("<HHHHi", buf, 4,
                     len(chunks), total_v, total_t, 0, data_start)

    # Write chunk table
    for i, c in enumerate(chunks):
        off = header_size + i * 16
        struct.pack_into("<hhhh HH I", buf, off,
                         c.cx, c.cy, c.cz, c.radius,
                         len(c.verts), len(c.tris),
                         chunk_offsets[i])

    # Write chunk data
    for i, c in enumerate(chunks):
        base = data_start + chunk_offsets[i]
        # Positions (GTE SVector: x, y, z, 0)
        for j, (x, y, z) in enumerate(c.verts):
            struct.pack_into("<hhhh", buf, base + j * 8, x, y, z, 0)
        # Colors
        col_base = base + len(c.verts) * 8
        for j, (r, g, b, a) in enumerate(c.colors):
            struct.pack_into("<BBBB", buf, col_base + j * 4, r, g, b, a)
        # Triangle indices (uint8, local to chunk)
        idx_base = col_base + len(c.verts) * 4
        for j, (v0, v1, v2) in enumerate(c.tris):
            struct.pack_into("<BBBB", buf, idx_base + j * 4, v0, v1, v2, 0)

    with open(path, "wb") as f:
        f.write(buf)

    pos_bytes = total_v * 8
    col_bytes = total_v * 4
    idx_bytes = total_t * 4
    overhead = data_start + sum(0 for _ in chunks)  # chunk table in data_start
    print(f"  PRM: {total_size} bytes ({total_size/1024:.1f} KB)")
    print(f"    {len(chunks)} chunks, {total_v} verts, {total_t} tris")
    print(f"    positions: {pos_bytes} B  colors: {col_bytes} B  indices: {idx_bytes} B")
    print(f"    header+table: {data_start} B")
    max_cv = max((len(c.verts) for c in chunks), default=0)
    max_ct = max((len(c.tris) for c in chunks), default=0)
    print(f"    max chunk: {max_cv} verts, {max_ct} tris")

# ── OBJ Export (debug) ────────────────────────────────────────────────────

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
            for v0, v1, v2 in c.tris:
                f.write(f"f {vert_offset+v0+1} {vert_offset+v1+1} {vert_offset+v2+1}\n")
            vert_offset += len(c.verts)
    print(f"  OBJ: {path} ({total_v} verts, {total_t} tris)")

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 extract_room.py <rom> <room_name> --prm out.prm [--obj out.obj]")
        print("  e.g.: python3 extract_room.py rom.z64 spot04_room_0 --prm kokiri.prm --obj kokiri.obj")
        sys.exit(1)

    rom_path = sys.argv[1]; room_name = sys.argv[2]
    prm_path = obj_path = None
    for flag, attr in [("--prm", "prm"), ("--obj", "obj")]:
        if flag in sys.argv:
            i = sys.argv.index(flag)
            if i + 1 < len(sys.argv):
                if attr == "prm": prm_path = sys.argv[i+1]
                else: obj_path = sys.argv[i+1]

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

    extractor = RoomExtractor(room_data)
    chunks = extractor.extract()

    total_v = sum(len(c.verts) for c in chunks)
    total_t = sum(len(c.tris) for c in chunks)
    print(f"\nExtracted: {total_v} verts, {total_t} tris in {len(chunks)} chunks")
    print(f"  DL commands: {extractor.stats['vtx_cmds']} VTX, "
          f"{extractor.stats['tri1_cmds']} TRI1, {extractor.stats['tri2_cmds']} TRI2")
    if extractor.stats["unknown_cmds"]:
        print(f"  Skipped: {', '.join(f'0x{c:02x}' for c in sorted(extractor.stats['unknown_cmds']))}")

    for i, c in enumerate(chunks):
        print(f"  chunk {i:2d}: {len(c.verts):4d}v {len(c.tris):4d}t  "
              f"center=({c.cx},{c.cy},{c.cz}) r={c.radius}")

    if prm_path:
        export_prm(chunks, prm_path)
    if obj_path:
        export_obj(chunks, obj_path)

if __name__ == "__main__":
    main()
