// OoT PS1 — Room loading, texture upload, chunk rendering, debug grid.

#include "scene.h"

using namespace psyqo::trig_literals;

// ── Room table ───────────────────────────────────────────────────────────

const char* const ROOM_FILES[NUM_ROOMS] = {
    "ROOMS/YDAN_0.PRM;1",
    "ROOMS/YDAN_1.PRM;1",
    "ROOMS/SPOT04_0.PRM;1",
    "ROOMS/SPOT00_0.PRM;1",
    "ROOMS/BMORI1_0.PRM;1",
    "ROOMS/HIDAN_0.PRM;1",
    "ROOMS/MIZUSIN0.PRM;1",
    "ROOMS/HAKADAN0.PRM;1",
    "ROOMS/SPOT15_0.PRM;1",
    "ROOMS/SPOT01_0.PRM;1",
};

const char* const ROOM_NAMES[NUM_ROOMS] = {
    "Deku Tree 1", "Deku Tree 2", "Kokiri Forest",
    "Hyrule Field", "Forest Temple", "Fire Temple",
    "Water Temple", "Shadow Temple", "Lon Lon Ranch",
    "Kakariko",
};

// Spawn 0 from each room's scene (world-space coordinates)
const SpawnPoint ROOM_SPAWNS[NUM_ROOMS] = {
    {    -4,     0,   603, -32768},  // Deku Tree 1
    {    -4,     0,   603, -32768},  // Deku Tree 2 (same scene)
    {   -68,   -80,   941,  25486},  // Kokiri Forest
    {   160,     0,  1415,  -3641},  // Hyrule Field
    {   110,   309,   781, -32768},  // Forest Temple
    {     5,     0,   983, -32768},  // Fire Temple
    {  -182,   620,   969, -32768},  // Water Temple
    {  -254,   -63,   734, -32768},  // Shadow Temple
    {  -225,  1086,  3743, -27307},  // Lon Lon Ranch
    { -2649,   138,  1063,  16384},  // Kakariko
};

// ── Room loading via CD-ROM ──────────────────────────────────────────────

void RoomScene::loadRoom(int idx) {
    m_loading = true;
    m_prm = nullptr;
    m_roomIdx = idx;

    app.m_loader.readFile(ROOM_FILES[idx], app.m_isoParser,
        [this](psyqo::Buffer<uint8_t>&& buffer) {
            m_roomBuf = eastl::move(buffer);
            m_prm = m_roomBuf.data();
            m_needUpload = (m_prm != nullptr);

            // Place skeleton at spawn point, reset orbit camera
            const auto& sp = ROOM_SPAWNS[m_roomIdx];
            m_skelX = sp.x;
            m_skelY = sp.y;
            m_skelZ = sp.z;
            m_camRotY = 0.0_pi;
            m_camRotX = 0.1_pi;
            m_camDist = 200;
            m_loading = false;
        });
}

// ── Upload textures to VRAM (room + skeleton) ────────────────────────────

