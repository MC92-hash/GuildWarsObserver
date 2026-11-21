#include "pch.h"
#include "TeamColors.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "RitualistAshes.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "RunLog.h"
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


// ---------------------------------------------------------------------------
// Flag Timeline — delegates to FlagTimelineBuilder
// ---------------------------------------------------------------------------

void ReplayWindow::BuildFlagTimeline()
{
    FlagTimelineBuilder::Input input;
    input.flagEvents = &m_replayCtx.stocData.flagEvents;
    input.lifecycle  = &m_replayCtx.stocData.lifecycle;
    input.mapObject  = &m_replayCtx.stocData.mapObject;
    input.doorEvents = &m_replayCtx.stocData.doorEvents;
    input.agents     = &m_replayCtx.agents;
    input.skills     = &m_replayCtx.stocData.skill;
    input.flagItems  = &m_flagItems;
    input.mapId      = m_replayCtx.mapId;
    m_flagTimeline = FlagTimelineBuilder::Build(input);
    m_flagTimelineBuilt = true;

    if (!m_flagTimeline.bundles.empty())
    {
        std::ofstream dbg("door_debug.log", std::ios::app);
        for (int ti = 0; ti < 2; ti++)
        {
            auto& ft = m_flagTimeline.teams[ti];
            dbg << "[FlagTeam] " << ti
                << " spawn=(" << ft.spawnX << "," << ft.spawnY << "," << ft.spawnZ << ")"
                << " events=" << ft.events.size() << "\n";
            for (auto& ev : ft.events)
                dbg << "  t=" << ev.time
                    << " -> " << FlagLocationName(ev.newLocation)
                    << " actor=" << ev.actorAgentId
                    << " carrier=" << ev.carrierAgentId
                    << " pos=(" << ev.x << "," << ev.y << "," << ev.z << ")\n";
        }
        for (auto& bundle : m_flagTimeline.bundles)
        {
            dbg << "[Bundle] item=" << bundle.itemId
                << " type=" << static_cast<int>(bundle.type)
                << " spawnResolved=" << (bundle.spawnResolved ? 1 : 0)
                << " spawn=(" << bundle.spawnX << "," << bundle.spawnY << "," << bundle.spawnZ << ")"
                << " events=" << bundle.events.size() << "\n";
            // The world agents the timeline was derived from, so a wrong timeline can
            // be traced back to the raw spans rather than guessed at.
            for (auto& [aid, ard] : m_replayCtx.agents) {
                if (ard.snapshots.empty()) continue;
                if (static_cast<int>(ard.snapshots.front().item_id) != bundle.itemId) continue;
                dbg << "  [span] agent=" << aid
                    << " type=" << static_cast<int>(ard.type)
                    << " orig=" << ard.originalAgentId
                    << " snaps=" << ard.snapshots.size()
                    << " " << ard.snapshots.front().time << ".." << ard.snapshots.back().time
                    << " at=(" << ard.snapshots.front().x << "," << ard.snapshots.front().y
                    << "," << ard.snapshots.front().z << ")\n";
            }
            for (auto& ev : bundle.events)
            {
                static const char* kLoc[] = { "Base", "Carried", "Ground", "Consumed" };
                dbg << "  t=" << ev.time
                    << " -> " << kLoc[static_cast<int>(ev.newLocation)]
                    << " actor=" << ev.actorAgentId
                    << " carrier=" << ev.carrierAgentId
                    << " world=" << ev.worldAgentId
                    << " pos=(" << ev.x << "," << ev.y << "," << ev.z << ")\n";
            }
        }
    }

    BuildFlagMessages();
    BuildGroundItemSpans();
}


// ---------------------------------------------------------------------------
// [FLAG-2] The carryables lying in the world: one span per Base / Ground state of
// each team's flag and of each vine seed / repair kit, to its next event or the
// match end.
// ---------------------------------------------------------------------------

namespace
{
// The models a lying carryable is drawn as (the same files the carrier holds).
constexpr uint32_t kGroundFlagModel      = 94192;
constexpr uint32_t kGroundRepairKitModel = 31976;
constexpr uint32_t kGroundVineSeedModel  = 94207;
// The item view's ground segment: Gw.exe 0x7FE1B0 plays it on every item it puts on the ground
// (PlaySequence(model, 0x303E5306, 3) at 0x7FE2A3).
constexpr uint32_t kGroundItemSegment    = 0x303E5306u;
}

