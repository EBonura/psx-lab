// OoT PS1 — Scene declarations shared across translation units.

#pragma once

#include "psyqo/application.hh"
#include "psyqo/buffer.hh"
#include "psyqo/cdrom-device.hh"
#include "psyqo/fixed-point.hh"
#include "psyqo/font.hh"
#include "psyqo/fragments.hh"
#include "psyqo/gpu.hh"
#include "psyqo/gte-kernels.hh"
#include "psyqo/gte-registers.hh"
#include "psyqo/iso9660-parser.hh"
#include "psyqo/ordering-table.hh"
#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/misc.hh"
#include "psyqo/primitives/quads.hh"
#include "psyqo/primitives/triangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/simplepad.hh"
#include "psyqo/soft-math.hh"
#include "psyqo/trigonometry.hh"

#include "psyqo-paths/cdrom-loader.hh"

#include "prm.h"
#include "skm.h"
#include "vram_alloc.h"

// ── Tuning constants ─────────────────────────────────────────────────────

constexpr int OT_SIZE      = 1024;
constexpr int MAX_TRIS     = 1200;
constexpr int MAX_VTX      = 256;
constexpr int SCREEN_W     = 320;
constexpr int SCREEN_H     = 240;
constexpr int H_PROJ       = 180;
constexpr int NUM_ROOMS    = 10;
constexpr int SKEL_SCALE   = 100;  // OoT Actor_SetScale(0.01) — applied at runtime

// ── Shared types ─────────────────────────────────────────────────────────

struct ScreenVtx {
    int16_t sx, sy;
    uint16_t sz;
    uint16_t pad;
};

struct BoneState {
    psyqo::Matrix33 rot;
    int32_t tx, ty, tz;
};

// ── Globals (defined in main.cpp) ────────────────────────────────────────

extern VramAlloc::Allocator g_vramAlloc;
extern ScreenVtx g_scratch[MAX_VTX];

// ── Room table (defined in room.cpp) ─────────────────────────────────────

struct SpawnPoint {
    int16_t x, y, z;
    int16_t rot_y;  // OoT s16 binary angle
};

extern const char* const ROOM_FILES[NUM_ROOMS];
extern const char* const ROOM_NAMES[NUM_ROOMS];
extern const SpawnPoint ROOM_SPAWNS[NUM_ROOMS];

// ── Application ──────────────────────────────────────────────────────────

class OotApp final : public psyqo::Application {
    void prepare() override;
    void createScene() override;
  public:
    psyqo::Trig<> m_trig;
    psyqo::SimplePad m_pad;
    psyqo::Font<> m_font;
    psyqo::CDRomDevice m_cdrom;
    psyqo::ISO9660Parser m_isoParser{&m_cdrom};
    psyqo::paths::CDRomLoader m_loader;
};

extern OotApp app;

// ── Room renderer scene ──────────────────────────────────────────────────

class RoomScene final : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;

    // Camera (orbit around skeleton)
    psyqo::Angle m_camRotY;      // orbit yaw
    psyqo::Angle m_camRotX;      // orbit pitch
    int32_t m_camDist = 400;     // orbit radius
    int32_t m_camX = 0, m_camY = 0, m_camZ = 0;  // computed each frame

    // Double-buffered rendering resources
    psyqo::OrderingTable<OT_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    eastl::array<psyqo::Fragments::SimpleFragment<psyqo::Prim::GouraudTexturedTriangle>, MAX_TRIS> m_tris[2];

    int m_triCount = 0;
    int m_parity = 0;

    // Room streaming
    int m_roomIdx = 0;
    psyqo::Buffer<uint8_t> m_roomBuf;
    const uint8_t* m_prm = nullptr;
    bool m_loading = false;
    bool m_needUpload = false;
    bool m_selectHeld = false;

    // Debug texture grid
    bool m_debugView = false;
    bool m_startHeld = false;
    eastl::array<psyqo::Fragments::SimpleFragment<psyqo::Prim::TexturedQuad>, VramAlloc::MAX_TEXTURES> m_debugQuads[2];

    // Skeleton state
    psyqo::Buffer<uint8_t> m_skelBuf;
    const uint8_t* m_skm = nullptr;
    SKM::LimbMeshCache m_limbCache;
    bool m_skelLoaded = false;
    bool m_skelVisible = true;
    int m_skelTexBase = 0;
    int32_t m_skelX = 0, m_skelY = 0, m_skelZ = 0;  // world position

    // Animation
    int m_animIdx = 0;
    int m_animFrame = 0;
    bool m_animPaused = true;

    // Input debounce
    bool m_triangleHeld = false;
    bool m_crossHeld = false;
    bool m_circleHeld = false;

    // Bone hierarchy
    BoneState m_bones[21];

    // Room methods (room.cpp)
    void loadRoom(int idx);
    void uploadTextures();
    void renderChunk(const PRM::ChunkDesc& chunk);
    void transformVertices(const PRM::Pos* pos, int count);
    void renderDebugGrid();

    // Skeleton methods (skeleton.cpp)
    void loadSkeleton();
    void computeBones(const int16_t* frame);
    void computeBoneRecurse(int limbIdx, const BoneState& parent, const int16_t* frame);
    void renderSkeleton(const psyqo::Matrix33& renderRot, int32_t camTX, int32_t camTY, int32_t camTZ);
    void drawLimb(int limbIdx, const psyqo::Matrix33& renderRot, int32_t camTX, int32_t camTY, int32_t camTZ);
};

extern RoomScene roomScene;
