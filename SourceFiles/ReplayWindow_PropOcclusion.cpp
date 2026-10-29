#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "RunLog.h"

// SEE-THROUGH PROPS IN THE WAY - the client's Props::UpdateOpacity (Engine\Map\Props\PrAnimate.cpp,
// Gw.exe 0x73CAA0), measured statically on 2026-10-20.
//
// THE CLIENT'S RULE. Every frame the map is given the camera's eye and look-at (GmView 0x4EB440 ->
// MapSetCamera 0x709C80 -> Props_SetCamera 0x739D00), and every prop visible that frame is tested
// against that one segment: a 2D circle of the model's radius around the prop first, then the
// prop's own triangles (GrModel ray 0x669430). A prop the segment crosses fades; the others come
// back. One opacity per prop, pushed to the whole model - no per-pixel term.
//
//   eligible    the model's flags (geometry header dword +0x24) lack 0x40, and the model reaches
//               more than 36 units up (0x957850). The 0x40 models are the ones that never fade -
//               trees among them, as the owner's screenshots show.
//   while hit   a hold timer counts down first (1.5 s from rest), then opacity -= 2/s, floor 0.2.
//   while clear the timer counts back up to 0.5 s, then opacity += 4/s to 1, and the timer is
//               re-armed to 1.5 s.
//   no option   only cinematics switch it off (props+0x114).
//
// THE REPLAY'S SEGMENTS. In follow mode the look-at is the followed agent, so the segment is the
// client's own: eye -> the agent at the client's look-at height. The free camera and the top view
// have no look-at; there the segments run to every player on screen (the fog of war respected).
//
// THE FREE CAMERA FLIES THROUGH THINGS. It has no floor (the terrain is culled from below, so a
// camera under a hill sees through it) and nothing in the client to follow, so the props it meets
// get out of its way: a short probe straight ahead of the eye, and any prop the eye is INSIDE (an
// odd number of crossings straight up), fade - whatever the client's eligibility says, a tree or a
// cliff included - and at once, without the client's 1.5 s hold, so the camera never sits in rock.
//
// HOW IT IS DRAWN. A fading prop leaves the main pass for MapRenderer::RenderGhostMeshes, after the
// characters: depth first, then blended at the opacity (object_color.a), so each pixel shows the
// nearest surface of the faded props once over everything behind them.

namespace
{
    constexpr float kMinOpacity         = 0.2f;   // 0x948248
    constexpr float kFadeOutRate        = 2.0f;   // per second (fadd st0,st0 at 0x73CCD7)
    constexpr float kFadeInRate         = 4.0f;   // 0x93EF28
    constexpr float kHoldFromRest       = 1.5f;   // 0x94FDF0, also the value at prop creation
    constexpr float kHoldBeforeReturn   = 0.5f;   // 0x9478CC
    constexpr float kMinModelTop        = 36.f;   // 0x957850
    constexpr uint32_t kModelFlagNoFade = 0x40;   // model flags bit tested at 0x73C282
    constexpr float kLookAtHeight       = 90.f;   // the client aims at 0.92 x the agent's height
    constexpr float kChestHeight        = 50.f;   // a player's centre (the clip follow box)
    constexpr float kViewProbeLength    = 150.f;  // straight ahead of a camera without a subject
    constexpr size_t kMaxPlayerSegments = 24;

    constexpr float kInsideProbeHeight  = 20000.f; // the "straight up" ray of the inside test

    struct Segment
    {
        DirectX::XMFLOAT3 a, b;
        bool anyProp = false;   // the camera's own probe: fades any prop, not only the client's
    };

