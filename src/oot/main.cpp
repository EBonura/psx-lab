// Zelda: Ocarina of Time — PS1 Port
// Native PRM v2 room renderer with gouraud-textured triangles.
//
// Render pipeline per chunk:
//   1. Batch-transform ALL vertices via GTE RTPT (3 at a time)
//   2. For each triangle:
//      a. Software NCLIP (cross product on pre-transformed screen coords)
//      b. Average Z → ordering table index
//      c. Per-vertex UVs + texture lookup
//      d. Insert into ordering table

#include "psyqo/application.hh"
#include "psyqo/fixed-point.hh"
#include "psyqo/fragments.hh"
#include "psyqo/gpu.hh"
#include "psyqo/gte-kernels.hh"
#include "psyqo/gte-registers.hh"
#include "psyqo/ordering-table.hh"
#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/misc.hh"
#include "psyqo/primitives/triangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/simplepad.hh"
#include "psyqo/soft-math.hh"
#include "psyqo/trigonometry.hh"

#include "prm.h"
#include "vram_alloc.h"
#include "mesh_data.h"

using namespace psyqo::trig_literals;
using namespace psyqo::fixed_point_literals;

namespace {

// ── Tuning constants ─────────────────────────────────────────────────────

constexpr int OT_SIZE      = 1024;
constexpr int MAX_TRIS     = 600;
constexpr int MAX_VTX      = 256;
constexpr int SCREEN_W     = 320;
constexpr int SCREEN_H     = 240;
constexpr int H_PROJ       = 180;

// ── VRAM allocator ───────────────────────────────────────────────────────
static VramAlloc::Allocator s_vramAlloc;

// ── Scratch buffers ──────────────────────────────────────────────────────

struct ScreenVtx {
    int16_t sx, sy;
    uint16_t sz;
    uint16_t pad;
};

static ScreenVtx s_scratch[MAX_VTX];

// ── Application ──────────────────────────────────────────────────────────

class OotApp final : public psyqo::Application {
    void prepare() override;
    void createScene() override;
  public:
    psyqo::Trig<> m_trig;
    psyqo::SimplePad m_pad;
};

// ── Room renderer scene ──────────────────────────────────────────────────

class RoomScene final : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;

    // Camera (freefly)
    psyqo::Angle m_camRotY = 0.0_pi;
    psyqo::Angle m_camRotX = 0.1_pi;
    int32_t m_camX = 0;
    int32_t m_camY = 200;
    int32_t m_camZ = -2500;

    // Double-buffered rendering resources
    psyqo::OrderingTable<OT_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    eastl::array<psyqo::Fragments::SimpleFragment<psyqo::Prim::GouraudTexturedTriangle>, MAX_TRIS> m_tris[2];

    int m_triCount;
    int m_parity;

    // Mesh pointer
    const uint8_t* m_prm = ROOM_DATA;

    void uploadTextures();
    void renderChunk(const PRM::ChunkDesc& chunk);
    void transformVertices(const PRM::Pos* pos, int count);
};

OotApp app;
RoomScene roomScene;

// ── Application setup ────────────────────────────────────────────────────

void OotApp::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
}

void OotApp::createScene() {
    m_pad.initialize();
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

    uploadTextures();
}

// ── Upload PRM textures to VRAM via allocator ────────────────────────────

void RoomScene::uploadTextures() {
    const auto* hdr = PRM::header(m_prm);
    const auto* descs = PRM::texDescs(m_prm);
    const uint8_t* tdata = PRM::texData(m_prm);

    s_vramAlloc.reset();

    for (int i = 0; i < hdr->num_textures; i++) {
        const auto& td = descs[i];
        uint16_t clut_n = PRM::texClutCount(td);

        int slot = s_vramAlloc.alloc(td.width, td.height, td.format, clut_n);
        if (slot < 0) continue;  // VRAM full — skip

        // Upload pixel data
        const uint16_t* pix = reinterpret_cast<const uint16_t*>(
            tdata + td.data_offset);
        gpu().uploadToVRAM(pix, s_vramAlloc.pixelRect(slot));

        // Upload CLUT (immediately after pixel data)
        uint32_t pix_bytes = PRM::texPixelSize(td);
        // Align to 2 bytes (CLUT is uint16_t array)
        pix_bytes = (pix_bytes + 1) & ~1u;
        const uint16_t* clut = reinterpret_cast<const uint16_t*>(
            tdata + td.data_offset + pix_bytes);
        gpu().uploadToVRAM(clut, s_vramAlloc.clutRect(slot));
    }
}

// ── Frame rendering ──────────────────────────────────────────────────────

