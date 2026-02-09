/*
PS1 Simple Room Renderer - OpenLara inspired
---------------------------------------------
A simple room with:
- 5x5 tile floor (25 tiles)
- 1 tile high perimeter walls
- First-person camera with analog controls

Controls:
- Left Stick: Look/turn camera
- Right Stick: Move/strafe
- D-Pad: Also works for movement
- L1/R1: Also works for turning
*/

#include "psyqo/application.hh"
#include "psyqo/advancedpad.hh"
#include "psyqo/fixed-point.hh"
#include "psyqo/font.hh"
#include "psyqo/fragments.hh"
#include "psyqo/gpu.hh"
#include "psyqo/gte-kernels.hh"
#include "psyqo/gte-registers.hh"
#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/quads.hh"
#include "psyqo/scene.hh"
#include "psyqo/soft-math.hh"
#include "psyqo/trigonometry.hh"
#include "psyqo/vector.hh"
#include "psyqo/xprintf.h"

using namespace psyqo::trig_literals;
using namespace psyqo::fixed_point_literals;

// Forward declare for Camera to use
class RoomTest;
extern RoomTest roomTest;

// FPS Camera - stores world-space position, computes view transform for GTE
// GTE computes: screen = project(R * V + T)
// For FPS camera: R = rotation matrix, T = -R * camera_world_pos
class Camera {
public:
    // World-space position (4.12 fixed-point, same scale as vertex coords)
    int32_t wx = 0;
    int32_t wy = 0;
    int32_t wz = 0;

    // Rotation (Y-axis only - horizontal look)
    psyqo::Angle rotY;

    // Move in camera's local space: forward along look dir, strafe perpendicular
    void moveLocal(int32_t forward, int32_t strafe, psyqo::Trig<>& trig) {
        auto s = trig.sin(rotY);
        auto c = trig.cos(rotY);
        // Camera forward in world = (sin(rotY), 0, cos(rotY))
        // Camera right in world   = (cos(rotY), 0, -sin(rotY))
        wx += (s.value * forward + c.value * strafe) >> 12;
        wz += (c.value * forward - s.value * strafe) >> 12;
    }

    // Rotate camera (horizontal look)
    void rotate(int32_t deltaY, psyqo::Trig<>& trig) {
        (void)trig;
        rotY.value += deltaY;
    }

    // Apply camera transform to GTE for rendering
    void applyToGTE(psyqo::Trig<>& trig) {
        // Set rotation matrix
        auto viewRot = psyqo::SoftMath::generateRotationMatrix33(rotY, psyqo::SoftMath::Axis::Y, trig);
        psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(viewRot);

        // Compute translation: T = -R * world_pos
        // PSYQo Y-rotation: R = [c 0 -s; 0 1 0; s 0 c]
        // R*P = (c*wx - s*wz, wy, s*wx + c*wz)
        auto s = trig.sin(rotY);
        auto c = trig.cos(rotY);
        int32_t tx = -((c.value * wx - s.value * wz) >> 12);
        int32_t ty = -wy;
        int32_t tz = -((s.value * wx + c.value * wz) >> 12);

        psyqo::GTE::write<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>(tx);
        psyqo::GTE::write<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>(ty);
        psyqo::GTE::write<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>(tz);
    }
};

// Room config
static constexpr int GRID_SIZE = 5;
static constexpr unsigned ORDERING_TABLE_SIZE = 256;
static constexpr unsigned MAX_QUADS = 64;  // 25 floor + 20 walls = 45
static constexpr uint32_t MIN_Z = 4;       // Near-plane: reject vertices at/behind camera

class RoomTest final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::Trig<> m_trig;
    psyqo::Font<4> m_font;  // Need 4 fragments for 4 chainprint calls
    psyqo::AdvancedPad m_input;
};

class RoomScene final : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;

    // Camera
    Camera m_camera;

    uint32_t m_frameCount = 0;
    int m_polyCount = 0;

    // Debug: analog stick raw values
    uint8_t m_debugLX = 0, m_debugLY = 0;
    uint8_t m_debugRX = 0, m_debugRY = 0;

    int m_parity = 0;

    psyqo::OrderingTable<ORDERING_TABLE_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    eastl::array<psyqo::Fragments::SimpleFragment<psyqo::Prim::Quad>, MAX_QUADS> m_quads[2];

    static constexpr psyqo::Color c_bg = {.r = 8, .g = 8, .b = 16};

    // Tile size in fixed-point (0.08 units per tile - slightly bigger than before)
    static constexpr int32_t TILE_FP = 328;      // 0.08 * 4096
    static constexpr int32_t HALF_ROOM = 820;    // 0.2 * 4096 (2.5 tiles from center)
    static constexpr int32_t FLOOR_Y = 164;      // 0.04 * 4096 (floor below camera)
    static constexpr int32_t WALL_H = 328;       // 0.08 * 4096 (wall height)

    void renderFloor(int& qi, psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot);
    void renderWalls(int& qi, psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot);
    bool renderQuad(int32_t x0, int32_t y0, int32_t z0,
                    int32_t x1, int32_t y1, int32_t z1,
                    int32_t x2, int32_t y2, int32_t z2,
                    int32_t x3, int32_t y3, int32_t z3,
                    psyqo::Color color, int& qi,
                    psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot);
};

