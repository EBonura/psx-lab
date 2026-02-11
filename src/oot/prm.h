// PRM v2 — PS1 Room Mesh format with textures
// Binary format for OoT room geometry + textures, optimized for PS1 GTE rendering.
//
// Layout (all little-endian, 4-byte aligned):
//   Header       20 bytes
//   ChunkDesc[]  num_chunks * 16 bytes
//   Chunk data   (contiguous)
//     Per chunk:  positions[nv*8] | colors[nv*4] | uvs[nv*2 padded] | indices[nt*4]
//   Texture section (at tex_start)
//     TexDesc[]  num_textures * 12 bytes
//     Per-texture: pixel data then CLUT data (contiguous blocks)
//
// Vertices are GTE-native SVectors (int16 x,y,z,0).
// Triangle indices are uint8, local to each chunk (max 255 verts/chunk).
// Each triangle has a tex_id linking to the texture table.

#pragma once
#include <stdint.h>

namespace PRM {

struct Header {
    uint8_t  magic[4];      // "PRM\x02"
    uint16_t num_chunks;
    uint16_t num_verts;     // total (stats only)
    uint16_t num_tris;      // total (stats only)
    uint16_t num_textures;
    uint32_t data_start;    // byte offset to first chunk's data
    uint32_t tex_start;     // byte offset to texture section
};
static_assert(sizeof(Header) == 20);

struct ChunkDesc {
    int16_t  cx, cy, cz;   // bounding sphere center
    int16_t  radius;        // bounding sphere radius
    uint16_t num_verts;
    uint16_t num_tris;
    uint32_t data_offset;   // from data_start to this chunk's positions[]
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

struct UV {
    uint8_t u, v;
};
static_assert(sizeof(UV) == 2);

struct Tri {
    uint8_t v0, v1, v2, tex_id;
};
static_assert(sizeof(Tri) == 4);

struct TexDesc {
    uint16_t width, height;
    uint8_t  format;            // 0 = 4-bit indexed, 1 = 8-bit indexed
    uint8_t  num_clut_colors;   // 0 means 256 (for 8-bit)
    uint16_t reserved;
    uint32_t data_offset;       // from tex data start (after TexDesc array)
};
static_assert(sizeof(TexDesc) == 12);

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

inline const UV* uvs(const uint8_t* prm, const ChunkDesc& c) {
    return reinterpret_cast<const UV*>(
        reinterpret_cast<const uint8_t*>(colors(prm, c)) + c.num_verts * sizeof(Color));
}

inline const Tri* triangles(const uint8_t* prm, const ChunkDesc& c) {
    // UVs are padded to 4-byte alignment
    uint32_t uv_size = (c.num_verts * sizeof(UV) + 3) & ~3u;
    return reinterpret_cast<const Tri*>(
        reinterpret_cast<const uint8_t*>(uvs(prm, c)) + uv_size);
}

// Texture section accessors
inline const TexDesc* texDescs(const uint8_t* prm) {
    return reinterpret_cast<const TexDesc*>(prm + header(prm)->tex_start);
}

inline const uint8_t* texData(const uint8_t* prm) {
    return prm + header(prm)->tex_start + header(prm)->num_textures * sizeof(TexDesc);
}

// Get pixel data size for a texture
inline uint32_t texPixelSize(const TexDesc& td) {
    if (td.format == 0) return (td.width * td.height + 1) / 2;  // 4-bit
    return td.width * td.height;                                  // 8-bit
}

// Get number of CLUT colors (handles 0 = 256)
inline uint32_t texClutCount(const TexDesc& td) {
    return td.num_clut_colors == 0 ? 256 : td.num_clut_colors;
}

}  // namespace PRM