void RoomScene::frame() {
    // ── Input ────────────────────────────────────────────────────────────
    using Pad = psyqo::SimplePad;
    constexpr int32_t MOVE_SPEED = 40;
    constexpr int32_t VERT_SPEED = 30;

    // Rotation
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Left))  m_camRotY -= 0.02_pi;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Right)) m_camRotY += 0.02_pi;
    if (m_camRotY < 0.0_pi)  m_camRotY += 2.0_pi;
    if (m_camRotY >= 2.0_pi) m_camRotY -= 2.0_pi;

    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Up))   m_camRotX -= 0.01_pi;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Down)) m_camRotX += 0.01_pi;

    // ── View matrix ──────────────────────────────────────────────────────
    auto rotY = psyqo::SoftMath::generateRotationMatrix33(
        m_camRotY, psyqo::SoftMath::Axis::Y, app.m_trig);
    auto rotX = psyqo::SoftMath::generateRotationMatrix33(
        m_camRotX, psyqo::SoftMath::Axis::X, app.m_trig);
    psyqo::Matrix33 viewRot;
    psyqo::SoftMath::multiplyMatrix33(rotX, rotY, &viewRot);

    // Forward/backward along camera look direction (column 2 of R^T = row 2 components)
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::L1)) {
        m_camX += (viewRot.vs[0].z.raw() * MOVE_SPEED) >> 12;
        m_camY += (viewRot.vs[1].z.raw() * MOVE_SPEED) >> 12;
        m_camZ += (viewRot.vs[2].z.raw() * MOVE_SPEED) >> 12;
    }
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::R1)) {
        m_camX -= (viewRot.vs[0].z.raw() * MOVE_SPEED) >> 12;
        m_camY -= (viewRot.vs[1].z.raw() * MOVE_SPEED) >> 12;
        m_camZ -= (viewRot.vs[2].z.raw() * MOVE_SPEED) >> 12;
    }

    // Vertical movement
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::L2)) m_camY += VERT_SPEED;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::R2)) m_camY -= VERT_SPEED;

    // Write rotation to GTE
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(viewRot);

    // Translation = -R * camPos
    int32_t tx = -((viewRot.vs[0].x.raw() * m_camX +
                    viewRot.vs[0].y.raw() * m_camY +
                    viewRot.vs[0].z.raw() * m_camZ) >> 12);
    int32_t ty = -((viewRot.vs[1].x.raw() * m_camX +
                    viewRot.vs[1].y.raw() * m_camY +
                    viewRot.vs[1].z.raw() * m_camZ) >> 12);
    int32_t tz = -((viewRot.vs[2].x.raw() * m_camX +
                    viewRot.vs[2].y.raw() * m_camY +
                    viewRot.vs[2].z.raw() * m_camZ) >> 12);

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

    // Render all chunks
    const auto* hdr = PRM::header(m_prm);
    const auto* cdescs = PRM::chunks(m_prm);
    for (int ci = 0; ci < hdr->num_chunks; ci++) {
        renderChunk(cdescs[ci]);
    }

    // Submit: clear screen + ordered geometry
    auto& clear = m_clear[m_parity];
    psyqo::Color bg{{.r = 0x08, .g = 0x06, .b = 0x12}};
    gpu().getNextClear(clear.primitive, bg);
    gpu().chain(clear);
    gpu().chain(ot);
}

// ── Batch vertex transform ───────────────────────────────────────────────

void RoomScene::transformVertices(const PRM::Pos* pos, int count) {
    int i = 0;

    for (; i + 2 < count; i += 3) {
        const auto* r0 = reinterpret_cast<const uint32_t*>(&pos[i]);
        const auto* r1 = reinterpret_cast<const uint32_t*>(&pos[i + 1]);
        const auto* r2 = reinterpret_cast<const uint32_t*>(&pos[i + 2]);

        psyqo::GTE::write<psyqo::GTE::Register::VXY0, psyqo::GTE::Unsafe>(r0[0]);
        psyqo::GTE::write<psyqo::GTE::Register::VZ0, psyqo::GTE::Unsafe>(r0[1]);
        psyqo::GTE::write<psyqo::GTE::Register::VXY1, psyqo::GTE::Unsafe>(r1[0]);
        psyqo::GTE::write<psyqo::GTE::Register::VZ1, psyqo::GTE::Unsafe>(r1[1]);
        psyqo::GTE::write<psyqo::GTE::Register::VXY2, psyqo::GTE::Unsafe>(r2[0]);
        psyqo::GTE::write<psyqo::GTE::Register::VZ2, psyqo::GTE::Safe>(r2[1]);
        psyqo::GTE::Kernels::rtpt();

        psyqo::GTE::read<psyqo::GTE::Register::SXY0>(
            reinterpret_cast<uint32_t*>(&s_scratch[i].sx));
        psyqo::GTE::read<psyqo::GTE::Register::SXY1>(
            reinterpret_cast<uint32_t*>(&s_scratch[i + 1].sx));
        psyqo::GTE::read<psyqo::GTE::Register::SXY2>(
            reinterpret_cast<uint32_t*>(&s_scratch[i + 2].sx));

        uint32_t sz1, sz2, sz3;
        psyqo::GTE::read<psyqo::GTE::Register::SZ1>(&sz1);
        psyqo::GTE::read<psyqo::GTE::Register::SZ2>(&sz2);
        psyqo::GTE::read<psyqo::GTE::Register::SZ3>(&sz3);
        s_scratch[i].sz     = static_cast<uint16_t>(sz1);
        s_scratch[i + 1].sz = static_cast<uint16_t>(sz2);
        s_scratch[i + 2].sz = static_cast<uint16_t>(sz3);
    }

    for (; i < count; i++) {
        const auto* r = reinterpret_cast<const uint32_t*>(&pos[i]);
        psyqo::GTE::write<psyqo::GTE::Register::VXY0, psyqo::GTE::Unsafe>(r[0]);
        psyqo::GTE::write<psyqo::GTE::Register::VZ0, psyqo::GTE::Safe>(r[1]);
        psyqo::GTE::Kernels::rtps();

        psyqo::GTE::read<psyqo::GTE::Register::SXY2>(
            reinterpret_cast<uint32_t*>(&s_scratch[i].sx));
        uint32_t sz;
        psyqo::GTE::read<psyqo::GTE::Register::SZ3>(&sz);
        s_scratch[i].sz = static_cast<uint16_t>(sz);
    }
}