RoomTest roomTest;
RoomScene roomScene;

void RoomTest::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
    m_font.uploadSystemFont(gpu());
}

void RoomTest::createScene() {
    m_input.initialize();
    pushScene(&roomScene);
}

void RoomScene::start(StartReason reason) {
    psyqo::GTE::clear<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>();
    psyqo::GTE::clear<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>();
    psyqo::GTE::clear<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>();

    psyqo::GTE::write<psyqo::GTE::Register::OFX, psyqo::GTE::Unsafe>(psyqo::FixedPoint<16>(160.0).raw());
    psyqo::GTE::write<psyqo::GTE::Register::OFY, psyqo::GTE::Unsafe>(psyqo::FixedPoint<16>(120.0).raw());
    psyqo::GTE::write<psyqo::GTE::Register::H, psyqo::GTE::Unsafe>(120);
    psyqo::GTE::write<psyqo::GTE::Register::ZSF3, psyqo::GTE::Unsafe>(ORDERING_TABLE_SIZE / 3);
    psyqo::GTE::write<psyqo::GTE::Register::ZSF4, psyqo::GTE::Unsafe>(ORDERING_TABLE_SIZE / 4);

    m_camera.wx = 0;
    m_camera.wy = 0;
    m_camera.wz = 0;
    m_camera.rotY.value = 0;
}

bool RoomScene::renderQuad(int32_t x0, int32_t y0, int32_t z0,
                           int32_t x1, int32_t y1, int32_t z1,
                           int32_t x2, int32_t y2, int32_t z2,
                           int32_t x3, int32_t y3, int32_t z3,
                           psyqo::Color color, int& qi,
                           psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot) {
    if (qi >= MAX_QUADS) return false;

    psyqo::Vec3 v0, v1, v2, v3;
    v0.x.value = x0; v0.y.value = y0; v0.z.value = z0;
    v1.x.value = x1; v1.y.value = y1; v1.z.value = z1;
    v2.x.value = x2; v2.y.value = y2; v2.z.value = z2;
    v3.x.value = x3; v3.y.value = y3; v3.z.value = z3;

    // Transform first 3 vertices
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::V0>(v0);
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::V1>(v1);
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::V2>(v2);
    psyqo::GTE::Kernels::rtpt();

    // Near-plane check: reject only if ALL 3 vertices are behind/at camera
    // SZ=0 means behind camera (GTE saturates negative Z to 0)
    uint32_t sz1 = psyqo::GTE::readRaw<psyqo::GTE::Register::SZ1, psyqo::GTE::Unsafe>();
    uint32_t sz2 = psyqo::GTE::readRaw<psyqo::GTE::Register::SZ2, psyqo::GTE::Unsafe>();
    uint32_t sz3 = psyqo::GTE::readRaw<psyqo::GTE::Register::SZ3, psyqo::GTE::Unsafe>();
    if (sz1 < MIN_Z && sz2 < MIN_Z && sz3 < MIN_Z) return false;

    // Backface culling: only use nclip when all vertices have valid screen coords.
    // When vertices are near the camera, GTE overflow produces garbage XY which
    // makes nclip unreliable. Skip it and render both sides for close geometry.
    bool allSafe = (sz1 >= 60 && sz2 >= 60 && sz3 >= 60);
    if (allSafe) {
        psyqo::GTE::Kernels::nclip();
        int32_t mac0 = 0;
        psyqo::GTE::read<psyqo::GTE::Register::MAC0>(reinterpret_cast<uint32_t*>(&mac0));
        if (mac0 <= 0) return false;
    }

    // Save first vertex BEFORE rtps shifts the FIFO
    eastl::array<psyqo::Vertex, 4> projected;
    psyqo::GTE::read<psyqo::GTE::Register::SXY0>(&projected[0].packed);

    // Transform 4th vertex - this shifts the FIFO
    psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V0>(v3);
    psyqo::GTE::Kernels::rtps();

    // Get average Z for all 4 vertices
    psyqo::GTE::Kernels::avsz4();
    int32_t zIndex = 0;
    psyqo::GTE::read<psyqo::GTE::Register::OTZ>(reinterpret_cast<uint32_t*>(&zIndex));
    if (zIndex <= 0 || zIndex >= ORDERING_TABLE_SIZE) return false;

    // After rtps, FIFO has shifted:
    // SXY0 = old SXY1 (v1), SXY1 = old SXY2 (v2), SXY2 = new v3
    psyqo::GTE::read<psyqo::GTE::Register::SXY0>(&projected[1].packed);
    psyqo::GTE::read<psyqo::GTE::Register::SXY1>(&projected[2].packed);
    psyqo::GTE::read<psyqo::GTE::Register::SXY2>(&projected[3].packed);

    // Clamp projected coords to prevent GPU max primitive size overflow (1023x511).
    // Near-camera vertices can project to extreme screen coords; the PS1 GPU drops
    // any quad where the bounding box exceeds these limits.
    for (auto& p : projected) {
        if (p.x < -351) p.x = -351;
        if (p.x > 672) p.x = 672;
        if (p.y < -135) p.y = -135;
        if (p.y > 376) p.y = 376;
    }

    auto& quad = m_quads[m_parity][qi];
    quad.primitive.setPointA(projected[0]);
    quad.primitive.setPointB(projected[1]);
    quad.primitive.setPointC(projected[2]);
    quad.primitive.setPointD(projected[3]);
    quad.primitive.setColor(color);
    quad.primitive.setOpaque();

    ot.insert(quad, zIndex);
    qi++;
    return true;
}

