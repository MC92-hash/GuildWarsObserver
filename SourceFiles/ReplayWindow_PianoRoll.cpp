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
// Spikes (owner's definition, 2026-10-27): 3 or more ENEMY damage skills hitting one player within
// 1.5 s. The hit time is the end of the cast (the start for an instant). A Monk's answer is any allied
// prot or heal on that player from 1.5 s before the first enemy cast starts until 3 s after the first
// hit; reaction is counted from that first enemy cast START, the earliest moment it can be read.
// ---------------------------------------------------------------------------

namespace
{
    constexpr float kSpikeWindow   = 1.5f;
    constexpr int   kSpikeMinHits  = 3;
    constexpr float kAnswerBefore  = 1.5f;
    constexpr float kAnswerAfter   = 3.f;

    float HitTime(const SkillUseEvent& ev) { return std::max(ev.endTime, ev.startTime); }

    ImU32 TeamColour(uint8_t team, int alpha)
    {
        return team == 1 ? IM_COL32(0x60, 0x9C, 0xFF, alpha) : team == 2 ? IM_COL32(0xE8, 0x50, 0x48, alpha)
                                                             : IM_COL32(0xC8, 0xC8, 0xC8, alpha);
    }
}

bool ReplayWindow::IsSpikeDamageSkill(int skillId) const
{
    const SkillInfo* si = m_skillView.IsLoaded() ? m_skillView.Get(skillId) : nullptr;
    if (!si) return false;
    for (const auto& sc : si->scales)
        if (sc.kind == SkillScaleKind::Damage || sc.kind == SkillScaleKind::LifeSteal || sc.kind == SkillScaleKind::LifeLoss)
            return true;
    return false;
}

// Prots and heals: the Monk's protection, healing and Divine Favor lines, Restoration Magic, and any
// other skill whose description heals.
bool ReplayWindow::IsSpikeAnswerSkill(int skillId) const
{
    const SkillInfo* si = m_skillView.IsLoaded() ? m_skillView.Get(skillId) : nullptr;
    if (!si) return false;
    if (si->attribute == 13 || si->attribute == 15 || si->attribute == 16 || si->attribute == 33) return true;
    for (const auto& sc : si->scales)
        if (sc.kind == SkillScaleKind::Heal) return true;
    return false;
}

void ReplayWindow::BuildSpikes()
{
    m_spikesBuilt = true;
    m_spikes.clear();
    if (!m_skillView.IsLoaded()) { m_spikesBuilt = false; return; }   // retried once the database is in

    // Every cast aimed at each player, split into enemy damage and allied answers.
    std::unordered_map<int, std::vector<CastRef>> dmgOn, ansOn;
    for (const auto& [casterId, caster] : m_replayCtx.agents)
        for (const auto& ev : caster.skillUseHistory)
        {
            if (ev.targetId < 0 || ev.targetId == casterId || ev.wasInterrupted || ev.wasCancelled) continue;
            auto tit = m_replayCtx.agents.find(ev.targetId);
            if (tit == m_replayCtx.agents.end() || tit->second.type != AgentType::Player) continue;
            const bool enemy = caster.teamId != tit->second.teamId;
            if (enemy && IsSpikeDamageSkill(ev.skillId))      dmgOn[ev.targetId].push_back({ &ev, casterId });
            else if (!enemy && IsSpikeAnswerSkill(ev.skillId)) ansOn[ev.targetId].push_back({ &ev, casterId });
        }

    for (auto& [target, hits] : dmgOn)
    {
        std::sort(hits.begin(), hits.end(), [](const CastRef& a, const CastRef& b) { return HitTime(*a.ev) < HitTime(*b.ev); });
        auto& answers = ansOn[target];
        std::sort(answers.begin(), answers.end(), [](const CastRef& a, const CastRef& b) { return a.ev->startTime < b.ev->startTime; });
        for (size_t i = 0; i + kSpikeMinHits - 1 < hits.size();)
        {
            size_t j = i;
            while (j + 1 < hits.size() && HitTime(*hits[j + 1].ev) - HitTime(*hits[i].ev) <= kSpikeWindow) ++j;
            if (j - i + 1 < static_cast<size_t>(kSpikeMinHits)) { ++i; continue; }
            SpikeInfo sp;
            sp.target   = target;
            sp.hits.assign(hits.begin() + i, hits.begin() + j + 1);
            sp.firstHit = HitTime(*sp.hits.front().ev);
            sp.lastHit  = HitTime(*sp.hits.back().ev);
            sp.cue      = FLT_MAX;
            for (const auto& h : sp.hits) sp.cue = std::min(sp.cue, h.ev->startTime);
            for (const auto& a : answers)
                if (a.ev->startTime >= sp.cue - kAnswerBefore && a.ev->startTime <= sp.firstHit + kAnswerAfter)
                    sp.answers.push_back(a);
            if (auto tit = m_replayCtx.agents.find(target); tit != m_replayCtx.agents.end())
                for (const auto& snap : tit->second.snapshots)
                {
                    if (snap.time < sp.cue) continue;
                    if (snap.time > sp.lastHit + 1.f) break;
                    sp.lowHp = sp.lowHp < 0.f ? snap.health_pct : std::min(sp.lowHp, snap.health_pct);
                    if (snap.is_dead) sp.lowHp = 0.f;
                }
            m_spikes.push_back(std::move(sp));
            i = j + 1;
        }
    }
    std::sort(m_spikes.begin(), m_spikes.end(), [](const SpikeInfo& a, const SpikeInfo& b) { return a.firstHit < b.firstHit; });
}


