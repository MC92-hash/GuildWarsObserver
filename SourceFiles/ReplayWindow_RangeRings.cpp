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

static const ReplayWindow::RingDef kRingDefs[ReplayWindow::kRingTypeCount] = {
    //                                                    thick  solid  dash       fillA
    { "Touch",      144.f,  IM_COL32(255, 64, 64, 255),  1.5f, true,  0,0,       0.15f },
    { "Adjacent",   166.f,  IM_COL32(255,112, 32, 255),  1.5f, true,  0,0,       0.15f },
    { "Nearby",     240.f,  IM_COL32(255,176, 32, 255),  1.0f, true,  0,0,       0.12f },
    { "In Area",    322.f,  IM_COL32(255,255, 64, 255),  1.0f, true,  0,0,       0.10f },
    { "Earshot",   1000.f,  IM_COL32( 64,255,128, 255),  1.0f, true,  0,0,       0.06f },
    { "Cast Range",1248.f,  IM_COL32( 64,192,255, 255),  2.0f, true,  0,0,       0.05f },
    { "Passive",   2512.f,  IM_COL32(192, 64,255, 255),  1.0f, true,  0,0,       0.03f },
    { "Compass",   5020.f,  IM_COL32(128,128,128, 255),  0.5f, false, 8,4,       0.02f },
};

static constexpr float kRingFillAlpha    = 0.15f;
static constexpr float kRingSelectFill   = 0.22f;
static constexpr float kRingDimFactor    = 0.50f;
static const ImU32 kRingSelectEdge = IM_COL32(128, 255, 128, 255);

