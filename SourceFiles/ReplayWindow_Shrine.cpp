#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------

namespace {

static float ShrineCaptureTime(int effectivePips)
{
    switch (effectivePips) {
    case 1: return 12.f;
    case 2: return 12.f;
    case 3: return 9.f;
    case 4: return 7.f;
    default: return 0.f;
    }
}

static float ShrinePipRate(int effectivePips)
{
    const float t = ShrineCaptureTime(effectivePips);
    return (t > 0.f) ? 1.f / t : 0.f;
}

} // namespace

void ReplayWindow::PrecomputeShrineTimeline()
{
    m_wurmsShrineSamples.clear();
    m_wurmsShrineCurrentSample = {};

    if (m_wurmsSouthShrineAgentId < 0) return;

    const auto& agents = m_replayCtx.agents;
    auto shrineIt = agents.find(m_wurmsSouthShrineAgentId);
    if (shrineIt == agents.end()) return;
    const AgentReplayData& shrine = shrineIt->second;
    if (shrine.snapshots.empty()) return;

    const float R2 = kWurmsShrineCaptureRadius * kWurmsShrineCaptureRadius;
    const InterpolationSettings& interp = m_replayCtx.interpSettings;
    const float matchEnd = std::max(1.f, m_replayCtx.maxReplayTime);
    constexpr float DT = 0.1f;

    auto countPlayers = [&](float tp, float& shX, float& shY, float& shZ)
        -> std::pair<int, int>
    {
        InterpolateAgentPosition(shrine, tp, interp, shX, shY, shZ);
        int b = 0, r = 0;
        for (int pid : m_playerIds)
        {
            auto pit = agents.find(pid);
            if (pit == agents.end()) continue;
            const AgentReplayData& ard = pit->second;
            if (ard.teamId != 1 && ard.teamId != 2) continue;
            if (ard.isDeadAtTime(tp) || !ard.isAliveAtTime(tp)) continue;
            float px, py, pz;
            InterpolateAgentPosition(ard, tp, interp, px, py, pz);
            const float dx = px - shX, dy = py - shY;
            if (dx * dx + dy * dy <= R2)
            {
                if (ard.teamId == 1) ++b; else ++r;
            }
        }
        return { b, r };
    };

    // ---- Forward simulation ----
    const int sampleCount = static_cast<int>(matchEnd / DT) + 2;
    m_wurmsShrineSamples.resize(sampleCount);

    int    owner        = 0;
    float  progress     = 0.f;
    uint8_t progressTeam = 0;
    bool   decapping    = false;
    size_t nextEvent    = 0;

    while (nextEvent < m_wurmsShrineCaptureEvents.size()
           && m_wurmsShrineCaptureEvents[nextEvent].first <= 1e-5f)
    {
        owner = m_wurmsShrineCaptureEvents[nextEvent].second;
        progress = 0.f; progressTeam = 0; decapping = false;
        ++nextEvent;
    }

    for (int si = 0; si < sampleCount; ++si)
    {
        const float t = si * DT;

        float shX, shY, shZ;
        auto [bRaw, rRaw] = countPlayers(t, shX, shY, shZ);
        const int bp  = std::min(bRaw, 4);
        const int rp  = std::min(rRaw, 4);
        const int eff = std::abs(bp - rp);
        const int dom = (bp > rp) ? 1 : (rp > bp) ? 2 : 0;
        const bool contested = (bp > 0 && rp > 0 && eff == 0);

        const float rate = ShrinePipRate(eff) * DT;

        if (eff > 0 && dom > 0)
        {
            if (owner == 0)
            {
                decapping = false;
                if (progressTeam == 0 || progress < 1e-5f)
                {
                    progressTeam = static_cast<uint8_t>(dom);
                    progress += rate;
                }
                else if (progressTeam == static_cast<uint8_t>(dom))
                {
                    progress += rate;
                }
                else
                {
                    progress -= rate;
                    if (progress <= 0.f)
                    {
                        progress = -progress;
                        progressTeam = static_cast<uint8_t>(dom);
                    }
                }
            }
            else
            {
                if (dom != owner)
                {
                    decapping = true;
                    progressTeam = static_cast<uint8_t>(owner);
                    progress += rate;
                }
                else if (decapping && progress > 1e-5f)
                {
                    progress -= rate;
                    if (progress <= 0.f)
                    {
                        progress = 0.f;
                        decapping = false;
                        progressTeam = 0;
                    }
                }
            }
        }

        ShrineState state;
        if (contested)
        {
            state = ShrineState::Contested;
        }
        else if (owner == 0)
        {
            if (progressTeam == 1 && progress > 0.001f)
                state = ShrineState::CapturingBlue;
            else if (progressTeam == 2 && progress > 0.001f)
                state = ShrineState::CapturingRed;
            else
                state = ShrineState::Neutral;
        }
        else
        {
            if (decapping && progress > 0.001f)
            {
                const int attacker = (owner == 1) ? 2 : 1;
                state = (attacker == 1) ? ShrineState::DecappingBlue
                                        : ShrineState::DecappingRed;
            }
            else
            {
                state = (owner == 1) ? ShrineState::OwnedByBlue
                                     : ShrineState::OwnedByRed;
            }
        }

        ShrineSample& s = m_wurmsShrineSamples[si];
        s.state         = state;
        s.ownerTeam     = static_cast<uint8_t>(owner);
        s.progressTeam  = progressTeam;
        s.bluePips      = bp;
        s.redPips       = rp;
        s.effectivePips = eff;
        s.progress      = progress;

        while (nextEvent < m_wurmsShrineCaptureEvents.size()
               && m_wurmsShrineCaptureEvents[nextEvent].first <= t + DT + 1e-5f)
        {
            owner = m_wurmsShrineCaptureEvents[nextEvent].second;
            progress = 0.f; progressTeam = 0; decapping = false;
            ++nextEvent;
        }
    }

    // ---- Normalization pass ----
    // Scale progress within each jumbo-bounded phase so it reaches 1.0
    // exactly at the jumbo timestamp.
    size_t phaseStartIdx = 0;
    for (const auto& [evTime, evTeam] : m_wurmsShrineCaptureEvents)
    {
        int evIdx = static_cast<int>(evTime / DT);
        if (evIdx >= sampleCount) evIdx = sampleCount - 1;
        if (evIdx < 0) evIdx = 0;

        const float rawAtEnd = m_wurmsShrineSamples[evIdx].progress;
        if (rawAtEnd > 0.001f)
        {
            for (size_t i = phaseStartIdx; i <= static_cast<size_t>(evIdx); ++i)
                m_wurmsShrineSamples[i].progress =
                    std::clamp(m_wurmsShrineSamples[i].progress / rawAtEnd, 0.f, 1.f);
        }
        phaseStartIdx = static_cast<size_t>(evIdx) + 1;
    }
    for (size_t i = phaseStartIdx; i < static_cast<size_t>(sampleCount); ++i)
        m_wurmsShrineSamples[i].progress = std::clamp(m_wurmsShrineSamples[i].progress, 0.f, 1.f);
}

