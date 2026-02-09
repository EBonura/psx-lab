#include <stdarg.h>
#include <stddef.h>

#include "psyqo/application.hh"
#include "psyqo/fragments.hh"
#include "psyqo/gpu.hh"
#include "psyqo/ordering-table.hh"
#include "psyqo/scene.hh"

#include "psyqo/advancedpad.hh"

#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/control.hh"
#include "psyqo/primitives/lines.hh"
#include "psyqo/primitives/misc.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/primitives/sprites.hh"

extern "C" {
#include "celeste.h"
#include "tilemap.h"
}

#include "font_data.h"
#include "gfx_data.h"

namespace {

// --- Constants ---
constexpr int SCALE = 2;
constexpr int OFS_X = 0;
// OFS_Y is dynamic: 0 = show top (clip bottom), -16 = show bottom (clip top)

constexpr int OT_SIZE = 512;
constexpr int MAX_SPRITES = 600;
constexpr int MAX_RECTS = 160;
constexpr int MAX_LINES = 32;
constexpr int MAX_TPAGES = 8;

// VRAM layout (in 16-bit pixel coordinates)
constexpr int16_t GFX_VRAM_X = 640, GFX_VRAM_Y = 0;
constexpr int16_t GFX_VRAM_W = 64, GFX_VRAM_H = 256;  // 256x256 4bpp = 64 16-bit words wide

constexpr int16_t FONT_VRAM_X = 704, FONT_VRAM_Y = 0;
constexpr int16_t FONT_VRAM_W = 64, FONT_VRAM_H = 170;  // 256x170 4bpp = 64 16-bit words wide

constexpr int16_t SPRITE_CLUT_X = 0, SPRITE_CLUT_Y = 496;
constexpr int16_t TEXT_CLUT_X0 = 0, TEXT_CLUT_Y = 497;

// --- Render state (accessible from C callback) ---
struct RenderState {
    psyqo::GPU* gpu;
    psyqo::OrderingTable<OT_SIZE>* ot;
    psyqo::Fragments::SimpleFragment<psyqo::Prim::Sprite16x16>* sprites;
    psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle>* rects;
    psyqo::Fragments::SimpleFragment<psyqo::Prim::Line>* lines;
    psyqo::Fragments::SimpleFragment<psyqo::Prim::TPage>* tpages;
    int spriteIdx, rectIdx, lineIdx, tpageIdx;
    int zCounter;
    int camX, camY;
    uint8_t pal[16];
    uint16_t btnState;
    bool tpageIsFont;
    int ofsY;          // dynamic vertical offset (PS1 pixels)
    int sprYSum;       // accumulated screen-Y of SPR draws (PICO-8 coords)
    int sprYCount;     // number of SPR draws this frame
};

RenderState g_rs;

// --- Helpers ---

psyqo::Color getDrawColor(int col) {
    int m = g_rs.pal[col & 15];
    psyqo::Color c;
    c.r = pico8_rgb[m][0];
    c.g = pico8_rgb[m][1];
    c.b = pico8_rgb[m][2];
    return c;
}

psyqo::PrimPieces::ClutIndex spriteClut() {
    return psyqo::PrimPieces::ClutIndex(
        static_cast<uint16_t>(SPRITE_CLUT_X >> 4),
        static_cast<uint16_t>(SPRITE_CLUT_Y));
}

psyqo::PrimPieces::ClutIndex textClut(int color) {
    int m = g_rs.pal[color & 15];
    return psyqo::PrimPieces::ClutIndex(
        static_cast<uint16_t>((TEXT_CLUT_X0 + m * 16) >> 4),
        static_cast<uint16_t>(TEXT_CLUT_Y));
}

void setTPageAttr(psyqo::PrimPieces::TPageAttr& attr, int pageX) {
    attr.setPageX(pageX);
    attr.setPageY(0);
    attr.set(psyqo::Prim::TPageAttr::Tex4Bits);
}

void ensureGfxTPage() {
    if (g_rs.tpageIsFont && g_rs.tpageIdx < MAX_TPAGES) {
        auto& f = g_rs.tpages[g_rs.tpageIdx++];
        setTPageAttr(f.primitive.attr, 10);
        g_rs.ot->insert(f, g_rs.zCounter--);
        g_rs.tpageIsFont = false;
    }
}

void ensureFontTPage() {
    if (!g_rs.tpageIsFont && g_rs.tpageIdx < MAX_TPAGES) {
        auto& f = g_rs.tpages[g_rs.tpageIdx++];
        setTPageAttr(f.primitive.attr, 11);
        g_rs.ot->insert(f, g_rs.zCounter--);
        g_rs.tpageIsFont = true;
    }
}

void addRect(int x, int y, int w, int h, psyqo::Color color) {
    if (g_rs.rectIdx >= MAX_RECTS) return;
    auto& f = g_rs.rects[g_rs.rectIdx++];
    f.primitive.setColor(color);
    f.primitive.position.x = x;
    f.primitive.position.y = y;
    f.primitive.size.w = w;
    f.primitive.size.h = h;
    g_rs.ot->insert(f, g_rs.zCounter);
}

psyqo::Rect makeRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    psyqo::Rect r;
    r.pos.x = x;
    r.pos.y = y;
    r.size.w = w;
    r.size.h = h;
    return r;
}