void ReplayWindow::DrawPianoRollPanel()
{
    if (!m_showPianoRoll) { m_pianoRollHighlightAgent = -1; return; }
    if (m_team1PlayerIds.empty() && m_team2PlayerIds.empty()) return;
    if (!m_spikesBuilt) BuildSpikes();

    auto* dev = m_deviceResources->GetD3DDevice();
    const float now = m_debugTimeline;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();
    EnsureSkillIconIndex();

    // Zoom table: half-window sizes in seconds
    static const float kZoomHalf[] = { 5.f, 10.f, 15.f, 30.f, 60.f };
    static const int   kZoomCount  = 5;
    m_pianoRollZoomIdx = std::clamp(m_pianoRollZoomIdx, 0, kZoomCount - 1);
    const float halfWin = kZoomHalf[m_pianoRollZoomIdx];
    const float winStart = now - halfWin;
    const float winEnd   = now + halfWin;
    const float winDur   = winEnd - winStart;

    // Layout: each player row holds their own casts (icons on a cast line) and, under it, a thin lane
    // with the casts aimed at them, in the caster's team colour. Around it: the match's spikes as a bar on
    // top, the spike at the playhead as a card on the right, one card per Monk at the bottom. The roll's
    // rows grow to fill the height left.
    constexpr float kNameColW   = 180.f;
    constexpr float kTimeAxisH  = 24.f;
    constexpr float kTeamLabelH = 24.f;
    constexpr float kSpikeBarH  = 54.f;
    constexpr float kCardW      = 320.f;
    constexpr float kMonkH      = 124.f;

    const int nBlue = (int)m_team1PlayerIds.size();
    const int nRed  = (int)m_team2PlayerIds.size();
    const int nBlueRows = m_pianoRollTeam1Open ? nBlue : 0;
    const int nRedRows  = m_pianoRollTeam2Open ? nRed  : 0;
    const int nRows = std::max(1, nBlueRows + nRedRows);
    const float minRowH = m_pianoRollLanes ? 38.f : 28.f;
    const float totalH = 32.f + kSpikeBarH + kTimeAxisH + 2.f * kTeamLabelH + nRows * minRowH * 1.3f + kMonkH;

    const float screenW = io.DisplaySize.x;

    // Panel window — Morale-style: standard ImGui title bar, matching colors
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.078f, 0.102f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,     ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::SetNextWindowSizeConstraints(ImVec2(500.f, 0.f), ImVec2(screenW, io.DisplaySize.y));
    if (m_panelLayout.HasSavedPosition("piano_roll"))
        m_panelLayout.ApplyPosition("piano_roll");
    else
        ImGui::SetNextWindowPos(ImVec2(screenW - 720.f, 80.f), ImGuiCond_FirstUseEver);
    if (m_panelLayout.HasSavedSize("piano_roll"))
        m_panelLayout.ApplySize("piano_roll");
    else
        ImGui::SetNextWindowSize(ImVec2(760.f, totalH + 30.f), ImGuiCond_FirstUseEver);

    const bool open = ImGui::Begin("Piano Roll", &m_showPianoRoll, ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();   // the zero padding is for the panel only; its tooltips keep the normal one
    if (!open)
    {
        m_panelLayout.TrackWindow("piano_roll");
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        return;
    }

    m_panelLayout.TrackWindow("piano_roll");

    // Clamp to viewport
    {
        const auto* vp = ImGui::GetMainViewport();
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSz  = ImGui::GetWindowSize();
        float cx = std::clamp(wPos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - wSz.x);
        float cy = std::clamp(wPos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - wSz.y);
        if (cx != wPos.x || cy != wPos.y) ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    const int cardSpike = PianoRollCardSpike(now);
    DrawPianoRollSpikeBar(winStart, winEnd, cardSpike);

    const float availH = ImGui::GetContentRegionAvail().y;
    const float rollH = std::max(100.f, availH - kMonkH);
    const float rollW = std::max(300.f, ImGui::GetContentRegionAvail().x - kCardW);
    const float fitRowH = (rollH - kTimeAxisH - 2.f * kTeamLabelH - 2.f) / nRows;
    const float kRowH = std::floor(std::clamp(fitRowH, minRowH, m_pianoRollLanes ? 64.f : 48.f));
    const float kInLaneH = m_pianoRollLanes ? std::round(std::clamp(kRowH * 0.26f, 10.f, 16.f)) : 0.f;
    const float kCastLaneH = kRowH - kInLaneH - 2.f;
    const float kIconSz = std::round(std::clamp(kCastLaneH - 9.f, 18.f, 30.f));

    // The roll in its own child: it scrolls only when the window is too short for the minimum rows.
    ImGui::BeginChild("##pr_roll", ImVec2(rollW, rollH), false, ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();   // the roll's own widgets keep the normal spacing
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const float panelW = ImGui::GetContentRegionAvail().x;
    const float tlLeft = cp.x + kNameColW;
    const float tlW = panelW - kNameColW;
    const float tlRight = tlLeft + tlW;
    auto timeToX = [&](float t) { return tlLeft + (t - winStart) / winDur * tlW; };
    float curY = cp.y;

    // Wheel over the timeline zooms; over the names it scrolls the rows.
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.f)
    {
        const float wheel = io.MouseWheel;
        if (io.MousePos.x < tlLeft)
            ImGui::SetScrollY(ImGui::GetScrollY() - wheel * 40.f);
        else if (wheel > 0.f && m_pianoRollZoomIdx > 0) { m_pianoRollZoomIdx--; m_pianoRollZoomToast = (float)ImGui::GetTime(); }
        else if (wheel < 0.f && m_pianoRollZoomIdx < kZoomCount - 1) { m_pianoRollZoomIdx++; m_pianoRollZoomToast = (float)ImGui::GetTime(); }
    }

    // ── Ruler: match clock, a label every few seconds, ticks every half second when there is room ──
    const float pps = tlW / winDur;
    float labelStep = 1.f;
    for (float st : { 1.f, 2.f, 5.f, 10.f, 15.f, 30.f })
        if (st * pps >= 56.f) { labelStep = st; break; }
    {
        const float axisTop = curY;
        dl->AddRectFilled(ImVec2(cp.x, axisTop), ImVec2(cp.x + panelW, axisTop + kTimeAxisH), IM_COL32(0, 0, 0, 40));
        const float tickStep = pps * 0.5f >= 6.f ? 0.5f : labelStep;
        for (float s = std::ceil(winStart / tickStep) * tickStep; s <= winEnd; s += tickStep)
        {
            const float x = std::round(timeToX(s));
            const bool labelled = std::fabs(std::fmod(s, labelStep)) < 1e-3f || std::fabs(std::fmod(s, labelStep) - labelStep) < 1e-3f;
            dl->AddLine(ImVec2(x, axisTop + kTimeAxisH - (labelled ? 7.f : 4.f)), ImVec2(x, axisTop + kTimeAxisH),
                        IM_COL32(255, 255, 255, labelled ? 150 : 80));
            if (labelled && s >= 0.f)
            {
                char lbl[16];
                const int m = static_cast<int>(s) / 60;
                snprintf(lbl, sizeof(lbl), "%d:%02d", m, static_cast<int>(s) - m * 60);
                const ImVec2 ts = font->CalcTextSizeA(13.f, FLT_MAX, 0.f, lbl);
                const float tx = x - ts.x * 0.5f;
                if (tx > tlLeft - 4.f && tx + ts.x < tlRight)
                    dl->AddText(font, 13.f, ImVec2(tx, axisTop + 2.f), IM_COL32(0xB0, 0xAA, 0x98, 0xFF), lbl);
            }
        }
        // The incoming lanes toggle sits in the ruler's corner, above the names.
        ImGui::SetCursorScreenPos(ImVec2(cp.x + 8.f, axisTop + 3.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.66f, 0.70f, 1.f));
        ImGui::Checkbox("Incoming lanes", &m_pianoRollLanes);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Under each player, a thin line for the casts aimed at them,\nin the caster's team colour.");
        curY += kTimeAxisH;
    }

    const float bodyTop = curY;
    const float nowLineX = timeToX(now);

    // Helper: snapshot for agent at time
    auto getSnap = [&](int agentId, float t) -> const AgentSnapshot*
    {
        auto ait = m_replayCtx.agents.find(agentId);
        if (ait == m_replayCtx.agents.end()) return nullptr;
        const auto& snaps = ait->second.snapshots;
        if (snaps.empty()) return nullptr;
        if (t <= snaps.front().time) return &snaps.front();
        if (t >= snaps.back().time)  return &snaps.back();
        int lo = 0, hi = (int)snaps.size() - 1;
        while (lo < hi) { int mid = lo + (hi - lo + 1) / 2; if (snaps[mid].time <= t) lo = mid; else hi = mid - 1; }
        return &snaps[lo];
    };

    // Casts in the window, a long one that began before it included.
    auto forEachInWindow = [&](const AgentReplayData& ard, auto&& fn) {
        const auto& hist = ard.skillUseHistory;
        auto it = std::lower_bound(hist.begin(), hist.end(), winStart - 10.f,
            [](const SkillUseEvent& ev, float t) { return ev.startTime < t; });
        for (; it != hist.end() && it->startTime <= winEnd; ++it)
            if (HitTime(*it) >= winStart) fn(*it);
    };

    std::unordered_map<int, float> rowTopOf;   // laid-out player rows, for the incoming lanes and links
    std::vector<CastRef> hoverPile;            // what the hover table shows this frame
    std::string hoverHeadline;
    const SkillUseEvent* hoverCast = nullptr;
    int hoverCaster = -1;

    // Casts aimed at `target` that hit within the spike window of `t` - the pile a hover lists.
    auto pileOn = [&](int target, float t) {
        std::vector<CastRef> out;
        for (const auto& [id, ard] : m_replayCtx.agents)
            for (const auto& ev : ard.skillUseHistory)
                if (ev.targetId == target && id != target && std::fabs(HitTime(ev) - t) <= kSpikeWindow)
                    out.push_back({ &ev, id });
        return out;
    };
    auto spikeHeadline = [&](int target, float t) -> std::string {
        for (const auto& sp : m_spikes)
            if (sp.target == target && t >= sp.cue - 0.5f && t <= sp.lastHit + 0.5f)
            {
                char buf[96];
                snprintf(buf, sizeof(buf), "Spike: %d damage skills hit in %.2f s", (int)sp.hits.size(), sp.lastHit - sp.firstHit);
                return buf;
            }
        return {};
    };

    // Helper: draw one team section (label + player rows)
    auto drawTeamBlock = [&](const std::vector<int>& playerIds, const char* teamName, bool& expanded,
                             ImU32 teamCol, ImU32 teamBg, ImU32 teamBorder)
    {
        int nPlayers = (int)playerIds.size();

        // Team label row
        ImVec2 tlMin(cp.x, curY);
        ImVec2 tlMax(cp.x + panelW, curY + kTeamLabelH);
        dl->AddRectFilled(tlMin, tlMax, teamBg);
        dl->AddLine(tlMin, ImVec2(tlMax.x, tlMin.y), teamBorder);
        dl->AddLine(ImVec2(tlMin.x, tlMax.y), tlMax, teamBorder);

        // Collapse triangle
        {
            float triX = cp.x + 8.f;
            float triY = curY + kTeamLabelH * 0.5f;
            float triSz = 5.f;
            ImU32 triCol = IM_COL32(255, 255, 255, 178);
            if (expanded)
                dl->AddTriangleFilled(ImVec2(triX, triY - triSz), ImVec2(triX + triSz * 2.f, triY - triSz),
                                      ImVec2(triX + triSz, triY + triSz), triCol);
            else
                dl->AddTriangleFilled(ImVec2(triX, triY - triSz), ImVec2(triX + triSz * 2.f, triY),
                                      ImVec2(triX, triY + triSz), triCol);
        }

        char teamBuf[128];
        if (expanded) snprintf(teamBuf, sizeof(teamBuf), "%s", teamName);
        else          snprintf(teamBuf, sizeof(teamBuf), "%s  (%d players)", teamName, nPlayers);
        dl->AddText(font, 14.f, ImVec2(cp.x + 22.f, curY + (kTeamLabelH - 14.f) * 0.5f), teamCol, teamBuf);

        if (ImGui::IsMouseHoveringRect(tlMin, tlMax) && ImGui::IsMouseClicked(0))
            expanded = !expanded;

        curY += kTeamLabelH;
        if (!expanded) return;

        for (int pi = 0; pi < nPlayers; pi++)
        {
            int agentId = playerIds[pi];
            auto ait = m_replayCtx.agents.find(agentId);
            if (ait == m_replayCtx.agents.end()) { curY += kRowH; continue; }
            const auto& ard = ait->second;

            const float rowTop = curY;
            const float rowBot = curY + kRowH;
            rowTopOf[agentId] = rowTop;

            const bool rowHovered = ImGui::IsMouseHoveringRect(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot));
            if (rowHovered) m_pianoRollHoverRow = agentId;
            const bool pinned = m_pianoRollPinned == agentId;

            const AgentSnapshot* snap = getSnap(agentId, now);
            const bool isDead = snap && snap->is_dead;

            dl->AddLine(ImVec2(cp.x, rowBot), ImVec2(cp.x + panelW, rowBot), IM_COL32(255, 255, 255, 8));
            if (isDead)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(204, 48, 48, 15));
            if (rowHovered)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(255, 255, 255, 10));
            if (pinned)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(0xF0, 0xC8, 0x50, 0x16));
            // The player picked from a party row's context menu (Cast timeline...).
            if (m_pianoRollHighlightAgent == agentId)
            {
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(0xFF, 0xD7, 0x64, 0x18));
                dl->AddRect(ImVec2(cp.x + 1.f, rowTop + 1.f), ImVec2(cp.x + panelW - 1.f, rowBot - 1.f),
                            IM_COL32(0xFF, 0xD7, 0x64, 0xC0), 0.f, 0, 1.5f);
            }

            // Player label: click pins the player's links, double-click opens their info panel.
            {
                const float profIconSz = 18.f;
                ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                const float iconY = rowTop + (kCastLaneH - profIconSz) * 0.5f;
                if (profTex)
                    dl->AddImage(profTex, ImVec2(cp.x + 6.f, iconY), ImVec2(cp.x + 6.f + profIconSz, iconY + profIconSz));

                ImU32 nameCol = isDead ? IM_COL32(0xCC, 0x30, 0x30, 0xFF)
                              : pinned ? IM_COL32(0xF0, 0xC8, 0x50, 0xFF) : TeamColour(ard.teamId, 0xFF);
                // The name cut with ".." only when it does not fit the column.
                std::string name = ard.playerName;
                const float nameMaxW = kNameColW - 26.f - 6.f;
                if (font->CalcTextSizeA(14.f, FLT_MAX, 0.f, name.c_str()).x > nameMaxW)
                {
                    while (!name.empty() && font->CalcTextSizeA(14.f, FLT_MAX, 0.f, (name + "..").c_str()).x > nameMaxW) name.pop_back();
                    name += "..";
                }
                dl->AddText(font, 14.f, ImVec2(cp.x + 26.f, rowTop + (kCastLaneH - 14.f) * 0.5f), nameCol, name.c_str());
                if (m_pianoRollLanes)
                    dl->AddText(font, 11.f, ImVec2(cp.x + 26.f, rowTop + kCastLaneH + (kInLaneH - 11.f) * 0.5f),
                                IM_COL32(0x80, 0x7A, 0x6C, 0xFF), "incoming");

                const bool overName = ImGui::IsMouseHoveringRect(ImVec2(cp.x, rowTop), ImVec2(cp.x + kNameColW, rowBot));
                if (overName && !m_annotationMgr.IsDrawModeActive())
                {
                    if (ImGui::IsMouseDoubleClicked(0))  OpenPlayerInfoPanel(agentId);
                    else if (ImGui::IsMouseClicked(0))   m_pianoRollPinned = pinned ? -1 : agentId;
                    ImGui::SetTooltip("Click: show who %s casts on and who casts on them\nDouble-click: player info",
                                      ard.playerName.c_str());
                }
            }

            // Own casts: the icon at the start, the cast as a line in the team colour under it.
            dl->PushClipRect(ImVec2(tlLeft, rowTop), ImVec2(tlRight, rowBot), true);
            const float iconY = rowTop + 3.f;
            const float lineY = iconY + kIconSz + 1.f;
            forEachInWindow(ard, [&](const SkillUseEvent& ev) {
                const float x0 = std::round(timeToX(ev.startTime));
                const float x1 = std::round(timeToX(HitTime(ev)));
                const bool linked = m_pianoRollPinned < 0 || m_pianoRollPinned == agentId || ev.targetId == m_pianoRollPinned;
                float a = ev.startTime > now ? 0.5f : 1.f;
                if (!linked) a *= 0.25f;
                const ImU32 edge = ev.wasInterrupted ? IM_COL32(0x90, 0x30, 0xD0, 0xFF)
                                 : ev.wasCancelled   ? IM_COL32(0xE0, 0x80, 0x20, 0xFF) : TeamColour(ard.teamId, 0xFF);
                const ImU32 edgeA = (edge & ~IM_COL32_A_MASK) | (static_cast<ImU32>(220 * a) << IM_COL32_A_SHIFT);
                if (x1 > x0)
                    dl->AddRectFilled(ImVec2(x0, lineY), ImVec2(x1, lineY + 2.f), edgeA, 1.f);
                if (ImTextureID tex = LoadSkillIcon(this, dev, ev.skillId, m_skillIconIndex, m_skillIconCache))
                    dl->AddImageRounded(tex, ImVec2(x0, iconY), ImVec2(x0 + kIconSz, iconY + kIconSz),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255 * a)), 2.f);
                dl->AddRect(ImVec2(x0 - 0.5f, iconY - 0.5f), ImVec2(x0 + kIconSz + 0.5f, iconY + kIconSz + 0.5f), edgeA, 2.f, 0, 1.f);

                if (ImGui::IsMouseHoveringRect(ImVec2(x0, iconY), ImVec2(std::max(x1, x0 + kIconSz), lineY + 2.f)))
                {
                    hoverCast = &ev;
                    hoverCaster = agentId;
                    if (ImGui::IsMouseClicked(0)) m_debugTimeline = ev.startTime;
                }
            });
            dl->PopClipRect();

            curY += kRowH;
        }
    };

    drawTeamBlock(m_team1PlayerIds, m_team1GuildHeader.empty() ? "Blue Team" : m_team1GuildHeader.c_str(),
                  m_pianoRollTeam1Open, IM_COL32(0x60, 0x9C, 0xFF, 0xFF), IM_COL32(0x4A, 0x90, 0xD8, 0x14),
                  IM_COL32(0x4A, 0x90, 0xD8, 0x26));
    drawTeamBlock(m_team2PlayerIds, m_team2GuildHeader.empty() ? "Red Team" : m_team2GuildHeader.c_str(),
                  m_pianoRollTeam2Open, IM_COL32(0xE8, 0x50, 0x48, 0xFF), IM_COL32(0xD0, 0x48, 0x48, 0x14),
                  IM_COL32(0xD0, 0x48, 0x48, 0x26));
    const float bodyBot = curY;

    dl->PushClipRect(ImVec2(tlLeft, bodyTop), ImVec2(tlRight, bodyBot), true);

    // Light grid on the ruler's labels.
    for (float s = std::ceil(winStart / labelStep) * labelStep; s <= winEnd; s += labelStep)
        dl->AddLine(ImVec2(std::round(timeToX(s)), bodyTop), ImVec2(std::round(timeToX(s)), bodyBot), IM_COL32(255, 255, 255, 10));

    // Spike bands on the target's row: from the first enemy cast start to the last hit.
    for (size_t si = 0; si < m_spikes.size(); ++si)
    {
        const SpikeInfo& sp = m_spikes[si];
        auto rit = rowTopOf.find(sp.target);
        if (rit == rowTopOf.end() || sp.lastHit < winStart || sp.cue > winEnd) continue;
        const bool onCard = static_cast<int>(si) == cardSpike;
        const float x0 = std::max(timeToX(sp.cue) - 2.f, tlLeft), x1 = std::min(timeToX(sp.lastHit) + 2.f, tlRight);
        dl->AddRectFilled(ImVec2(x0, rit->second + 1.f), ImVec2(x1, rit->second + kRowH - 1.f), IM_COL32(0xE8, 0x50, 0x48, onCard ? 0x30 : 0x1C));
        dl->AddRect(ImVec2(x0, rit->second + 1.f), ImVec2(x1, rit->second + kRowH - 1.f),
                    onCard ? IM_COL32(0xFF, 0x8A, 0x80, 0xFF) : IM_COL32(0xE8, 0x50, 0x48, 0xB0), 2.f, 0, onCard ? 1.5f : 1.f);
        char lbl[24];
        snprintf(lbl, sizeof(lbl), "SPIKE x%d", (int)sp.hits.size());
        dl->AddText(font, 12.f, ImVec2(x1 + 4.f, rit->second + 2.f), IM_COL32(0xFF, 0x8A, 0x80, 0xFF), lbl);
    }

    // Incoming lanes: every cast aimed at a laid-out player, as a mark at its hit with a thin line
    // back to its start, in the caster's team colour.
    if (m_pianoRollLanes)
        for (const auto& [casterId, caster] : m_replayCtx.agents)
            forEachInWindow(caster, [&](const SkillUseEvent& ev) {
                if (ev.targetId < 0 || ev.targetId == casterId) return;
                auto rit = rowTopOf.find(ev.targetId);
                if (rit == rowTopOf.end()) return;
                const bool linked = m_pianoRollPinned < 0 || m_pianoRollPinned == casterId || ev.targetId == m_pianoRollPinned;
                const int alpha = (int)((ev.startTime > now ? 120 : 230) * (linked ? 1.f : 0.25f));
                const ImU32 col = TeamColour(caster.teamId, alpha);
                const float laneY = rit->second + kCastLaneH;
                const float xs = std::round(timeToX(ev.startTime)), xh = std::round(timeToX(HitTime(ev)));
                if (xh > xs) dl->AddLine(ImVec2(xs, laneY + kInLaneH * 0.5f), ImVec2(xh, laneY + kInLaneH * 0.5f), col, 1.f);
                dl->AddRectFilled(ImVec2(xh - 1.f, laneY + 1.f), ImVec2(xh + 2.f, laneY + kInLaneH - 1.f), col);
                if (ImGui::IsMouseHoveringRect(ImVec2(xh - 3.f, laneY), ImVec2(xh + 4.f, laneY + kInLaneH)))
                {
                    hoverPile = pileOn(ev.targetId, HitTime(ev));
                    hoverHeadline = spikeHeadline(ev.targetId, HitTime(ev));
                }
            });

    // Links: from a cast to the moment it hits its target's lane - the hovered cast, or every cast by
    // and on the pinned player.
    auto drawLink = [&](const SkillUseEvent& ev, int casterId) {
        auto a = rowTopOf.find(casterId), b = rowTopOf.find(ev.targetId);
        if (a == rowTopOf.end() || b == rowTopOf.end() || ev.targetId == casterId) return;
        auto cit = m_replayCtx.agents.find(casterId);
        const ImU32 col = TeamColour(cit != m_replayCtx.agents.end() ? cit->second.teamId : 0, 220);
        const ImVec2 p0(timeToX(ev.startTime) + kIconSz, a->second + 3.f + kIconSz * 0.5f);
        const ImVec2 p1(timeToX(HitTime(ev)), b->second + (m_pianoRollLanes ? kCastLaneH + kInLaneH * 0.5f : kCastLaneH * 0.5f));
        const float bend = std::max(24.f, std::fabs(p1.x - p0.x) * 0.5f);
        dl->AddBezierCubic(p0, ImVec2(p0.x + bend, p0.y), ImVec2(p1.x - bend, p1.y), p1, col, 1.5f);
        dl->AddCircleFilled(p1, 3.f, IM_COL32(0xF0, 0xC8, 0x50, 0xFF));
    };
    if (m_pianoRollPinned >= 0)
        for (const auto& [casterId, caster] : m_replayCtx.agents)
            forEachInWindow(caster, [&](const SkillUseEvent& ev) {
                if (casterId == m_pianoRollPinned || ev.targetId == m_pianoRollPinned) drawLink(ev, casterId);
            });
    if (hoverCast)
    {
        drawLink(*hoverCast, hoverCaster);
        if (hoverCast->targetId >= 0 && hoverCast->targetId != hoverCaster)
        {
            // The hovered cast with the others its side landed on the same target around it.
            auto cit = m_replayCtx.agents.find(hoverCaster);
            const uint8_t team = cit != m_replayCtx.agents.end() ? cit->second.teamId : 0;
            for (const auto& c : pileOn(hoverCast->targetId, HitTime(*hoverCast)))
            {
                auto oit = m_replayCtx.agents.find(c.casterId);
                if (oit != m_replayCtx.agents.end() && oit->second.teamId == team) hoverPile.push_back(c);
            }
            hoverHeadline = spikeHeadline(hoverCast->targetId, HitTime(*hoverCast));
        }
        else
            hoverPile = { { hoverCast, hoverCaster } };
    }

    // ── Now line ───────────────────────────────────────────────────────
    dl->AddLine(ImVec2(nowLineX, bodyTop), ImVec2(nowLineX, bodyBot), IM_COL32(0xF0, 0xC8, 0x50, 0xD0), 1.5f);

    // Edge fades
    const ImU32 kFadeSolid = IM_COL32(14, 20, 26, 234);
    const ImU32 kFadeTrans = IM_COL32(14, 20, 26, 0);
    dl->AddRectFilledMultiColor(ImVec2(tlLeft, bodyTop), ImVec2(tlLeft + 16.f, bodyBot), kFadeSolid, kFadeTrans, kFadeTrans, kFadeSolid);
    dl->AddRectFilledMultiColor(ImVec2(tlRight - 16.f, bodyTop), ImVec2(tlRight, bodyBot), kFadeTrans, kFadeSolid, kFadeSolid, kFadeTrans);
    dl->PopClipRect();

    if (!hoverPile.empty())
        DrawCastTableTooltip(hoverPile, hoverHeadline.empty() ? nullptr : hoverHeadline.c_str());

    // ── Zoom toast ─────────────────────────────────────────────────────
    {
        float elapsed = (float)ImGui::GetTime() - m_pianoRollZoomToast;
        if (elapsed < 1.2f)
        {
            float alpha = (elapsed < 0.8f) ? 1.f : 1.f - (elapsed - 0.8f) / 0.4f;
            char zoomTxt[16];
            snprintf(zoomTxt, sizeof(zoomTxt), "Â± %.0fs", halfWin);
            ImVec2 ts = font->CalcTextSizeA(12.f, FLT_MAX, 0.f, zoomTxt);
            float tx = tlLeft + tlW * 0.5f - ts.x * 0.5f;
            float ty = bodyTop + 8.f;
            dl->AddRectFilled(ImVec2(tx - 8.f, ty - 4.f), ImVec2(tx + ts.x + 8.f, ty + ts.y + 4.f), IM_COL32(0, 0, 0, (int)(180 * alpha)), 6.f);
            dl->AddText(font, 12.f, ImVec2(tx, ty), IM_COL32(255, 255, 255, (int)(220 * alpha)), zoomTxt);
        }
    }

    // Advance cursor so ImGui knows the content size
    ImGui::SetCursorScreenPos(ImVec2(cp.x, curY));
    ImGui::Dummy(ImVec2(panelW, 1.f));
    ImGui::EndChild();

    // The spike at the playhead, beside the roll; the Monks under both.
    ImGui::SameLine(0.f, 0.f);
    ImGui::BeginChild("##pr_card", ImVec2(0.f, rollH), false);
    DrawPianoRollSpikeCard(cardSpike);
    ImGui::EndChild();
    DrawPianoRollMonkCards();

    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        m_pianoRollHoverRow = -1;

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}