void ReplayWindow::BuildGroundItemSpans()
{
    m_groundItemSpans.clear();
    if (!m_flagTimeline.valid) return;

    // The flag's team colour as the client's old packed word: red 0xE038 (the identity), blue 0xE9A0.
    // The FLAG_ITEM word of the lying item wins (ids are recycled between the teams); the team's own
    // colour stands in when the item is not known.
    constexpr uint16_t kRedWord  = 0xE038;
    constexpr uint16_t kBlueWord = 0xE9A0;

    for (int ti = 0; ti < 2; ti++)
    {
        const auto& ft = m_flagTimeline.teams[ti];
        const uint16_t teamWord = Team::IndexIsRed(ti) ? kRedWord : kBlueWord;

        // Before its first event the flag is at its base (locationAtTime's default), with no Base event
        // of its own when the first thing that happens to it is a pickup.
        if (!ft.events.empty() && ft.events.front().time > 0.f &&
            ft.events.front().newLocation != FlagLocation::Base)
        {
            GroundItemSpan span;
            span.owner = ti;
            span.modelFileId = kGroundFlagModel;
            span.start = 0.f;
            span.end = ft.events.front().time;
            span.x = ft.spawnX; span.y = ft.spawnY; span.z = ft.spawnZ;
            span.colourWord = teamWord;
            m_groundItemSpans.push_back(span);
        }

        for (size_t i = 0; i < ft.events.size(); i++)
        {
            const FlagTimelineEvent& ev = ft.events[i];
            if (ev.newLocation != FlagLocation::Base && ev.newLocation != FlagLocation::Ground)
                continue;

            GroundItemSpan span;
            span.owner = ti;
            span.modelFileId = kGroundFlagModel;
            span.start = ev.time;
            span.end   = (i + 1 < ft.events.size()) ? ft.events[i + 1].time
                                                    : std::max(m_replayCtx.maxReplayTime, ev.time);
            if (span.end <= span.start) continue;
            ft.positionAtTime(ev.time, span.x, span.y, span.z);
            span.worldAgentId = ev.flagWorldAgentId;

            if (ev.flagWorldAgentId >= 0)
            {
                auto it = m_replayCtx.agents.find(ev.flagWorldAgentId);
                if (it != m_replayCtx.agents.end() && !it->second.snapshots.empty())
                    span.colourWord = m_flagItems.ColourWordAt(it->second.snapshots.front().item_id, ev.time);
            }
            if (span.colourWord == 0)
                span.colourWord = teamWord;

            m_groundItemSpans.push_back(span);
        }
    }

    // Vine seeds and repair kits: at their spawn (when a world agent revealed it) and dropped.
    for (size_t b = 0; b < m_flagTimeline.bundles.size() && 2 + b < kGroundItemOwners; b++)
    {
        const BundleTimeline& bundle = m_flagTimeline.bundles[b];
        const uint32_t model = bundle.type == BundleType::VineSeed  ? kGroundVineSeedModel
                             : bundle.type == BundleType::RepairKit ? kGroundRepairKitModel : 0u;
        if (model == 0) continue;

        auto addSpan = [&](float start, float end, float x, float y, float z, int worldAgent) {
            if (end <= start) return;
            GroundItemSpan span;
            span.owner = 2 + static_cast<int>(b);
            span.modelFileId = model;
            span.start = start; span.end = end;
            span.x = x; span.y = y; span.z = z;
            span.worldAgentId = worldAgent;
            m_groundItemSpans.push_back(span);
        };

        // Untouched at its spawn until its first event (locationAtTime's default).
        if (bundle.spawnResolved && !bundle.events.empty() && bundle.events.front().time > 0.f &&
            bundle.events.front().newLocation != BundleLocation::Base)
            addSpan(0.f, bundle.events.front().time, bundle.spawnX, bundle.spawnY, bundle.spawnZ, -1);

        for (size_t i = 0; i < bundle.events.size(); i++)
        {
            const BundleTimelineEvent& ev = bundle.events[i];
            if (ev.newLocation != BundleLocation::Ground &&
                !(ev.newLocation == BundleLocation::Base && bundle.spawnResolved))
                continue;
            const float end = (i + 1 < bundle.events.size()) ? bundle.events[i + 1].time
                                                             : std::max(m_replayCtx.maxReplayTime, ev.time);
            float x = 0.f, y = 0.f, z = 0.f;
            bundle.positionAtTime(ev.time, x, y, z);
            // The recording first sees a starting item some seconds in (13-25 s), but it lies at its spawn
            // from the start, as the icon already says: its first Base span opens at 0.
            const float start = (i == 0 && ev.newLocation == BundleLocation::Base) ? 0.f : ev.time;
            addSpan(start, end, x, y, z, ev.worldAgentId);
        }
    }

    std::ofstream dbg("door_debug.log", std::ios::app);
    dbg << "[GroundItem] " << m_groundItemSpans.size() << " span(s)\n";
    for (const auto& s : m_groundItemSpans)
        dbg << "  owner=" << s.owner << " model=" << s.modelFileId << " " << s.start << ".." << s.end
            << " pos=(" << s.x << "," << s.y << "," << s.z << ")"
            << " word=0x" << std::hex << s.colourWord << std::dec << " agent=" << s.worldAgentId << "\n";
}

const ReplayWindow::GroundItemSpan* ReplayWindow::GroundItemSpanAt(int owner, float t) const
{
    for (const auto& s : m_groundItemSpans)
        if (s.owner == owner && t >= s.start && t < s.end)
            return &s;
    return nullptr;
}

XMFLOAT3 ReplayWindow::GroundItemRenderPos(const GroundItemSpan& span) const
{
    XMFLOAT3 at = ApplyMapTransformToPos(span.x, span.y, span.z, m_replayCtx.mapTransform);
    // An item's AGENT_ADD records no height (0.0): the terrain under it, then.
    float ground = 0.f;
    if (span.z == 0.f && m_mapRenderer && m_mapRenderer->TryGetGroundHeight(at.x, at.z, ground))
        at.y = ground;
    return at;
}

bool ReplayWindow::GroundItemVisible(const GroundItemSpan& span, float& alpha) const
{
    alpha = 1.f;
    // An enemy flag you have no vision on would not be on your compass; your own is never hidden.
    // A bundle belongs to no team: it is down to whether the perspective sees the spot.
    const bool ownFlag = span.owner < 2 && Team::FromIndex01(span.owner) == m_fogPerspective;
    if (m_fogPerspective > 0 && !ownFlag && IsPositionInFog(span.x, span.y))
    {
        if (!m_fogGhostMode) return false;
        alpha = 0.3f;
    }
    return true;
}

bool ReplayWindow::GroundItemClose(const XMFLOAT3& at) const
{
    if (!m_uiLayout.lodEnabled || !m_mapRenderer) return true;
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return true;
    const XMFLOAT3 c = cam->GetPosition3f();
    const float dx = at.x - c.x, dy = at.y - c.y, dz = at.z - c.z;
    return dx * dx + dy * dy + dz * dz <= m_uiLayout.lodDotDist * m_uiLayout.lodDotDist;
}

const XMFLOAT4X4& ReplayWindow::GroundItemPose(uint32_t modelFileId)
{
    auto found = m_groundItemPoses.find(modelFileId);
    if (found != m_groundItemPoses.end()) return found->second;

    XMFLOAT4X4& pose = m_groundItemPoses[modelFileId];
    XMStoreFloat4x4(&pose, XMMatrixIdentity());
    if (!m_datManager || !m_hashIndex) return pose;
    auto it = m_hashIndex->find(static_cast<int>(modelFileId));
    if (it == m_hashIndex->end() || it->second.empty()) return pose;
    const auto bytes = m_datManager->read_file_cached(static_cast<uint32_t>(it->second.at(0)));
    if (!bytes || bytes->empty()) return pose;
    auto parsed = GW::Parsers::ParseAnimationFromFile(bytes->data(), bytes->size());
    if (!parsed || !parsed->IsValid())
    {
        RunLog::Line("ground items: model %u has no readable clip - it lies as authored", modelFileId);
        return pose;
    }
    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*parsed));
    clip->BuildAnimationGroups();
    size_t segment = SIZE_MAX;
    for (size_t i = 0; i < clip->animationSegments.size(); i++)
        if (clip->animationSegments[i].hash == kGroundItemSegment)
            segment = i;
    if (segment == SIZE_MAX)
    {
        RunLog::Line("ground items: model %u has no segment 0x%08X - it lies as authored", modelFileId,
                     kGroundItemSegment);
        return pose;
    }
    GW::Animation::AnimationController controller;
    controller.Initialize(clip);
    controller.SetSegment(segment);
    const auto& bones = controller.GetBoneMatrices();
    if (!bones.empty()) pose = bones[0];
    RunLog::Line("ground items: model %u stands on segment 0x%08X (%zu bone(s))", modelFileId,
                 kGroundItemSegment, bones.size());
    return pose;
}