void RoomScene::uploadTextures() {
    g_vramAlloc.reset();

    // Upload room textures
    if (m_prm) {
        const auto* hdr = PRM::header(m_prm);
        const auto* descs = PRM::texDescs(m_prm);
        const uint8_t* tdata = PRM::texData(m_prm);

        for (int i = 0; i < hdr->num_textures; i++) {
            const auto& td = descs[i];
            uint16_t clut_n = PRM::texClutCount(td);

            int slot = g_vramAlloc.alloc(td.width, td.height, td.format, clut_n);
            if (slot < 0) continue;

            const uint16_t* pix = reinterpret_cast<const uint16_t*>(
                tdata + td.data_offset);
            gpu().uploadToVRAM(pix, g_vramAlloc.pixelRect(slot));

            uint32_t pix_bytes = PRM::texPixelSize(td);
            pix_bytes = (pix_bytes + 1) & ~1u;
            const uint16_t* clut = reinterpret_cast<const uint16_t*>(
                tdata + td.data_offset + pix_bytes);
            gpu().uploadToVRAM(clut, g_vramAlloc.clutRect(slot));
        }
    }

    // Upload skeleton textures (appended after room textures)
    m_skelTexBase = g_vramAlloc.numSlots();
    if (m_skm) {
        const auto* shdr = SKM::header(m_skm);
        const auto* sdescs = SKM::texDescs(m_skm);
        const uint8_t* stdata = SKM::texData(m_skm);

        for (int i = 0; i < shdr->num_textures; i++) {
            const auto& td = sdescs[i];
            uint16_t clut_n = SKM::texClutCount(td);

            int slot = g_vramAlloc.alloc(td.width, td.height, td.format, clut_n);
            if (slot < 0) continue;

            const uint16_t* pix = reinterpret_cast<const uint16_t*>(
                stdata + td.data_offset);
            gpu().uploadToVRAM(pix, g_vramAlloc.pixelRect(slot));

            uint32_t pix_bytes = SKM::texPixelSize(td);
            pix_bytes = (pix_bytes + 1) & ~1u;
            const uint16_t* clut = reinterpret_cast<const uint16_t*>(
                stdata + td.data_offset + pix_bytes);
            gpu().uploadToVRAM(clut, g_vramAlloc.clutRect(slot));
        }
    }
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
            reinterpret_cast<uint32_t*>(&g_scratch[i].sx));
        psyqo::GTE::read<psyqo::GTE::Register::SXY1>(
            reinterpret_cast<uint32_t*>(&g_scratch[i + 1].sx));
        psyqo::GTE::read<psyqo::GTE::Register::SXY2>(
            reinterpret_cast<uint32_t*>(&g_scratch[i + 2].sx));

        uint32_t sz1, sz2, sz3;
        psyqo::GTE::read<psyqo::GTE::Register::SZ1>(&sz1);
        psyqo::GTE::read<psyqo::GTE::Register::SZ2>(&sz2);
        psyqo::GTE::read<psyqo::GTE::Register::SZ3>(&sz3);
        g_scratch[i].sz     = static_cast<uint16_t>(sz1);
        g_scratch[i + 1].sz = static_cast<uint16_t>(sz2);
        g_scratch[i + 2].sz = static_cast<uint16_t>(sz3);
    }

    for (; i < count; i++) {
        const auto* r = reinterpret_cast<const uint32_t*>(&pos[i]);
        psyqo::GTE::write<psyqo::GTE::Register::VXY0, psyqo::GTE::Unsafe>(r[0]);
        psyqo::GTE::write<psyqo::GTE::Register::VZ0, psyqo::GTE::Safe>(r[1]);
        psyqo::GTE::Kernels::rtps();

        psyqo::GTE::read<psyqo::GTE::Register::SXY2>(
            reinterpret_cast<uint32_t*>(&g_scratch[i].sx));
        uint32_t sz;
        psyqo::GTE::read<psyqo::GTE::Register::SZ3>(&sz);
        g_scratch[i].sz = static_cast<uint16_t>(sz);
    }
}

// ── Render one chunk ─────────────────────────────────────────────────────

void RoomScene::renderChunk(const PRM::ChunkDesc& chunk) {
    if (chunk.num_verts == 0 || chunk.num_tris == 0) return;

    const auto* pos = PRM::positions(m_prm, chunk);
    const auto* col = PRM::colors(m_prm, chunk);
    const auto* uv = PRM::uvs(m_prm, chunk);
    const auto* tri = PRM::triangles(m_prm, chunk);

    transformVertices(pos, chunk.num_verts);

    for (int t = 0; t < chunk.num_tris && m_triCount < MAX_TRIS; t++) {
        const auto& idx = tri[t];
        const auto& sv0 = g_scratch[idx.v0];
        const auto& sv1 = g_scratch[idx.v1];
        const auto& sv2 = g_scratch[idx.v2];

        if (sv0.sz == 0 || sv1.sz == 0 || sv2.sz == 0) continue;

        int32_t dx0 = sv1.sx - sv0.sx;
        int32_t dy0 = sv1.sy - sv0.sy;
        int32_t dx1 = sv2.sx - sv0.sx;
        int32_t dy1 = sv2.sy - sv0.sy;
        int32_t cross = dx0 * dy1 - dx1 * dy0;
        if (cross <= 0) continue;

        if (sv0.sx < -512 || sv0.sx > 512 || sv0.sy < -512 || sv0.sy > 512) continue;
        if (sv1.sx < -512 || sv1.sx > 512 || sv1.sy < -512 || sv1.sy > 512) continue;
        if (sv2.sx < -512 || sv2.sx > 512 || sv2.sy < -512 || sv2.sy > 512) continue;

        uint32_t sumZ = uint32_t(sv0.sz) + sv1.sz + sv2.sz;
        int32_t otIdx = static_cast<int32_t>((sumZ * (OT_SIZE / 3)) >> 12);
        if (otIdx <= 0 || otIdx >= OT_SIZE) continue;

        auto& frag = m_tris[m_parity][m_triCount];
        auto& p = frag.primitive;

        p.pointA.x = sv0.sx; p.pointA.y = sv0.sy;
        p.pointB.x = sv1.sx; p.pointB.y = sv1.sy;
        p.pointC.x = sv2.sx; p.pointC.y = sv2.sy;

        psyqo::Color neutral{{.r = 128, .g = 128, .b = 128}};
        p.setColorA(neutral);
        p.setColorB(neutral);
        p.setColorC(neutral);

        const auto& ti = g_vramAlloc.info(idx.tex_id);
        p.uvA.u = (uv[idx.v0].u & ti.u_mask) + ti.u_off;
        p.uvA.v = (uv[idx.v0].v & ti.v_mask) + ti.v_off;
        p.uvB.u = (uv[idx.v1].u & ti.u_mask) + ti.u_off;
        p.uvB.v = (uv[idx.v1].v & ti.v_mask) + ti.v_off;
        p.uvC.u = (uv[idx.v2].u & ti.u_mask) + ti.u_off;
        p.uvC.v = (uv[idx.v2].v & ti.v_mask) + ti.v_off;

        p.tpage = ti.tpage;
        p.clutIndex = ti.clut;

        m_ots[m_parity].insert(frag, otIdx);
        m_triCount++;
    }
}

