// SKM v1 — PS1 Skeletal Mesh format
// Binary format for skeletal mesh + animations, optimized for PS1 GTE rendering.
//
// Layout (all little-endian, 4-byte aligned):
//   Header         20 bytes
//   LimbDesc[]     num_limbs * 12 bytes
//   Mesh data      (at mesh_start, per limb sequential)
//     Per limb:    positions[nv*8] | colors[nv*4] | uvs[nv*2 padded] | indices[nt*4]
//   Anim section   (at anim_start)
//     AnimDesc[]   num_anims * 8 bytes
//     Frame data   (contiguous, 134 bytes per frame)
//   Texture section (at tex_start, same layout as PRM)
//
// Skeleton uses child/sibling tree traversal (0xFF = none).
// Animation frames: root_pos(6) + 21 limb rotations(126) + face(2) = 134 bytes.
// Rotations are ZYX Euler s16 binary angles (full circle = 0x10000).

#pragma once
#include <stdint.h>

namespace SKM {

struct Header {
    uint8_t  magic[4];       // "SKM\x01"
    uint8_t  num_limbs;
    uint8_t  num_anims;
    uint16_t num_textures;
    uint32_t mesh_start;     // byte offset to per-limb mesh data
    uint32_t anim_start;     // byte offset to animation section
    uint32_t tex_start;      // byte offset to texture section
};
static_assert(sizeof(Header) == 20);

struct LimbDesc {
    int16_t  joint_x, joint_y, joint_z;
    uint8_t  child;          // child limb index (0xFF = none)
    uint8_t  sibling;        // sibling limb index (0xFF = none)
    uint16_t num_verts;
    uint16_t num_tris;
};
static_assert(sizeof(LimbDesc) == 12);

struct AnimDesc {
    uint16_t frame_count;
    uint8_t  flags;          // bit 0 = loop
    uint8_t  reserved;
    uint32_t data_offset;    // from animation data base (after AnimDesc array)
};
static_assert(sizeof(AnimDesc) == 8);

// Per-frame layout (134 bytes):
//   s16 root_x, root_y, root_z     (6 bytes — root translation)
//   s16 rot[21][3]                  (126 bytes — ZYX euler per limb)
//   u16 face                        (2 bytes — eye/mouth index)
static constexpr int FRAME_SIZE = 134;

// GTE-native vertex types (same as PRM)
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
    uint8_t  format;          // 0 = 4-bit, 1 = 8-bit
    uint8_t  num_clut_colors; // 0 means 256
    uint16_t reserved;
    uint32_t data_offset;     // from tex data start
};
static_assert(sizeof(TexDesc) == 12);

// ── Runtime accessors (zero-copy from binary blob) ──

inline const Header* header(const uint8_t* skm) {
    return reinterpret_cast<const Header*>(skm);
}

inline const LimbDesc* limbs(const uint8_t* skm) {
    return reinterpret_cast<const LimbDesc*>(skm + sizeof(Header));
}

inline const uint8_t* meshBase(const uint8_t* skm) {
    return skm + header(skm)->mesh_start;
}

// Get mesh data pointers for a specific limb
inline const Pos* limbPositions(const uint8_t* skm, int limbIdx) {
    const auto* h = header(skm);
    const auto* ls = limbs(skm);
    const uint8_t* p = skm + h->mesh_start;
    for (int i = 0; i < limbIdx; i++) {
        uint16_t nv = ls[i].num_verts;
        uint16_t nt = ls[i].num_tris;
        p += nv * 8 + nv * 4 + ((nv * 2 + 3) & ~3u) + nt * 4;
    }
    return reinterpret_cast<const Pos*>(p);
}

inline const Color* limbColors(const uint8_t* skm, int limbIdx) {
    const auto* ls = limbs(skm);
    return reinterpret_cast<const Color*>(
        reinterpret_cast<const uint8_t*>(limbPositions(skm, limbIdx))
        + ls[limbIdx].num_verts * sizeof(Pos));
}

inline const UV* limbUVs(const uint8_t* skm, int limbIdx) {
    const auto* ls = limbs(skm);
    return reinterpret_cast<const UV*>(
        reinterpret_cast<const uint8_t*>(limbColors(skm, limbIdx))
        + ls[limbIdx].num_verts * sizeof(Color));
}