int ReplayWindow::GroundItemAgent(const GroundItemSpan& span, float t) const
{
    auto alive = [&](int id) {
        auto it = m_replayCtx.agents.find(id);
        return it != m_replayCtx.agents.end() && !it->second.snapshots.empty();
    };
    if (span.worldAgentId >= 0 && alive(span.worldAgentId)) return span.worldAgentId;

    // Otherwise the flag or bundle agent standing nearest the spot at that moment.
    int best = -1;
    float bestD = 300.f * 300.f;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty() || (ard.type != AgentType::Flag && ard.type != AgentType::Item)) continue;
        if (t < ard.snapshots.front().time - 1.f || t > ard.snapshots.back().time + 1.f) continue;
        const auto& s = ard.snapshots.front();
        const float d = (s.x - span.x) * (s.x - span.x) + (s.y - span.y) * (s.y - span.y);
        if (d < bestD) { bestD = d; best = id; }
    }
    return best;
}

bool ReplayWindow::GroundItemAgentLying(int agent, float t) const
{
    if (agent < 0 || !m_flagTimelineBuilt) return false;
    for (int owner = 0; owner < kGroundItemOwners; owner++)
        if (const GroundItemSpan* span = GroundItemSpanAt(owner, t))
            if (GroundItemAgent(*span, t) == agent) return true;
    return false;
}

// The vine seeds and repair kits lying in the world, as their own models on the ground pose: the stand-in
// templates the carriers' hands use (no dye to compose). The flags are drawn by DrawGroundFlagModels.
void ReplayWindow::DrawGroundItemModels(bool secondaryView)
{
    bool* drawn = secondaryView ? m_groundItemModelDrawnPip : m_groundItemModelDrawn;
    std::fill(drawn, drawn + kGroundItemOwners, false);
    DrawGroundFlagModels(secondaryView);
    if (!m_flagTimelineBuilt || m_groundItemSpans.empty() || !m_weaponModelsLoaded) return;

    auto* context = m_deviceResources->GetD3DDeviceContext();
    auto* meshManager = m_mapRenderer->GetMeshManager();
    if (!context || !meshManager) return;

    bool shadersBound = false;
    int boundPixelShader = -1;
    for (int owner = 2; owner < kGroundItemOwners; owner++)
    {
        const GroundItemSpan* span = GroundItemSpanAt(owner, m_debugTimeline);
        if (!span) continue;
        float alpha = 1.f;
        if (!GroundItemVisible(*span, alpha)) continue;
        const XMFLOAT3 at = GroundItemRenderPos(*span);
        if (!GroundItemClose(at)) continue;
        auto tmplIt = m_weaponModelTemplates.find(span->modelFileId);
        if (tmplIt == m_weaponModelTemplates.end() || !tmplIt->second.gpuReady) continue;
        WeaponModelTemplate& tmpl = tmplIt->second;

        const XMMATRIX world = XMLoadFloat4x4(&GroundItemPose(span->modelFileId)) *
                               XMMatrixTranslation(at.x, at.y, at.z);
        if (!shadersBound)
        {
            m_mapRenderer->BindRegularVertexShader();
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            shadersBound = true;
        }
        const int wantPixelShader = (tmpl.pixelShaderType == PixelShaderType::NewModel) ? 1 : 0;
        if (wantPixelShader != boundPixelShader)
        {
            m_mapRenderer->BindModelPixelShader(wantPixelShader == 1);
            boundPixelShader = wantPixelShader;
        }
        for (size_t k = 0; k < tmpl.gpuMeshes.size(); k++)
        {
            if (!tmpl.gpuMeshes[k]) continue;
            PerObjectCB cb = tmpl.templateCBs[k];
            XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
            cb.mesh_alpha = alpha;
            meshManager->SetPerObjectCB(cb);
            tmpl.gpuMeshes[k]->Draw(context, m_mapRenderer->GetLODQuality());
        }
        drawn[owner] = true;
    }
}


// ---------------------------------------------------------------------------
// Build display messages from flag timeline events
// ---------------------------------------------------------------------------

void ReplayWindow::BuildFlagMessages()
{
    m_flagMessages.clear();
    if (!m_flagTimeline.valid) return;

    for (auto& ev : m_flagTimeline.allEvents)
    {
        if (ev.eventType == FlagTimelineEventType::Spawn ||
            ev.eventType == FlagTimelineEventType::GroundSpawn)
            continue;

        FlagEventMessage msg;
        msg.time         = ev.time;
        msg.flagTeam     = static_cast<int>(ev.flagTeam);
        msg.eventType    = ev.eventType;
        msg.standAgentId = ev.standAgentId;

        int actorId = ev.actorAgentId;
        if (actorId >= 0) {
            auto it = m_replayCtx.agents.find(actorId);
            if (it != m_replayCtx.agents.end()) {
                msg.playerName = it->second.playerName.empty()
                    ? it->second.categoryName : it->second.playerName;
                msg.playerTeam = Team::Index01(it->second.teamId);
            }
        }

        if (msg.playerName.empty() && ev.eventType != FlagTimelineEventType::Return)
            continue;

        m_flagMessages.push_back(std::move(msg));
    }

    // Repair kits and vine seeds are announced the same way. A drop only names the
    // player in the carry it ends, so the timeline is walked in order rather than
    // event by event.
    for (auto& bundle : m_flagTimeline.bundles)
    {
        int carrier = -1;
        for (auto& ev : bundle.events)
        {
            int actorId = -1;
            FlagTimelineEventType kind = FlagTimelineEventType::Pickup;

            if (ev.newLocation == BundleLocation::Carried) {
                actorId = ev.carrierAgentId;
                carrier = actorId;
            } else if (ev.newLocation == BundleLocation::Ground) {
                actorId = carrier;
                kind    = FlagTimelineEventType::Drop;
                carrier = -1;
            } else {
                carrier = -1;
                continue;
            }
            if (actorId < 0) continue;

            auto it = m_replayCtx.agents.find(actorId);
            if (it == m_replayCtx.agents.end()) continue;

            FlagEventMessage msg;
            msg.time       = ev.time;
            msg.eventType  = kind;
            msg.bundleType = bundle.type;
            msg.playerName = it->second.playerName.empty()
                ? it->second.categoryName : it->second.playerName;
            msg.playerTeam = Team::Index01(it->second.teamId);
            if (msg.playerName.empty()) continue;

            m_flagMessages.push_back(std::move(msg));
        }
    }

    std::sort(m_flagMessages.begin(), m_flagMessages.end(),
        [](const FlagEventMessage& a, const FlagEventMessage& b) { return a.time < b.time; });
}


