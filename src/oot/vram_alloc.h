// VRAM Allocator — manages PS1 VRAM layout for textures and CLUTs.
//
// PS1 VRAM is 1024×512 @ 16bpp. Layout:
//   X=0..319,  Y=0..479:  framebuffers (2× 320×240)
//   X=0..319,  Y=480..495: FastFill danger zone (cleared every frame)
//   X=320..1023, Y=0..495: texture pixel data (strip-packed)
//   X=0..1023, Y=496..511: CLUT data (16 rows)

#pragma once

#include <stdint.h>
#include "psyqo/primitives/common.hh"

namespace VramAlloc {

struct TexInfo {
    psyqo::PrimPieces::TPageAttr tpage;
    psyqo::PrimPieces::ClutIndex clut;
    uint8_t u_off, v_off;
    uint8_t u_mask, v_mask;  // texel_w-1, texel_h-1 (wrap UVs to texture bounds)
};

static constexpr int MAX_TEXTURES = 32;

// Texture region: right of framebuffers, above CLUT rows
static constexpr int16_t TEX_X0   = 320;
static constexpr int16_t TEX_X1   = 1024;
static constexpr int16_t TEX_Y0   = 0;
static constexpr int16_t TEX_Y1   = 496;

// CLUT region: bottom 16 rows, full width
static constexpr int16_t CLUT_X1  = 1024;
static constexpr int16_t CLUT_Y0  = 496;
static constexpr int16_t CLUT_Y1  = 512;

struct Slot {
    TexInfo info;
    int16_t vram_x, vram_y;    // pixel data position in VRAM
    int16_t vram_w, vram_h;    // pixel data size (VRAM pixels)
    int16_t clut_x, clut_y;    // CLUT position in VRAM
    int16_t clut_w;            // CLUT width (VRAM pixels, 16-aligned)
};

class Allocator {
  public:
    void reset() {
        m_numSlots = 0;
        m_texCurX  = TEX_X0;
        m_texCurY  = TEX_Y0;
        m_texRowH  = 0;
        m_clutCurX = 0;
        m_clutCurY = CLUT_Y0;
    }

    // Allocate VRAM for one texture + its CLUT. Returns slot index, or -1.
    int alloc(uint16_t texel_w, uint16_t texel_h,
              uint8_t format,           // 0=4bit, 1=8bit
              uint16_t num_clut_colors) {
        if (m_numSlots >= MAX_TEXTURES) return -1;

        // Compute VRAM pixel width for this texture
        int16_t vw = (format == 0) ? (texel_w / 4) : (texel_w / 2);
        int16_t vh = static_cast<int16_t>(texel_h);

        // TPage-aware strip packing.
        // PS1 textures must not straddle a texture page boundary:
        //   4-bit pages span 64 VRAM pixels (256 texels)
        //   8-bit pages span 128 VRAM pixels (256 texels)
        // TPage base is always at a multiple of 64 VRAM pixels.
        int16_t page_span = (format == 0) ? 64 : 128;

        auto fitsCurrent = [&](int16_t x) {
            return (x % 64) + vw <= page_span && x + vw <= TEX_X1;
        };

        // Row wrap if we can't fit at all on this row
        if (!fitsCurrent(m_texCurX)) {
            // Try next 64-px page boundary on same row
            int16_t next = static_cast<int16_t>(((m_texCurX + 63) / 64) * 64);
            if (fitsCurrent(next)) {
                m_texCurX = next;
            } else {
                // Wrap to next row
                m_texCurY += m_texRowH;
                m_texCurX = TEX_X0;
                m_texRowH = 0;
            }
        }
        if (m_texCurY + vh > TEX_Y1) return -1;  // out of space

        int16_t vx = m_texCurX;
        int16_t vy = m_texCurY;
        m_texCurX += vw;
        if (vh > m_texRowH) m_texRowH = vh;

        // CLUT: 16-pixel aligned width
        int16_t cw = static_cast<int16_t>((num_clut_colors + 15) & ~15);
        if (m_clutCurX + cw > CLUT_X1) {
            m_clutCurY++;
            m_clutCurX = 0;
        }
        if (m_clutCurY >= CLUT_Y1) return -1;  // out of CLUT space

        int16_t cx = m_clutCurX;
        int16_t cy = m_clutCurY;
        m_clutCurX += cw;

        // Compute TPage and offsets
        auto& s = m_slots[m_numSlots];
        s.vram_x = vx;
        s.vram_y = vy;
        s.vram_w = vw;
        s.vram_h = vh;
        s.clut_x = cx;
        s.clut_y = cy;
        s.clut_w = cw;

        s.info.tpage = psyqo::PrimPieces::TPageAttr();
        s.info.tpage.setPageX(vx / 64).setPageY(vy / 256);
        if (format == 0)
            s.info.tpage.set(psyqo::Prim::TPageAttr::Tex4Bits);
        else
            s.info.tpage.set(psyqo::Prim::TPageAttr::Tex8Bits);

        if (format == 0)
            s.info.u_off = static_cast<uint8_t>((vx % 64) * 4);
        else
            s.info.u_off = static_cast<uint8_t>((vx % 64) * 2);
        s.info.v_off = static_cast<uint8_t>(vy % 256);

        s.info.clut = psyqo::PrimPieces::ClutIndex(cx / 16, cy);

        // UV masks for wrapping (textures are power-of-2)
        s.info.u_mask = static_cast<uint8_t>(texel_w - 1);
        s.info.v_mask = static_cast<uint8_t>(texel_h - 1);

        return m_numSlots++;
    }

    const TexInfo& info(int slot) const { return m_slots[slot].info; }

    psyqo::Rect pixelRect(int slot) const {
        const auto& s = m_slots[slot];
        psyqo::Rect r;
        r.pos.x = s.vram_x;
        r.pos.y = s.vram_y;
        r.size.w = s.vram_w;
        r.size.h = s.vram_h;
        return r;
    }

    psyqo::Rect clutRect(int slot) const {
        const auto& s = m_slots[slot];
        psyqo::Rect r;
        r.pos.x = s.clut_x;
        r.pos.y = s.clut_y;
        r.size.w = s.clut_w;
        r.size.h = 1;
        return r;
    }

    int numSlots() const { return m_numSlots; }

  private:
    Slot m_slots[MAX_TEXTURES];
    int  m_numSlots = 0;

    // Texture strip packer cursor
    int16_t m_texCurX = TEX_X0;
    int16_t m_texCurY = TEX_Y0;
    int16_t m_texRowH = 0;

    // CLUT linear packer cursor
    int16_t m_clutCurX = 0;
    int16_t m_clutCurY = CLUT_Y0;
};

}  // namespace VramAlloc
