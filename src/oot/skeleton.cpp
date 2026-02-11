// OoT PS1 — Skeleton loading, bone hierarchy, limb rendering.

#include "scene.h"

// ── OoT angle conversion ─────────────────────────────────────────────────

// OoT s16 binary angle (0x10000 = full circle) → psyqo::Angle (FixedPoint<10>)
// Full circle: OoT = 65536, psyqo = 2048. Ratio = 32.
static inline psyqo::Angle ootAngle(int16_t raw) {
    return psyqo::Angle(static_cast<int32_t>(raw) / 32, psyqo::Angle::RAW);
}

// Build ZYX Euler rotation matrix matching OoT's Matrix_TranslateRotateZYX
static psyqo::Matrix33 eulerZYX(int16_t rz, int16_t ry, int16_t rx,
                                 const psyqo::Trig<>& trig) {
    auto mz = psyqo::SoftMath::generateRotationMatrix33(
        ootAngle(rz), psyqo::SoftMath::Axis::Z, trig);
    auto my = psyqo::SoftMath::generateRotationMatrix33(
        ootAngle(ry), psyqo::SoftMath::Axis::Y, trig);
    auto mx = psyqo::SoftMath::generateRotationMatrix33(
        ootAngle(rx), psyqo::SoftMath::Axis::X, trig);
    psyqo::Matrix33 zy, zyx;
    psyqo::SoftMath::multiplyMatrix33(mz, my, &zy);
    psyqo::SoftMath::multiplyMatrix33(zy, mx, &zyx);
    return zyx;
}

// ── Skeleton loading via CD-ROM ──────────────────────────────────────────

void RoomScene::loadSkeleton() {
    app.m_loader.readFile("LINK.SKM;1", app.m_isoParser,
        [this](psyqo::Buffer<uint8_t>&& buffer) {
            m_skelBuf = eastl::move(buffer);
            if (m_skelBuf.size() > sizeof(SKM::Header)) {
                m_skm = m_skelBuf.data();
                m_limbCache.build(m_skm);
                m_skelLoaded = true;
            }
            loadRoom(0);
        });
}

// ── Bone hierarchy computation ───────────────────────────────────────────

void RoomScene::computeBones(const int16_t* frame) {
    int16_t rootX, rootY, rootZ;
    SKM::frameRootPos(frame, rootX, rootY, rootZ);

    int16_t rz, ry, rx;
    SKM::frameLimbRot(frame, 0, rz, ry, rx);

    m_bones[0].rot = eulerZYX(rz, ry, rx, app.m_trig);
    m_bones[0].tx = rootX;
    m_bones[0].ty = rootY;
    m_bones[0].tz = rootZ;

    const auto* ls = SKM::limbs(m_skm);
    if (ls[0].child != 0xFF)
        computeBoneRecurse(ls[0].child, m_bones[0], frame);
}

void RoomScene::computeBoneRecurse(int limbIdx, const BoneState& parent,
                                    const int16_t* frame) {
    const auto* ls = SKM::limbs(m_skm);
    const auto& limb = ls[limbIdx];

    int16_t rz, ry, rx;
    SKM::frameLimbRot(frame, limbIdx, rz, ry, rx);

    psyqo::Matrix33 localRot = eulerZYX(rz, ry, rx, app.m_trig);

    psyqo::SoftMath::multiplyMatrix33(parent.rot, localRot, &m_bones[limbIdx].rot);

    int32_t jx = limb.joint_x, jy = limb.joint_y, jz = limb.joint_z;
    auto& pr = parent.rot;
    m_bones[limbIdx].tx = parent.tx +
        ((pr.vs[0].x.raw() * jx + pr.vs[0].y.raw() * jy + pr.vs[0].z.raw() * jz) >> 12);
    m_bones[limbIdx].ty = parent.ty +
        ((pr.vs[1].x.raw() * jx + pr.vs[1].y.raw() * jy + pr.vs[1].z.raw() * jz) >> 12);
    m_bones[limbIdx].tz = parent.tz +
        ((pr.vs[2].x.raw() * jx + pr.vs[2].y.raw() * jy + pr.vs[2].z.raw() * jz) >> 12);

    if (limb.child != 0xFF)
        computeBoneRecurse(limb.child, m_bones[limbIdx], frame);
    if (limb.sibling != 0xFF)
        computeBoneRecurse(limb.sibling, parent, frame);
}