    // Segment p0 -> p1 against the sphere: the closest point of the segment to the centre.
    bool SegmentTouchesSphere(const Segment& s, const DirectX::XMFLOAT3& c, float r)
    {
        const float dx = s.b.x - s.a.x, dy = s.b.y - s.a.y, dz = s.b.z - s.a.z;
        const float len2 = dx * dx + dy * dy + dz * dz;
        float t = 0.f;
        if (len2 > 1e-6f)
            t = std::clamp(((c.x - s.a.x) * dx + (c.y - s.a.y) * dy + (c.z - s.a.z) * dz) / len2, 0.f, 1.f);
        const float px = s.a.x + dx * t - c.x, py = s.a.y + dy * t - c.y, pz = s.a.z + dz * t - c.z;
        return px * px + py * py + pz * pz <= r * r;
    }

    // Slab test of the segment a + t * d, t in [0, 1], against an axis-aligned box.
    bool SegmentTouchesBox(const float a[3], const float d[3], const float mn[3], const float mx[3])
    {
        float t0 = 0.f, t1 = 1.f;
        for (int i = 0; i < 3; ++i)
        {
            if (fabsf(d[i]) < 1e-8f)
            {
                if (a[i] < mn[i] || a[i] > mx[i]) return false;
                continue;
            }
            float ta = (mn[i] - a[i]) / d[i];
            float tb = (mx[i] - a[i]) / d[i];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return true;
    }

    // Moller-Trumbore, both faces: the segment a + t * d crosses the triangle for some t in (0, 1).
    bool SegmentCrossesTriangle(const float a[3], const float d[3],
                                const DirectX::XMFLOAT3& v0, const DirectX::XMFLOAT3& v1,
                                const DirectX::XMFLOAT3& v2)
    {
        const float e1[3] = { v1.x - v0.x, v1.y - v0.y, v1.z - v0.z };
        const float e2[3] = { v2.x - v0.x, v2.y - v0.y, v2.z - v0.z };
        const float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (fabsf(det) < 1e-9f) return false;
        const float inv = 1.f / det;
        const float s[3] = { a[0] - v0.x, a[1] - v0.y, a[2] - v0.z };
        const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0.f || u > 1.f) return false;
        const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < 0.f || u + v > 1.f) return false;
        const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        return t > 1e-4f && t < 1.f - 1e-4f;
    }

    // How many triangles of the mesh the segment a + t * d crosses.
    int CountCrossings(const Mesh& mesh, const float a[3], const float d[3])
    {
        int n = 0;
        const size_t nv = mesh.vertices.size();
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            if (SegmentCrossesTriangle(a, d, mesh.vertices[i0].position, mesh.vertices[i1].position,
                                       mesh.vertices[i2].position))
                ++n;
        }
        return n;
    }

    // The client's per-frame step for one prop (Props::UpdateOpacity).
    void StepOpacity(float& opacity, float& hold, bool hit, float dt)
    {
        if (hit)
        {
            hold = std::max(hold - dt, 0.f);
            if (hold <= 0.f)
                opacity = std::max(opacity - kFadeOutRate * dt, kMinOpacity);
        }
        else if (hold < kHoldBeforeReturn)
        {
            hold = std::min(hold + dt, kHoldBeforeReturn);
        }
        else
        {
            opacity = std::min(opacity + kFadeInRate * dt, 1.f);
            if (opacity >= 1.f)
                hold = kHoldFromRest;
        }
    }
}