static ImU32 ScaleAlpha(ImU32 col, float factor)
{
    ImU8 a = (ImU8)((col >> 24) & 0xFF);
    a = (ImU8)(a * factor);
    return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

void ReplayWindow::DrawRangeRings()
{
    if (!m_showRangeRings) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const MapTransform& t = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    constexpr int kSamples = 64;
    constexpr float kPI2 = 6.28318530718f;
    constexpr float kZOffset = 2.f;

    bool hasSelection = (m_ringAgentFilter >= 0);

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player) continue;
        if (ard.snapshots.empty()) continue;
        if (ard.isDeadAtTime(m_debugTimeline)) continue;

        if (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId))
            continue;

        if (m_ringHiddenAgents.count(agentId)) continue;

        bool isSelected = (m_ringAgentFilter == agentId);

        if (!isSelected)
        {
            if (hasSelection)
            {
                bool teamEnabled = (ard.teamId == 1 && m_ringShowRed)
                                || (ard.teamId == 2 && m_ringShowBlue);
                if (!teamEnabled) continue;
            }
            else
            {
                if (ard.teamId == 1 && !m_ringShowRed) continue;
                if (ard.teamId == 2 && !m_ringShowBlue)  continue;
            }
        }

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);

        ImU32 teamCol = GetAgentTeamColor(ard.teamId);

        for (int ri = 0; ri < kRingTypeCount; ++ri)
        {
            bool showRing = m_ringType[ri];
            bool isHoverPreview = (!showRing && m_ringHoveredType == ri);
            if (!showRing && !isHoverPreview) continue;

            const auto& def = kRingDefs[ri];
            float radius = def.radius;

            float dimFactor = (!isSelected && hasSelection) ? 0.25f : 1.f;
            float thickMul  = (isSelected && hasSelection) ? 1.6f : 1.f;
            ImU32 edgeCol = teamCol;
            if (dimFactor < 1.f) edgeCol = ScaleAlpha(edgeCol, dimFactor);
            if (isHoverPreview) edgeCol = ScaleAlpha(teamCol, 0.40f);

            ImVec2 pts[kSamples];
            bool vis[kSamples];
            int visCount = 0;

            for (int i = 0; i < kSamples; ++i)
            {
                float angle = (float(i) / kSamples) * kPI2;
                float wx = sx + cosf(angle) * radius;
                float wy = sy + sinf(angle) * radius;

                XMFLOAT3 mp = ApplyMapTransformToPos(wx, wy, sz, t);

                if (terrain)
                    mp.y = terrain->get_height_at(mp.x, mp.z) + kZOffset;

                float scrX, scrY;
                vis[i] = ProjectToScreen(viewProj, vpW, vpH, mp, scrX, scrY);
                pts[i] = ImVec2(scrX, scrY);
                if (vis[i]) visCount++;
            }

            if (visCount < 3) continue;

            // Fill pass (brighter when selected, heavily dimmed for teammates)
            {
                float fa = isHoverPreview ? 0.10f : (isSelected ? def.fillAlpha * 2.0f : def.fillAlpha);
                if (dimFactor < 1.f) fa *= dimFactor;
                ImU32 baseRGB = teamCol;
                ImU32 fillCol = (baseRGB & 0x00FFFFFF) | ((ImU32)(fa * 255.f) << 24);
                ImVector<ImVec2> polyPts;
                for (int i = 0; i < kSamples; ++i)
                    if (vis[i]) polyPts.push_back(pts[i]);
                if (polyPts.Size >= 3)
                    dl->AddConvexPolyFilled(polyPts.Data, polyPts.Size, fillCol);
            }

            // Ring edge pass
            float thick = def.thickness * thickMul;
            if (def.solid)
            {
                for (int i = 0; i < kSamples; ++i)
                {
                    int j = (i + 1) % kSamples;
                    if (vis[i] && vis[j])
                        dl->AddLine(pts[i], pts[j], edgeCol, thick);
                }
            }
            else
            {
                float dashOn  = def.dashOn;
                float dashOff = def.dashOff;
                float cycle = dashOn + dashOff;
                float accum = 0.f;
                for (int i = 0; i < kSamples; ++i)
                {
                    int j = (i + 1) % kSamples;
                    float dx = pts[j].x - pts[i].x;
                    float dy = pts[j].y - pts[i].y;
                    float segLen = sqrtf(dx * dx + dy * dy);

                    if (vis[i] && vis[j])
                    {
                        float pos = fmodf(accum, cycle);
                        if (pos < dashOn)
                            dl->AddLine(pts[i], pts[j], edgeCol, thick);
                    }
                    accum += segLen;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Spirit Range Circles — drawn for hovered or selected spirits
// ---------------------------------------------------------------------------

void ReplayWindow::DrawSpiritRanges()
{
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;
    if (m_hoveredAgentId < 0 && m_followedAgentId < 0) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const MapTransform& t = m_replayCtx.mapTransform;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    constexpr int kSamples = 48;
    constexpr float kPI2 = 6.28318530718f;
    constexpr float kZOffset = 2.f;
    constexpr float kFillAlpha = 0.12f;
    constexpr float kEdgeThickness = 1.5f;

    for (int sid : m_spiritIds)
    {
        if (sid != m_hoveredAgentId && sid != m_followedAgentId) continue;

        auto it = m_replayCtx.agents.find(sid);
        if (it == m_replayCtx.agents.end()) continue;
        const auto& ard = it->second;
        if (ard.type != AgentType::Spirit) continue;
        if (ard.snapshots.empty()) continue;

        if (m_debugTimeline < ard.snapshots.front().time ||
            m_debugTimeline > ard.snapshots.back().time)
            continue;
        if (ard.overlapHidden) continue;
        if (ard.isDeadAtTime(m_debugTimeline) || !ard.isAliveAtTime(m_debugTimeline))
            continue;

        float radius = GetSpiritRange(ard.modelId);
        if (radius <= 0.f) continue;

        ImU32 baseColor = IsNatureRitual(ard.modelId)
            ? IM_COL32(0x80, 0xFF, 0x80, 0xFF)
            : GetAgentTeamColor(ard.teamId);

        float sx, sy, sz;
        SnapAgentPosition(ard, m_debugTimeline, sx, sy, sz);

        ImVec2 pts[kSamples];
        bool vis[kSamples];
        int visCount = 0;

        for (int i = 0; i < kSamples; ++i)
        {
            float angle = (float(i) / kSamples) * kPI2;
            float wx = sx + cosf(angle) * radius;
            float wy = sy + sinf(angle) * radius;

            XMFLOAT3 mp = ApplyMapTransformToPos(wx, wy, sz, t);
            if (terrain)
                mp.y = terrain->get_height_at(mp.x, mp.z) + kZOffset;

            float scrX, scrY;
            vis[i] = ProjectToScreen(viewProj, vpW, vpH, mp, scrX, scrY);
            pts[i] = ImVec2(scrX, scrY);
            if (vis[i]) visCount++;
        }

        if (visCount < 3) continue;

        // Fill pass
        {
            ImU32 fillCol = (baseColor & 0x00FFFFFF) | ((ImU32)(kFillAlpha * 255.f) << 24);
            ImVector<ImVec2> polyPts;
            for (int i = 0; i < kSamples; ++i)
                if (vis[i]) polyPts.push_back(pts[i]);
            if (polyPts.Size >= 3)
                dl->AddConvexPolyFilled(polyPts.Data, polyPts.Size, fillCol);
        }

        // Edge pass
        ImU32 edgeCol = (baseColor & 0x00FFFFFF) | (0xC0u << 24);
        for (int i = 0; i < kSamples; ++i)
        {
            int j = (i + 1) % kSamples;
            if (vis[i] && vis[j])
                dl->AddLine(pts[i], pts[j], edgeCol, kEdgeThickness);
        }
    }
}

// ---------------------------------------------------------------------------
// Isle of Wurms — South Health Shrine capture radius (1010u)
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Range Ring Toolbar UI
// ---------------------------------------------------------------------------

void ReplayWindow::DrawRangeRingToolbar()
{
    if (!m_showRangeRings) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 0.f), ImVec2(vp->Size.x, vp->Size.y));
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    m_panelLayout.ApplyPosition("range_rings");
    if (ImGui::Begin("Range Rings", &m_showRangeRings,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        m_panelLayout.TrackWindow("range_rings");
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - sz.x);
        float cy = std::clamp(pos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - sz.y);
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
        auto RingPill = [](const char* label, bool active, ImU32 accent = 0) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
            } else {
                bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
                tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
                hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
                bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        auto TeamPill = [](const char* label, bool active, int team) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                if (team == 1) {
                    bg  = ImVec4(0.25f, 0.06f, 0.06f, 1.f);
                    tx  = ImVec4(1.f, 0.42f, 0.42f, 1.f);
                    hov = ImVec4(0.30f, 0.10f, 0.10f, 1.f);
                    bdr = ImVec4(1.f, 0.42f, 0.42f, 0.85f);
                } else if (team == 2) {
                    bg  = ImVec4(0.05f, 0.12f, 0.25f, 1.f);
                    tx  = ImVec4(0.29f, 0.78f, 1.f, 1.f);
                    hov = ImVec4(0.08f, 0.16f, 0.30f, 1.f);
                    bdr = ImVec4(0.29f, 0.78f, 1.f, 0.85f);
                } else {
                    bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                    tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                    hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                    bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
                }
            } else {
                bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
                tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
                hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
                bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        // None / All buttons
        if (RingPill("None", false))
            for (int i = 0; i < kRingTypeCount; ++i) m_ringType[i] = false;
        ImGui::SameLine();
        if (RingPill("All", false))
            for (int i = 0; i < kRingTypeCount; ++i) m_ringType[i] = true;

        ImGui::Separator();

        // Ring type pills (two rows: 4 + 4)
        m_ringHoveredType = -1;
        for (int i = 0; i < kRingTypeCount; ++i)
        {
            if (i == 4) {} // new line
            else if (i > 0) ImGui::SameLine();

            ImGui::PushID(i);
            bool clicked = RingPill(kRingDefs[i].name, m_ringType[i], kRingDefs[i].color);

            if (ImGui::IsItemHovered())
            {
                m_ringHoveredType = i;
                ImGui::SetTooltip("%s \xe2\x80\x94 %.0f units", kRingDefs[i].name, kRingDefs[i].radius);
            }

            if (clicked)
            {
                if (ImGui::GetIO().MouseDoubleClicked[0])
                {
                    if (m_ringSoloActive && m_ringType[i])
                    {
                        for (int k = 0; k < kRingTypeCount; ++k)
                            m_ringType[k] = m_ringSoloPrev[k];
                        m_ringSoloActive = false;
                    }
                    else
                    {
                        for (int k = 0; k < kRingTypeCount; ++k)
                            m_ringSoloPrev[k] = m_ringType[k];
                        for (int k = 0; k < kRingTypeCount; ++k)
                            m_ringType[k] = (k == i);
                        m_ringSoloActive = true;
                    }
                }
                else
                {
                    m_ringType[i] = !m_ringType[i];
                    m_ringSoloActive = false;
                }
            }
            ImGui::PopID();
        }

        ImGui::Separator();

        // Team filter (independent toggles)
        if (TeamPill("Red", m_ringShowRed, 1))
            m_ringShowRed = !m_ringShowRed;
        ImGui::SameLine();
        if (TeamPill("Blue", m_ringShowBlue, 2))
            m_ringShowBlue = !m_ringShowBlue;

        if (m_ringAgentFilter >= 0)
        {
            ImGui::SameLine();
            auto it = m_replayCtx.agents.find(m_ringAgentFilter);
            ImVec4 agentCol(1.f, 0.91f, 0.69f, 1.f);
            if (it != m_replayCtx.agents.end()) {
                if (it->second.teamId == 1) agentCol = ImVec4(1.f, 0.42f, 0.42f, 1.f);
                else if (it->second.teamId == 2) agentCol = ImVec4(0.29f, 0.78f, 1.f, 1.f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, agentCol);
            std::string lbl = (it != m_replayCtx.agents.end())
                ? std::format("Agent: {} [x]", it->second.playerName)
                : std::format("Agent: #{} [x]", m_ringAgentFilter);
            if (ImGui::SmallButton(lbl.c_str()))
                m_ringAgentFilter = -1;
            ImGui::PopStyleColor();
        }

        // Per-character ring visibility
        ImGui::Separator();

        if (ImGui::SmallButton("Show All##Rings"))
            m_ringHiddenAgents.clear();
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide All##Rings"))
        {
            for (int id : m_playerIds)
                m_ringHiddenAgents.insert(id);
        }

        ID3D11Device* dev = m_deviceResources->GetD3DDevice();
        const float iconSz = 16.f;

        auto DrawTeamRings = [&](const char* header, const std::vector<int>& ids, bool teamEnabled)
        {
            if (!teamEnabled) ImGui::BeginDisabled();
            if (ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (int id : ids)
                {
                    auto it = m_replayCtx.agents.find(id);
                    if (it == m_replayCtx.agents.end()) continue;
                    const auto& ard = it->second;

                    bool visible = !m_ringHiddenAgents.count(id);

                    ImTextureID profTex = (ard.primaryProf > 0)
                        ? LoadProfIcon(dev, ard.primaryProf) : nullptr;
                    if (profTex)
                    {
                        ImGui::Image(profTex, ImVec2(iconSz, iconSz));
                        ImGui::SameLine(0, 4);
                    }

                    if (ImGui::Checkbox(("##ring_" + std::to_string(id)).c_str(), &visible))
                    {
                        if (visible)
                            m_ringHiddenAgents.erase(id);
                        else
                            m_ringHiddenAgents.insert(id);
                    }
                    ImGui::SameLine(0, 4);
                    std::string label = ard.playerName.empty()
                        ? ard.categoryName : ard.playerName;
                    ImGui::TextUnformatted(label.c_str());
                }
                ImGui::TreePop();
            }
            if (!teamEnabled) ImGui::EndDisabled();
        };

        DrawTeamRings("Red Team", m_team1PlayerIds, m_ringShowRed);
        DrawTeamRings("Blue Team", m_team2PlayerIds, m_ringShowBlue);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}
