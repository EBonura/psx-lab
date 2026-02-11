// N64 Graphics Binary Interface (GBI) types for F3DEX2
// Minimal definitions needed to interpret OoT display lists on PS1.
// Reference: zeldaret/oot include/ultra64/gbi.h

#pragma once

#include <stdint.h>

namespace N64 {

// Display list command (64-bit, two 32-bit words)
struct Gfx {
    uint32_t w0, w1;
};

// Vertex (16 bytes, matches N64 RSP vertex format)
struct Vtx {
    int16_t x, y, z;      // Model-space position
    uint16_t flag;         // Usually 0
    int16_t s, t;          // Texture coords (S10.5 fixed-point)
    uint8_t r, g, b, a;   // Vertex color (or normal xyz + alpha)
};
static_assert(sizeof(Vtx) == 16, "Vtx must be 16 bytes");

// F3DEX2 opcodes
constexpr uint8_t G_VTX             = 0x01;
constexpr uint8_t G_TRI1            = 0x05;
constexpr uint8_t G_TRI2            = 0x06;
constexpr uint8_t G_GEOMETRYMODE    = 0xD9;  // Combined set/clear
constexpr uint8_t G_MTX             = 0xDA;
constexpr uint8_t G_POPMTX          = 0xD8;
constexpr uint8_t G_TEXTURE         = 0xD7;
constexpr uint8_t G_DL              = 0xDE;
constexpr uint8_t G_ENDDL           = 0xDF;
constexpr uint8_t G_SETPRIMCOLOR    = 0xFA;
constexpr uint8_t G_SETENVCOLOR     = 0xFB;
constexpr uint8_t G_SETCOMBINE      = 0xFC;
constexpr uint8_t G_SETTIMG         = 0xFD;
constexpr uint8_t G_LOADBLOCK       = 0xF3;
constexpr uint8_t G_SETTILE         = 0xF5;
constexpr uint8_t G_SETTILESIZE     = 0xF2;
constexpr uint8_t G_RDPPIPESYNC     = 0xE7;
constexpr uint8_t G_RDPLOADSYNC     = 0xE6;
constexpr uint8_t G_RDPTILESYNC     = 0xE5;
constexpr uint8_t G_SETSCISSOR      = 0xED;
constexpr uint8_t G_SETOTHERMODE_L  = 0xE2;
constexpr uint8_t G_SETOTHERMODE_H  = 0xE3;
constexpr uint8_t G_FILLRECT        = 0xF6;
constexpr uint8_t G_SETFILLCOLOR    = 0xF7;
constexpr uint8_t G_SETFOGCOLOR     = 0xF8;
constexpr uint8_t G_SETBLENDCOLOR   = 0xF9;

// Decode helpers
inline uint8_t opcode(const Gfx& g) { return g.w0 >> 24; }

// G_VTX: w0 = [01] [numv:8b @12] [(v0+numv):7b @1]  w1 = pointer
inline unsigned vtxCount(const Gfx& g) { return (g.w0 >> 12) & 0xFF; }
inline unsigned vtxV0(const Gfx& g) {
    return ((g.w0 >> 1) & 0x7F) - vtxCount(g);
}
inline const Vtx* vtxData(const Gfx& g) {
    return reinterpret_cast<const Vtx*>(g.w1);
}

// G_TRI1: w0 = [05] [v0*2] [v1*2] [v2*2]  w1 = 0
// G_TRI2: w0 = [06] [v0*2] [v1*2] [v2*2]  w1 = [00] [v3*2] [v4*2] [v5*2]
inline unsigned triV0(const Gfx& g) { return ((g.w0 >> 16) & 0xFF) / 2; }
inline unsigned triV1(const Gfx& g) { return ((g.w0 >> 8) & 0xFF) / 2; }
inline unsigned triV2(const Gfx& g) { return (g.w0 & 0xFF) / 2; }
inline unsigned tri2V3(const Gfx& g) { return ((g.w1 >> 16) & 0xFF) / 2; }
inline unsigned tri2V4(const Gfx& g) { return ((g.w1 >> 8) & 0xFF) / 2; }
inline unsigned tri2V5(const Gfx& g) { return (g.w1 & 0xFF) / 2; }

// G_SETPRIMCOLOR: w0 = [FA][00][minlod][lodfrac]  w1 = [R][G][B][A]
inline uint8_t primR(const Gfx& g) { return g.w1 >> 24; }
inline uint8_t primG(const Gfx& g) { return g.w1 >> 16; }
inline uint8_t primB(const Gfx& g) { return g.w1 >> 8; }
inline uint8_t primA(const Gfx& g) { return g.w1; }

// G_DL: w0 = [DE][00][push_flag][00]  w1 = pointer
// push_flag: 0 = call (push return addr), 1 = branch (no return)
inline bool dlIsBranch(const Gfx& g) { return (g.w0 >> 16) & 0xFF; }
inline const Gfx* dlTarget(const Gfx& g) {
    return reinterpret_cast<const Gfx*>(g.w1);
}

}  // namespace N64