// Forward declarations for functions defined later in this file

// ---------------------------------------------------------------------------
// Flag rendering — draw team-colored flag icons from FlagTimeline
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFlags()
{
    if (!m_flagTimelineBuilt || !m_flagTimeline.valid) return;
    if (!m_agentsClassified) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;
    const MapTransform& t = m_replayCtx.mapTransform;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texRed  = LoadFlagIcon(dev, "Red_flag_waving.svg.png");
    ImTextureID texBlue = LoadFlagIcon(dev, "Blue_flag_waving.svg.png");
    const float iconSz = std::clamp(vpH * 0.035f, 18.f, 32.f);

    // Fog of war: a marker the chosen perspective has no vision on is dropped
    // outright, or dimmed to the same 0.3 the 3D agent models fade to when
    // ghost mode is on.
    constexpr float kFogGhostAlpha = 0.3f;

    auto DrawFlagAt = [&](float worldX, float worldY, float worldZ,
                          ImTextureID tex, int teamIdx, const char* label,
                          float alphaMul, bool marker)
    {
        XMFLOAT3 pos = ApplyMapTransformToPos(worldX, worldY, worldZ, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) return;

        // The dot and icon stand in for a flag drawn as nothing else (`marker`).
        constexpr float kFlagDotRadius = 5.f;
        if (marker)
        {
            ImU32 dotColor = Team::IndexIsRed(teamIdx) ? IM_COL32(255, 100, 90, 200)
                                                       : IM_COL32(100, 160, 255, 200);
            dl->AddCircleFilled(ImVec2(scrX, scrY), kFlagDotRadius,
                                ScaleColorAlpha(dotColor, alphaMul));
            dl->AddCircle(ImVec2(scrX, scrY), kFlagDotRadius,
                          ScaleColorAlpha(IM_COL32(0, 0, 0, 180), alphaMul), 0, 1.5f);

            if (tex)
            {
                float offsetY = iconSz * 0.8f;
                ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - offsetY - iconSz);
                ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
                dl->AddImage(tex, iconTL, iconBR, ImVec2(0, 0), ImVec2(1, 1),
                             ScaleColorAlpha(IM_COL32_WHITE, alphaMul));
            }
        }

        if (label)
        {
            ImFont* font = ImGui::GetFont();
            float fontSize = font->FontSize;
            ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
            float tx = scrX - textSz.x * 0.5f;
            float offsetY = iconSz * 0.8f;
            float ty = scrY - offsetY - iconSz - fontSize - 2.f;
            dl->AddText(ImVec2(tx + 1.f, ty + 1.f),
                        ScaleColorAlpha(IM_COL32(0, 0, 0, 200), alphaMul), label);
            dl->AddText(ImVec2(tx, ty),
                        ScaleColorAlpha(IM_COL32(255, 255, 255, 230), alphaMul), label);
        }
    };

    // --- Captured flag on stand (pulsing glow + icon) ---
    StandOwner standOwner = m_flagTimeline.stand.ownerAtTime(m_debugTimeline);
    if (standOwner != StandOwner::Neutral)
    {
        int standTi = (standOwner == StandOwner::Red) ? 1 : 0;
        ImTextureID standTex = Team::IndexIsRed(standTi) ? texRed : texBlue;
        float sx = m_flagTimeline.stand.standX;
        float sy = m_flagTimeline.stand.standY;
        float sz = m_flagTimeline.stand.standZ;

        XMFLOAT3 standPos = ApplyMapTransformToPos(sx, sy, sz, t);
        float standScrX, standScrY;
        if (ProjectToScreen(viewProj, vpW, vpH, standPos, standScrX, standScrY) && standTex)
        {
            float offsetY = iconSz * 0.8f;
            ImVec2 iconTL(standScrX - iconSz * 0.5f, standScrY - offsetY - iconSz);
            ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);

            ImVec2 center((iconTL.x + iconBR.x) * 0.5f, (iconTL.y + iconBR.y) * 0.5f);
            float glowRadius = iconSz * 0.75f;
            float pulse = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 1.8f);
            ImU32 glowCol = Team::IndexIsRed(standTi)
                ? IM_COL32(255, 60, 50,  (int)(50 * pulse))
                : IM_COL32(60, 130, 255, (int)(50 * pulse));
            dl->AddCircleFilled(center, glowRadius, glowCol, 32);
            dl->AddImage(standTex, iconTL, iconBR);
        }
    }

    // --- Captured flag on obelisk (pulsing glow + icon + label) ---
    if (m_flagTimeline.obelisk.standAgentId >= 0)
    {
        StandOwner obeliskOwner = m_flagTimeline.obelisk.ownerAtTime(m_debugTimeline);
        if (obeliskOwner != StandOwner::Neutral)
        {
            int obTi = (obeliskOwner == StandOwner::Red) ? 1 : 0;
            ImTextureID obTex = Team::IndexIsRed(obTi) ? texRed : texBlue;
            float ox = m_flagTimeline.obelisk.standX;
            float oy = m_flagTimeline.obelisk.standY;
            float oz = m_flagTimeline.obelisk.standZ;

            XMFLOAT3 obPos = ApplyMapTransformToPos(ox, oy, oz, t);
            float obScrX, obScrY;
            if (ProjectToScreen(viewProj, vpW, vpH, obPos, obScrX, obScrY) && obTex)
            {
                float offsetY = iconSz * 0.8f;
                ImVec2 iconTL(obScrX - iconSz * 0.5f, obScrY - offsetY - iconSz);
                ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);

                ImVec2 center((iconTL.x + iconBR.x) * 0.5f, (iconTL.y + iconBR.y) * 0.5f);
                float glowRadius = iconSz * 0.75f;
                float pulse = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 2.2f);
                ImU32 glowCol = Team::IndexIsRed(obTi)
                    ? IM_COL32(255, 80, 70,  (int)(45 * pulse))
                    : IM_COL32(80, 160, 255, (int)(45 * pulse));
                dl->AddCircleFilled(center, glowRadius, glowCol, 32);
                dl->AddImage(obTex, iconTL, iconBR);
            }
        }
    }

    // --- Active flag per team ---
    for (int ti = 0; ti < 2; ti++)
    {
        auto& ft = m_flagTimeline.teams[ti];
        if (ft.events.empty()) continue;

        ImTextureID tex = Team::IndexIsRed(ti) ? texRed : texBlue;
        FlagLocation loc = ft.locationAtTime(m_debugTimeline);

        if (loc == FlagLocation::Stand) continue;

        float worldX = 0, worldY = 0, worldZ = 0;
        const char* label = nullptr;

        if (loc == FlagLocation::Carried)
        {
            int carrierId = ft.carrierAtTime(m_debugTimeline);
            if (carrierId >= 0)
            {
                auto it = m_replayCtx.agents.find(carrierId);
                if (it != m_replayCtx.agents.end() && !it->second.snapshots.empty())
                {
                    InterpolateAgentPosition(it->second, m_debugTimeline,
                                             m_replayCtx.interpSettings, worldX, worldY, worldZ);
                }
                else
                {
                    ft.positionAtTime(m_debugTimeline, worldX, worldY, worldZ);
                }
            }
            else
            {
                ft.positionAtTime(m_debugTimeline, worldX, worldY, worldZ);
            }
            label = "Flag (Carried)";
        }
        else
        {
            ft.positionAtTime(m_debugTimeline, worldX, worldY, worldZ);
            if (loc == FlagLocation::Ground)
                label = "Flag (Dropped)";
        }

        // Fog of war: an enemy flag you have no vision on would not be on your
        // compass, whether a runner is carrying it or it is lying in their base.
        // teams[] is [0]=blue,[1]=red, so the owning team id is Team::FromIndex01(ti);
        // your own flag is never hidden from you.
        float flagAlpha = 1.f;
        if (m_fogPerspective > 0 && Team::FromIndex01(ti) != m_fogPerspective
            && IsPositionInFog(worldX, worldY))
        {
            if (!m_fogGhostMode) continue;
            flagAlpha = kFogGhostAlpha;
        }

        // [FLAG-2] Close up a carried flag is the one in the runner's hand: no icon and no label. Only
        // past the 3D-agent distance (lodDotDist), where the runner is a profession icon, does it show.
        if (loc == FlagLocation::Carried && GroundItemClose(ApplyMapTransformToPos(worldX, worldY, worldZ, t)))
            continue;

        // [FLAG-2] A lying flag the 3D pass drew as its own model keeps only its label.
        const bool asModel = loc != FlagLocation::Carried && m_groundItemModelDrawn[ti];
        DrawFlagAt(worldX, worldY, worldZ, tex, ti, label, flagAlpha, !asModel);
    }

    // --- Vine seeds and repair kits ---
    // A carried bundle is already drawn above its carrier by DrawBundleItems, so
    // only the ones lying in the world need a marker here.
    for (size_t bi = 0; bi < m_flagTimeline.bundles.size(); bi++)
    {
        const auto& bundle = m_flagTimeline.bundles[bi];
        if (bundle.events.empty()) continue;

        BundleLocation bloc = bundle.locationAtTime(m_debugTimeline);
        if (bloc == BundleLocation::Consumed || bloc == BundleLocation::Carried) continue;
        if (bloc == BundleLocation::Base && !bundle.spawnResolved) continue;

        bool isSeed = (bundle.type == BundleType::VineSeed);
        // [FLAG-2] A lying bundle the 3D pass drew as its own model keeps only its label.
        const bool asModel = 2 + bi < static_cast<size_t>(kGroundItemOwners) && m_groundItemModelDrawn[2 + bi];

        float worldX = 0, worldY = 0, worldZ = 0;
        bundle.positionAtTime(m_debugTimeline, worldX, worldY, worldZ);

        // Fog of war: seeds and repair kits are neutral world items, so there is
        // no owning team to exempt - it comes down purely to whether anyone on
        // the chosen perspective can see the spot they are lying on.
        float bundleAlpha = 1.f;
        if (m_fogPerspective > 0 && IsPositionInFog(worldX, worldY))
        {
            if (!m_fogGhostMode) continue;
            bundleAlpha = kFogGhostAlpha;
        }

        XMFLOAT3 pos = ApplyMapTransformToPos(worldX, worldY, worldZ, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        constexpr float kBundleDotRadius = 5.f;
        float offsetY = iconSz * 0.8f;
        if (!asModel)
        {
            ImU32 dotCol = isSeed ? IM_COL32(120, 220, 120, 200) : IM_COL32(210, 180, 120, 200);
            dl->AddCircleFilled(ImVec2(scrX, scrY), kBundleDotRadius,
                                ScaleColorAlpha(dotCol, bundleAlpha));
            dl->AddCircle(ImVec2(scrX, scrY), kBundleDotRadius,
                          ScaleColorAlpha(IM_COL32(0, 0, 0, 180), bundleAlpha), 0, 1.5f);

            if (ImTextureID tex = LoadNPCIcon(dev, isSeed ? "Vine Seed.png" : "RepairKit.png"))
            {
                ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - offsetY - iconSz);
                ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
                dl->AddImage(tex, iconTL, iconBR, ImVec2(0, 0), ImVec2(1, 1),
                             ScaleColorAlpha(IM_COL32_WHITE, bundleAlpha));
            }
        }

        const char* label;
        if (isSeed) label = (bloc == BundleLocation::Ground) ? "Seed (Dropped)" : "Seed";
        else        label = (bloc == BundleLocation::Ground) ? "Repair Kit (Dropped)"
                                                             : "Repair Kit";
        ImFont* font = ImGui::GetFont();
        float fontSize = font->FontSize;
        ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
        float tx = scrX - textSz.x * 0.5f;
        float ty = scrY - offsetY - iconSz - fontSize - 2.f;
        dl->AddText(ImVec2(tx + 1.f, ty + 1.f),
                    ScaleColorAlpha(IM_COL32(0, 0, 0, 200), bundleAlpha), label);
        dl->AddText(ImVec2(tx, ty),
                    ScaleColorAlpha(IM_COL32(255, 255, 255, 230), bundleAlpha), label);
    }

    // --- [FLAG-2] A lying flag, seed or repair kit is selectable, like a gadget ---
    // Hovered over its model (its standing height, as a 3D agent) or its icon; a click follows its agent.
    // Only when nothing else is under the cursor, so a player standing on it keeps the click.
    // The followed one gets the selection ring under it, as a followed player does.
    if (m_cameraMode == CameraMode::FollowAgent && m_followedAgentId >= 0)
    {
        for (int owner = 0; owner < kGroundItemOwners; owner++)
        {
            const GroundItemSpan* span = GroundItemSpanAt(owner, m_debugTimeline);
            if (!span || GroundItemAgent(*span, m_debugTimeline) != m_followedAgentId) continue;
            float alpha = 1.f;
            if (GroundItemVisible(*span, alpha)) DrawFollowRing(dl, viewProj, vpW, vpH, GroundItemRenderPos(*span));
            break;
        }
    }
    if (!ImGui::GetIO().WantCaptureMouse && !m_rightMouseDown && !m_annotationMgr.IsDrawModeActive() &&
        m_hoveredAgentId < 0)
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (int owner = 0; owner < kGroundItemOwners; owner++)
        {
            const GroundItemSpan* span = GroundItemSpanAt(owner, m_debugTimeline);
            if (!span) continue;
            float alpha = 1.f;
            if (!GroundItemVisible(*span, alpha)) continue;
            const XMFLOAT3 at = GroundItemRenderPos(*span);
            float bx, by, topX, topY;
            if (!ProjectToScreen(viewProj, vpW, vpH, at, bx, by)) continue;
            bool hit;
            if (m_groundItemModelDrawn[owner] &&
                ProjectToScreen(viewProj, vpW, vpH, XMFLOAT3(at.x, at.y + 120.f, at.z), topX, topY))
            {
                const float minY = std::min(topY, by), maxY = std::max(topY, by);
                const float halfW = std::max((maxY - minY) * 0.35f, 14.f);
                hit = mouse.x >= bx - halfW && mouse.x <= bx + halfW && mouse.y >= minY - 4.f && mouse.y <= maxY + 4.f;
            }
            else
            {
                // the dot and the icon above it
                const float top = by - iconSz * 1.8f;
                hit = std::abs(mouse.x - bx) <= iconSz * 0.6f && mouse.y >= top && mouse.y <= by + 6.f;
            }
            if (!hit) continue;
            const int agent = GroundItemAgent(*span, m_debugTimeline);
            if (agent < 0) break;
            m_hoveredAgentId = agent;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                EnterFollowMode(agent);
            break;
        }
    }

    // --- Ritualist urns ---
    // The urn itself is gone almost as soon as it lands, so there is nothing to
    // mark on the ground for more than a frame or two. Name it instead, and hold
    // the name up long enough to be read before fading it out.
    for (const auto& drop : m_flagTimeline.ashesDrops)
    {
        const AshesSkill* skill = LookupAshesSkill(drop.skillId);
        if (!skill) continue;

        constexpr float kMinVisible = 2.5f;
        constexpr float kFadeTail   = 0.7f;
        const float until = std::max(drop.endTime, drop.startTime + kMinVisible);
        if (m_debugTimeline < drop.startTime || m_debugTimeline > until) continue;

        const float remaining = until - m_debugTimeline;
        const float fade = std::clamp(remaining / kFadeTail, 0.f, 1.f);

        XMFLOAT3 pos = ApplyMapTransformToPos(drop.x, drop.y, drop.z, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        ImFont* font = ImGui::GetFont();
        const float fontSize = font->FontSize;
        ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, skill->droppedName);
        const float tx = scrX - textSz.x * 0.5f;
        const float ty = scrY - fontSize - 2.f;

        const ImU8 a = static_cast<ImU8>(fade * 255.f);
        dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, (ImU8)(fade * 200.f)),
                    skill->droppedName);
        dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, a), skill->droppedName);
    }
}