void RoomScene::renderFloor(int& qi, psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot) {
    psyqo::Color dark = {.r = 64, .g = 64, .b = 80};
    psyqo::Color light = {.r = 96, .g = 96, .b = 112};

    for (int tz = 0; tz < GRID_SIZE; tz++) {
        for (int tx = 0; tx < GRID_SIZE; tx++) {
            int32_t x0 = tx * TILE_FP - HALF_ROOM;
            int32_t z0 = tz * TILE_FP - HALF_ROOM;
            int32_t x1 = x0 + TILE_FP;
            int32_t z1 = z0 + TILE_FP;

            // Floor facing UP - winding: front-left, front-right, back-left, back-right
            psyqo::Color col = ((tx + tz) & 1) ? dark : light;
            renderQuad(x0, FLOOR_Y, z1,   // front-left
                       x1, FLOOR_Y, z1,   // front-right
                       x0, FLOOR_Y, z0,   // back-left
                       x1, FLOOR_Y, z0,   // back-right
                       col, qi, ot);
        }
    }
}

void RoomScene::renderWalls(int& qi, psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot) {
    int32_t y0 = FLOOR_Y;
    int32_t y1 = FLOOR_Y - WALL_H;

    psyqo::Color north = {.r = 140, .g = 70, .b = 70};
    psyqo::Color south = {.r = 70, .g = 140, .b = 70};
    psyqo::Color east = {.r = 70, .g = 70, .b = 140};
    psyqo::Color west = {.r = 140, .g = 140, .b = 70};

    // North wall (Z = -HALF_ROOM) - facing +Z (inward)
    for (int tx = 0; tx < GRID_SIZE; tx++) {
        int32_t x0 = tx * TILE_FP - HALF_ROOM;
        int32_t x1 = x0 + TILE_FP;
        int32_t z = -HALF_ROOM;
        renderQuad(x0, y0, z, x1, y0, z, x0, y1, z, x1, y1, z, north, qi, ot);
    }

    // South wall (Z = +HALF_ROOM) - facing -Z (inward)
    for (int tx = 0; tx < GRID_SIZE; tx++) {
        int32_t x0 = tx * TILE_FP - HALF_ROOM;
        int32_t x1 = x0 + TILE_FP;
        int32_t z = HALF_ROOM;
        renderQuad(x1, y0, z, x0, y0, z, x1, y1, z, x0, y1, z, south, qi, ot);
    }

    // East wall (X = +HALF_ROOM) - facing -X
    for (int tz = 0; tz < GRID_SIZE; tz++) {
        int32_t z0 = tz * TILE_FP - HALF_ROOM;
        int32_t z1 = z0 + TILE_FP;
        int32_t x = HALF_ROOM;
        renderQuad(x, y0, z0, x, y0, z1, x, y1, z0, x, y1, z1, east, qi, ot);
    }

    // West wall (X = -HALF_ROOM) - facing +X
    for (int tz = 0; tz < GRID_SIZE; tz++) {
        int32_t z0 = tz * TILE_FP - HALF_ROOM;
        int32_t z1 = z0 + TILE_FP;
        int32_t x = -HALF_ROOM;
        renderQuad(x, y0, z1, x, y0, z0, x, y1, z1, x, y1, z0, west, qi, ot);
    }
}