// ---------------------------------------------------------------------------
// Around the roll: the match's spikes as a bar, the spike at the playhead as a card, one card per Monk.
// Every label carries a (?) with a one-line explanation; the numbers are shown, never judged.
// ---------------------------------------------------------------------------

namespace
{
    const ImVec4 kPrGold(1.f, 0.84f, 0.30f, 1.f);
    const ImVec4 kPrDim(0.62f, 0.60f, 0.54f, 1.f);
    const ImVec4 kPrSpike(1.f, 0.54f, 0.50f, 1.f);
    constexpr ImU32 kPrRule = IM_COL32(0x66, 0x54, 0x26, 0x66);   // the panel's separator colour

    ImVec4 PrTeamText(uint8_t team)
    {
        return team == 1 ? ImVec4(0.45f, 0.66f, 1.f, 1.f) : team == 2 ? ImVec4(1.f, 0.45f, 0.40f, 1.f)
                                                          : ImVec4(0.85f, 0.85f, 0.85f, 1.f);
    }

    void PrClock(float t, char* buf, size_t n)
    {
        const int m = static_cast<int>(t / 60.f);
        snprintf(buf, n, "%d:%04.1f", m, t - m * 60.f);
    }

    // A (?) with a wrapped explanation on hover.
    void PrHelp(const char* help)
    {
        ImGui::TextDisabled("(?)");
        if (!ImGui::IsItemHovered()) return;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // The call: when the SECOND enemy starts casting on the player - the first moment the casts read as a
    // spike rather than one cast. Reaction is counted from here (owner, 2026-10-27).
    template <class Hits> float PrCall(const Hits& hits)
    {
        std::vector<float> starts;
        for (const auto& h : hits) starts.push_back(h.ev->startTime);
        std::sort(starts.begin(), starts.end());
        return starts.size() > 1 ? starts[1] : starts.empty() ? 0.f : starts[0];
    }

    float PrMedian(std::vector<float> v)
    {
        if (v.empty()) return NAN;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
}

// The spike the card shows: the one picked with the arrows or the bar while the playhead is near it,
// else the last one whose first cast has started, until 8 s after its last hit.
int ReplayWindow::PianoRollCardSpike(float now) const
{
    if (m_pianoRollSpikeSel >= 0 && m_pianoRollSpikeSel < static_cast<int>(m_spikes.size()))
    {
        const SpikeInfo& sp = m_spikes[m_pianoRollSpikeSel];
        if (now >= sp.cue - 1.5f && now <= sp.lastHit + 8.f) return m_pianoRollSpikeSel;
    }
    int best = -1;
    for (size_t i = 0; i < m_spikes.size(); ++i)
        if (m_spikes[i].cue - 1.f <= now && now - m_spikes[i].lastHit < 8.f) best = static_cast<int>(i);
    return best;
}

// One mark per spike for the whole match: spikes on the blue team above the line, on the red team
// below it, in the colour of the team that cast them; the taller, the more skills hit. The gold box is
// the part of the match the roll shows. Click a mark to jump to it, click or drag elsewhere to scrub.
void ReplayWindow::DrawPianoRollSpikeBar(float winStart, float winEnd, int cardSpike)
{
    constexpr float kH = 54.f, kLabelW = 180.f, kPad = 8.f;
    static bool sDragScrub = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    const float bx0 = p0.x + kLabelW + kPad, bx1 = p0.x + w - kPad;
    const float mid = std::round(p0.y + kH * 0.5f);
    auto X = [&](float t) { return bx0 + std::clamp(t / maxT, 0.f, 1.f) * (bx1 - bx0); };

    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + kH), IM_COL32(0, 0, 0, 40));
    dl->AddLine(ImVec2(p0.x, p0.y + kH - 0.5f), ImVec2(p0.x + w, p0.y + kH - 0.5f), kPrRule);
    dl->AddLine(ImVec2(p0.x + kLabelW, p0.y), ImVec2(p0.x + kLabelW, p0.y + kH), kPrRule);