void ReplayWindow::DrawFlagDebugWindow()
{
    ImGui::SetNextWindowSize(ImVec2(950, 650), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Flag Timeline", &m_showFlagDebugWindow))
    {
        ImGui::End();
        return;
    }

    if (!m_flagTimelineBuilt || !m_flagTimeline.valid)
    {
        if (!m_replayCtx.stocLoaded)
            ImGui::TextWrapped("Waiting for StoC data to load...");
        else if (!m_agentsClassified)
            ImGui::TextWrapped("Waiting for agent classification...");
        else if (m_replayCtx.stocData.flagEvents.empty())
            ImGui::TextWrapped("No flag_events.txt data in this replay.");
        else
            ImGui::TextWrapped("Flag timeline not yet built.");
        ImGui::End();
        return;
    }

    float t = m_debugTimeline;
    char timeBuf[32];
    int sec = static_cast<int>(t);
    int ms  = static_cast<int>((t - sec) * 1000.f);
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d.%03d", sec / 60, sec % 60, ms);

    // Current State Summary
    ImGui::SeparatorText("Current State");
    ImGui::Text("Playback: %s", timeBuf);

    for (int ti = 0; ti < 2; ti++)
    {
        const char* teamLabel = Team::IndexIsRed(ti) ? "Red" : "Blue";
        auto& ft = m_flagTimeline.teams[ti];
        if (ft.events.empty()) {
            ImGui::Text("%s Flag: [no data]", teamLabel);
            continue;
        }
        FlagLocation loc = ft.locationAtTime(t);
        int carrierId = ft.carrierAtTime(t);
        float fx, fy, fz;
        ft.positionAtTime(t, fx, fy, fz);

        ImGui::Text("%s Flag: [%s] at (%.0f, %.0f, %.0f) | carrier %d | base (%.0f, %.0f)",
                    teamLabel, FlagLocationName(loc), fx, fy, fz, carrierId,
                    ft.spawnX, ft.spawnY);
    }

    StandOwner standOwner = m_flagTimeline.stand.ownerAtTime(t);
    ImGui::Text("Tower Stand: %s (at %.0f, %.0f)", StandOwnerName(standOwner),
                m_flagTimeline.stand.standX, m_flagTimeline.stand.standY);

    if (m_flagTimeline.obelisk.standAgentId >= 0) {
        StandOwner obeliskOwner = m_flagTimeline.obelisk.ownerAtTime(t);
        ImGui::Text("Obelisk Stand: %s (at %.0f, %.0f)", StandOwnerName(obeliskOwner),
                    m_flagTimeline.obelisk.standX, m_flagTimeline.obelisk.standY);
    }
    ImGui::Spacing();

    // Merged Event Timeline
    if (ImGui::CollapsingHeader("All Events (chronological)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##flag_tl_all", 8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 250)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Team",     ImGuiTableColumnFlags_WidthFixed, 45.f);
            ImGui::TableSetupColumn("Event",    ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 65.f);
            ImGui::TableSetupColumn("Actor",    ImGuiTableColumnFlags_WidthFixed, 55.f);
            ImGui::TableSetupColumn("Carrier",  ImGuiTableColumnFlags_WidthFixed, 55.f);
            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 150.f);
            ImGui::TableSetupColumn("Flag Agent", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_flagTimeline.allEvents.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = m_flagTimeline.allEvents[row];
                    ImGui::TableNextRow();

                    bool nearCurrent = (ev.time >= t - 1.f && ev.time <= t + 1.f);
                    if (nearCurrent)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(80, 80, 40, 100));

                    int s = static_cast<int>(ev.time);
                    int m = static_cast<int>((ev.time - s) * 1000.f);

                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(std::format("{:02d}:{:02d}.{:03d}##ftl{}", s/60, s%60, m, row).c_str(),
                                          false, ImGuiSelectableFlags_SpanAllColumns))
                        m_debugTimeline = ev.time;
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ev.flagTeam == FlagTeam::Red ? "Red" : "Blue");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(FlagEventTypeName(ev.eventType));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(FlagLocationName(ev.newLocation));
                    ImGui::TableNextColumn();
                    if (ev.actorAgentId >= 0) ImGui::Text("%d", ev.actorAgentId);
                    else ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn();
                    if (ev.carrierAgentId >= 0) ImGui::Text("%d", ev.carrierAgentId);
                    else ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f, %.0f, %.0f", ev.x, ev.y, ev.z);
                    ImGui::TableNextColumn();
                    if (ev.flagWorldAgentId >= 0) ImGui::Text("%d", ev.flagWorldAgentId);
                    else if (ev.standAgentId >= 0) ImGui::Text("stand:%d", ev.standAgentId);
                    else ImGui::TextUnformatted("-");
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    // Stand Control Timeline
    if (ImGui::CollapsingHeader("Stand Control Timeline", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Stand Agent: %d  Position: (%.0f, %.0f, %.0f)",
                    m_flagTimeline.stand.standAgentId,
                    m_flagTimeline.stand.standX, m_flagTimeline.stand.standY, m_flagTimeline.stand.standZ);

        if (ImGui::BeginTable("##stand_tl", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 150)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",          ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Owner",         ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Morale Expiry", ImGuiTableColumnFlags_WidthFixed, 100.f);
            ImGui::TableSetupColumn("Stand Agent",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_flagTimeline.stand.events.size()); i++)
            {
                auto& sc = m_flagTimeline.stand.events[i];
                ImGui::TableNextRow();

                bool nearCurrent = (sc.time >= t - 1.f && sc.time <= t + 1.f);
                if (nearCurrent)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(80, 80, 40, 100));

                int s = static_cast<int>(sc.time);
                int m = static_cast<int>((sc.time - s) * 1000.f);

                ImGui::TableNextColumn();
                ImGui::Text("%02d:%02d.%03d", s/60, s%60, m);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(StandOwnerName(sc.owner));
                ImGui::TableNextColumn();
                if (sc.moraleExpiry > 0) {
                    int es = static_cast<int>(sc.moraleExpiry);
                    ImGui::Text("%02d:%02d", es/60, es%60);
                } else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                ImGui::Text("%d", sc.standAgentId);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    // Per-Team Event Timelines
    for (int ti = 0; ti < 2; ti++)
    {
        const char* label = Team::IndexIsRed(ti) ? "Red Team Timeline" : "Blue Team Timeline";
        if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        auto& ft = m_flagTimeline.teams[ti];
        ImGui::Text("  Base: (%.0f, %.0f, %.0f) | Events: %d",
                    ft.spawnX, ft.spawnY, ft.spawnZ, static_cast<int>(ft.events.size()));

        if (ImGui::BeginTable(std::format("##team_tl_{}", ti).c_str(), 7,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 180)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Event",    ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 65.f);
            ImGui::TableSetupColumn("Actor",    ImGuiTableColumnFlags_WidthFixed, 55.f);
            ImGui::TableSetupColumn("Carrier",  ImGuiTableColumnFlags_WidthFixed, 55.f);
            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 150.f);
            ImGui::TableSetupColumn("Flag Agent", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int row = 0; row < static_cast<int>(ft.events.size()); row++)
            {
                auto& ev = ft.events[row];
                ImGui::TableNextRow();

                bool nearCurrent = (ev.time >= t - 1.f && ev.time <= t + 1.f);
                if (nearCurrent)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(80, 80, 40, 100));

                int s = static_cast<int>(ev.time);
                int m = static_cast<int>((ev.time - s) * 1000.f);

                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::format("{:02d}:{:02d}.{:03d}##ttl{}_{}", s/60, s%60, m, ti, row).c_str(),
                                      false, ImGuiSelectableFlags_SpanAllColumns))
                    m_debugTimeline = ev.time;
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FlagEventTypeName(ev.eventType));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FlagLocationName(ev.newLocation));
                ImGui::TableNextColumn();
                if (ev.actorAgentId >= 0) ImGui::Text("%d", ev.actorAgentId);
                else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (ev.carrierAgentId >= 0) ImGui::Text("%d", ev.carrierAgentId);
                else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                ImGui::Text("%.0f, %.0f, %.0f", ev.x, ev.y, ev.z);
                ImGui::TableNextColumn();
                if (ev.flagWorldAgentId >= 0) ImGui::Text("%d", ev.flagWorldAgentId);
                else if (ev.standAgentId >= 0) ImGui::Text("stand:%d", ev.standAgentId);
                else ImGui::TextUnformatted("-");
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    ImGui::End();
}