// --- PICO-8 callback ---

int pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...) {
    va_list args;
    int ret = 0;
    va_start(args, call);

#define INT_ARG() va_arg(args, int)
#define BOOL_ARG() (Celeste_P8_bool_t) va_arg(args, int)

    switch (call) {
        case CELESTE_P8_MUSIC: {
            INT_ARG();
            INT_ARG();
            INT_ARG();
        } break;

        case CELESTE_P8_SFX: {
            INT_ARG();
        } break;

        case CELESTE_P8_SPR: {
            int sprite = INT_ARG();
            int x = INT_ARG();
            int y = INT_ARG();
            int cols = INT_ARG();
            int rows = INT_ARG();
            int flipx = BOOL_ARG();
            int flipy = BOOL_ARG();
            (void)cols;
            (void)rows;
            (void)flipx;
            (void)flipy;

            if (sprite >= 0 && g_rs.spriteIdx < MAX_SPRITES) {
                ensureGfxTPage();
                auto& f = g_rs.sprites[g_rs.spriteIdx++];
                int screenY = y - g_rs.camY;
                f.primitive.position.x = (x - g_rs.camX) * SCALE + OFS_X;
                f.primitive.position.y = screenY * SCALE + g_rs.ofsY;
                f.primitive.texInfo.u = (sprite % 16) * 16;
                f.primitive.texInfo.v = (sprite / 16) * 16;
                f.primitive.texInfo.clut = spriteClut();
                g_rs.ot->insert(f, g_rs.zCounter--);
                g_rs.sprYSum += screenY;
                g_rs.sprYCount++;
            }
        } break;

        case CELESTE_P8_BTN: {
            int b = INT_ARG();
            ret = (g_rs.btnState & (1 << b)) ? 1 : 0;
        } break;

        case CELESTE_P8_PAL: {
            int a = INT_ARG();
            int b = INT_ARG();
            if (a >= 0 && a < 16 && b >= 0 && b < 16) {
                g_rs.pal[a] = b;
            }
        } break;

        case CELESTE_P8_PAL_RESET: {
            for (int i = 0; i < 16; i++) g_rs.pal[i] = i;
        } break;

        case CELESTE_P8_CIRCFILL: {
            int cx = INT_ARG() - g_rs.camX;
            int cy = INT_ARG() - g_rs.camY;
            int r = INT_ARG();
            int col = INT_ARG();

            auto color = getDrawColor(col);
            int px = cx * SCALE + OFS_X;
            int py = cy * SCALE + g_rs.ofsY;

            if (r <= 1) {
                addRect(px - SCALE, py, SCALE * 3, SCALE, color);
                addRect(px, py - SCALE, SCALE, SCALE * 3, color);
            } else if (r <= 2) {
                addRect(px - SCALE * 2, py - SCALE, SCALE * 5, SCALE * 3, color);
                addRect(px - SCALE, py - SCALE * 2, SCALE * 3, SCALE * 5, color);
            } else if (r <= 3) {
                addRect(px - SCALE * 3, py - SCALE, SCALE * 7, SCALE * 3, color);
                addRect(px - SCALE, py - SCALE * 3, SCALE * 3, SCALE * 7, color);
                addRect(px - SCALE * 2, py - SCALE * 2, SCALE * 5, SCALE * 5, color);
            } else {
                // Midpoint circle
                int f = 1 - r, ddFx = 1, ddFy = -2 * r;
                int ix = 0, iy = r;
                int rs = r * SCALE;
                addRect(px - rs, py, rs * 2 + SCALE, SCALE, color);
                addRect(px, py - rs, SCALE, rs * 2 + SCALE, color);
                while (ix < iy) {
                    if (f >= 0) {
                        iy--;
                        ddFy += 2;
                        f += ddFy;
                    }
                    ix++;
                    ddFx += 2;
                    f += ddFx;
                    int sx = ix * SCALE, sy = iy * SCALE;
                    addRect(px - sx, py + sy, sx * 2 + SCALE, SCALE, color);
                    addRect(px - sx, py - sy, sx * 2 + SCALE, SCALE, color);
                    addRect(px - sy, py + sx, sy * 2 + SCALE, SCALE, color);
                    addRect(px - sy, py - sx, sy * 2 + SCALE, SCALE, color);
                }
            }
            g_rs.zCounter--;
        } break;

        case CELESTE_P8_PRINT: {
            const char* str = va_arg(args, const char*);
            int x = INT_ARG() - g_rs.camX;
            int y = INT_ARG() - g_rs.camY;
            int col = INT_ARG() % 16;

            ensureFontTPage();
            auto clut = textClut(col);

            for (const char* p = str; *p; p++) {
                char c = *p & 0x7F;
                if (g_rs.spriteIdx >= MAX_SPRITES) break;

                auto& f = g_rs.sprites[g_rs.spriteIdx++];
                f.primitive.position.x = x * SCALE + OFS_X;
                f.primitive.position.y = y * SCALE + g_rs.ofsY;
                f.primitive.texInfo.u = (c % 16) * 16;
                f.primitive.texInfo.v = (c / 16) * 16;
                f.primitive.texInfo.clut = clut;
                g_rs.ot->insert(f, g_rs.zCounter);

                x += 4;
            }
            g_rs.zCounter--;
        } break;

        case CELESTE_P8_RECTFILL: {
            int x0 = INT_ARG() - g_rs.camX;
            int y0 = INT_ARG() - g_rs.camY;
            int x1 = INT_ARG() - g_rs.camX;
            int y1 = INT_ARG() - g_rs.camY;
            int col = INT_ARG();

            int px0 = x0 * SCALE + OFS_X;
            int py0 = y0 * SCALE + g_rs.ofsY;
            int px1 = (x1 + 1) * SCALE + OFS_X;
            int py1 = (y1 + 1) * SCALE + g_rs.ofsY;
            addRect(px0, py0, px1 - px0, py1 - py0, getDrawColor(col));
            g_rs.zCounter--;
        } break;

        case CELESTE_P8_LINE: {
            int x0 = INT_ARG() - g_rs.camX;
            int y0 = INT_ARG() - g_rs.camY;
            int x1 = INT_ARG() - g_rs.camX;
            int y1 = INT_ARG() - g_rs.camY;
            int col = INT_ARG();

            if (g_rs.lineIdx < MAX_LINES) {
                auto& f = g_rs.lines[g_rs.lineIdx++];
                f.primitive.setColor(getDrawColor(col));
                f.primitive.pointA.x = x0 * SCALE + OFS_X;
                f.primitive.pointA.y = y0 * SCALE + g_rs.ofsY;
                f.primitive.pointB.x = x1 * SCALE + OFS_X;
                f.primitive.pointB.y = y1 * SCALE + g_rs.ofsY;
                g_rs.ot->insert(f, g_rs.zCounter--);
            }
        } break;

        case CELESTE_P8_MGET: {
            int tx = INT_ARG();
            int ty = INT_ARG();
            ret = tilemap_data[tx + ty * 128];
        } break;

        case CELESTE_P8_CAMERA: {
            g_rs.camX = INT_ARG();
            g_rs.camY = INT_ARG();
        } break;

        case CELESTE_P8_FGET: {
            int tile = INT_ARG();
            int flag = INT_ARG();
            ret = (tile < (int)(sizeof(tile_flags) / sizeof(*tile_flags))) &&
                  (tile_flags[tile] & (1 << flag));
        } break;

        case CELESTE_P8_MAP: {
            int mx = INT_ARG(), my = INT_ARG();
            int tx = INT_ARG(), ty = INT_ARG();
            int mw = INT_ARG(), mh = INT_ARG();
            int mask = INT_ARG();

            ensureGfxTPage();
            auto clut = spriteClut();

            for (int yi = 0; yi < mh; yi++) {
                for (int xi = 0; xi < mw; xi++) {
                    int tile = tilemap_data[xi + mx + (yi + my) * 128];
                    if (tile == 0 && mask != 0) continue;
                    bool match = (mask == 0) ||
                                 (mask == 4 && tile_flags[tile] == 4) ||
                                 ((tile < (int)(sizeof(tile_flags) / sizeof(*tile_flags))) &&
                                  (tile_flags[tile] & (1 << (mask != 4 ? mask - 1 : mask))));
                    if (!match) continue;
                    if (g_rs.spriteIdx >= MAX_SPRITES) goto map_done;

                    {
                        auto& f = g_rs.sprites[g_rs.spriteIdx++];
                        f.primitive.position.x =
                            (tx + xi * 8 - g_rs.camX) * SCALE + OFS_X;
                        f.primitive.position.y =
                            (ty + yi * 8 - g_rs.camY) * SCALE + g_rs.ofsY;
                        f.primitive.texInfo.u = (tile % 16) * 16;
                        f.primitive.texInfo.v = (tile / 16) * 16;
                        f.primitive.texInfo.clut = clut;
                        g_rs.ot->insert(f, g_rs.zCounter);
                    }
                }
            }
        map_done:
            g_rs.zCounter--;
        } break;
    }

#undef INT_ARG
#undef BOOL_ARG

    va_end(args);
    return ret;
}

