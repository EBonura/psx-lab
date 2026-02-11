// Zelda: Ocarina of Time — PS1 Port
// CD-ROM streaming room renderer with gouraud-textured triangles.
// Skeletal mesh overlay with hierarchical bone transforms.
//
// Rooms are loaded from disc on demand via CDRomDevice + ISO9660Parser.
// Only one room is resident in RAM at a time. Select button cycles rooms.
// Link's skeleton renders as an overlay at world origin with animation.
//
// Render pipeline per chunk/limb:
//   1. Batch-transform ALL vertices via GTE RTPT (3 at a time)
//   2. For each triangle:
//      a. Software NCLIP (cross product on pre-transformed screen coords)
//      b. Average Z → ordering table index
//      c. Per-vertex UVs + texture lookup
//      d. Insert into ordering table

#include "scene.h"

using namespace psyqo::trig_literals;
using namespace psyqo::fixed_point_literals;

// ── Global instances ─────────────────────────────────────────────────────

OotApp app;
RoomScene roomScene;
VramAlloc::Allocator g_vramAlloc;
ScreenVtx g_scratch[MAX_VTX];

// ── Application setup ────────────────────────────────────────────────────

void OotApp::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
    m_cdrom.prepare();
}

void OotApp::createScene() {
    m_pad.initialize();
    m_font.uploadSystemFont(gpu());
    pushScene(&roomScene);
}

// ── Scene start ──────────────────────────────────────────────────────────

void RoomScene::start(StartReason) {
    psyqo::GTE::write<psyqo::GTE::Register::OFX, psyqo::GTE::Unsafe>(
        psyqo::FixedPoint<16>(SCREEN_W / 2.0).raw());
    psyqo::GTE::write<psyqo::GTE::Register::OFY, psyqo::GTE::Unsafe>(
        psyqo::FixedPoint<16>(SCREEN_H / 2.0).raw());
    psyqo::GTE::write<psyqo::GTE::Register::H, psyqo::GTE::Unsafe>(H_PROJ);
    psyqo::GTE::write<psyqo::GTE::Register::ZSF3, psyqo::GTE::Unsafe>(OT_SIZE / 3);
    psyqo::GTE::write<psyqo::GTE::Register::ZSF4, psyqo::GTE::Unsafe>(OT_SIZE / 4);

    loadSkeleton();
}

// ── Frame rendering ──────────────────────────────────────────────────────