// ---------------------------------------------------------------------------
// Flag event messages — displayed below the Time Elapsed counter
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFlagEventMessages()
{
    if (m_flagMessages.empty()) return;

    float curTime = m_debugTimeline;

    constexpr float kDisplayDuration = 5.0f;
    constexpr float kFadeStart      = 4.0f;
    constexpr int   kMaxVisible     = 2;

    // Collect up to kMaxVisible active messages (most recent first)
    struct ActiveMsg { const FlagEventMessage* msg; float age; };
    std::vector<ActiveMsg> actives;
    for (auto& msg : m_flagMessages)
    {
        float age = curTime - msg.time;
        if (age < 0.f || age > kDisplayDuration) continue;
        actives.push_back({ &msg, age });
    }
    if (actives.empty()) return;

    // Sort by time descending (newest first), keep up to kMaxVisible
    std::sort(actives.begin(), actives.end(),
        [](const ActiveMsg& a, const ActiveMsg& b) { return a.msg->time > b.msg->time; });
    if (static_cast<int>(actives.size()) > kMaxVisible)
        actives.resize(kMaxVisible);
    // Reverse so oldest draws on top, newest on bottom (chat order)
    std::reverse(actives.begin(), actives.end());

    ImFont* font = m_latoRegular ? m_latoRegular : ImGui::GetFont();
    float fontSize = font->FontSize;
    float lineH = fontSize + 6.f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float posX = m_uiLayout.useCustom ? m_uiLayout.timerX : 0.50f;
    float posY = m_uiLayout.useCustom ? m_uiLayout.timerY : 0.12f;
    float cx = vp->Pos.x + vp->Size.x * posX;
    float baseY = vp->Pos.y + vp->Size.y * posY;
    float msgStartY = baseY + fontSize * 2.f + 30.f;

    struct Segment { std::string text; ImU32 color; };

    auto drawSegmentsCentered = [&](const std::vector<Segment>& segs, float y, float alpha)
    {
        int a = static_cast<int>(alpha * 255);
        ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(alpha * 230));
        float totalW = 0.f;
        for (auto& s : segs)
            totalW += font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, s.text.c_str()).x;
        float x = cx - totalW * 0.5f;
        for (auto& s : segs) {
            ImU32 col = (s.color & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24);
            dl->AddText(font, fontSize, ImVec2(x, y + 1), shadow, s.text.c_str());
            dl->AddText(font, fontSize, ImVec2(x, y), col, s.text.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, s.text.c_str()).x;
        }
        return totalW;
    };

    float curY = msgStartY;

    for (auto& am : actives)
    {
        const auto* active = am.msg;
        float alpha = 1.f;
        if (am.age > kFadeStart)
            alpha = std::clamp(kDisplayDuration - am.age, 0.f, 1.f) / (kDisplayDuration - kFadeStart);
        if (alpha <= 0.f) continue;

        ImU32 whiteCol = IM_COL32(255, 255, 255, 255);
        ImU32 blueCol  = IM_COL32(0x99, 0xCB, 0xFD, 255);
        ImU32 redCol   = IM_COL32(0xFF, 0x99, 0x9A, 255);

        ImU32 playerCol = Team::IndexIsRed(active->playerTeam) ? redCol : blueCol;
        ImU32 flagCol   = Team::IndexIsRed(active->flagTeam)   ? redCol : blueCol;
        const char* flagTeamName = Team::IndexIsRed(active->flagTeam) ? "red" : "blue";

        std::vector<Segment> line1, line2;

        if (active->bundleType != BundleType::Unknown)
        {
            const char* what = (active->bundleType == BundleType::RepairKit)
                ? " a repair kit!" : " a vine seed!";
            line1.push_back({ active->playerName, playerCol });
            line1.push_back({ (active->eventType == FlagTimelineEventType::Drop)
                                  ? " has dropped" : " picked up", whiteCol });
            line1.push_back({ what, whiteCol });
            drawSegmentsCentered(line1, curY, alpha);
            curY += lineH;
            continue;
        }

        switch (active->eventType)
        {
        case FlagTimelineEventType::Pickup:
            line1.push_back({ active->playerName, playerCol });
            line1.push_back({ " picked up ", whiteCol });
            line1.push_back({ flagTeamName, flagCol });
            line1.push_back({ "'s team flag!", whiteCol });
            break;
        case FlagTimelineEventType::Drop:
            line1.push_back({ active->playerName, playerCol });
            line1.push_back({ " has dropped ", whiteCol });
            line1.push_back({ flagTeamName, flagCol });
            line1.push_back({ "'s team flag!", whiteCol });
            break;
        case FlagTimelineEventType::Return:
            if (!active->playerName.empty()) {
                line1.push_back({ active->playerName, playerCol });
                line1.push_back({ " has returned ", whiteCol });
            } else {
                const char* returnTeamName = Team::IndexIsRed(active->flagTeam) ? "Blue" : "Red";
                ImU32 returnTeamCol = Team::IndexIsRed(active->flagTeam) ? blueCol : redCol;
                line1.push_back({ returnTeamName, returnTeamCol });
                line1.push_back({ " team has returned ", whiteCol });
            }
            line1.push_back({ flagTeamName, flagCol });
            line1.push_back({ "'s team flag!", whiteCol });
            break;
        case FlagTimelineEventType::Stick:
        {
            bool isObelisk = (m_flagTimeline.obelisk.standAgentId >= 0 &&
                              active->standAgentId == m_flagTimeline.obelisk.standAgentId);
            line1.push_back({ active->playerName, playerCol });
            if (isObelisk) {
                line1.push_back({ " has taken control of the obelisk!", whiteCol });
            } else {
                const char* teamName = Team::IndexIsRed(active->flagTeam) ? "Red" : "Blue";
                line1.push_back({ " has taken control of the watchtower!", whiteCol });
                line2.push_back({ teamName, flagCol });
                line2.push_back({ " team will earn a morale boost every two minutes they hold the watchtower.", whiteCol });
            }
            break;
        }
        default:
            continue;
        }

        drawSegmentsCentered(line1, curY, alpha);
        curY += lineH;

        if (!line2.empty()) {
            drawSegmentsCentered(line2, curY, alpha);
            curY += lineH;
        }
    }
}
