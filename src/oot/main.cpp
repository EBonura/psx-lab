// Zelda: Ocarina of Time — PS1 Port
// Native PRM room renderer with batch GTE vertex transform.
//
// Render pipeline per chunk:
//   1. Frustum cull bounding sphere (TODO)
//   2. Batch-transform ALL vertices via GTE RTPT (3 at a time)
//      → store screen XY + Z in scratch arrays
//   3. For each triangle:
//      a. Software NCLIP (cross product on pre-transformed screen coords)
//      b. Average Z → ordering table index
//      c. Per-vertex colors → gouraud triangle primitive
//      d. Insert into ordering table
//
// This avoids re-transforming shared vertices (each vert shared ~4-6 tris).

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
#include "mesh_data.h"

using namespace psyqo::trig_literals;
using namespace psyqo::fixed_point_literals;

namespace {

// ── Tuning constants ─────────────────────────────────────────────────────

constexpr int OT_SIZE    = 1024;   // Depth buckets (more = better Z precision)
constexpr int MAX_TRIS   = 600;    // Max visible tris per frame after culling
constexpr int MAX_VTX    = 256;    // Max verts per chunk (matches PRM limit)
constexpr int SCREEN_W   = 320;
constexpr int SCREEN_H   = 240;
constexpr int H_PROJ     = 180;    // Projection plane distance

// ── Scratch buffers for batch vertex transform ───────────────────────────

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

    // Camera
    psyqo::Angle m_camRotY = 0.0_pi;
    psyqo::Angle m_camRotX = 0.1_pi;
    int32_t m_camDist = 2500;
    int32_t m_camY = -200;

    // Double-buffered rendering resources
    psyqo::OrderingTable<OT_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    eastl::array<psyqo::Fragments::SimpleFragment<psyqo::Prim::GouraudTriangle>, MAX_TRIS> m_tris[2];

    int m_triCount;
    int m_parity;

    // Mesh pointer
    const uint8_t* m_prm = ROOM_DATA;

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
}

// ── Frame rendering ──────────────────────────────────────────────────────

void RoomScene::frame() {
    // ── Input ────────────────────────────────────────────────────────────
    using Pad = psyqo::SimplePad;

    // D-pad Left/Right: rotate Y
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Left))  m_camRotY -= 0.02_pi;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Right)) m_camRotY += 0.02_pi;
    if (m_camRotY < 0.0_pi)  m_camRotY += 2.0_pi;
    if (m_camRotY >= 2.0_pi) m_camRotY -= 2.0_pi;

    // D-pad Up/Down: tilt X
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Up))   m_camRotX -= 0.01_pi;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::Down)) m_camRotX += 0.01_pi;

    // L1/R1: zoom
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::L1)) m_camDist -= 50;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::R1)) m_camDist += 50;
    if (m_camDist < 200)  m_camDist = 200;
    if (m_camDist > 8000) m_camDist = 8000;

    // L2/R2: camera height
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::L2)) m_camY -= 30;
    if (app.m_pad.isButtonPressed(Pad::Pad1, Pad::Button::R2)) m_camY += 30;

    // ── View matrix ──────────────────────────────────────────────────────
    auto rotY = psyqo::SoftMath::generateRotationMatrix33(
        m_camRotY, psyqo::SoftMath::Axis::Y, app.m_trig);
    auto rotX = psyqo::SoftMath::generateRotationMatrix33(
        m_camRotX, psyqo::SoftMath::Axis::X, app.m_trig);
    psyqo::Matrix33 viewRot;
    psyqo::SoftMath::multiplyMatrix33(rotX, rotY, &viewRot);
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(viewRot);

    // Translation: offset camera from room center
    psyqo::GTE::clear<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>();
    psyqo::GTE::write<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(static_cast<int32_t>(m_camY)));
    psyqo::GTE::write<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(m_camDist));

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
    // PRM::Pos is {int16_t x,y,z,pad} = 8 bytes.
    // In LE memory: word[0] = (y<<16)|x = GTE VXY format, word[1] = (pad<<16)|z = GTE VZ format.
    // Write directly to GTE registers — zero conversion overhead.

    int i = 0;

    // Batch of 3: RTPT (≈23 cycles for 3 vertices)
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

    // Remainder: RTPS (single vertex, ≈14 cycles)
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
    const auto* tri = PRM::triangles(m_prm, chunk);

    // Step 1: Batch-transform all vertices
    transformVertices(pos, chunk.num_verts);

    // Step 2: Emit triangles using pre-transformed screen coords
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
        if (cross >= 0) continue;  // Backface cull (interior faces visible)

        // Screen bounds reject (prevents giant triangles from vertices behind camera)
        if (sv0.sx < -512 || sv0.sx > 512 || sv0.sy < -512 || sv0.sy > 512) continue;
        if (sv1.sx < -512 || sv1.sx > 512 || sv1.sy < -512 || sv1.sy > 512) continue;
        if (sv2.sx < -512 || sv2.sx > 512 || sv2.sy < -512 || sv2.sy > 512) continue;

        // Average Z → OT index (matches GTE avsz3: OTZ = sumZ * ZSF3 >> 12)
        uint32_t sumZ = uint32_t(sv0.sz) + sv1.sz + sv2.sz;
        int32_t otIdx = static_cast<int32_t>((sumZ * (OT_SIZE / 3)) >> 12);
        if (otIdx <= 0 || otIdx >= OT_SIZE) continue;

        // Vertex colors (boost zero-color verts — they use normals on N64, no lighting on PS1)
        auto fixColor = [](const PRM::Color& c) -> psyqo::Color {
            uint8_t r = c.r, g = c.g, b = c.b;
            if (r == 0 && g == 0 && b == 0) { r = 80; g = 70; b = 60; }  // ambient fallback
            return {{.r = r, .g = g, .b = b}};
        };

        // Set up gouraud-shaded triangle
        auto& frag = m_tris[m_parity][m_triCount];
        auto& p = frag.primitive;

        p.pointA.x = sv0.sx; p.pointA.y = sv0.sy;
        p.pointB.x = sv1.sx; p.pointB.y = sv1.sy;
        p.pointC.x = sv2.sx; p.pointC.y = sv2.sy;

        p.setColorA(fixColor(col[idx.v0]));
        p.setColorB(fixColor(col[idx.v1]));
        p.setColorC(fixColor(col[idx.v2]));
        p.setOpaque();

        m_ots[m_parity].insert(frag, otIdx);
        m_triCount++;
    }
}

}  // namespace

int main() { return app.run(); }