void ReplayWindow::DrawWurmsShrineCaptureRadius()
{
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;
    if (!IsIsleOfWurmsMap(m_replayCtx.mapId)) return;
    if (m_wurmsSouthShrineAgentId < 0) return;
    if (m_wurmsShrineSamples.empty()) return;

    const int lastIdx = static_cast<int>(m_wurmsShrineSamples.size()) - 1;
    const float fIdx  = m_debugTimeline / m_wurmsShrineSampleDt;
    const int   idx0  = std::clamp(static_cast<int>(fIdx), 0, lastIdx);
    const int   idx1  = std::min(idx0 + 1, lastIdx);
    const float frac  = fIdx - static_cast<float>(idx0);

    m_wurmsShrineCurrentSample = m_wurmsShrineSamples[idx0];
    const float p0 = m_wurmsShrineSamples[idx0].progress;
    const float p1 = m_wurmsShrineSamples[idx1].progress;
    // Interpolate progress for smooth animation; skip across phase resets
    if (m_wurmsShrineSamples[idx0].ownerTeam == m_wurmsShrineSamples[idx1].ownerTeam
        && p1 >= p0 - 0.15f)
    {
        m_wurmsShrineCurrentSample.progress = p0 + (p1 - p0) * frac;
    }
    const ShrineSample& s = m_wurmsShrineCurrentSample;

    const int shrineId = m_wurmsSouthShrineAgentId;

    const bool showByFocus  = (m_hoveredAgentId == shrineId)
                           || (m_followedAgentId == shrineId);
    const bool showByOwned  = (s.state == ShrineState::OwnedByBlue)
                           || (s.state == ShrineState::OwnedByRed);
    const bool showByActive = (s.state == ShrineState::CapturingBlue)
                           || (s.state == ShrineState::CapturingRed)
                           || (s.state == ShrineState::DecappingBlue)
                           || (s.state == ShrineState::DecappingRed)
                           || (s.state == ShrineState::Contested);
    if (!showByFocus && !showByOwned && !showByActive) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float vpH = vp->Size.y;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const MapTransform& mt = m_replayCtx.mapTransform;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    constexpr int kSeg = 48;
    constexpr float kPI2 = 6.28318530718f;
    constexpr float kZOff = 2.f;
    constexpr float kFillAlpha = 0.14f;

    auto it = m_replayCtx.agents.find(shrineId);
    if (it == m_replayCtx.agents.end()) return;
    const AgentReplayData& srd = it->second;
    if (srd.snapshots.empty()) return;

    float sx, sy, sz;
    InterpolateAgentPosition(srd, m_debugTimeline, m_replayCtx.interpSettings,
                             sx, sy, sz);
    const float radius = kWurmsShrineCaptureRadius;

    auto projectPt = [&](float wx, float wy, ImVec2& out, bool& vis) {
        XMFLOAT3 mp = ApplyMapTransformToPos(wx, wy, sz, mt);
        if (terrain) mp.y = terrain->get_height_at(mp.x, mp.z) + kZOff;
        float scX, scY;
        vis = ProjectToScreen(viewProj, vpW, vpH, mp, scX, scY);
        out = ImVec2(scX, scY);
    };

    ImVec2 outerPts[kSeg];
    bool   outerVis[kSeg];
    int    outerCount = 0;
    for (int i = 0; i < kSeg; ++i)
    {
        const float ang = (float(i) / kSeg) * kPI2;
        projectPt(sx + cosf(ang) * radius, sy + sinf(ang) * radius,
                  outerPts[i], outerVis[i]);
        if (outerVis[i]) ++outerCount;
    }
    if (outerCount < 3) return;

    // ---- Drawing helpers ----
    auto drawFullFill = [&](ImU32 col) {
        ImVector<ImVec2> poly;
        for (int i = 0; i < kSeg; ++i)
            if (outerVis[i]) poly.push_back(outerPts[i]);
        if (poly.Size >= 3)
            dl->AddConvexPolyFilled(poly.Data, poly.Size, col);
    };

    auto drawPartialDisc = [&](float prog, ImU32 col) {
        const float rFill = radius * std::clamp(prog, 0.f, 1.f);
        ImVec2 cScr{};  bool cVis = false;
        {
            XMFLOAT3 mp = ApplyMapTransformToPos(sx, sy, sz, mt);
            if (terrain) mp.y = terrain->get_height_at(mp.x, mp.z) + kZOff;
            float cx, cy;
            cVis = ProjectToScreen(viewProj, vpW, vpH, mp, cx, cy);
            cScr = ImVec2(cx, cy);
        }
        ImVec2 fp[kSeg]; bool fv[kSeg];
        for (int i = 0; i < kSeg; ++i)
        {
            const float ang = (float(i) / kSeg) * kPI2;
            projectPt(sx + cosf(ang) * rFill, sy + sinf(ang) * rFill,
                      fp[i], fv[i]);
        }
        if (cVis)
        {
            for (int i = 0; i < kSeg; ++i)
            {
                const int j = (i + 1) % kSeg;
                if (fv[i] && fv[j])
                {
                    const ImVec2 tri[3] = { cScr, fp[i], fp[j] };
                    dl->AddConvexPolyFilled(tri, 3, col);
                }
            }
        }
        else
        {
            ImVector<ImVec2> poly;
            for (int i = 0; i < kSeg; ++i)
                if (fv[i]) poly.push_back(fp[i]);
            if (poly.Size >= 3)
                dl->AddConvexPolyFilled(poly.Data, poly.Size, col);
        }
    };

    auto drawAnnulus = [&](float prog, ImU32 col) {
        const float rIn = radius * std::clamp(prog, 0.f, 1.f);
        if (rIn < 1.f) { drawFullFill(col); return; }
        ImVec2 ip[kSeg]; bool iv[kSeg];
        for (int i = 0; i < kSeg; ++i)
        {
            const float ang = (float(i) / kSeg) * kPI2;
            projectPt(sx + cosf(ang) * rIn, sy + sinf(ang) * rIn,
                      ip[i], iv[i]);
        }
        for (int i = 0; i < kSeg; ++i)
        {
            const int j = (i + 1) % kSeg;
            if (!outerVis[i] || !outerVis[j] || !iv[i] || !iv[j]) continue;
            const ImVec2 q[4] = { ip[i], ip[j], outerPts[j], outerPts[i] };
            dl->AddConvexPolyFilled(q, 4, col);
        }
    };

    auto drawEdge = [&](ImU32 col, float thick = 1.6f) {
        for (int i = 0; i < kSeg; ++i)
        {
            const int j = (i + 1) % kSeg;
            if (outerVis[i] && outerVis[j])
                dl->AddLine(outerPts[i], outerPts[j], col, thick);
        }
    };

    auto drawContestedEdge = [&]() {
        const float pulse = 0.45f + 0.55f * sinf((float)ImGui::GetTime() * 3.2f);
        const ImU32 amber = IM_COL32(255, 200, 80, (ImU8)(pulse * 200.f));
        constexpr float dashOn = 10.f, dashOff = 8.f;
        const float cycle = dashOn + dashOff;
        float accum = 0.f;
        for (int i = 0; i < kSeg; ++i)
        {
            const int j = (i + 1) % kSeg;
            if (!outerVis[i] || !outerVis[j]) { accum = 0.f; continue; }
            const float dx = outerPts[j].x - outerPts[i].x;
            const float dy = outerPts[j].y - outerPts[i].y;
            if (fmodf(accum, cycle) < dashOn)
                dl->AddLine(outerPts[i], outerPts[j], amber, 2.2f);
            accum += sqrtf(dx * dx + dy * dy);
        }
    };

    // ---- Render by state ----
    const ImU32 greyEdge = IM_COL32(160, 160, 160, 220);
    const ImU32 greyFill = IM_COL32(140, 140, 140, 55);

    switch (s.state)
    {
    case ShrineState::Neutral:
        drawFullFill(greyFill);
        drawEdge(greyEdge, 1.5f);
        break;

    case ShrineState::OwnedByBlue:
    case ShrineState::OwnedByRed: {
        const ImU32 rgb  = GetAgentTeamColor(s.ownerTeam);
        const ImU32 fill = (rgb & 0x00FFFFFF) | ((ImU32)(kFillAlpha * 255.f) << 24);
        const ImU32 edge = (rgb & 0x00FFFFFF) | (0xD0u << 24);
        drawFullFill(fill);
        drawEdge(edge);
        break;
    }

    case ShrineState::CapturingBlue:
    case ShrineState::CapturingRed: {
        const int   capTeam = (s.state == ShrineState::CapturingBlue) ? 1 : 2;
        const ImU32 rgb  = GetAgentTeamColor(capTeam);
        const ImU32 fill = (rgb & 0x00FFFFFF) | ((ImU32)(kFillAlpha * 255.f) << 24);
        const ImU32 edge = (rgb & 0x00FFFFFF) | (0xD0u << 24);
        if (s.progress > 0.001f)
            drawPartialDisc(s.progress, fill);
        drawEdge(edge);
        break;
    }

    case ShrineState::DecappingBlue:
    case ShrineState::DecappingRed: {
        const int   actTeam = (s.state == ShrineState::DecappingBlue) ? 1 : 2;
        const ImU32 ownerRgb  = GetAgentTeamColor(s.ownerTeam);
        const ImU32 actRgb    = GetAgentTeamColor(actTeam);
        const ImU32 fill = (ownerRgb & 0x00FFFFFF) | ((ImU32)(kFillAlpha * 255.f) << 24);
        const ImU32 edge = (actRgb   & 0x00FFFFFF) | (0xD0u << 24);
        drawAnnulus(s.progress, fill);
        drawEdge(edge);
        break;
    }

    case ShrineState::Contested: {
        if (s.ownerTeam == 0 && s.progressTeam > 0 && s.progress > 0.001f)
        {
            const ImU32 rgb = GetAgentTeamColor(s.progressTeam);
            drawPartialDisc(s.progress,
                            (rgb & 0x00FFFFFF) | ((ImU32)(kFillAlpha * 255.f) << 24));
        }
        else if (s.ownerTeam > 0)
        {
            const ImU32 rgb = GetAgentTeamColor(s.ownerTeam);
            const ImU32 fill = (rgb & 0x00FFFFFF) | ((ImU32)(kFillAlpha * 255.f) << 24);
            if (s.progress > 0.001f)
                drawAnnulus(s.progress, fill);
            else
                drawFullFill(fill);
        }
        else
        {
            drawFullFill(IM_COL32(90, 85, 70, 45));
        }
        drawContestedEdge();
        break;
    }
    }

    // Ownership beam: visible when shrine is owned (persists during decap)
    if (s.ownerTeam > 0)
        DrawShrineBeam(s.ownerTeam, sx, sy, sz);
}