// Segment a -> b against one prop's own triangles: the bounding sphere first, then each mesh's box
// in its own space, then the triangles.
bool ReplayWindow::SegmentHitsOcclusionProp(const OcclusionProp& prop, const DirectX::XMFLOAT3& from,
                                            const DirectX::XMFLOAT3& to) const
{
    using namespace DirectX;
    if (!SegmentTouchesSphere(Segment{ from, to }, prop.center, prop.radius)) return false;
    MeshManager* meshes = m_mapRenderer->GetMeshManager();
    for (const OcclusionMesh& om : prop.meshes)
    {
        if (!meshes->GetMeshShouldRender(om.meshId)) continue;
        const XMMATRIX inv = XMLoadFloat4x4(&om.invWorld);
        XMFLOAT3 la, lb;
        XMStoreFloat3(&la, XMVector3TransformCoord(XMLoadFloat3(&from), inv));
        XMStoreFloat3(&lb, XMVector3TransformCoord(XMLoadFloat3(&to), inv));
        const float a[3] = { la.x, la.y, la.z };
        const float d[3] = { lb.x - la.x, lb.y - la.y, lb.z - la.z };
        const float mn[3] = { om.boxMin.x, om.boxMin.y, om.boxMin.z };
        const float mx[3] = { om.boxMax.x, om.boxMax.y, om.boxMax.z };
        if (!SegmentTouchesBox(a, d, mn, mx)) continue;

        auto inst = meshes->GetMesh(om.meshId);
        if (!inst) continue;
        const Mesh& mesh = inst->GetMesh();
        const size_t nv = mesh.vertices.size();
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            if (SegmentCrossesTriangle(a, d, mesh.vertices[i0].position, mesh.vertices[i1].position,
                                       mesh.vertices[i2].position))
                return true;
        }
    }
    return false;
}

// How many of the sight lines from the eye to these points are blocked: by any prop - trees and
// cliffs included, the ones the client never fades - or by the ground between. Highlights' camera
// looks for its clearest angle with it.
int ReplayWindow::CountBlockedSightLines(const DirectX::XMFLOAT3& eye, const std::vector<DirectX::XMFLOAT3>& points) const
{
    if (!m_mapRenderer) return 0;
    int blocked = 0;
    for (const DirectX::XMFLOAT3& p : points)
    {
        bool hit = false;
        for (int k = 1; k < 8 && !hit; ++k)
        {
            const float f = k / 8.f;
            const float x = eye.x + (p.x - eye.x) * f, y = eye.y + (p.y - eye.y) * f, z = eye.z + (p.z - eye.z) * f;
            float ground = 0.f;
            if (m_mapRenderer->TryGetGroundHeight(x, z, ground) && ground > y) hit = true;
        }
        for (const OcclusionProp& prop : m_occlusionProps)
        {
            if (hit) break;
            hit = SegmentHitsOcclusionProp(prop, eye, p);
        }
        if (hit) ++blocked;
    }
    return blocked;
}

void ReplayWindow::ResetPropOcclusion()
{
    m_occlusionProps.clear();
    m_occlusionPropsBuiltFrom = 0;
}