// --- Application ---

class CelesteApp : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::AdvancedPad m_pad;
};

class CelesteScene : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;

    psyqo::OrderingTable<OT_SIZE> m_ot[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::Sprite16x16> m_sprites[2][MAX_SPRITES];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle> m_rects[2][MAX_RECTS];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::Line> m_lines[2][MAX_LINES];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::TPage> m_tpages[2][MAX_TPAGES];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    int m_viewOfsY = -16;   // current smooth offset (PS1 pixels, -16 to 0)
    int m_lastAvgY = 120;   // last frame's average sprite Y (PICO-8 coords)
};

CelesteApp app;
CelesteScene scene;

void CelesteApp::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W256)
        .set(psyqo::GPU::VideoMode::NTSC)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
}

void CelesteApp::createScene() {
    m_pad.initialize();
    pushScene(&scene);
}

void CelesteScene::start(StartReason reason) {
    if (reason != StartReason::Create) return;

    auto& gpu = app.gpu();

    // Upload GFX spritesheet
    gpu.uploadToVRAM(gfx_data, makeRect(GFX_VRAM_X, GFX_VRAM_Y, GFX_VRAM_W, GFX_VRAM_H));

    // Upload font
    gpu.uploadToVRAM(font_data, makeRect(FONT_VRAM_X, FONT_VRAM_Y, FONT_VRAM_W, FONT_VRAM_H));

    // Upload sprite CLUT
    gpu.uploadToVRAM(pico8_clut, makeRect(SPRITE_CLUT_X, SPRITE_CLUT_Y, 16, 1));

    // Upload text CLUTs (one per color)
    for (int i = 0; i < 16; i++) {
        gpu.uploadToVRAM(
            text_cluts[i],
            makeRect(static_cast<int16_t>(TEXT_CLUT_X0 + i * 16), TEXT_CLUT_Y, 16, 1));
    }

    // Initialize game
    Celeste_P8_set_call_func(pico8emu);
    Celeste_P8_set_rndseed(42);
    Celeste_P8_init();
}