// ---------------------------------------------------------------------------
// Isle of Wurms — Shrine ownership beam
// ---------------------------------------------------------------------------

namespace {

struct AdditiveBlendCBData {
    ID3D11DeviceContext* ctx;
    ID3D11BlendState*    bs;
};

static void BeamSetAdditiveBlend(const ImDrawList*, const ImDrawCmd* cmd)
{
    auto* d = static_cast<AdditiveBlendCBData*>(cmd->UserCallbackData);
    const float f[4] = { 0, 0, 0, 0 };
    d->ctx->OMSetBlendState(d->bs, f, 0xFFFFFFFF);
}

constexpr float kBeamBaseHalfWidth = 30.f;
constexpr float kBeamTopHalfWidth  = 6.f;
constexpr float kBeamHeight        = 1100.f;
constexpr int   kBeamSubdivisions  = 12;
constexpr float kHaloRadius        = 35.f;
constexpr int   kHaloSegments      = 24;

struct BeamLayerDef {
    float widthScale;
    ImU32 edgeColor;
    ImU32 centerColor;
};

} // namespace

void ReplayWindow::DrawShrineBeam(uint8_t ownerTeam, float sx, float sy, float sz)
{
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float vpH = vp->Size.y;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const MapTransform& mt = m_replayCtx.mapTransform;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    XMFLOAT3 baseWorld = ApplyMapTransformToPos(sx, sy, sz, mt);
    if (terrain)
        baseWorld.y = terrain->get_height_at(baseWorld.x, baseWorld.z);

    XMFLOAT3 look = cam->GetLook3f();
    float rxz = sqrtf(look.x * look.x + look.z * look.z);
    XMFLOAT3 right;
    if (rxz > 0.001f)
    {
        right = { -look.z / rxz, 0.f, look.x / rxz };
    }
    else
    {
        XMFLOAT3 r3 = cam->GetRight3f();
        float rlen = sqrtf(r3.x * r3.x + r3.z * r3.z);
        right = (rlen > 0.001f)
            ? XMFLOAT3{ r3.x / rlen, 0.f, r3.z / rlen }
            : XMFLOAT3{ 1.f, 0.f, 0.f };
    }

    float baseHW = kBeamBaseHalfWidth * mt.scaleX;
    float topHW  = kBeamTopHalfWidth  * mt.scaleX;
    float height = kBeamHeight        * mt.scaleY;

    XMFLOAT3 topWorld = { baseWorld.x, baseWorld.y + height, baseWorld.z };

    auto projectWorld = [&](XMFLOAT3 w, ImVec2& out) -> bool {
        float scX, scY;
        bool vis = ProjectToScreen(viewProj, vpW, vpH, w, scX, scY);
        out = ImVec2(scX, scY);
        return vis;
    };

    BeamLayerDef layers[3];
    if (ownerTeam == 1) // Red
    {
        layers[0] = { 1.00f, IM_COL32(0xB0, 0x20, 0x20, 166), IM_COL32(0xE0, 0x30, 0x30, 166) };
        layers[1] = { 0.75f, IM_COL32(0xF0, 0x60, 0x40, 230), IM_COL32(0xFF, 0x80, 0x60, 230) };
        layers[2] = { 0.35f, IM_COL32(0xFF, 0xDD, 0xD0, 255), IM_COL32(0xFF, 0xFF, 0xFF, 255) };
    }
    else // Blue
    {
        layers[0] = { 1.00f, IM_COL32(0x20, 0x60, 0xC0, 166), IM_COL32(0x40, 0x90, 0xF0, 166) };
        layers[1] = { 0.75f, IM_COL32(0x6A, 0xB8, 0xFF, 230), IM_COL32(0x88, 0xCC, 0xFF, 230) };
        layers[2] = { 0.35f, IM_COL32(0xDD, 0xF0, 0xFF, 255), IM_COL32(0xFF, 0xFF, 0xFF, 255) };
    }

    // ---- Switch to additive blending ----
    static AdditiveBlendCBData cbData;
    cbData.ctx = m_deviceResources->GetD3DDeviceContext();
    cbData.bs  = m_additiveBS.Get();
    dl->AddCallback(BeamSetAdditiveBlend, &cbData);

    // ---- Floor halo (ellipse at shrine base) ----
    {
        float haloR = kHaloRadius * mt.scaleX;
        ImU32 haloCenterCol = (ownerTeam == 1)
            ? IM_COL32(0x3A, 0x80, 0xE0, 140)
            : IM_COL32(0xE0, 0x30, 0x20, 140);
        ImU32 haloEdgeCol = (ownerTeam == 1)
            ? IM_COL32(0x3A, 0x80, 0xE0, 0)
            : IM_COL32(0xE0, 0x30, 0x20, 0);

        ImVec2 centerScr;
        bool centerVis = projectWorld(baseWorld, centerScr);

        if (centerVis)
        {
            constexpr float kPI2 = 6.28318530718f;
            ImVec2 ringPts[kHaloSegments];
            bool   ringVis[kHaloSegments];
            for (int i = 0; i < kHaloSegments; ++i)
            {
                float ang = (float(i) / kHaloSegments) * kPI2;
                XMFLOAT3 hp = {
                    baseWorld.x + cosf(ang) * haloR * right.x + sinf(ang) * haloR * 0.25f * 0.f,
                    baseWorld.y,
                    baseWorld.z + cosf(ang) * haloR * right.z + sinf(ang) * haloR * 0.25f * 0.f
                };
                float fwdX = look.x, fwdZ = look.z;
                if (rxz > 0.001f) { fwdX /= rxz; fwdZ /= rxz; }
                hp.x = baseWorld.x + cosf(ang) * haloR * right.x + sinf(ang) * haloR * 0.25f * fwdX;
                hp.z = baseWorld.z + cosf(ang) * haloR * right.z + sinf(ang) * haloR * 0.25f * fwdZ;
                ringVis[i] = projectWorld(hp, ringPts[i]);
            }
            for (int i = 0; i < kHaloSegments; ++i)
            {
                int j = (i + 1) % kHaloSegments;
                if (!ringVis[i] || !ringVis[j]) continue;
                dl->AddTriangleFilled(centerScr, ringPts[i], ringPts[j], haloCenterCol);
            }
            for (int i = 0; i < kHaloSegments; ++i)
            {
                int j = (i + 1) % kHaloSegments;
                if (!ringVis[i] || !ringVis[j]) continue;
                ImVec2 midI = ImVec2((centerScr.x + ringPts[i].x) * 0.5f,
                                     (centerScr.y + ringPts[i].y) * 0.5f);
                ImVec2 midJ = ImVec2((centerScr.x + ringPts[j].x) * 0.5f,
                                     (centerScr.y + ringPts[j].y) * 0.5f);
                const ImVec2 outerQ[4] = { midI, midJ, ringPts[j], ringPts[i] };
                dl->AddConvexPolyFilled(outerQ, 4, haloEdgeCol);
            }
        }
    }

    // ---- Beam layers ----
    for (int li = 0; li < 3; ++li)
    {
        const BeamLayerDef& L = layers[li];
        float bHW = baseHW * L.widthScale;
        float tHW = topHW  * L.widthScale;
        const int N = kBeamSubdivisions;

        for (int i = 0; i < N; ++i)
        {
            float t0 = (float(i) / N) * 2.f - 1.f;
            float t1 = (float(i + 1) / N) * 2.f - 1.f;

            XMFLOAT3 bl = {
                baseWorld.x + right.x * bHW * t0,
                baseWorld.y,
                baseWorld.z + right.z * bHW * t0
            };
            XMFLOAT3 br = {
                baseWorld.x + right.x * bHW * t1,
                baseWorld.y,
                baseWorld.z + right.z * bHW * t1
            };
            XMFLOAT3 tl = {
                topWorld.x + right.x * tHW * t0,
                topWorld.y,
                topWorld.z + right.z * tHW * t0
            };
            XMFLOAT3 tr = {
                topWorld.x + right.x * tHW * t1,
                topWorld.y,
                topWorld.z + right.z * tHW * t1
            };

            ImVec2 sbl, sbr, stl, str;
            bool vbl = projectWorld(bl, sbl);
            bool vbr = projectWorld(br, sbr);
            bool vtl = projectWorld(tl, stl);
            bool vtr = projectWorld(tr, str);
            if (!vbl && !vbr && !vtl && !vtr) continue;

            float d0 = fabsf(t0);
            float d1 = fabsf(t1);
            auto lerpCol = [](ImU32 edge, ImU32 center, float dist) -> ImU32 {
                float f = 1.f - dist;
                int re = (edge >> 0) & 0xFF,  ge = (edge >> 8) & 0xFF,  be = (edge >> 16) & 0xFF,  ae = (edge >> 24) & 0xFF;
                int rc = (center >> 0) & 0xFF, gc = (center >> 8) & 0xFF, bc = (center >> 16) & 0xFF, ac = (center >> 24) & 0xFF;
                int ro = re + (int)((rc - re) * f);
                int go = ge + (int)((gc - ge) * f);
                int bo = be + (int)((bc - be) * f);
                int ao = ae + (int)((ac - ae) * f);
                return IM_COL32(ro, go, bo, ao);
            };

            ImU32 c0 = lerpCol(L.edgeColor, L.centerColor, d0);
            ImU32 c1 = lerpCol(L.edgeColor, L.centerColor, d1);

            ImVec2 uv = dl->_Data->TexUvWhitePixel;
            dl->PrimReserve(6, 4);
            ImDrawIdx idx = (ImDrawIdx)dl->_VtxCurrentIdx;
            dl->_VtxWritePtr[0] = { stl, uv, c0 };
            dl->_VtxWritePtr[1] = { str, uv, c1 };
            dl->_VtxWritePtr[2] = { sbr, uv, c1 };
            dl->_VtxWritePtr[3] = { sbl, uv, c0 };
            dl->_VtxWritePtr += 4;
            dl->_IdxWritePtr[0] = idx;     dl->_IdxWritePtr[1] = (ImDrawIdx)(idx + 1); dl->_IdxWritePtr[2] = (ImDrawIdx)(idx + 2);
            dl->_IdxWritePtr[3] = idx;     dl->_IdxWritePtr[4] = (ImDrawIdx)(idx + 2); dl->_IdxWritePtr[5] = (ImDrawIdx)(idx + 3);
            dl->_IdxWritePtr += 6;
            dl->_VtxCurrentIdx += 4;
        }
    }

    // ---- Restore normal blend state ----
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}