void RoomScene::frame() {
    gpu().waitChainIdle();

    if (m_needUpload) {
        uploadTextures();
        m_needUpload = false;
    }

    // ── Input ────────────────────────────────────────────────────────────
    using Pad = psyqo::SimplePad;
    constexpr int32_t CAM_TARGET_Y = 40;   // look-at height above skeleton root
    constexpr int32_t CAM_DIST_MIN = 20;
    constexpr int32_t CAM_DIST_MAX = 500;
    constexpr int32_t CAM_ZOOM_SPEED = 10;

    // Room cycling: Select button (debounced)
    bool selectNow = app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Select);
    if (selectNow && !m_selectHeld && !m_loading) {
        int next = (m_roomIdx + 1) % NUM_ROOMS;
        loadRoom(next);
    }
    m_selectHeld = selectNow;

    // Debug view toggle: Start button (debounced)
    bool startNow = app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Start);
    if (startNow && !m_startHeld) m_debugView = !m_debugView;
    m_startHeld = startNow;

    if (m_debugView) { renderDebugGrid(); return; }

    // Skeleton toggle: Triangle (debounced)
    bool triNow = app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Triangle);
    if (triNow && !m_triangleHeld) m_skelVisible = !m_skelVisible;
    m_triangleHeld = triNow;

    // Animation controls (Circle = next anim, Cross = pause)
    if (m_skelVisible && m_skelLoaded) {
        bool circNow = app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Circle);
        if (circNow && !m_circleHeld) {
            m_animIdx = (m_animIdx + 1) % SKM::header(m_skm)->num_anims;
            m_animFrame = 0;
        }
        m_circleHeld = circNow;

        bool crossNow = app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Cross);
        if (crossNow && !m_crossHeld) m_animPaused = !m_animPaused;
        m_crossHeld = crossNow;
    }

    // Orbit rotation (D-pad)
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Left))  m_camRotY -= 0.02_pi;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Right)) m_camRotY += 0.02_pi;
    if (m_camRotY < 0.0_pi)  m_camRotY += 2.0_pi;
    if (m_camRotY >= 2.0_pi) m_camRotY -= 2.0_pi;

    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Up))   m_camRotX += 0.01_pi;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Down)) m_camRotX -= 0.01_pi;
    if (m_camRotX < 0.02_pi) m_camRotX = psyqo::Angle(0.02_pi);
    if (m_camRotX > 0.45_pi) m_camRotX = psyqo::Angle(0.45_pi);

    // Orbit distance (L1/R1 zoom)
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::L1))
        m_camDist -= CAM_ZOOM_SPEED;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::R1))
        m_camDist += CAM_ZOOM_SPEED;
    if (m_camDist < CAM_DIST_MIN) m_camDist = CAM_DIST_MIN;
    if (m_camDist > CAM_DIST_MAX) m_camDist = CAM_DIST_MAX;

    // ── View matrix ──────────────────────────────────────────────────────
    auto rotY = psyqo::SoftMath::generateRotationMatrix33(
        m_camRotY, psyqo::SoftMath::Axis::Y, app.m_trig);
    auto rotX = psyqo::SoftMath::generateRotationMatrix33(
        m_camRotX, psyqo::SoftMath::Axis::X, app.m_trig);
    psyqo::Matrix33 viewRot;
    psyqo::SoftMath::multiplyMatrix33(rotY, rotX, &viewRot);

    // Orbit camera: position = target - forward * distance
    // Forward = row 2 of viewRot (camera Z axis in world space)
    int32_t targetX = m_skelX;
    int32_t targetY = m_skelY + CAM_TARGET_Y;
    int32_t targetZ = m_skelZ;
    m_camX = targetX - ((viewRot.vs[2].x.raw() * m_camDist) >> 12);
    m_camY = targetY - ((viewRot.vs[2].y.raw() * m_camDist) >> 12);
    m_camZ = targetZ - ((viewRot.vs[2].z.raw() * m_camDist) >> 12);

    // Negate Y row: OoT is Y-up, PS1 screen Y goes down.
    psyqo::Matrix33 renderRot = viewRot;
    renderRot.vs[1].x = -renderRot.vs[1].x;
    renderRot.vs[1].y = -renderRot.vs[1].y;
    renderRot.vs[1].z = -renderRot.vs[1].z;

    // Translation = -renderRot * camPos
    int32_t tx = -((renderRot.vs[0].x.raw() * m_camX +
                    renderRot.vs[0].y.raw() * m_camY +
                    renderRot.vs[0].z.raw() * m_camZ) >> 12);
    int32_t ty = -((renderRot.vs[1].x.raw() * m_camX +
                    renderRot.vs[1].y.raw() * m_camY +
                    renderRot.vs[1].z.raw() * m_camZ) >> 12);
    int32_t tz = -((renderRot.vs[2].x.raw() * m_camX +
                    renderRot.vs[2].y.raw() * m_camY +
                    renderRot.vs[2].z.raw() * m_camZ) >> 12);

    // Write camera view matrix to GTE (used by room rendering)
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(renderRot);
    psyqo::GTE::write<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(tx));
    psyqo::GTE::write<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(ty));
    psyqo::GTE::write<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(tz));

    // Reset per-frame state
    m_parity = gpu().getParity();
    m_triCount = 0;
    auto& ot = m_ots[m_parity];
    ot.clear();

    // Render room chunks (GTE has camera matrix)
    if (!m_loading && m_prm) {
        const auto* hdr = PRM::header(m_prm);
        const auto* cdescs = PRM::chunks(m_prm);
        for (int ci = 0; ci < hdr->num_chunks; ci++) {
            renderChunk(cdescs[ci]);
        }
    }

    // Render skeleton overlay (reloads GTE per limb)
    if (m_skelVisible && m_skelLoaded) {
        renderSkeleton(renderRot, tx, ty, tz);
    }

    // Submit: clear screen + ordered geometry
    auto& clear = m_clear[m_parity];
    psyqo::Color bg{{.r = 0x08, .g = 0x06, .b = 0x12}};
    gpu().getNextClear(clear.primitive, bg);
    gpu().chain(clear);
    gpu().chain(ot);

    // Debug HUD
    psyqo::Color white{{.r = 255, .g = 255, .b = 255}};
    if (m_loading) {
        app.m_font.printf(gpu(), {{.x = 8, .y = 8}}, white, "Loading %s...", ROOM_NAMES[m_roomIdx]);
    } else if (m_prm) {
        const auto* hdr = PRM::header(m_prm);
        app.m_font.printf(gpu(), {{.x = 8, .y = 8}}, white,
            "[%d/%d] %s  %dv %dt", m_roomIdx + 1, NUM_ROOMS,
            ROOM_NAMES[m_roomIdx], hdr->num_verts, hdr->num_tris);
    } else {
        app.m_font.printf(gpu(), {{.x = 8, .y = 8}}, white, "No room data (buf=%d)", m_roomBuf.size());
    }

    // Skeleton HUD
    if (m_skelVisible && m_skelLoaded) {
        const auto* shdr = SKM::header(m_skm);
        const auto* ad = &SKM::animDescs(m_skm)[m_animIdx];
        psyqo::Color cyan{{.r = 100, .g = 255, .b = 255}};
        app.m_font.printf(gpu(), {{.x = 8, .y = SCREEN_H - 16}}, cyan,
            "SKEL anim:%d/%d f:%d/%d %s",
            m_animIdx + 1, shdr->num_anims,
            m_animFrame + 1, ad->frame_count,
            m_animPaused ? "||" : ">");
    }
}

int main() { return app.run(); }