void ReplayWindow::UpdatePropOcclusion(float dt)
{
    using namespace DirectX;
    if (!m_mapRenderer || m_loadingPhase != LoadingPhase::Ready) return;
    MeshManager* meshes = m_mapRenderer->GetMeshManager();
    auto& propMeshIds = m_mapRenderer->GetPropsMeshIds();

    auto applyOpacity = [&](OcclusionProp& p, float opacity)
    {
        for (const OcclusionMesh& om : p.meshes)
        {
            auto data = meshes->GetMeshPerObjectData(om.meshId);
            if (!data.has_value()) continue;
            PerObjectCB cb = data.value();
            cb.object_color.w = opacity;
            meshes->UpdateMeshPerObjectData(om.meshId, cb);
            meshes->SetMeshGhost(om.meshId, opacity < 1.f);
        }
        p.applied = opacity;
    };

    // ---- The props the client lets fade, once they are all placed -----------------------------
    if (propMeshIds.size() != m_occlusionPropsBuiltFrom)
    {
        for (OcclusionProp& p : m_occlusionProps)
            if (p.applied != 1.f) applyOpacity(p, 1.f);
        m_occlusionProps.clear();
        m_occlusionPropsBuiltFrom = propMeshIds.size();

        const auto& propsInfo = m_mapFile.props_info_chunk.prop_array.props_info;
        int noFadeFlag = 0, tooLow = 0;
        for (const auto& [propIndex, ids] : propMeshIds)
        {
            // The model's flags: the geometry header dword at +0x24 (the client's loader 0x795B70
            // copies it to the model at 0x7960FA). Props the replay adds itself (indices past the
            // map's list) and models that did not parse are left opaque, as they always were.
            if (propIndex >= propsInfo.size()) continue;
            const uint32_t fileIndex = propsInfo[propIndex].filename_index;
            if (fileIndex >= m_propModelFiles.size()) continue;
            const auto* model = std::get_if<FFNA_ModelFile>(&m_propModelFiles[fileIndex]);
            if (!model || !model->parsed_correctly) continue;
            uint32_t modelFlags = 0;
            std::memcpy(&modelFlags, model->geometry_chunk.sub_1.f0x24, sizeof(modelFlags));

            OcclusionProp prop;
            prop.propIndex = propIndex;
            float modelTop = -FLT_MAX;
            XMVECTOR wmin = XMVectorReplicate(FLT_MAX), wmax = XMVectorReplicate(-FLT_MAX);
            for (int id : ids)
            {
                auto inst = meshes->GetMesh(id);
                if (!inst) continue;
                const Mesh& mesh = inst->GetMesh();
                if (mesh.vertices.empty() || mesh.indices.size() < 3) continue;

                OcclusionMesh om;
                om.meshId = id;
                XMVECTOR lmin = XMVectorReplicate(FLT_MAX), lmax = XMVectorReplicate(-FLT_MAX);
                for (const GWVertex& v : mesh.vertices)
                {
                    const XMVECTOR p = XMLoadFloat3(&v.position);
                    lmin = XMVectorMin(lmin, p);
                    lmax = XMVectorMax(lmax, p);
                }
                XMStoreFloat3(&om.boxMin, lmin);
                XMStoreFloat3(&om.boxMax, lmax);
                modelTop = std::max(modelTop, om.boxMax.y);

                const XMMATRIX world = XMLoadFloat4x4(&inst->GetPerObjectData().world);
                XMVECTOR det;
                XMStoreFloat4x4(&om.invWorld, XMMatrixInverse(&det, world));
                if (fabsf(XMVectorGetX(det)) < 1e-12f) continue;

                for (int c = 0; c < 8; ++c)
                {
                    const XMVECTOR corner = XMVectorSet(c & 1 ? om.boxMax.x : om.boxMin.x,
                                                        c & 2 ? om.boxMax.y : om.boxMin.y,
                                                        c & 4 ? om.boxMax.z : om.boxMin.z, 1.f);
                    const XMVECTOR w = XMVector3TransformCoord(corner, world);
                    wmin = XMVectorMin(wmin, w);
                    wmax = XMVectorMax(wmax, w);
                }
                prop.meshes.push_back(om);
            }
            if (prop.meshes.empty()) continue;
            // The client's two gates: the model's flag, and the model (unscaled, LOD 0) reaching
            // more than 36 units up. A prop failing either is still kept, for the free camera.
            prop.clientFades = true;
            if (modelFlags & kModelFlagNoFade) { ++noFadeFlag; prop.clientFades = false; }
            else if (modelTop <= kMinModelTop) { ++tooLow; prop.clientFades = false; }
            XMStoreFloat3(&prop.center, (wmin + wmax) * 0.5f);
            prop.radius = XMVectorGetX(XMVector3Length(wmax - wmin)) * 0.5f;
            m_occlusionProps.push_back(std::move(prop));
        }
        RunLog::Line("see-through props: %zu of %zu tested, the client fades %zu (%d never fade by model "
                     "flag 0x40, %d too low; those fade only for the free camera)",
                     m_occlusionProps.size(), propMeshIds.size(),
                     m_occlusionProps.size() - noFadeFlag - tooLow, noFadeFlag, tooLow);
    }
    if (m_occlusionProps.empty()) return;

    // ---- The segments of this frame ------------------------------------------------------------
    std::vector<Segment> segments;
    const XMFLOAT3 eye = m_mapRenderer->GetCamera()->GetPosition3f();
    const bool enabled = GuiGlobalConstants::replay_fade_occluding_props;
    const bool freeCamera = m_cameraMode != CameraMode::FollowAgent && !m_topViewActive &&
                            !m_topViewTransitioning;
    if (enabled)
    {
        const Camera* cam = m_mapRenderer->GetCamera();
        const bool following = m_followCenterValid && m_cameraMode == CameraMode::FollowAgent &&
                               !m_topViewActive && !m_topViewTransitioning;
        if (following)
        {
            segments.push_back({ eye, XMFLOAT3(m_followCenter.x, m_followCenter.y + kLookAtHeight, m_followCenter.z) });
        }
        else
        {
            const XMFLOAT3 look = cam->GetLook3f();
            segments.push_back({ eye, XMFLOAT3(eye.x + look.x * kViewProbeLength,
                                               eye.y + look.y * kViewProbeLength,
                                               eye.z + look.z * kViewProbeLength), freeCamera });

            const XMMATRIX viewProj = cam->GetView() * cam->GetProj();
            const InterpolationSettings& is = m_replayCtx.interpSettings;
            size_t players = 0;
            for (const auto& [agentId, ard] : m_replayCtx.agents)
            {
                if (players >= kMaxPlayerSegments) break;
                if (ard.type != AgentType::Player || ard.snapshots.empty()) continue;
                if (!AgentOnStageAt(agentId, ard, m_debugTimeline)) continue;
                if (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId)) continue;
                float sx, sy, sz;
                InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);
                XMFLOAT3 p = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
                p.y += kChestHeight;
                const XMVECTOR clip = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.f), viewProj);
                const float w = XMVectorGetW(clip);
                if (w <= 0.f || fabsf(XMVectorGetX(clip)) > w || fabsf(XMVectorGetY(clip)) > w) continue;
                segments.push_back({ eye, p });
                ++players;
            }
        }
    }

    // ---- Which props the segments cross, and the client's step toward it -----------------------
    for (OcclusionProp& prop : m_occlusionProps)
    {
        bool hit = false;
        bool cameraHit = false;   // the free camera's own: no hold, any prop

        // The free camera inside the prop: an odd number of crossings straight up from the eye.
        if (enabled && freeCamera)
        {
            const float ex = eye.x - prop.center.x, ey = eye.y - prop.center.y, ez = eye.z - prop.center.z;
            if (ex * ex + ey * ey + ez * ez <= prop.radius * prop.radius)
            {
                for (const OcclusionMesh& om : prop.meshes)
                {
                    if (!meshes->GetMeshShouldRender(om.meshId)) continue;
                    const XMMATRIX inv = XMLoadFloat4x4(&om.invWorld);
                    XMFLOAT3 la, lb;
                    XMStoreFloat3(&la, XMVector3TransformCoord(XMLoadFloat3(&eye), inv));
                    if (la.x < om.boxMin.x || la.x > om.boxMax.x || la.y < om.boxMin.y || la.y > om.boxMax.y ||
                        la.z < om.boxMin.z || la.z > om.boxMax.z)
                        continue;
                    const XMFLOAT3 up(eye.x, eye.y + kInsideProbeHeight, eye.z);
                    XMStoreFloat3(&lb, XMVector3TransformCoord(XMLoadFloat3(&up), inv));
                    const float a[3] = { la.x, la.y, la.z };
                    const float d[3] = { lb.x - la.x, lb.y - la.y, lb.z - la.z };
                    auto inst = meshes->GetMesh(om.meshId);
                    if (inst && (CountCrossings(inst->GetMesh(), a, d) & 1))
                    {
                        hit = cameraHit = true;
                        break;
                    }
                }
            }
        }

        for (const Segment& s : segments)
        {
            if (hit) break;
            if (!s.anyProp && !prop.clientFades) continue;
            if (SegmentHitsOcclusionProp(prop, s.a, s.b))
            {
                hit = true;
                cameraHit = s.anyProp;
            }
        }

        if (cameraHit)
            prop.hold = 0.f;
        StepOpacity(prop.opacity, prop.hold, hit, dt);
        if (prop.opacity != prop.applied)
            applyOpacity(prop, prop.opacity);
    }
}