void RoomScene::frame() {
    m_frameCount++;

    using Pad = psyqo::AdvancedPad::Pad;
    using Button = psyqo::AdvancedPad::Button;

    constexpr int16_t rotSpeed = 20;
    constexpr int32_t moveSpeed = 12;

    // Get raw analog stick values (0-255, 0x80 = center)
    uint8_t rawLX = roomTest.m_input.getAdc(Pad::Pad1a, 2);
    uint8_t rawLY = roomTest.m_input.getAdc(Pad::Pad1a, 3);
    uint8_t rawRX = roomTest.m_input.getAdc(Pad::Pad1a, 0);
    uint8_t rawRY = roomTest.m_input.getAdc(Pad::Pad1a, 1);

    // If raw values are at extremes (no analog controller), treat as centered
    if (rawLX <= 2 || rawLX >= 253) rawLX = 128;
    if (rawLY <= 2 || rawLY >= 253) rawLY = 128;
    if (rawRX <= 2 || rawRX >= 253) rawRX = 128;
    if (rawRY <= 2 || rawRY >= 253) rawRY = 128;

    // Convert to signed (-128 to 127)
    int leftX = (int)rawLX - 128;
    int leftY = (int)rawLY - 128;
    int rightX = (int)rawRX - 128;
    int rightY = (int)rawRY - 128;

    // Store for debug display
    m_debugLX = rawLX;
    m_debugLY = rawLY;
    m_debugRX = rawRX;
    m_debugRY = rawRY;

    // Apply deadzone (values within +/-10 of center are ignored)
    constexpr int DEADZONE = 10;
    if (leftX > -DEADZONE && leftX < DEADZONE) leftX = 0;
    if (leftY > -DEADZONE && leftY < DEADZONE) leftY = 0;
    if (rightX > -DEADZONE && rightX < DEADZONE) rightX = 0;
    if (rightY > -DEADZONE && rightY < DEADZONE) rightY = 0;

    // Left stick: Camera rotation (horizontal look)
    m_camera.rotate((leftX * rotSpeed) >> 9, roomTest.m_trig);

    // Right stick: Movement (forward/back with Y, strafe with X)
    int32_t fwd = 0;
    int32_t strafe = 0;

    if (rightY != 0) {
        fwd = (-rightY * moveSpeed) >> 7;
    }
    if (rightX != 0) {
        strafe = (rightX * moveSpeed) >> 7;
    }

    // D-Pad: Up/Down = walk, Left/Right = turn
    if (roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Up)) {
        fwd += moveSpeed;
    }
    if (roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Down)) {
        fwd -= moveSpeed;
    }
    if (roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Left)) {
        m_camera.rotate(-rotSpeed, roomTest.m_trig);
    }
    if (roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Right)) {
        m_camera.rotate(rotSpeed, roomTest.m_trig);
    }

    // L1/R1: strafe
    if (roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::L1)) {
        strafe -= moveSpeed;
    }
    if (roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::R1)) {
        strafe += moveSpeed;
    }

    // Apply movement through camera (handles rotation internally)
    if (fwd != 0 || strafe != 0) {
        m_camera.moveLocal(fwd, strafe, roomTest.m_trig);
    }

    // Rendering
    m_parity = gpu().getParity();
    auto& ot = m_ots[m_parity];
    auto& clear = m_clear[m_parity];

    gpu().getNextClear(clear.primitive, c_bg);
    gpu().chain(clear);

    // Apply camera transform to GTE
    m_camera.applyToGTE(roomTest.m_trig);

    // Render
    int qi = 0;
    renderFloor(qi, ot);
    renderWalls(qi, ot);
    m_polyCount = qi;

    // Chain geometry first
    gpu().chain(ot);

    // HUD - use chainprintf to add to DMA chain (after geometry)
    psyqo::Color white = {{.r = 255, .g = 255, .b = 255}};
    psyqo::Color yellow = {{.r = 255, .g = 255, .b = 0}};

    roomTest.m_font.chainprintf(roomTest.gpu(), {{.x = 4, .y = 4}}, yellow, "ROOM Polys:%d", m_polyCount);
    roomTest.m_font.chainprintf(roomTest.gpu(), {{.x = 4, .y = 20}}, white, "L:%d,%d R:%d,%d", m_debugLX, m_debugLY, m_debugRX, m_debugRY);

    // Debug D-pad and shoulder buttons
    bool dU = roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Up);
    bool dD = roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Down);
    bool dL = roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Left);
    bool dR = roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::Right);
    bool bL1 = roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::L1);
    bool bR1 = roomTest.m_input.isButtonPressed(Pad::Pad1a, Button::R1);
    roomTest.m_font.chainprintf(roomTest.gpu(), {{.x = 4, .y = 36}}, white, "D:%d%d%d%d L1:%d R1:%d", dU, dD, dL, dR, bL1, bR1);
}

int main() { return roomTest.run(); }