// ── Debug texture grid ───────────────────────────────────────────────────

void RoomScene::renderDebugGrid() {
    gpu().waitChainIdle();
    m_parity = gpu().getParity();
    auto& ot = m_ots[m_parity];
    ot.clear();

    if (m_prm) {
        const auto* hdr = PRM::header(m_prm);
        const auto* descs = PRM::texDescs(m_prm);
        int numTex = hdr->num_textures;
        if (numTex > VramAlloc::MAX_TEXTURES) numTex = VramAlloc::MAX_TEXTURES;

        constexpr int COLS = 8;
        constexpr int CELL_W = 40;
        constexpr int CELL_H = 52;
        constexpr int QUAD_SZ = 36;
        constexpr int16_t TOP_Y = 20;

        for (int i = 0; i < numTex; i++) {
            int col = i % COLS;
            int row = i / COLS;
            int16_t cx = static_cast<int16_t>(col * CELL_W + (CELL_W - QUAD_SZ) / 2);
            int16_t cy = static_cast<int16_t>(TOP_Y + row * CELL_H);

            const auto& td = descs[i];
            const auto& ti = g_vramAlloc.info(i);

            auto& frag = m_debugQuads[m_parity][i];
            auto& q = frag.primitive;
            q.setColor(psyqo::Color{{.r = 128, .g = 128, .b = 128}});

            q.pointA.x = cx;            q.pointA.y = cy;
            q.pointB.x = cx + QUAD_SZ;  q.pointB.y = cy;
            q.pointC.x = cx;            q.pointC.y = cy + QUAD_SZ;
            q.pointD.x = cx + QUAD_SZ;  q.pointD.y = cy + QUAD_SZ;

            uint8_t maxU = static_cast<uint8_t>(td.width > QUAD_SZ ? QUAD_SZ - 1 : td.width - 1);
            uint8_t maxV = static_cast<uint8_t>(td.height > QUAD_SZ ? QUAD_SZ - 1 : td.height - 1);
            q.uvA.u = ti.u_off;          q.uvA.v = ti.v_off;
            q.uvB.u = ti.u_off + maxU;   q.uvB.v = ti.v_off;
            q.uvC.u = ti.u_off;          q.uvC.v = ti.v_off + maxV;
            q.uvD.u = ti.u_off + maxU;   q.uvD.v = ti.v_off + maxV;

            q.tpage = ti.tpage;
            q.clutIndex = ti.clut;

            ot.insert(frag, 1);
        }
    }

    auto& clear = m_clear[m_parity];
    psyqo::Color bg{{.r = 0x10, .g = 0x10, .b = 0x10}};
    gpu().getNextClear(clear.primitive, bg);
    gpu().chain(clear);
    gpu().chain(ot);

    psyqo::Color white{{.r = 255, .g = 255, .b = 255}};
    if (m_prm) {
        const auto* hdr = PRM::header(m_prm);
        app.m_font.printf(gpu(), {{.x = 4, .y = 4}}, white,
            "[%d/%d] %s  TEX:%d", m_roomIdx + 1, NUM_ROOMS,
            ROOM_NAMES[m_roomIdx], hdr->num_textures);

        const auto* descs = PRM::texDescs(m_prm);
        int numTex = hdr->num_textures;
        if (numTex > VramAlloc::MAX_TEXTURES) numTex = VramAlloc::MAX_TEXTURES;
        constexpr int COLS = 8;
        constexpr int CELL_W = 40;
        constexpr int CELL_H = 52;
        constexpr int QUAD_SZ = 36;
        constexpr int16_t TOP_Y = 20;

        psyqo::Color gray{{.r = 160, .g = 160, .b = 160}};
        for (int i = 0; i < numTex; i++) {
            int col = i % COLS;
            int row = i / COLS;
            int16_t lx = static_cast<int16_t>(col * CELL_W + 2);
            int16_t ly = static_cast<int16_t>(TOP_Y + row * CELL_H + QUAD_SZ + 2);
            const auto& td = descs[i];
            app.m_font.printf(gpu(), {{.x = lx, .y = ly}}, gray,
                "%d %dx%d", i, td.width, td.height);
        }
    } else {
        app.m_font.printf(gpu(), {{.x = 4, .y = 4}}, white, "No room data");
    }
}