    // Label column.
    dl->AddText(font, 16.f, ImVec2(p0.x + 10.f, p0.y + 7.f), ImGui::ColorConvertFloat4ToU32(kPrGold), "Spikes");
    char buf[96];
    snprintf(buf, sizeof(buf), "%d in the match", static_cast<int>(m_spikes.size()));
    dl->AddText(font, 14.f, ImVec2(p0.x + 10.f, p0.y + 30.f), ImGui::ColorConvertFloat4ToU32(kPrDim), buf);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.f + font->CalcTextSizeA(16.f, FLT_MAX, 0.f, "Spikes").x, p0.y + 7.f));
    PrHelp("One mark = one spike: 3+ enemy damage skills on one player in 1.5 s.\n"
           "Top: spikes on blue. Bottom: spikes on red.\n"
           "Taller = more skills. Click a mark to jump to it.");

    // Team lanes.
    dl->AddRectFilled(ImVec2(p0.x + kLabelW + 1.f, p0.y), ImVec2(p0.x + w, mid - 1.f), IM_COL32(0x60, 0x9C, 0xFF, 0x0A));
    dl->AddRectFilled(ImVec2(p0.x + kLabelW + 1.f, mid + 1.f), ImVec2(p0.x + w, p0.y + kH - 1.f), IM_COL32(0xE8, 0x50, 0x48, 0x0A));
    dl->AddLine(ImVec2(bx0, mid), ImVec2(bx1, mid), IM_COL32(255, 255, 255, 22));
    for (float m = 60.f; m < maxT; m += 60.f)
        dl->AddLine(ImVec2(std::round(X(m)), mid - 3.f), ImVec2(std::round(X(m)), mid + 3.f), IM_COL32(255, 255, 255, 50));

    // Marks.
    const AgentReplayData* monk = nullptr;
    if (auto it = m_replayCtx.agents.find(m_pianoRollMonkSel); it != m_replayCtx.agents.end()) monk = &it->second;
    const bool overBar = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(bx0 - kPad, p0.y), ImVec2(bx1 + kPad, p0.y + kH));
    int hovered = -1;
    float hoverD = 5.f;
    for (size_t i = 0; i < m_spikes.size(); ++i)
    {
        const SpikeInfo& sp = m_spikes[i];
        auto tit = m_replayCtx.agents.find(sp.target);
        const uint8_t team = tit != m_replayCtx.agents.end() ? tit->second.teamId : 0;
        const bool top = team != 2;
        const float x = std::round(X(sp.firstHit));
        const float h = std::min(6.f + static_cast<float>(sp.hits.size()) * 3.2f, kH * 0.5f - 9.f);
        const bool faded = monk && team != monk->teamId;
        const ImU32 col = TeamColour(team == 1 ? 2 : 1, faded ? 0x30 : 0xE6);
        const float y0 = top ? mid - 2.f - h : mid + 2.f, y1 = y0 + h;
        dl->AddRectFilled(ImVec2(x - 1.5f, y0), ImVec2(x + 1.5f, y1), col);
        // The picked Monk: a gold dot on the spikes they answered, a ring on the others of their team.
        if (monk && !faded)
        {
            bool answered = false;
            for (const auto& a : sp.answers) answered |= a.casterId == m_pianoRollMonkSel;
            const ImVec2 c(x, top ? y0 - 5.f : y1 + 5.f);
            if (answered) dl->AddCircleFilled(c, 2.6f, IM_COL32(0xF0, 0xC8, 0x50, 0xFF));
            else          dl->AddCircle(c, 2.6f, IM_COL32(0xF0, 0xC8, 0x50, 0x99), 0, 1.f);
        }
        if (static_cast<int>(i) == cardSpike)
            dl->AddRect(ImVec2(x - 3.5f, y0 - 2.f), ImVec2(x + 3.5f, y1 + 2.f), IM_COL32(255, 255, 255, 0xE0), 1.f);
        if (overBar && std::fabs(ImGui::GetIO().MousePos.x - x) < hoverD)
        {
            hoverD = std::fabs(ImGui::GetIO().MousePos.x - x);
            hovered = static_cast<int>(i);
        }
    }

    // The roll's window and the playhead.
    const float wx0 = std::round(X(winStart)), wx1 = std::round(X(winEnd)), nx = std::round(X(m_debugTimeline));
    dl->AddRectFilled(ImVec2(wx0, p0.y + 1.f), ImVec2(wx1, p0.y + kH - 2.f), IM_COL32(0xF0, 0xC8, 0x50, 0x12));
    dl->AddRect(ImVec2(wx0, p0.y + 1.f), ImVec2(wx1, p0.y + kH - 2.f), IM_COL32(0xF0, 0xC8, 0x50, 0xB0), 0.f, 0, 1.f);
    dl->AddLine(ImVec2(nx, p0.y), ImVec2(nx, p0.y + kH - 1.f), IM_COL32(0xF0, 0xC8, 0x50, 0xFF), 1.5f);

    // Click a mark to jump to its spike; click or drag elsewhere to scrub.
    ImGui::SetCursorScreenPos(ImVec2(bx0 - kPad, p0.y));
    ImGui::InvisibleButton("##pr_spikebar", ImVec2(bx1 - bx0 + 2.f * kPad, kH));
    const float mouseT = std::clamp((ImGui::GetIO().MousePos.x - bx0) / std::max(1.f, bx1 - bx0), 0.f, 1.f) * maxT;
    if (ImGui::IsItemActivated())
    {
        sDragScrub = hovered < 0;
        if (hovered >= 0)
        {
            const SpikeInfo& sp = m_spikes[hovered];
            m_debugTimeline = std::max(0.f, sp.cue - 1.f);
            m_pianoRollSpikeSel = hovered;
            m_pianoRollPinned = sp.target;
        }
    }
    if (ImGui::IsItemActive() && sDragScrub) m_debugTimeline = mouseT;
    if (ImGui::IsItemHovered() && hovered >= 0 && !ImGui::IsItemActive())
    {
        const SpikeInfo& sp = m_spikes[hovered];
        ImGui::BeginTooltip();
        if (auto tit = m_replayCtx.agents.find(sp.target); tit != m_replayCtx.agents.end())
            ImGui::TextColored(PrTeamText(tit->second.teamId), "%s", tit->second.playerName.c_str());
        ImGui::SameLine();
        PrClock(sp.firstHit, buf, sizeof(buf));
        ImGui::TextColored(kPrGold, "%s", buf);
        ImGui::Text("%d skills hit in", static_cast<int>(sp.hits.size()));
        ImGui::SameLine(0.f, 4.f); ImGui::TextColored(kPrGold, "%.2f s", sp.lastHit - sp.firstHit);
        if (sp.lowHp >= 0.f)
        {
            if (sp.lowHp <= 0.f) ImGui::Text("The player died.");
            else                 ImGui::Text("Lowest HP %d%%", static_cast<int>(std::round(sp.lowHp * 100.f)));
        }
        if (sp.answers.empty()) ImGui::TextColored(kPrDim, "No prot or heal on the player.");
        else
        {
            float land = FLT_MAX;
            for (const auto& a : sp.answers) land = std::min(land, HitTime(*a.ev));
            ImGui::Text("First answer took effect");
            ImGui::SameLine(0.f, 4.f); ImGui::TextColored(kPrGold, "%+.2f s", land - sp.firstHit);
            ImGui::SameLine(0.f, 4.f); ImGui::TextColored(kPrDim, "from the first hit");
        }
        ImGui::EndTooltip();
    }
    else if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
    {
        const int m = static_cast<int>(mouseT) / 60;
        ImGui::SetTooltip("%d:%02d", m, static_cast<int>(mouseT) - m * 60);
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + kH));
}

