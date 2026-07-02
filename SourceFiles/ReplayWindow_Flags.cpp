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


// ---------------------------------------------------------------------------
// Flag Timeline — delegates to FlagTimelineBuilder
// ---------------------------------------------------------------------------

void ReplayWindow::BuildFlagTimeline()
{
    FlagTimelineBuilder::Input input;
    input.flagEvents = &m_replayCtx.stocData.flagEvents;
    input.lifecycle  = &m_replayCtx.stocData.lifecycle;
    input.mapObject  = &m_replayCtx.stocData.mapObject;
    input.agents     = &m_replayCtx.agents;
    input.mapId      = m_replayCtx.mapId;
    m_flagTimeline = FlagTimelineBuilder::Build(input);
    m_flagTimelineBuilt = true;
    BuildFlagMessages();
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
                msg.playerTeam = (it->second.teamId == 2) ? 1 : 0;
            }
        }

        if (msg.playerName.empty() && ev.eventType != FlagTimelineEventType::Return)
            continue;

        m_flagMessages.push_back(std::move(msg));
    }
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

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texRed  = LoadFlagIcon(dev, "Red_flag_waving.svg.png");
    ImTextureID texBlue = LoadFlagIcon(dev, "Blue_flag_waving.svg.png");
    const float iconSz = std::clamp(vpH * 0.035f, 18.f, 32.f);

    auto DrawFlagAt = [&](float worldX, float worldY, float worldZ,
                          ImTextureID tex, int teamIdx, const char* label)
    {
        XMFLOAT3 pos = ApplyMapTransformToPos(worldX, worldY, worldZ, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) return;

        constexpr float kFlagDotRadius = 5.f;
        ImU32 dotColor = (teamIdx == 0) ? IM_COL32(255, 100, 90, 200)
                                        : IM_COL32(100, 160, 255, 200);
        dl->AddCircleFilled(ImVec2(scrX, scrY), kFlagDotRadius, dotColor);
        dl->AddCircle(ImVec2(scrX, scrY), kFlagDotRadius, IM_COL32(0, 0, 0, 180), 0, 1.5f);

        if (tex)
        {
            float offsetY = iconSz * 0.8f;
            ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - offsetY - iconSz);
            ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
            dl->AddImage(tex, iconTL, iconBR);
        }

        if (label)
        {
            ImFont* font = ImGui::GetFont();
            float fontSize = font->FontSize;
            ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
            float tx = scrX - textSz.x * 0.5f;
            float offsetY = iconSz * 0.8f;
            float ty = scrY - offsetY - iconSz - fontSize - 2.f;
            dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 200), label);
            dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), label);
        }
    };

    // --- Captured flag on stand (pulsing glow + icon) ---
    StandOwner standOwner = m_flagTimeline.stand.ownerAtTime(m_debugTimeline);
    if (standOwner != StandOwner::Neutral)
    {
        int standTi = (standOwner == StandOwner::Red) ? 0 : 1;
        ImTextureID standTex = (standTi == 0) ? texRed : texBlue;
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
            ImU32 glowCol = (standTi == 0)
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
            int obTi = (obeliskOwner == StandOwner::Red) ? 0 : 1;
            ImTextureID obTex = (obTi == 0) ? texRed : texBlue;
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
                ImU32 glowCol = (obTi == 0)
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

        ImTextureID tex = (ti == 0) ? texRed : texBlue;
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

        DrawFlagAt(worldX, worldY, worldZ, tex, ti, label);
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
        const char* teamLabel = (ti == 0) ? "Red" : "Blue";
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
        const char* label = (ti == 0) ? "Blue Team Timeline" : "Red Team Timeline";
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
    ImDrawList* dl = ImGui::GetForegroundDrawList();

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

        ImU32 playerCol = (active->playerTeam == 0) ? redCol : blueCol;
        ImU32 flagCol   = (active->flagTeam == 0)   ? redCol : blueCol;
        const char* flagTeamName = (active->flagTeam == 0) ? "red" : "blue";

        std::vector<Segment> line1, line2;

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
                const char* returnTeamName = (active->flagTeam == 0) ? "Blue" : "Red";
                ImU32 returnTeamCol = (active->flagTeam == 0) ? blueCol : redCol;
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
                const char* teamName = (active->flagTeam == 0) ? "Red" : "Blue";
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
