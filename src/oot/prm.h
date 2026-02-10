// PRM — PS1 Room Mesh format
// Binary format for OoT room geometry, optimized for PS1 GTE rendering.
//
// Layout (all little-endian, 4-byte aligned):
//   Header      16 bytes
//   ChunkDesc[] num_chunks * 16 bytes
//   Chunk data  (contiguous)
//     Per chunk:  positions[nv*8] | colors[nv*4] | indices[nt*4]
//
// Vertices are GTE-native SVectors (int16 x,y,z,0).
// Triangle indices are uint8, local to each chunk (max 255 verts/chunk).
// Chunks carry bounding spheres for frustum culling.

#pragma once
#include <stdint.h>

namespace PRM {

struct Header {
    uint8_t  magic[4];     // "PRM\x01"
    uint16_t num_chunks;
    uint16_t num_verts;    // total (stats only)
    uint16_t num_tris;     // total (stats only)
    uint16_t flags;
    uint32_t data_start;   // byte offset to first chunk's data
};
static_assert(sizeof(Header) == 16);

struct ChunkDesc {
    int16_t  cx, cy, cz;  // bounding sphere center
    int16_t  radius;       // bounding sphere radius
    uint16_t num_verts;
    uint16_t num_tris;
    uint32_t data_offset;  // from data_start to this chunk's positions[]
};
static_assert(sizeof(ChunkDesc) == 16);

// GTE-native vertex position (SVector)
struct Pos {
    int16_t x, y, z, pad;
};
static_assert(sizeof(Pos) == 8);

struct Color {
    uint8_t r, g, b, a;
};
static_assert(sizeof(Color) == 4);

struct Tri {
    uint8_t v0, v1, v2, pad;
};
static_assert(sizeof(Tri) == 4);

// ── Runtime accessors (zero-copy, reads directly from the binary blob) ──

inline const Header* header(const uint8_t* prm) {
    return reinterpret_cast<const Header*>(prm);
}

inline const ChunkDesc* chunks(const uint8_t* prm) {
    return reinterpret_cast<const ChunkDesc*>(prm + sizeof(Header));
}

inline const Pos* positions(const uint8_t* prm, const ChunkDesc& c) {
    return reinterpret_cast<const Pos*>(prm + header(prm)->data_start + c.data_offset);
}

inline const Color* colors(const uint8_t* prm, const ChunkDesc& c) {
    return reinterpret_cast<const Color*>(
        reinterpret_cast<const uint8_t*>(positions(prm, c)) + c.num_verts * sizeof(Pos));
}

inline const Tri* triangles(const uint8_t* prm, const ChunkDesc& c) {
    return reinterpret_cast<const Tri*>(
        reinterpret_cast<const uint8_t*>(colors(prm, c)) + c.num_verts * sizeof(Color));
}

}  // namespace PRM