// The spike at the playhead: who took it, how hard, the hits, and the team's answers.
void ReplayWindow::DrawPianoRollSpikeCard(int cardSpike)
{
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x + 0.5f, wp.y), ImVec2(wp.x + 0.5f, wp.y + ImGui::GetWindowHeight()), kPrRule);

    constexpr float kMargin = 12.f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 4.f));
    // Indent, not a group: SameLine(x) inside a group is offset by the group's start.
    ImGui::SetCursorPosY(8.f);
    ImGui::Indent(kMargin);
    const float cw = ImGui::GetContentRegionAvail().x - kMargin - ImGui::GetStyle().ScrollbarSize;
    const float lineH = ImGui::GetTextLineHeight();
    const float iconSz = std::round(lineH + 4.f);
    char buf[96];

    auto agentOf = [&](int id) -> const AgentReplayData* {
        auto it = m_replayCtx.agents.find(id);
        return it != m_replayCtx.agents.end() ? &it->second : nullptr;
    };
    auto isHeal = [&](int skillId) {
        const SkillInfo* si = m_skillView.IsLoaded() ? m_skillView.Get(skillId) : nullptr;
        if (!si || si->attribute == 15) return false;
        for (const auto& sc : si->scales)
            if (sc.kind == SkillScaleKind::Heal) return true;
        return false;
    };
    // Text centred on an icon of height h drawn just before it on the same line.
    auto vText = [&](float h, const ImVec4& col, const char* text) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - lineH) * 0.5f);
        ImGui::TextColored(col, "%s", text);
    };
    // Text right-aligned to the card's edge, on the current line.
    auto rightText = [&](float h, const ImVec4& col, const char* text) {
        ImGui::SameLine(kMargin + cw - ImGui::CalcTextSize(text).x);
        vText(h, col, text);
    };
    auto who = [&](int id, float sz) {
        const AgentReplayData* a = agentOf(id);
        if (!a) return;
        if (ImTextureID prof = a->primaryProf > 0 ? LoadProfIcon(dev, a->primaryProf) : nullptr)
        {
            ImGui::Image(prof, ImVec2(sz, sz));
            ImGui::SameLine(0.f, 4.f);
        }
        vText(sz, PrTeamText(a->teamId), a->playerName.c_str());
    };
    auto section = [&](const char* title, const char* help, const char* right) {
        ImGui::Dummy(ImVec2(1.f, 2.f));
        ImGui::TextColored(kPrDim, "%s", title);
        if (help) { ImGui::SameLine(0.f, 4.f); PrHelp(help); }
        if (right) { ImGui::SameLine(kMargin + cw - ImGui::CalcTextSize(right).x); ImGui::TextColored(kPrDim, "%s", right); }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y - 1.f), ImVec2(p.x + cw, p.y - 1.f), kPrRule);
        ImGui::Dummy(ImVec2(1.f, 2.f));
    };

    // ── Stepper: previous / next spike (of the picked Monk's team, when one is picked) ──
    {
        std::vector<int> list;
        const AgentReplayData* monk = agentOf(m_pianoRollMonkSel);
        for (size_t i = 0; i < m_spikes.size(); ++i)
        {
            const AgentReplayData* t = agentOf(m_spikes[i].target);
            if (!monk || (t && t->teamId == monk->teamId)) list.push_back(static_cast<int>(i));
        }
        const auto cur = std::find(list.begin(), list.end(), cardSpike);
        const float now = m_debugTimeline;
        auto go = [&](int idx) {
            if (idx < 0) return;
            m_pianoRollSpikeSel = idx;
            m_debugTimeline = std::max(0.f, m_spikes[idx].cue - 1.f);
            m_pianoRollPinned = m_spikes[idx].target;
        };
        if (ImGui::ArrowButton("##pr_prev", ImGuiDir_Left))
        {
            int idx = -1;
            if (cur != list.end()) { if (cur != list.begin()) idx = *(cur - 1); }
            else for (int i : list) if (m_spikes[i].cue < now - 1.1f) idx = i;
            go(idx);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous spike");
        if (cur != list.end()) snprintf(buf, sizeof(buf), "Spike %d / %d", static_cast<int>(cur - list.begin()) + 1, static_cast<int>(list.size()));
        else                   snprintf(buf, sizeof(buf), "%d spikes", static_cast<int>(list.size()));
        const float arrowW = ImGui::GetFrameHeight();
        ImGui::SameLine(kMargin + (cw - ImGui::CalcTextSize(buf).x) * 0.5f);
        vText(arrowW, kPrGold, buf);
        ImGui::SameLine(kMargin + cw - arrowW);
        if (ImGui::ArrowButton("##pr_next", ImGuiDir_Right))
        {
            int idx = -1;
            if (cur != list.end()) { if (cur + 1 != list.end()) idx = *(cur + 1); }
            else for (int i : list) if (m_spikes[i].cue > now) { idx = i; break; }
            go(idx);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(monk ? "Next spike on %s's team" : "Next spike", monk ? monk->playerName.c_str() : "");
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y + 2.f), ImVec2(p.x + cw, p.y + 2.f), kPrRule);
        ImGui::Dummy(ImVec2(1.f, 6.f));
    }

    if (cardSpike < 0 || cardSpike >= static_cast<int>(m_spikes.size()))
    {
        ImGui::PushTextWrapPos(kMargin + cw);
        ImGui::TextColored(kPrDim, "No spike here. Click a mark on the bar above, or use the arrows, to go to one.");
        ImGui::PopTextWrapPos();
        ImGui::Unindent(kMargin);
        ImGui::PopStyleVar();
        return;
    }
    const SpikeInfo& sp = m_spikes[cardSpike];

    // ── Title: SPIKE, its time, the player who took it ──
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(font, 15.f, p, ImGui::ColorConvertFloat4ToU32(kPrSpike), "SPIKE");
        ImGui::Dummy(ImVec2(1.f, 15.f));
        PrClock(sp.firstHit, buf, sizeof(buf));
        rightText(15.f, kPrGold, buf);
        ImGui::TextColored(kPrDim, "on");
        ImGui::SameLine(0.f, 6.f);
        who(sp.target, iconSz);
    }

    // ── Three facts in boxes ──
    {
        ImGui::Dummy(ImVec2(1.f, 2.f));
        constexpr float kGap = 6.f, kBoxH = 52.f;
        const float boxW = std::floor((cw - 2.f * kGap) / 3.f);
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        struct Fact { const char* label; const char* help; std::string value; ImVec4 col; float bar; };
        char v1[16], v2[16], v3[16];
        snprintf(v1, sizeof(v1), "%d", static_cast<int>(sp.hits.size()));
        snprintf(v2, sizeof(v2), "%.2f s", sp.lastHit - sp.firstHit);
        if (sp.lowHp < 0.f)       snprintf(v3, sizeof(v3), "-");
        else if (sp.lowHp <= 0.f) snprintf(v3, sizeof(v3), "died");
        else                      snprintf(v3, sizeof(v3), "%d%%", static_cast<int>(std::round(sp.lowHp * 100.f)));
        const Fact facts[3] = {
            { "Skills",    "How many enemy skills hit the player. 3 or more within 1.5 seconds is a spike.", v1, ImVec4(0.92f, 0.92f, 0.92f, 1.f), -1.f },
            { "Hit in",    "How close together the hits landed. The smaller the number, the harder the spike is to stop.", v2, kPrGold, -1.f },
            { "Lowest HP", "How low the player's health went. 0% means they died.", v3, ImVec4(0.92f, 0.92f, 0.92f, 1.f), std::max(0.f, sp.lowHp) },
        };
        for (int i = 0; i < 3; ++i)
        {
            const Fact& f = facts[i];
            const ImVec2 r0(b0.x + i * (boxW + kGap), b0.y), r1(r0.x + boxW, r0.y + kBoxH);
            ImGui::SetCursorScreenPos(r0);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##fact", ImVec2(boxW, kBoxH));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(r0, r1, hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10), 3.f);
            dl->AddText(font, 13.f, ImVec2(r0.x + 7.f, r0.y + 4.f), ImGui::ColorConvertFloat4ToU32(kPrDim), f.label);
            dl->AddText(font, 19.f, ImVec2(r0.x + 7.f, r0.y + 20.f), ImGui::ColorConvertFloat4ToU32(f.col), f.value.c_str());
            if (f.bar >= 0.f)
            {
                const float by = r1.y - 6.f;
                dl->AddRectFilled(ImVec2(r0.x + 7.f, by), ImVec2(r1.x - 7.f, by + 3.f), IM_COL32(255, 255, 255, 22), 1.5f);
                if (f.bar > 0.f)
                    dl->AddRectFilled(ImVec2(r0.x + 7.f, by), ImVec2(r0.x + 7.f + (boxW - 14.f) * std::min(f.bar, 1.f), by + 3.f), IM_COL32(0xC8, 0x3C, 0x34, 0xE0), 1.5f);
            }
            if (hov)
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.f);
                ImGui::TextUnformatted(f.help);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(b0.x, b0.y + kBoxH + 4.f));
    }

    // One line per cast: icon + skill, a value on the right, "by" + caster under it.
    auto castLine = [&](const CastRef& c, const char* tag, const char* right, const ImVec4& rightCol, const char* extra = nullptr) {
        if (ImTextureID tex = LoadSkillIcon(this, dev, c.ev->skillId, m_skillIconIndex, m_skillIconCache))
            ImGui::Image(tex, ImVec2(iconSz, iconSz));
        else
            ImGui::Dummy(ImVec2(iconSz, iconSz));
        ImGui::SameLine(0.f, 6.f);
        vText(iconSz, ImVec4(1.f, 1.f, 1.f, 1.f), GetSkillDisplayName(c.ev->skillId).c_str());
        if (tag) { ImGui::SameLine(0.f, 6.f); vText(iconSz, kPrDim, tag); }
        rightText(iconSz, rightCol, right);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iconSz + 6.f);
        ImGui::TextColored(kPrDim, "by");
        ImGui::SameLine(0.f, 5.f);
        who(c.casterId, lineH);
        if (extra) { ImGui::SameLine(0.f, 8.f); vText(lineH, kPrDim, extra); }
        ImGui::Dummy(ImVec2(1.f, 2.f));
    };

    // ── Hits ──
    section("HITS", nullptr, "hits at");
    {
        const ImVec2 h0 = ImGui::GetCursorScreenPos();
        for (const CastRef& h : sp.hits)
        {
            PrClock(HitTime(*h.ev), buf, sizeof(buf));
            castLine(h, nullptr, buf, kPrGold);
        }
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(h0, ImVec2(h0.x + cw, ImGui::GetCursorScreenPos().y)))
        {
            snprintf(buf, sizeof(buf), "Spike: %d damage skills hit in %.2f s", static_cast<int>(sp.hits.size()), sp.lastHit - sp.firstHit);
            DrawCastTableTooltip(sp.hits, buf);
        }
    }

    // ── Answers ──
    section("ANSWERS", "Prots and heals from teammates.\n"
                       "Timing: when it landed. 0 = the first hit, below 0 = up before the damage.\n"
                       "Reaction: how fast they started casting.", "timing");
    if (sp.answers.empty())
        ImGui::TextColored(kPrDim, "No prot or heal on the player.");
    const float call = PrCall(sp.hits);
    for (const CastRef& a : sp.answers)
    {
        char extra[48];
        snprintf(buf, sizeof(buf), "%+.2f s", HitTime(*a.ev) - sp.firstHit);
        snprintf(extra, sizeof(extra), "reaction %+.2f s", a.ev->startTime - call);
        castLine(a, isHeal(a.ev->skillId) ? "heal" : "prot", buf, kPrGold, extra);
    }
    ImGui::Dummy(ImVec2(1.f, 8.f));
    ImGui::Unindent(kMargin);
    ImGui::PopStyleVar();
}