// ── Skeleton rendering ───────────────────────────────────────────────────

void RoomScene::renderSkeleton(const psyqo::Matrix33& renderRot,
                                int32_t camTX, int32_t camTY, int32_t camTZ) {
    if (!m_skm) return;

    const auto* shdr = SKM::header(m_skm);

    // Advance animation
    if (!m_animPaused) {
        m_animFrame++;
        const auto* ad = &SKM::animDescs(m_skm)[m_animIdx];
        if (m_animFrame >= ad->frame_count) {
            m_animFrame = (ad->flags & 1) ? 0 : ad->frame_count - 1;
        }
    }

    const int16_t* frame = SKM::animFrame(m_skm, m_animIdx, m_animFrame);
    computeBones(frame);

    for (int i = 0; i < shdr->num_limbs; i++) {
        drawLimb(i, renderRot, camTX, camTY, camTZ);
    }
}

void RoomScene::drawLimb(int limbIdx, const psyqo::Matrix33& renderRot,
                          int32_t camTX, int32_t camTY, int32_t camTZ) {
    const auto* ls = SKM::limbs(m_skm);
    if (ls[limbIdx].num_verts == 0 || ls[limbIdx].num_tris == 0) return;

    // View-space rotation = camera × bone world rotation
    psyqo::Matrix33 viewRot;
    psyqo::SoftMath::multiplyMatrix33(renderRot, m_bones[limbIdx].rot, &viewRot);

    // View-space translation = camera_rot × bone_world_pos + camera_trans
    int32_t bx = m_bones[limbIdx].tx + m_skelX;
    int32_t by = m_bones[limbIdx].ty + m_skelY;
    int32_t bz = m_bones[limbIdx].tz + m_skelZ;
    int32_t vtx = ((renderRot.vs[0].x.raw() * bx +
                    renderRot.vs[0].y.raw() * by +
                    renderRot.vs[0].z.raw() * bz) >> 12) + camTX;
    int32_t vty = ((renderRot.vs[1].x.raw() * bx +
                    renderRot.vs[1].y.raw() * by +
                    renderRot.vs[1].z.raw() * bz) >> 12) + camTY;
    int32_t vtz = ((renderRot.vs[2].x.raw() * bx +
                    renderRot.vs[2].y.raw() * by +
                    renderRot.vs[2].z.raw() * bz) >> 12) + camTZ;

    // Write per-limb view matrix to GTE
    psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(viewRot);
    psyqo::GTE::write<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(vtx));
    psyqo::GTE::write<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(vty));
    psyqo::GTE::write<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>(
        static_cast<uint32_t>(vtz));

    // Transform limb vertices
    const auto* pos = m_limbCache.positions(m_skm, limbIdx);
    transformVertices(reinterpret_cast<const PRM::Pos*>(pos), ls[limbIdx].num_verts);

    // Emit textured triangles
    const auto* uv = m_limbCache.uvs(m_skm, limbIdx);
    const auto* tri = m_limbCache.triangles(m_skm, limbIdx);
    const auto* shdr = SKM::header(m_skm);

    for (int t = 0; t < ls[limbIdx].num_tris && m_triCount < MAX_TRIS; t++) {
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
        if (cross >= 0) continue;

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

        int texSlot = m_skelTexBase + idx.tex_id;
        if (idx.tex_id < shdr->num_textures && texSlot < g_vramAlloc.numSlots()) {
            const auto& ti = g_vramAlloc.info(texSlot);
            p.uvA.u = (uv[idx.v0].u & ti.u_mask) + ti.u_off;
            p.uvA.v = (uv[idx.v0].v & ti.v_mask) + ti.v_off;
            p.uvB.u = (uv[idx.v1].u & ti.u_mask) + ti.u_off;
            p.uvB.v = (uv[idx.v1].v & ti.v_mask) + ti.v_off;
            p.uvC.u = (uv[idx.v2].u & ti.u_mask) + ti.u_off;
            p.uvC.v = (uv[idx.v2].v & ti.v_mask) + ti.v_off;
            p.tpage = ti.tpage;
            p.clutIndex = ti.clut;
        }

        m_ots[m_parity].insert(frag, otIdx);
        m_triCount++;
    }
}