inline const Tri* limbTriangles(const uint8_t* skm, int limbIdx) {
    const auto* ls = limbs(skm);
    uint32_t uv_sz = (ls[limbIdx].num_verts * sizeof(UV) + 3) & ~3u;
    return reinterpret_cast<const Tri*>(
        reinterpret_cast<const uint8_t*>(limbUVs(skm, limbIdx)) + uv_sz);
}

// Animation accessors
inline const AnimDesc* animDescs(const uint8_t* skm) {
    return reinterpret_cast<const AnimDesc*>(skm + header(skm)->anim_start);
}

inline const uint8_t* animDataBase(const uint8_t* skm) {
    return skm + header(skm)->anim_start
           + header(skm)->num_anims * sizeof(AnimDesc);
}

inline const int16_t* animFrame(const uint8_t* skm, int animIdx, int frame) {
    const auto* ad = &animDescs(skm)[animIdx];
    const uint8_t* base = animDataBase(skm) + ad->data_offset;
    return reinterpret_cast<const int16_t*>(base + frame * FRAME_SIZE);
}

// Frame data accessors (from a frame's s16 pointer)
// Layout: [root_x, root_y, root_z, rot0_z, rot0_y, rot0_x, rot1_z, ...]
inline void frameRootPos(const int16_t* frame, int16_t& x, int16_t& y, int16_t& z) {
    x = frame[0]; y = frame[1]; z = frame[2];
}

inline void frameLimbRot(const int16_t* frame, int limbIdx,
                         int16_t& rz, int16_t& ry, int16_t& rx) {
    // Limb 0 rotation starts at index 3 (after root pos)
    // OoT Vec3s stores {x, y, z} — map to rx, ry, rz
    const int16_t* r = &frame[3 + limbIdx * 3];
    rx = r[0]; ry = r[1]; rz = r[2];
}

// Texture section (same as PRM)
inline const TexDesc* texDescs(const uint8_t* skm) {
    return reinterpret_cast<const TexDesc*>(skm + header(skm)->tex_start);
}

inline const uint8_t* texData(const uint8_t* skm) {
    return skm + header(skm)->tex_start
           + header(skm)->num_textures * sizeof(TexDesc);
}

inline uint32_t texPixelSize(const TexDesc& td) {
    if (td.format == 0) return (td.width * td.height + 1) / 2;
    return td.width * td.height;
}

inline uint32_t texClutCount(const TexDesc& td) {
    return td.num_clut_colors == 0 ? 256 : td.num_clut_colors;
}

// Cache limb mesh offsets to avoid O(n^2) traversal at runtime
struct LimbMeshCache {
    uint32_t offsets[21];  // byte offset from mesh_start for each limb

    void build(const uint8_t* skm) {
        const auto* ls = limbs(skm);
        uint32_t off = 0;
        for (int i = 0; i < header(skm)->num_limbs && i < 21; i++) {
            offsets[i] = off;
            uint16_t nv = ls[i].num_verts;
            uint16_t nt = ls[i].num_tris;
            off += nv * 8 + nv * 4 + ((nv * 2 + 3) & ~3u) + nt * 4;
        }
    }

    const Pos* positions(const uint8_t* skm, int limbIdx) const {
        return reinterpret_cast<const Pos*>(
            skm + header(skm)->mesh_start + offsets[limbIdx]);
    }

    const Color* colors(const uint8_t* skm, int limbIdx) const {
        return reinterpret_cast<const Color*>(
            reinterpret_cast<const uint8_t*>(positions(skm, limbIdx))
            + limbs(skm)[limbIdx].num_verts * sizeof(Pos));
    }

    const UV* uvs(const uint8_t* skm, int limbIdx) const {
        return reinterpret_cast<const UV*>(
            reinterpret_cast<const uint8_t*>(colors(skm, limbIdx))
            + limbs(skm)[limbIdx].num_verts * sizeof(Color));
    }

    const Tri* triangles(const uint8_t* skm, int limbIdx) const {
        uint32_t uv_sz = (limbs(skm)[limbIdx].num_verts * sizeof(UV) + 3) & ~3u;
        return reinterpret_cast<const Tri*>(
            reinterpret_cast<const uint8_t*>(uvs(skm, limbIdx)) + uv_sz);
    }
};

}  // namespace SKM