void CelesteScene::frame() {
    int buf = app.gpu().getParity();

    // Set up render state for callback
    g_rs.gpu = &app.gpu();
    g_rs.ot = &m_ot[buf];
    g_rs.sprites = m_sprites[buf];
    g_rs.rects = m_rects[buf];
    g_rs.lines = m_lines[buf];
    g_rs.tpages = m_tpages[buf];
    g_rs.spriteIdx = 0;
    g_rs.rectIdx = 0;
    g_rs.lineIdx = 0;
    g_rs.tpageIdx = 0;
    g_rs.zCounter = OT_SIZE - 2;  // reserve OT_SIZE-1 for initial TPage
    g_rs.camX = 0;
    g_rs.camY = 0;
    g_rs.tpageIsFont = true;  // forces first ensureGfxTPage to emit TPage cmd
    for (int i = 0; i < 16; i++) g_rs.pal[i] = i;
    g_rs.sprYSum = 0;
    g_rs.sprYCount = 0;

    // Compute smooth vertical offset from previous frame's sprite positions
    // PICO-8 Y 0=top, 127=bottom. Map to OFS: top→0, bottom→-16
    int targetOfsY = -16 * m_lastAvgY / 127;
    if (targetOfsY < -16) targetOfsY = -16;
    if (targetOfsY > 0) targetOfsY = 0;
    // Smooth: move up to 1px/frame toward target (16px range / 1px = ~0.27s transition)
    if (m_viewOfsY < targetOfsY) m_viewOfsY++;
    else if (m_viewOfsY > targetOfsY) m_viewOfsY--;
    g_rs.ofsY = m_viewOfsY;

    // Read controller input
    using Pad = psyqo::AdvancedPad;
    g_rs.btnState = 0;
    if (app.m_pad.isButtonPressed(Pad::Pad::Pad1a, Pad::Left))   g_rs.btnState |= (1 << 0);
    if (app.m_pad.isButtonPressed(Pad::Pad::Pad1a, Pad::Right))  g_rs.btnState |= (1 << 1);
    if (app.m_pad.isButtonPressed(Pad::Pad::Pad1a, Pad::Up))     g_rs.btnState |= (1 << 2);
    if (app.m_pad.isButtonPressed(Pad::Pad::Pad1a, Pad::Down))   g_rs.btnState |= (1 << 3);
    if (app.m_pad.isButtonPressed(Pad::Pad::Pad1a, Pad::Cross))  g_rs.btnState |= (1 << 4);  // jump
    if (app.m_pad.isButtonPressed(Pad::Pad::Pad1a, Pad::Circle)) g_rs.btnState |= (1 << 5);  // dash

    // Clear OT
    m_ot[buf].clear();

    // Set initial GFX TPage at highest Z
    {
        auto& f = m_tpages[buf][g_rs.tpageIdx++];
        setTPageAttr(f.primitive.attr, 10);
        m_ot[buf].insert(f, OT_SIZE - 1);
        g_rs.tpageIsFont = false;
    }

    // Game update + draw at 60fps (physics constants scaled for 60fps)
    Celeste_P8_update();
    Celeste_P8_draw();

    // Update vertical camera tracking from this frame's sprite positions
    if (g_rs.sprYCount > 0) {
        m_lastAvgY = g_rs.sprYSum / g_rs.sprYCount;
    }

    // Detect freeze frames: if draw produced no visible primitives (only the
    // initial TPage), skip the screen clear so the back buffer retains the
    // last drawn content — matching PICO-8's retained-mode display behavior.
    bool drawEmpty = (g_rs.spriteIdx == 0 && g_rs.rectIdx == 0 && g_rs.lineIdx == 0);
    if (!drawEmpty) {
        psyqo::Color bg;
        bg.r = 0;
        bg.g = 0;
        bg.b = 0;
        app.gpu().getNextClear(m_clear[buf].primitive, bg);
        app.gpu().chain(m_clear[buf]);
    }
    app.gpu().chain(m_ot[buf]);
}

}  // namespace

int main() { return app.run(); }