// ── Render one chunk ─────────────────────────────────────────────────────

void RoomScene::renderChunk(const PRM::ChunkDesc& chunk) {
    if (chunk.num_verts == 0 || chunk.num_tris == 0) return;

    const auto* pos = PRM::positions(m_prm, chunk);
    const auto* col = PRM::colors(m_prm, chunk);
    const auto* uv = PRM::uvs(m_prm, chunk);
    const auto* tri = PRM::triangles(m_prm, chunk);

    // Step 1: Batch-transform all vertices
    transformVertices(pos, chunk.num_verts);

    // Step 2: Emit gouraud-textured triangles
    for (int t = 0; t < chunk.num_tris && m_triCount < MAX_TRIS; t++) {
        const auto& idx = tri[t];
        const auto& sv0 = s_scratch[idx.v0];
        const auto& sv1 = s_scratch[idx.v1];
        const auto& sv2 = s_scratch[idx.v2];

        // Near-plane reject
        if (sv0.sz == 0 || sv1.sz == 0 || sv2.sz == 0) continue;

        // Software NCLIP: 2D cross product of screen-space edges
        int32_t dx0 = sv1.sx - sv0.sx;
        int32_t dy0 = sv1.sy - sv0.sy;
        int32_t dx1 = sv2.sx - sv0.sx;
        int32_t dy1 = sv2.sy - sv0.sy;
        int32_t cross = dx0 * dy1 - dx1 * dy0;
        if (cross >= 0) continue;  // Backface cull

        // Screen bounds reject
        if (sv0.sx < -512 || sv0.sx > 512 || sv0.sy < -512 || sv0.sy > 512) continue;
        if (sv1.sx < -512 || sv1.sx > 512 || sv1.sy < -512 || sv1.sy > 512) continue;
        if (sv2.sx < -512 || sv2.sx > 512 || sv2.sy < -512 || sv2.sy > 512) continue;

        // Average Z → OT index
        uint32_t sumZ = uint32_t(sv0.sz) + sv1.sz + sv2.sz;
        int32_t otIdx = static_cast<int32_t>((sumZ * (OT_SIZE / 3)) >> 12);
        if (otIdx <= 0 || otIdx >= OT_SIZE) continue;

        auto& frag = m_tris[m_parity][m_triCount];
        auto& p = frag.primitive;

        // Positions
        p.pointA.x = sv0.sx; p.pointA.y = sv0.sy;
        p.pointB.x = sv1.sx; p.pointB.y = sv1.sy;
        p.pointC.x = sv2.sx; p.pointC.y = sv2.sy;

        // Neutral modulation (128 = 1.0x on PS1) so texture shows at full brightness
        psyqo::Color neutral{{.r = 128, .g = 128, .b = 128}};
        p.setColorA(neutral);
        p.setColorB(neutral);
        p.setColorC(neutral);

        // Per-triangle texture lookup
        const auto& ti = s_vramAlloc.info(idx.tex_id);
        p.uvA.u = uv[idx.v0].u + ti.u_off;
        p.uvA.v = uv[idx.v0].v + ti.v_off;
        p.uvB.u = uv[idx.v1].u + ti.u_off;
        p.uvB.v = uv[idx.v1].v + ti.v_off;
        p.uvC.u = uv[idx.v2].u + ti.u_off;
        p.uvC.v = uv[idx.v2].v + ti.v_off;

        p.tpage = ti.tpage;
        p.clutIndex = ti.clut;

        m_ots[m_parity].insert(frag, otIdx);
        m_triCount++;
    }
}

}  // namespace

int main() { return app.run(); }