// One card per Monk, for the spikes on their own team, grouped by team. Click a card to mark on the
// spike bar the spikes that Monk answered; hover it to see each spike one by one.
void ReplayWindow::DrawPianoRollMonkCards()
{
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = std::max(40.f, ImGui::GetContentRegionAvail().y);
    dl->AddLine(ImVec2(p0.x, p0.y + 0.5f), ImVec2(p0.x + w, p0.y + 0.5f), kPrRule);
    dl->AddLine(ImVec2(p0.x + std::round(w * 0.5f), p0.y), ImVec2(p0.x + std::round(w * 0.5f), p0.y + h), kPrRule);
    char buf[96];

    auto agentOf = [&](int id) -> const AgentReplayData* {
        auto it = m_replayCtx.agents.find(id);
        return it != m_replayCtx.agents.end() ? &it->second : nullptr;
    };
    auto isHeal = [&](int skillId) {
        const SkillInfo* si = m_skillView.IsLoaded() ? m_skillView.Get(skillId) : nullptr;
        if (!si || si->attribute == 15) return false;
        for (const auto& sc : si->scales)
            if (sc.kind == SkillScaleKind::Heal) return true;
        return false;
    };

    for (int side = 0; side < 2; ++side)
    {
        const std::vector<int>& ids = side == 0 ? m_team1PlayerIds : m_team2PlayerIds;
        const std::string& guild = side == 0 ? m_team1GuildHeader : m_team2GuildHeader;
        const uint8_t team = static_cast<uint8_t>(side + 1);
        const float x0 = p0.x + side * std::round(w * 0.5f) + 10.f;
        const float colW = std::round(w * 0.5f) - 20.f;

        snprintf(buf, sizeof(buf), "MONKS  %s", guild.empty() ? (side == 0 ? "Blue Team" : "Red Team") : guild.c_str());
        dl->AddText(font, 14.f, ImVec2(x0, p0.y + 6.f), TeamColour(team, 0xFF), buf);
        ImGui::SetCursorScreenPos(ImVec2(x0 + font->CalcTextSizeA(14.f, FLT_MAX, 0.f, buf).x + 6.f, p0.y + 5.f));
        ImGui::PushID(side);
        // The (?) as a short legend: one line per number on the card.
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextColored(kPrGold, "Monk cards");
            ImGui::TextColored(kPrDim, "Spikes on their own team only.");
            ImGui::Spacing();
            const float valX = ImGui::GetFontSize() * 6.f;
            auto row = [&](const char* name, const char* text) {
                ImGui::TextUnformatted(name);
                ImGui::SameLine(valX);
                ImGui::TextColored(kPrDim, "%s", text);
            };
            row("covered",  "Spikes they helped on.");
            row("first",    "Times they helped first.");
            row("timing",   "When their help landed. 0 = the first hit.");
            row("",         "Below 0 = it was up before the damage.");
            row("reaction", "How fast they started casting.");
            ImGui::Spacing();
            ImGui::TextColored(kPrDim, "Click a card to see their spikes on the bar.");
            ImGui::EndTooltip();
        }

        std::vector<int> monks;
        for (int id : ids) if (const auto* a = agentOf(id); a && a->primaryProf == 3) monks.push_back(id);
        if (monks.empty())
        {
            dl->AddText(font, 12.f, ImVec2(x0, p0.y + 30.f), ImGui::ColorConvertFloat4ToU32(kPrDim), "No Monk on this team.");
            ImGui::PopID();
            continue;
        }

        constexpr float kGap = 8.f;
        const float cardH = std::min(86.f, h - 34.f);
        const float cardW = std::min(250.f, (colW - kGap * (monks.size() - 1)) / monks.size());
        for (size_t mi = 0; mi < monks.size(); ++mi)
        {
            const int monkId = monks[mi];
            const AgentReplayData* m = agentOf(monkId);
            std::vector<const SpikeInfo*> mine;
            for (const auto& sp : m_spikes)
                if (const auto* t = agentOf(sp.target); t && t->teamId == team) mine.push_back(&sp);
            int covered = 0, first = 0;
            std::vector<float> timings, reactions, healTimes;
            for (const SpikeInfo* sp : mine)
            {
                const CastRef* own = nullptr;
                float land = FLT_MAX;
                for (const auto& a : sp->answers)
                    if (a.casterId == monkId)
                    {
                        if (!own) own = &a;
                        land = std::min(land, HitTime(*a.ev));
                    }
                if (!own) continue;
                ++covered;
                if (sp->answers.front().casterId == monkId) ++first;
                timings.push_back(land - sp->firstHit);
                reactions.push_back(own->ev->startTime - PrCall(sp->hits));
                for (const auto& a : sp->answers)
                    if (a.casterId == monkId && isHeal(a.ev->skillId)) { healTimes.push_back(HitTime(*a.ev) - sp->firstHit); break; }
            }

            const ImVec2 r0(x0 + mi * (cardW + kGap), p0.y + 28.f), r1(r0.x + cardW, r0.y + cardH);
            ImGui::SetCursorScreenPos(r0);
            ImGui::PushID(monkId);
            if (ImGui::InvisibleButton("##monk", ImVec2(cardW, cardH)))
                m_pianoRollMonkSel = m_pianoRollMonkSel == monkId ? -1 : monkId;
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered(), sel = m_pianoRollMonkSel == monkId;
            dl->AddRectFilled(r0, r1, hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 9), 3.f);
            if (sel) dl->AddRect(r0, r1, IM_COL32(0xF0, 0xC8, 0x50, 0xFF), 3.f, 0, 1.5f);

            dl->PushClipRect(r0, r1, true);
            // Name
            float tx = r0.x + 7.f;
            if (ImTextureID prof = m->primaryProf > 0 ? LoadProfIcon(dev, m->primaryProf) : nullptr)
            {
                dl->AddImage(prof, ImVec2(tx, r0.y + 6.f), ImVec2(tx + 18.f, r0.y + 24.f));
                tx += 22.f;
            }
            dl->AddText(font, 15.f, ImVec2(tx, r0.y + 7.f), ImGui::ColorConvertFloat4ToU32(PrTeamText(team)), m->playerName.c_str());
            // Covered bar
            snprintf(buf, sizeof(buf), "%d / %d", covered, static_cast<int>(mine.size()));
            const float numW = font->CalcTextSizeA(14.f, FLT_MAX, 0.f, buf).x;
            const float by = r0.y + 33.f, barR = r1.x - 14.f - numW;
            dl->AddRectFilled(ImVec2(r0.x + 7.f, by), ImVec2(barR, by + 5.f), IM_COL32(255, 255, 255, 22), 2.5f);
            if (!mine.empty() && covered > 0)
                dl->AddRectFilled(ImVec2(r0.x + 7.f, by), ImVec2(r0.x + 7.f + (barR - r0.x - 7.f) * covered / mine.size(), by + 5.f), TeamColour(team, 0xD0), 2.5f);
            dl->AddText(font, 14.f, ImVec2(r1.x - 7.f - numW, by - 5.f), IM_COL32(0xEA, 0xEA, 0xEA, 0xFF), buf);
            // first / reaction / heal time
            const float colStep = (cardW - 14.f) / 3.f, ly = r0.y + 46.f;
            const char* labels[3] = { "first", "timing", "reaction" };
            char vals[3][16];
            snprintf(vals[0], sizeof(vals[0]), "%d", first);
            const float tMed = PrMedian(timings), rMed = PrMedian(reactions);
            if (std::isnan(tMed)) snprintf(vals[1], sizeof(vals[1]), "-"); else snprintf(vals[1], sizeof(vals[1]), "%+.2f s", tMed);
            if (std::isnan(rMed)) snprintf(vals[2], sizeof(vals[2]), "-"); else snprintf(vals[2], sizeof(vals[2]), "%+.2f s", rMed);
            for (int c = 0; c < 3; ++c)
            {
                const float cx = r0.x + 7.f + c * colStep;
                dl->AddText(font, 12.f, ImVec2(cx, ly), ImGui::ColorConvertFloat4ToU32(kPrDim), labels[c]);
                dl->AddText(font, 15.f, ImVec2(cx, ly + 15.f),
                            c == 0 ? IM_COL32(0xEA, 0xEA, 0xEA, 0xFF) : ImGui::ColorConvertFloat4ToU32(kPrGold), vals[c]);
            }
            dl->PopClipRect();

            // Hover: a short summary - how often they answered, how fast (each spike as a dot on a
            // small axis), and what they answered with. The spike-by-spike view is the spike bar.
            if (hov && !mine.empty())
            {
                ImGui::BeginTooltip();
                ImGui::TextColored(PrTeamText(team), "%s", m->playerName.c_str());
                ImGui::SameLine(); ImGui::TextColored(kPrDim, "on the %d spikes of their team", static_cast<int>(mine.size()));
                ImGui::Spacing();

                auto line = [&](const char* label, const ImVec4& col, const char* value) {
                    ImGui::TextColored(kPrDim, "%s", label);
                    ImGui::SameLine(ImGui::GetFontSize() * 9.f);
                    ImGui::TextColored(col, "%s", value);
                };
                const ImVec4 kWhite(0.92f, 0.92f, 0.92f, 1.f);
                snprintf(buf, sizeof(buf), "%d of %d spikes", covered, static_cast<int>(mine.size()));
                line("Helped on", kWhite, buf);
                snprintf(buf, sizeof(buf), "%d times", first);
                line("First to help", kWhite, buf);
                if (!timings.empty())
                {
                    snprintf(buf, sizeof(buf), "%+.2f s", PrMedian(timings));
                    line("Usual timing", kPrGold, buf);
                    snprintf(buf, sizeof(buf), "%+.2f s", PrMedian(reactions));
                    line("Usual reaction", kPrGold, buf);
                }
                if (!healTimes.empty())
                {
                    snprintf(buf, sizeof(buf), "%+.2f s", PrMedian(healTimes));
                    line("Usual heal time", kPrGold, buf);
                }

                // Every timing as a dot on one axis, from 2 s before the first hit to 3 s after.
                if (!timings.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextColored(kPrDim, "Timing on each spike");
                    constexpr float kLo = -2.f, kHi = 3.f;
                    const float axisW = ImGui::GetFontSize() * 20.f, axisH = 30.f;
                    const ImVec2 a0 = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(axisW, axisH));
                    ImDrawList* tdl = ImGui::GetWindowDrawList();
                    auto ax = [&](float v) { return a0.x + (std::clamp(v, kLo, kHi) - kLo) / (kHi - kLo) * axisW; };
                    const float cy = a0.y + 10.f;
                    tdl->AddLine(ImVec2(a0.x, cy), ImVec2(a0.x + axisW, cy), IM_COL32(255, 255, 255, 40));
                    for (float g = kLo; g <= kHi + 0.01f; g += 0.5f)
                    {
                        const bool whole = std::fabs(g - std::round(g)) < 0.01f;
                        tdl->AddLine(ImVec2(ax(g), cy - (whole ? 4.f : 2.f)), ImVec2(ax(g), cy + (whole ? 4.f : 2.f)),
                                     IM_COL32(255, 255, 255, whole ? 90 : 45));
                        if (whole)
                        {
                            char lbl[8];
                            snprintf(lbl, sizeof(lbl), "%+d", static_cast<int>(std::round(g)));
                            const float tw = font->CalcTextSizeA(11.f, FLT_MAX, 0.f, lbl).x;
                            tdl->AddText(font, 11.f, ImVec2(ax(g) - tw * 0.5f, cy + 6.f), ImGui::ColorConvertFloat4ToU32(kPrDim), lbl);
                        }
                    }
                    tdl->AddLine(ImVec2(ax(0.f), cy - 7.f), ImVec2(ax(0.f), cy + 4.f), IM_COL32(0xFF, 0x8A, 0x80, 0xC0), 1.f);
                    for (float r : timings)
                        tdl->AddCircleFilled(ImVec2(ax(r), cy), 3.f, TeamColour(team, 0x90));
                    const float med = ax(PrMedian(timings));
                    tdl->AddRectFilled(ImVec2(med - 1.f, cy - 7.f), ImVec2(med + 1.f, cy + 7.f), IM_COL32(0xF0, 0xC8, 0x50, 0xFF));
                    ImGui::TextColored(kPrDim, "0 = the first hit. Left of 0: up before the damage. Gold = their usual timing.");
                }

                // What they answered with, most used first.
                std::vector<std::pair<int, int>> used;   // skill, count
                for (const SpikeInfo* sp : mine)
                    for (const auto& a : sp->answers)
                        if (a.casterId == monkId)
                        {
                            auto it = std::find_if(used.begin(), used.end(), [&](const auto& u) { return u.first == a.ev->skillId; });
                            if (it == used.end()) used.push_back({ a.ev->skillId, 1 }); else ++it->second;
                        }
                if (!used.empty())
                {
                    std::sort(used.begin(), used.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
                    ImGui::Spacing();
                    ImGui::TextColored(kPrDim, "Answered with");
                    const float sz = ImGui::GetTextLineHeight() + 2.f;
                    for (size_t u = 0; u < used.size() && u < 6; ++u)
                    {
                        if (ImTextureID tex = LoadSkillIcon(this, dev, used[u].first, m_skillIconIndex, m_skillIconCache))
                            ImGui::Image(tex, ImVec2(sz, sz));
                        else
                            ImGui::Dummy(ImVec2(sz, sz));
                        ImGui::SameLine(0.f, 6.f);
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.f);
                        ImGui::Text("%s", GetSkillDisplayName(used[u].first).c_str());
                        ImGui::SameLine(ImGui::GetFontSize() * 14.f);
                        ImGui::TextColored(kPrDim, "x%d", used[u].second);
                        ImGui::SameLine(0.f, 6.f);
                        ImGui::TextColored(kPrDim, "%s", isHeal(used[u].first) ? "heal" : "prot");
                    }
                }
                ImGui::Spacing();
                ImGui::TextColored(kPrDim, sel ? "Click: stop marking their spikes." : "Click: mark their spikes on the bar above, then step through them with the arrows.");
                ImGui::EndTooltip();
            }
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h - 1.f));
    ImGui::Dummy(ImVec2(w, 1.f));
}
