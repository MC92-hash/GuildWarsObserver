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
#include "ProtEfficiency.h"
#include <imgui_internal.h>
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
    // The roll's toolbar: spike boxes on or off, and the skill being searched (0 none).
    bool sPrSpikeBoxes = true;

    // The panel's own font at its own size. Not ui::DisplayFont: the replay window has its own ImGui
    // context, and the main window's baked sizes draw nothing in it; a scaled base font blurs.
    ImFont* PrFont(float) { return ImGui::GetFont(); }

    // GW's own left / right arrow (Textures\Game_UI\ui_left_right.png: four 16 px cells - left lit,
    // right lit, left, right). Lit while hovered; true when clicked.
    bool PrGwArrow(ID3D11Device* dev, const char* id, bool right, float size)
    {
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hov = ImGui::IsItemHovered();
        const int cell = (hov ? 0 : 2) + (right ? 1 : 0);
        ImDrawList* adl = ImGui::GetWindowDrawList();
        if (ImTextureID tex = dev ? LoadGameUITexture(dev, "ui_left_right.png") : nullptr)
            adl->AddImage(tex, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                          ImVec2(cell * 0.25f, 0.f), ImVec2((cell + 1) * 0.25f, 1.f));
        else
            adl->AddText(ImGui::GetItemRectMin(), IM_COL32(225, 190, 80, 255), right ? ">" : "<");
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return pressed;
    }
    std::vector<int> sPrFindSkills;   // the skills found on the roll
    char sPrFindText[64] = {};

    constexpr float kSpikeWindow   = 1.5f;
    constexpr int   kSpikeMinHits  = 3;
    constexpr float kAnswerBefore  = 1.5f;
    constexpr float kAnswerAfter   = 3.f;

    float HitTime(const SkillUseEvent& ev) { return std::max(ev.endTime, ev.startTime); }

    bool PrPicked(const std::vector<int>& picked, int id) { return std::find(picked.begin(), picked.end(), id) != picked.end(); }

    // Whether a cast belongs to the pick: by a picked player (out), on one (in), or either (both).
    // Nothing picked: every cast does.
    bool PrInPick(const std::vector<int>& picked, int dir, int casterId, int targetId)
    {
        if (picked.empty()) return true;
        const bool out = PrPicked(picked, casterId);
        const bool in  = targetId >= 0 && targetId != casterId && PrPicked(picked, targetId);
        return dir == 1 ? out : dir == 2 ? in : out || in;
    }

    ImU32 TeamColour(uint8_t team, int alpha)
    {
        return team == 1 ? IM_COL32(74, 200, 255, alpha) : team == 2 ? IM_COL32(255, 107, 107, alpha)
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

// Prots and heals: the Monk's protection, healing and Divine Favor lines, and any other skill whose
// description heals. Restoration Magic only where it heals: its resurrections (Flesh of My Flesh,
// Death Pact Signet) are neither (owner, 2026-10-27).
bool ReplayWindow::IsSpikeAnswerSkill(int skillId) const
{
    const SkillInfo* si = m_skillView.IsLoaded() ? m_skillView.Get(skillId) : nullptr;
    if (!si) return false;
    if (si->attribute == 13 || si->attribute == 15 || si->attribute == 16) return true;
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
    static const float kZoomHalf[] = { 1.f, 2.5f, 5.f, 10.f, 15.f, 30.f, 60.f };
    static const int   kZoomCount  = 7;
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
    const float minRowH = m_pianoRollLanes ? 32.f : 26.f;
    const float totalH = 90.f + kSpikeBarH + kTimeAxisH + 2.f * kTeamLabelH + nRows * minRowH * 1.3f + kMonkH;

    const float screenW = io.DisplaySize.x;

    // Panel window — Morale-style: standard ImGui title bar, matching colors
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,     ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip,        ImVec4(0.94f, 0.78f, 0.31f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.94f, 0.78f, 0.31f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive,  ImVec4(0.94f, 0.78f, 0.31f, 0.90f));
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
        ImGui::PopStyleColor(8);
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
    {
        // A player picked through a spike (the card's arrows, the bar) follows the card to the next
        // spike's target as the timeline plays on; a player picked by hand stays.
        static int sCardTarget = -1;
        if (cardSpike >= 0)
        {
            const int target = m_spikes[cardSpike].target;
            if (target != sCardTarget && m_pianoRollFollowSpikes && m_pianoRollPinFromSpike &&
                m_pianoRollPinned.size() == 1 && m_pianoRollPinned[0] == sCardTarget)
                m_pianoRollPinned = { target };
            sCardTarget = target;
        }
    }
    DrawPianoRollSpikeBar(winStart, winEnd, cardSpike);

    // ── Toolbar: three rows that never move - the players picked, the skills found, the view ──
    {
        constexpr float kBarH   = 30.f;
        constexpr float kTitleW = 62.f;   // the rows' titles, one column
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        const float tx0 = ImGui::GetCursorScreenPos().x;
        const float tw = ImGui::GetContentRegionAvail().x;
        float rowY = ImGui::GetCursorScreenPos().y;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 4.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 3.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.04f, 0.05f, 0.06f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.09f, 0.10f, 0.12f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.12f, 0.12f, 0.13f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.40f, 0.33f, 0.15f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,      ImVec4(225 / 255.f, 190 / 255.f, 80 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.04f, 0.05f, 0.06f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.12f, 0.12f, 0.13f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.16f, 0.15f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.88f, 0.75f, 0.31f, 0.35f));
        const ImVec4 kGoldText(225 / 255.f, 190 / 255.f, 80 / 255.f, 1.f);
        const ImVec4 kDimText(0.48f, 0.50f, 0.53f, 1.f);

        // A row: its title in the left column, the cursor on the row's first widget.
        auto beginRow = [&](const char* title) {
            wdl->AddText(ImVec2(tx0 + 10.f, rowY + (kBarH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(0xB0, 0xAA, 0x98, 0xFF), title);
            ImGui::SetCursorScreenPos(ImVec2(tx0 + 10.f + kTitleW, rowY + (kBarH - ImGui::GetFrameHeight()) * 0.5f));
        };
        auto endRow = [&]() {
            rowY += kBarH;
            wdl->AddLine(ImVec2(tx0, rowY - 0.5f), ImVec2(tx0 + tw, rowY - 0.5f), IM_COL32(0x66, 0x54, 0x26, 0x50));
        };
        // Joined buttons, the chosen one lit in gold; greyed and inert when `enabled` is false.
        struct Seg { int value; const char* label; const char* help; };
        auto segmented = [&](const char* id, int& value, std::initializer_list<Seg> segs, bool enabled, const char* offHelp) {
            ImGui::PushID(id);
            ImGui::BeginDisabled(!enabled);
            bool first = true;
            for (const Seg& s : segs)
            {
                if (!first) ImGui::SameLine(0.f, 0.f);
                first = false;
                const bool on = value == s.value;
                if (on) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.75f, 0.31f, 0.28f)); ImGui::PushStyleColor(ImGuiCol_Text, kGoldText); }
                if (ImGui::Button(s.label)) value = s.value;
                if (on) ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", enabled ? s.help : offHelp);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        };

        // ── Players: what of theirs shows, then the picked players (click a name on the roll) ──
        beginRow("Players");
        segmented("pr_dir", m_pianoRollPinDir, {
            { 1, "Their casts",   "Only the casts BY the picked players, with a line to each target." },
            { 2, "Casts on them", "Only the casts AIMED AT the picked players, with a line from each caster." },
            { 0, "Both",          "The casts by the picked players and the casts aimed at them." } },
            !m_pianoRollPinned.empty(), "Pick a player first: click a name on the left of the roll.");
        ImGui::SameLine(0.f, 14.f);
        if (m_pianoRollPinned.empty())
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kDimText, "Click a name on the roll to pick a player, Ctrl+click to pick more.");
        }
        else
        {
            int drop = -1;
            for (int id : m_pianoRollPinned)
            {
                auto pit = m_replayCtx.agents.find(id);
                if (pit == m_replayCtx.agents.end()) continue;
                char lbl[96];
                snprintf(lbl, sizeof(lbl), "%s  x##pr_pick%d", pit->second.playerName.c_str(), id);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(TeamColour(pit->second.teamId, 255)));
                if (ImGui::Button(lbl)) drop = id;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to unpick %s.", pit->second.playerName.c_str());
                ImGui::SameLine(0.f, 4.f);
            }
            if (drop >= 0) m_pianoRollPinned.erase(std::find(m_pianoRollPinned.begin(), m_pianoRollPinned.end(), drop));
            ImGui::SameLine(0.f, 8.f);
            if (ImGui::SmallButton("Clear##pr_pick_clear")) m_pianoRollPinned.clear();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unpick every player.");
            if (m_pianoRollPinned.empty()) m_pianoRollPinFromSpike = false;
        }
        endRow();

        // ── Skills: the search box always there, the skills found after it as icons ──
        beginRow("Skills");
        {
            static std::vector<int> sUsed;
            static const void* sUsedOwner = nullptr;
            if (sUsedOwner != m_spikes.data() || sUsed.empty())
            {
                sUsedOwner = m_spikes.data();
                std::unordered_set<int> ids;
                for (const auto& [id, ard] : m_replayCtx.agents)
                    for (const auto& ev : ard.skillUseHistory) ids.insert(ev.skillId);
                sUsed.assign(ids.begin(), ids.end());
                std::sort(sUsed.begin(), sUsed.end(), [&](int a, int b) { return GetSkillDisplayName(a) < GetSkillDisplayName(b); });
                std::erase_if(sPrFindSkills, [&](int s) { return !ids.count(s); });
            }

            ImGui::SetNextItemWidth(200.f);
            ImGui::InputTextWithHint("##pr_find", "Find a skill...", sPrFindText, sizeof(sPrFindText));
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("Type a skill name and pick it from the list.\nPick several to follow them together.");
            const ImVec2 inMin = ImGui::GetItemRectMin(), inMax = ImGui::GetItemRectMax();
            const bool inputActive = ImGui::IsItemActive();
            static bool sListHovered = false;
            if (sPrFindText[0] && (inputActive || sListHovered))
            {
                std::string q = sPrFindText;
                std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                std::vector<int> hits;
                for (int id : sUsed)
                {
                    if (PrPicked(sPrFindSkills, id)) continue;
                    std::string low = GetSkillDisplayName(id);
                    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    if (low.find(q) != std::string::npos) hits.push_back(id);
                }
                const float iconH = 22.f;
                const float rowH = std::max(iconH + 2.f, ImGui::GetTextLineHeightWithSpacing());
                const float dropW = std::max(inMax.x - inMin.x, 260.f);
                const float maxH = std::min(320.f, std::max(1.f, (float)hits.size()) * rowH + 12.f);
                ImGui::SetNextWindowPos(ImVec2(inMin.x, inMax.y));
                ImGui::SetNextWindowSizeConstraints(ImVec2(dropW, 0.f), ImVec2(dropW, maxH));
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.078f, 0.98f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.40f, 0.33f, 0.15f, 0.70f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(225 / 255.f, 190 / 255.f, 80 / 255.f, 0.14f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(225 / 255.f, 190 / 255.f, 80 / 255.f, 0.24f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
                sListHovered = false;
                if (ImGui::Begin("##pr_find_list", nullptr,
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
                    sListHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                    if (hits.empty())
                        ImGui::TextColored(kDimText, "No skill matches");
                    for (int id : hits)
                    {
                        const ImVec2 pos = ImGui::GetCursorScreenPos();
                        ImGui::PushID(id);
                        if (ImGui::Selectable("##row", false, 0, ImVec2(0.f, rowH)))
                        {
                            sPrFindSkills.push_back(id);
                            sPrFindText[0] = 0;
                            sListHovered = false;
                        }
                        ImGui::PopID();
                        ImDrawList* ldl = ImGui::GetWindowDrawList();
                        if (ImTextureID tex = LoadSkillIcon(this, dev, id, m_skillIconIndex, m_skillIconCache))
                            ldl->AddImage(tex, ImVec2(pos.x + 4.f, pos.y + 1.f), ImVec2(pos.x + 4.f + iconH, pos.y + 1.f + iconH));
                        ldl->AddText(ImVec2(pos.x + iconH + 10.f, pos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                                     IM_COL32(216, 220, 226, 255), GetSkillDisplayName(id).c_str());
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(4);
            }
            else
                sListHovered = false;

            if (sPrFindSkills.empty())
            {
                ImGui::SameLine(0.f, 14.f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kDimText, "The skills found are framed in gold on the roll.");
            }
            else
            {
                // The skills found, one icon each: click one to drop it.
                const float chip = ImGui::GetFrameHeight();
                int drop = -1;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.f, 1.f));
                for (size_t i = 0; i < sPrFindSkills.size(); ++i)
                {
                    const int sid = sPrFindSkills[i];
                    ImGui::SameLine(0.f, i == 0 ? 10.f : 3.f);
                    ImGui::PushID(sid);
                    bool clicked = false;
                    if (ImTextureID tex = LoadSkillIcon(this, dev, sid, m_skillIconIndex, m_skillIconCache))
                        clicked = ImGui::ImageButton("##chip", tex, ImVec2(chip - 2.f, chip - 2.f));
                    else
                        clicked = ImGui::Button("?##chip", ImVec2(chip, chip));
                    ImGui::PopID();
                    if (clicked) drop = sid;
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s\nClick to drop it from the search.", GetSkillDisplayName(sid).c_str());
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                }
                ImGui::PopStyleVar();

                // Every use of the skills found, by the picked players when some are, in time order.
                std::vector<float> uses;
                for (const auto& [id, ard] : m_replayCtx.agents)
                    for (const auto& ev : ard.skillUseHistory)
                        if (PrPicked(sPrFindSkills, ev.skillId) && PrInPick(m_pianoRollPinned, m_pianoRollPinDir, id, ev.targetId))
                            uses.push_back(ev.startTime);
                std::sort(uses.begin(), uses.end());
                const float arrow = 20.f;
                ImGui::SameLine(0.f, 12.f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - arrow) * 0.5f);
                if (PrGwArrow(dev, "##pr_find_prev", false, arrow))
                    for (auto it = uses.rbegin(); it != uses.rend(); ++it)
                        if (*it < now - 0.05f) { m_debugTimeline = *it; break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous use");
                ImGui::SameLine(0.f, 4.f);
                if (PrGwArrow(dev, "##pr_find_next", true, arrow))
                    for (float u : uses)
                        if (u > now + 0.05f) { m_debugTimeline = u; break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next use");
                ImGui::SameLine(0.f, 8.f);
                const int at = static_cast<int>(std::upper_bound(uses.begin(), uses.end(), now + 0.05f) - uses.begin());
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kGoldText, "%d / %d uses", at, static_cast<int>(uses.size()));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(m_pianoRollPinned.empty() ? "Uses of these skills up to now, out of the whole match."
                                                                : "Uses of these skills up to now, out of the whole match,\n"
                                                                  "counting only the picked players' casts (Players row).");
                ImGui::SameLine(0.f, 10.f);
                if (ImGui::SmallButton("Clear##pr_find_clear")) sPrFindSkills.clear();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop every skill from the search.");
                if (drop >= 0) sPrFindSkills.erase(std::find(sPrFindSkills.begin(), sPrFindSkills.end(), drop));
            }
        }
        endRow();

        // ── View: the casts outside the picks, the roll's marks, the zoom hint ──
        beginRow("View");
        {
            int rest = m_pianoRollHideRest ? 1 : 0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Other casts");
            ImGui::SameLine(0.f, 6.f);
            segmented("pr_rest", rest, {
                { 0, "Dimmed", "The casts outside the picked players and skills stay, dimmed." },
                { 1, "Hidden", "The casts outside the picked players and skills are not drawn." } },
                !m_pianoRollPinned.empty() || !sPrFindSkills.empty(), "Pick a player or find a skill first.");
            m_pianoRollHideRest = rest == 1;
        }
        ImGui::SameLine(0.f, 22.f);
        ImGui::Checkbox("Casts received", &m_pianoRollLanes);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Under each name, a thin line for every cast aimed at that player,\n"
                              "in the caster's team colour: it runs for the cast time, from the\n"
                              "cast's start to its end, with a mark at the end.");
        ImGui::SameLine(0.f, 16.f);
        ImGui::Checkbox("Spike boxes", &sPrSpikeBoxes);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The red boxes around spikes on the roll.");
        ImGui::SameLine(0.f, 16.f);
        if (ImGui::Checkbox("Follow spikes", &m_pianoRollFollowSpikes) && !m_pianoRollFollowSpikes)
            m_pianoRollSpikeSel = cardSpike;   // the card stays on the spike it shows
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: the spike card moves to the spike at the playhead.\n"
                              "Off: the card stays put; it changes only with its arrows or the spike bar,\n"
                              "and the players you picked are never changed for you.");
        ImGui::SameLine(0.f, 16.f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            // The key: each line drawn the way the roll draws it, then what it means.
            ImGui::BeginTooltip();
            ImGui::TextColored(kGoldText, "Reading the roll");
            ImGui::Spacing();
            ImGui::TextUnformatted("Each player's own casts: the skill icon where the cast starts,\n"
                                   "and under it a line for the cast time, until the cast ends.");
            ImGui::Spacing();
            ImDrawList* kdl = ImGui::GetWindowDrawList();
            const float sw = 34.f, lh = ImGui::GetTextLineHeight();
            auto keyRow = [&](auto&& draw, const char* text, ImU32 textCol) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                draw(p, lh);
                ImGui::SetCursorScreenPos(ImVec2(p.x + sw + 10.f, p.y));
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol), "%s", text);
            };
            auto castLine = [&](ImU32 c1, ImU32 c2) {
                return [=](ImVec2 p, float h) {
                    const float y = std::round(p.y + h * 0.5f);
                    if (c2) {
                        kdl->AddRectFilled(ImVec2(p.x, y - 1.f), ImVec2(p.x + sw * 0.5f - 1.f, y + 1.f), c1, 1.f);
                        kdl->AddRectFilled(ImVec2(p.x + sw * 0.5f + 1.f, y - 1.f), ImVec2(p.x + sw, y + 1.f), c2, 1.f);
                    } else
                        kdl->AddRectFilled(ImVec2(p.x, y - 1.f), ImVec2(p.x + sw, y + 1.f), c1, 1.f);
                };
            };
            keyRow(castLine(TeamColour(1, 255), TeamColour(2, 255)), "Team colour: the cast went through", IM_COL32(216, 220, 226, 255));
            keyRow(castLine(IM_COL32(0x90, 0x30, 0xD0, 0xFF), 0), "Purple: the cast was interrupted", IM_COL32(0xB8, 0x70, 0xF0, 0xFF));
            keyRow(castLine(IM_COL32(0xE0, 0x80, 0x20, 0xFF), 0), "Orange: the cast was cancelled by the player", IM_COL32(0xE0, 0x80, 0x20, 0xFF));
            ImGui::Spacing();
            keyRow([&](ImVec2 p, float h) {
                    const float y = std::round(p.y + h * 0.5f);
                    kdl->AddLine(ImVec2(p.x + 2.f, y), ImVec2(p.x + sw - 6.f, y), TeamColour(2, 230), 1.f);
                    kdl->AddRectFilled(ImVec2(p.x + sw - 6.f, y - 4.f), ImVec2(p.x + sw - 3.f, y + 4.f), TeamColour(2, 230));
                }, "Casts received: the thin line under a name, in the caster's team colour", IM_COL32(216, 220, 226, 255));
            keyRow([&](ImVec2 p, float h) {
                    kdl->AddRect(ImVec2(p.x + sw * 0.5f - h * 0.5f, p.y), ImVec2(p.x + sw * 0.5f + h * 0.5f, p.y + h),
                                 IM_COL32(0xE1, 0xBE, 0x50, 0xFF), 3.f, 0, 2.f);
                }, "Gold frame: a skill found in the Skills row", IM_COL32(0xE1, 0xBE, 0x50, 0xFF));
            keyRow([&](ImVec2 p, float h) {
                    kdl->AddRectFilled(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + sw, p.y + h - 1.f), IM_COL32(0xE8, 0x50, 0x48, 0x30));
                    kdl->AddRect(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + sw, p.y + h - 1.f), IM_COL32(0xFF, 0x8A, 0x80, 0xFF), 2.f, 0, 1.5f);
                }, "Red box: a spike", IM_COL32(0xFF, 0x8A, 0x80, 0xFF));
            ImGui::EndTooltip();
        }
        {
            const char* hint = "Mouse wheel on the timeline: zoom";
            const float hw = ImGui::CalcTextSize(hint).x;
            ImGui::SameLine();
            const float hx = tx0 + tw - hw - 12.f;
            if (hx > ImGui::GetCursorScreenPos().x + 10.f)
            {
                ImGui::SetCursorScreenPos(ImVec2(hx, rowY + (kBarH - ImGui::GetTextLineHeight()) * 0.5f));
                ImGui::TextDisabled("%s", hint);
            }
        }
        endRow();

        ImGui::PopStyleColor(9);
        ImGui::PopStyleVar(4);
        ImGui::SetCursorScreenPos(ImVec2(tx0, rowY));
    }

    const float availH = ImGui::GetContentRegionAvail().y;
    const float rollH = std::max(100.f, availH - kMonkH);
    const float rollW = std::max(300.f, ImGui::GetContentRegionAvail().x - kCardW);
    const float fitRowH = (rollH - kTimeAxisH - 2.f * kTeamLabelH - 2.f) / nRows;
    const float kRowH = std::floor(std::clamp(fitRowH, minRowH, m_pianoRollLanes ? 44.f : 34.f));
    const float kInLaneH = m_pianoRollLanes ? std::round(std::clamp(kRowH * 0.24f, 8.f, 11.f)) : 0.f;
    const float kCastLaneH = kRowH - kInLaneH - 2.f;
    const float kIconSz = std::round(std::clamp(kCastLaneH - 7.f, 18.f, 26.f));

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
    for (float st : { 0.5f, 1.f, 2.f, 5.f, 10.f, 15.f, 30.f })
        if (st * pps >= 56.f) { labelStep = st; break; }
    {
        const float axisTop = curY;
        dl->AddRectFilled(ImVec2(cp.x, axisTop), ImVec2(cp.x + panelW, axisTop + kTimeAxisH), IM_COL32(0, 0, 0, 40));
        const float tickStep = pps * 0.1f >= 8.f ? 0.1f : pps * 0.5f >= 6.f ? 0.5f : labelStep;
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
                if (labelStep < 1.f) snprintf(lbl, sizeof(lbl), "%d:%04.1f", m, s - m * 60.f);
                else                 snprintf(lbl, sizeof(lbl), "%d:%02d", m, static_cast<int>(s) - m * 60);
                const ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, lbl);
                const float tx = x - ts.x * 0.5f;
                if (tx > tlLeft - 4.f && tx + ts.x < tlRight)
                    dl->AddText(font, font->FontSize, ImVec2(tx, axisTop + 2.f), IM_COL32(0xB0, 0xAA, 0x98, 0xFF), lbl);
            }
        }
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

    std::unordered_map<int, float> rowTopOf;   // laid-out player rows, for the hits-taken lines and links
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
        (void)teamBg; (void)teamBorder;
        dl->AddLine(ImVec2(tlMin.x + 8.f, tlMax.y - 1.f), ImVec2(tlMax.x, tlMax.y - 1.f), (teamCol & 0x00FFFFFFu) | 0x55000000u);

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
        dl->AddText(font, font->FontSize, ImVec2(cp.x + 22.f, curY + (kTeamLabelH - 14.f) * 0.5f), teamCol, teamBuf);

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
            const bool pinned = PrPicked(m_pianoRollPinned, agentId);

            const AgentSnapshot* snap = getSnap(agentId, now);
            const bool isDead = snap && snap->is_dead;

            dl->AddLine(ImVec2(cp.x, rowBot), ImVec2(cp.x + panelW, rowBot), IM_COL32(255, 255, 255, 8));
            if (isDead)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(0, 0, 0, 70));   // dead: the row darkens
            if (rowHovered)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(255, 255, 255, 10));
            if (pinned)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(0xE1, 0xBE, 0x50, 0x16));
            // The player picked from a party row's context menu (Cast timeline...).
            if (m_pianoRollHighlightAgent == agentId)
            {
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot), IM_COL32(0xE1, 0xBE, 0x50, 0x18));
                dl->AddRect(ImVec2(cp.x + 1.f, rowTop + 1.f), ImVec2(cp.x + panelW - 1.f, rowBot - 1.f),
                            IM_COL32(0xE1, 0xBE, 0x50, 0xC0), 0.f, 0, 1.5f);
            }

            // Player label: click pins the player's links, double-click opens their info panel.
            {
                const float profIconSz = 18.f;
                ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                const float iconY = rowTop + (kRowH - 2.f - profIconSz) * 0.5f;
                if (profTex)
                    dl->AddImage(profTex, ImVec2(cp.x + 6.f, iconY), ImVec2(cp.x + 6.f + profIconSz, iconY + profIconSz));

                ImU32 nameCol = pinned ? IM_COL32(0xE1, 0xBE, 0x50, 0xFF)
                              : isDead ? IM_COL32(216, 220, 226, 120) : IM_COL32(216, 220, 226, 255);
                // The name cut with ".." only when it does not fit the column.
                std::string name = ard.playerName;
                const float skullSz = font->FontSize;
                const float nameMaxW = kNameColW - 26.f - 6.f - (isDead ? skullSz + 4.f : 0.f);
                if (font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, name.c_str()).x > nameMaxW)
                {
                    while (!name.empty() && font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, (name + "..").c_str()).x > nameMaxW) name.pop_back();
                    name += "..";
                }
                const float nameY = rowTop + (kRowH - 2.f - font->FontSize) * 0.5f;
                dl->AddText(font, font->FontSize, ImVec2(cp.x + 26.f, nameY), nameCol, name.c_str());
                // Dead at the playhead: the combat log's skull after the name.
                if (isDead)
                    if (ImTextureID skull = LoadFlagIcon(dev, "death.png"))
                    {
                        const float sx = cp.x + 26.f + font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, name.c_str()).x + 4.f;
                        dl->AddImage(skull, ImVec2(sx, nameY), ImVec2(sx + skullSz, nameY + skullSz));
                    }

                const bool overName = ImGui::IsMouseHoveringRect(ImVec2(cp.x, rowTop), ImVec2(cp.x + kNameColW, rowBot));
                if (overName && !m_annotationMgr.IsDrawModeActive())
                {
                    if (ImGui::IsMouseDoubleClicked(0))  OpenPlayerInfoPanel(agentId);
                    else if (ImGui::IsMouseClicked(0))
                    {
                        // Ctrl+click adds or removes the player; a plain click picks them alone (again: none).
                        auto& pick = m_pianoRollPinned;
                        if (io.KeyCtrl)
                        {
                            if (pinned) pick.erase(std::find(pick.begin(), pick.end(), agentId));
                            else        pick.push_back(agentId);
                        }
                        else if (pinned && pick.size() == 1) pick.clear();
                        else                                 pick = { agentId };
                        m_pianoRollPinFromSpike = false;
                    }
                    ImGui::SetTooltip("Click: pick %s - the Players row above sets what shows\n"
                                      "Ctrl+click: add or remove them from the picked players\nDouble-click: player info",
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
                const bool linked = PrInPick(m_pianoRollPinned, m_pianoRollPinDir, agentId, ev.targetId);
                const bool found = PrPicked(sPrFindSkills, ev.skillId);
                const bool skillOk = sPrFindSkills.empty() || found;
                if ((!linked || !skillOk) && m_pianoRollHideRest) return;
                float a = ev.startTime > now ? 0.5f : 1.f;
                if (!linked) a *= 0.12f;
                else if (!skillOk) a *= 0.3f;
                const ImU32 edge = ev.wasInterrupted ? IM_COL32(0x90, 0x30, 0xD0, 0xFF)
                                 : ev.wasCancelled   ? IM_COL32(0xE0, 0x80, 0x20, 0xFF) : TeamColour(ard.teamId, 0xFF);
                const ImU32 edgeA = (edge & ~IM_COL32_A_MASK) | (static_cast<ImU32>(220 * a) << IM_COL32_A_SHIFT);
                if (x1 > x0)
                    dl->AddRectFilled(ImVec2(x0, lineY), ImVec2(x1, lineY + 2.f), edgeA, 1.f);
                if (ImTextureID tex = LoadSkillIcon(this, dev, ev.skillId, m_skillIconIndex, m_skillIconCache))
                    dl->AddImageRounded(tex, ImVec2(x0, iconY), ImVec2(x0 + kIconSz, iconY + kIconSz),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255 * a)), 2.f);
                dl->AddRect(ImVec2(x0 - 0.5f, iconY - 0.5f), ImVec2(x0 + kIconSz + 0.5f, iconY + kIconSz + 0.5f), edgeA, 2.f, 0, 1.f);
                if (found)
                    dl->AddRect(ImVec2(x0 - 2.5f, iconY - 2.5f), ImVec2(x0 + kIconSz + 2.5f, iconY + kIconSz + 2.5f),
                                IM_COL32(0xE1, 0xBE, 0x50, 0xFF), 3.f, 0, 2.f);

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
                  m_pianoRollTeam1Open, IM_COL32(74, 200, 255, 255), IM_COL32(0x4A, 0x90, 0xD8, 0x14),
                  IM_COL32(0x4A, 0x90, 0xD8, 0x26));
    drawTeamBlock(m_team2PlayerIds, m_team2GuildHeader.empty() ? "Red Team" : m_team2GuildHeader.c_str(),
                  m_pianoRollTeam2Open, IM_COL32(255, 107, 107, 255), IM_COL32(0xD0, 0x48, 0x48, 0x14),
                  IM_COL32(0xD0, 0x48, 0x48, 0x26));
    const float bodyBot = curY;

    dl->PushClipRect(ImVec2(tlLeft, bodyTop), ImVec2(tlRight, bodyBot), true);

    // Light grid on the ruler's labels.
    for (float s = std::ceil(winStart / labelStep) * labelStep; s <= winEnd; s += labelStep)
        dl->AddLine(ImVec2(std::round(timeToX(s)), bodyTop), ImVec2(std::round(timeToX(s)), bodyBot), IM_COL32(255, 255, 255, 10));

    // Spike bands on the target's row: from the first enemy cast start to the last hit.
    for (size_t si = 0; si < m_spikes.size() && sPrSpikeBoxes; ++si)
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
        dl->AddText(font, font->FontSize, ImVec2(x1 + 4.f, rit->second + 2.f), IM_COL32(0xFF, 0x8A, 0x80, 0xFF), lbl);
    }

    // Hits taken: every cast aimed at a laid-out player, as a mark at its hit with a thin line
    // back to its start, in the caster's team colour.
    if (m_pianoRollLanes)
        for (const auto& [casterId, caster] : m_replayCtx.agents)
            forEachInWindow(caster, [&](const SkillUseEvent& ev) {
                if (ev.targetId < 0 || ev.targetId == casterId) return;
                auto rit = rowTopOf.find(ev.targetId);
                if (rit == rowTopOf.end()) return;
                const bool linked = PrInPick(m_pianoRollPinned, m_pianoRollPinDir, casterId, ev.targetId) &&
                                    (sPrFindSkills.empty() || PrPicked(sPrFindSkills, ev.skillId));
                if (!linked && m_pianoRollHideRest) return;
                const int alpha = (int)((ev.startTime > now ? 120 : 230) * (linked ? 1.f : 0.12f));
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
        dl->AddCircleFilled(p1, 3.f, IM_COL32(0xE1, 0xBE, 0x50, 0xFF));
    };
    if (!m_pianoRollPinned.empty())
        for (const auto& [casterId, caster] : m_replayCtx.agents)
            forEachInWindow(caster, [&](const SkillUseEvent& ev) {
                if (PrInPick(m_pianoRollPinned, m_pianoRollPinDir, casterId, ev.targetId) &&
                    (sPrFindSkills.empty() || PrPicked(sPrFindSkills, ev.skillId))) drawLink(ev, casterId);
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
    dl->AddLine(ImVec2(nowLineX, bodyTop), ImVec2(nowLineX, bodyBot), IM_COL32(0xE1, 0xBE, 0x50, 0xD0), 1.5f);

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
            snprintf(zoomTxt, sizeof(zoomTxt), "\xc2\xb1 %gs", halfWin);
            ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, zoomTxt);
            float tx = tlLeft + tlW * 0.5f - ts.x * 0.5f;
            float ty = bodyTop + 8.f;
            dl->AddRectFilled(ImVec2(tx - 8.f, ty - 4.f), ImVec2(tx + ts.x + 8.f, ty + ts.y + 4.f), IM_COL32(0, 0, 0, (int)(180 * alpha)), 6.f);
            dl->AddText(font, font->FontSize, ImVec2(tx, ty), IM_COL32(255, 255, 255, (int)(220 * alpha)), zoomTxt);
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
    ImGui::PopStyleColor(8);
}




// ---------------------------------------------------------------------------
// Around the roll: the match's spikes as a bar, the spike at the playhead as a card, one card per Monk.
// Every label carries a (?) with a one-line explanation; the numbers are shown, never judged.
// ---------------------------------------------------------------------------

namespace
{
    const ImVec4 kPrGold(225 / 255.f, 190 / 255.f, 80 / 255.f, 1.f);   // the Character panel's heading gold
    const ImVec4 kPrDim(0.48f, 0.50f, 0.53f, 1.f);                       // and its muted text
    const ImVec4 kPrSpike(1.f, 0.54f, 0.50f, 1.f);
    constexpr ImU32 kPrRule = IM_COL32(0x66, 0x54, 0x26, 0x66);   // the panel's separator colour

    ImVec4 PrTeamText(uint8_t team)
    {
        return team == 1 ? ImVec4(74 / 255.f, 200 / 255.f, 1.f, 1.f) : team == 2 ? ImVec4(1.f, 107 / 255.f, 107 / 255.f, 1.f)
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

    constexpr const char* kHelpTiming =
        "Timing\n"
        "From: the first enemy hit on the player.\n"
        "To: the moment their prot or heal took effect (their cast finished).\n"
        "So their own cast time is included.\n"
        "Below 0: it was already on the player before the damage came.";
    constexpr const char* kHelpReaction =
        "Reaction\n"
        "From: the moment a second enemy started casting on the player.\n"
        "To: the moment they started casting.\n"
        "So their own cast time is not included.";
    constexpr const char* kHelpTypical =
        "On a card: their median over all their spikes.";
    constexpr const char* kHelpCovered = "Spikes on their team they helped on\n(a prot or heal on the player).";
    constexpr const char* kHelpFirst   = "Times they were the first teammate to help.";
    constexpr const char* kHelpProtUse =
        "While this prot was up:\n"
        "hits = enemy hits the player took.\n"
        "reversed = damage turned into Health (Reversal of Fortune, Life Sheath).\n"
        "offset = Health given back after big hits (Spirit Bond).\n"
        "prevented = damage that never landed (Shielding Hands, Shield of Absorption,\n"
        "Aura of Faith), at the Monk's Protection Prayers. ~ = an estimate.\n"
        "capped = hits held to 10% of max Health (Protective Spirit).";

    // A prot's figures as one short line: "3 hits · 120 damage reversed".
    std::string PrProtLine(int hits, int kind, int reversed, int offset, int prevented, int endHeal,
                           int healBoost, int capped, int healedBack)
    {
        std::string s = std::to_string(hits) + (hits == 1 ? " hit" : " hits");
        auto add = [&](const std::string& part) { s += "  \xc2\xb7  " + part; };
        switch (static_cast<ProtEfficiency::Kind>(kind))
        {
        case ProtEfficiency::Kind::Reversed:         if (reversed > 0) add(std::to_string(reversed) + " damage reversed"); break;
        case ProtEfficiency::Kind::Offset:           if (offset > 0) add(std::to_string(offset) + " damage offset"); break;
        case ProtEfficiency::Kind::FlatReduction:    if (prevented > 0) add("~" + std::to_string(prevented) + " prevented");
                                                     if (endHeal > 0) add(std::to_string(endHeal) + " healed at end"); break;
        case ProtEfficiency::Kind::PercentReduction: if (prevented > 0) add("~" + std::to_string(prevented) + " prevented");
                                                     if (healBoost > 0) add("+" + std::to_string(healBoost) + " heal boost"); break;
        case ProtEfficiency::Kind::Cap:              if (capped > 0) add(std::to_string(capped) + " capped at 10%"); break;
        default:                                     if (healedBack > 0) add(std::to_string(healedBack) + " healed back"); break;
        }
        return s;
    }

    void PrTip(const char* text)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }

    float PrMedian(std::vector<float> v)
    {
        if (v.empty()) return NAN;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
}

// What one prot did: the effect timeline says when it was up on the player; the hits are the enemy damage
// numbers in that window (a weapon hit and its vampiric part in one tick count once), the Health given
// back is the prot's own heal packets from its caster (the attribution names their skill). No per-skill
// rule: anything that puts an effect on the player counts. Cached per cast for the match.
ReplayWindow::ProtUse ReplayWindow::PianoRollProtUse(const CastRef& answer, int target) const
{
    static std::unordered_map<const SkillUseEvent*, ProtUse> sCache;
    static const void* sOwner = nullptr;
    static bool sWithRanks = false;
    const EffectTimeline::Table* fx = EffectTimelineTable();
    if (sOwner != m_spikes.data() || sWithRanks != m_effectTimelineUsedAttributes)
    {
        sCache.clear();
        sOwner = m_spikes.data();
        sWithRanks = m_effectTimelineUsedAttributes;
    }
    if (auto it = sCache.find(answer.ev); it != sCache.end()) return it->second;

    ProtUse r;
    const float land = HitTime(*answer.ev);
    if (fx)
        if (auto bit = fx->byAgent.find(target); bit != fx->byAgent.end())
            for (const auto& e : bit->second)
            {
                if (e.skillId != answer.ev->skillId || (e.caster != 0 && e.caster != answer.casterId)) continue;
                if (e.start < land - 0.5f || e.start > land + 1.f) continue;
                r.isProt = true;
                r.start = e.start;
                r.end = std::max(e.end, e.start);
                break;
            }
    if (r.isProt)
    {
        auto tit = m_replayCtx.agents.find(target);
        const uint8_t team = tit != m_replayCtx.agents.end() ? tit->second.teamId : 0;
        int lastCaster = -1;
        float lastTime = -1.f;
        ProtEfficiency::Inputs in;
        in.skillId = answer.ev->skillId;
        in.rank = SkillRankBandAt(answer.ev->skillId, answer.casterId, land).best;
        in.maxHp = static_cast<int>(FloaterMaxHp(target, r.start));
        for (const DamagePop& pop : DamagePopsOn(target))
        {
            if (pop.time < r.start - 0.02f) continue;
            if (pop.time > r.end + 0.05f) break;
            const uint32_t mhp = pop.maxHpFix ? pop.maxHpFix : PacketMaxHp(target, pop.time, pop.value);
            const int amount = mhp > 0 ? static_cast<int>(std::lround(std::fabs(pop.value) * mhp)) : 0;
            if (pop.heal)
            {
                if (pop.skillId == answer.ev->skillId && pop.casterId == answer.casterId) r.healedBack += amount;
                else if (pop.time <= r.end + 0.02f) in.heals.push_back(amount);
                continue;
            }
            if (pop.time > r.end + 0.02f) continue;
            auto cit = m_replayCtx.agents.find(pop.casterId);
            if (cit == m_replayCtx.agents.end() || cit->second.teamId == team) continue;
            if (pop.casterId == lastCaster && pop.time - lastTime < 0.03f)
            {
                if (!in.hits.empty()) in.hits.back() += amount;   // the same hit's second number
                continue;
            }
            lastCaster = pop.casterId;
            lastTime = pop.time;
            ++r.hits;
            in.hits.push_back(amount);
        }
        in.ownHeal = r.healedBack;
        const ProtEfficiency::Result fx2 = ProtEfficiency::Evaluate(in);
        r.kind = static_cast<int>(fx2.kind);
        r.reversed = fx2.reversed;
        r.offset = fx2.offset;
        r.prevented = fx2.prevented;
        r.endHeal = fx2.endHeal;
        r.healBoost = fx2.healBoost;
        r.capped = fx2.capped;
        r.estimate = fx2.estimate;
    }
    sCache[answer.ev] = r;
    return r;
}

// The spike the card shows: the one picked with the arrows or the bar while the playhead is near it,
// else the last one whose first cast has started, until 8 s after its last hit.
int ReplayWindow::PianoRollCardSpike(float now) const
{
    // Not following: only the spike picked with the arrows or the bar, wherever the playhead is.
    if (!m_pianoRollFollowSpikes)
        return m_pianoRollSpikeSel >= 0 && m_pianoRollSpikeSel < static_cast<int>(m_spikes.size()) ? m_pianoRollSpikeSel : -1;
    int best = -1;   // the latest spike whose first cast has started (within 1 s), until 8 s after it
    for (size_t i = 0; i < m_spikes.size(); ++i)
        if (m_spikes[i].cue - 1.f <= now && now - m_spikes[i].lastHit < 8.f &&
            (best < 0 || m_spikes[i].cue >= m_spikes[best].cue)) best = static_cast<int>(i);
    // A spike picked with the arrows or the bar stays until a later one starts.
    if (m_pianoRollSpikeSel >= 0 && m_pianoRollSpikeSel < static_cast<int>(m_spikes.size()))
    {
        const SpikeInfo& sel = m_spikes[m_pianoRollSpikeSel];
        if (now >= sel.cue - 1.5f && now <= sel.lastHit + 8.f && (best < 0 || m_spikes[best].cue <= sel.cue))
            return m_pianoRollSpikeSel;
    }
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
    dl->AddText(font, font->FontSize, ImVec2(p0.x + 10.f, p0.y + 7.f), ImGui::ColorConvertFloat4ToU32(kPrGold), "Spikes");
    char buf[96];
    snprintf(buf, sizeof(buf), "%d in the match", static_cast<int>(m_spikes.size()));
    dl->AddText(font, font->FontSize, ImVec2(p0.x + 10.f, p0.y + 30.f), ImGui::ColorConvertFloat4ToU32(kPrDim), buf);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.f + font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, "Spikes").x, p0.y + 7.f));
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
            if (answered) dl->AddCircleFilled(c, 2.6f, IM_COL32(0xE1, 0xBE, 0x50, 0xFF));
            else          dl->AddCircle(c, 2.6f, IM_COL32(0xE1, 0xBE, 0x50, 0x99), 0, 1.f);
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
    dl->AddRectFilled(ImVec2(wx0, p0.y + 1.f), ImVec2(wx1, p0.y + kH - 2.f), IM_COL32(0xE1, 0xBE, 0x50, 0x12));
    dl->AddRect(ImVec2(wx0, p0.y + 1.f), ImVec2(wx1, p0.y + kH - 2.f), IM_COL32(0xE1, 0xBE, 0x50, 0xB0), 0.f, 0, 1.f);
    dl->AddLine(ImVec2(nx, p0.y), ImVec2(nx, p0.y + kH - 1.f), IM_COL32(0xE1, 0xBE, 0x50, 0xFF), 1.5f);

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
            if (m_pianoRollFollowSpikes) { m_pianoRollPinned = { sp.target }; m_pianoRollPinFromSpike = true; }
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
        if (si->attribute == 13) return true;   // Healing Prayers: Infuse Health heals without a Heal scale
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
    auto section = [&](const char* title, const char* help, const char* right, const char* rightHelp = nullptr,
                       const char* figure = nullptr) {
        ImGui::Dummy(ImVec2(1.f, 6.f));
        ImGui::TextColored(ImVec4(225 / 255.f, 190 / 255.f, 80 / 255.f, 1.f), "%s", title);
        if (figure) { ImGui::SameLine(0.f, 10.f); ImGui::TextColored(ImVec4(1.f, 0.45f, 0.40f, 1.f), "%s", figure); }
        if (help) { ImGui::SameLine(0.f, 4.f); PrHelp(help); }
        if (right)
        {
            const float qW = rightHelp ? ImGui::CalcTextSize("(?)").x + 4.f : 0.f;
            ImGui::SameLine(kMargin + cw - ImGui::CalcTextSize(right).x - qW);
            ImGui::TextColored(kPrDim, "%s", right);
            if (rightHelp)
            {
                ImGui::SameLine(0.f, 4.f);
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) PrTip(rightHelp);
            }
        }
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
            if (m_pianoRollFollowSpikes) { m_pianoRollPinned = { m_spikes[idx].target }; m_pianoRollPinFromSpike = true; }
        };
        constexpr float kArrow = 22.f;
        if (PrGwArrow(dev, "##pr_prev", false, kArrow))
        {
            int idx = -1;
            if (cur != list.end()) { if (cur != list.begin()) idx = *(cur - 1); }
            else for (int i : list) if (m_spikes[i].cue < now - 1.1f) idx = i;
            go(idx);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous spike");
        if (cur != list.end()) snprintf(buf, sizeof(buf), "Spike %d / %d", static_cast<int>(cur - list.begin()) + 1, static_cast<int>(list.size()));
        else                   snprintf(buf, sizeof(buf), "%d spikes", static_cast<int>(list.size()));
        const float arrowW = kArrow;
        ImGui::SameLine(kMargin + (cw - ImGui::CalcTextSize(buf).x) * 0.5f);
        vText(arrowW, kPrGold, buf);
        ImGui::SameLine(kMargin + cw - arrowW);
        if (PrGwArrow(dev, "##pr_next", true, kArrow))
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

    // The numbers the casts put on the player, from the recorded damage and heal packets: a hit takes
    // its caster's damage numbers with its skill (from its start until 3 s after its cast end - a
    // projectile lands later); failing that, the caster's numbers in the tick nearest its end. A heal
    // takes its caster's heal numbers with its skill near its end, plus a Divine Favor bonus (no skill)
    // in the same tick. Each number is used once.
    const std::vector<DamagePop>& pops = DamagePopsOn(sp.target);
    std::vector<char> popUsed(pops.size(), 0);
    auto popAmount = [&](const DamagePop& pop) {
        const uint32_t mhp = pop.maxHpFix ? pop.maxHpFix : PacketMaxHp(sp.target, pop.time, pop.value);
        return mhp > 0 ? static_cast<int>(std::lround(std::fabs(pop.value) * mhp)) : 0;
    };
    auto amountOf = [&](const CastRef& c, bool heal, float from, float to) -> int {
        auto first = std::lower_bound(pops.begin(), pops.end(), from, [](const DamagePop& p, float t) { return p.time < t; });
        int pick = -1;
        float best = FLT_MAX;
        for (auto it = first; it != pops.end() && it->time <= to; ++it)
        {
            const size_t i = static_cast<size_t>(it - pops.begin());
            if (popUsed[i] || it->heal != heal || it->casterId != c.casterId) continue;
            const float d = std::fabs(it->time - HitTime(*c.ev)) - (it->skillId == c.ev->skillId ? 100.f : 0.f);
            if (d < best) { best = d; pick = static_cast<int>(i); }
        }
        if (pick < 0) return -1;
        int total = 0;
        const float tick = pops[pick].time;
        const int skill = pops[pick].skillId;
        for (size_t i = 0; i < pops.size(); ++i)
        {
            const DamagePop& q = pops[i];
            if (popUsed[i] || q.heal != heal || q.casterId != c.casterId || std::fabs(q.time - tick) > 0.03f) continue;
            if (static_cast<int>(i) != pick && q.skillId != skill && q.skillId != 0) continue;
            popUsed[i] = 1;
            total += popAmount(q);
        }
        return total;
    };
    std::vector<int> hitDamage, answerHeal;
    int spikeDamage = 0;
    bool damageKnown = false;
    for (const CastRef& h : sp.hits)
    {
        hitDamage.push_back(amountOf(h, false, h.ev->startTime, HitTime(*h.ev) + 3.f));
        if (hitDamage.back() >= 0) { spikeDamage += hitDamage.back(); damageKnown = true; }
    }
    for (const CastRef& a : sp.answers)
        answerHeal.push_back(isHeal(a.ev->skillId) ? amountOf(a, true, HitTime(*a.ev) - 0.05f, HitTime(*a.ev) + 0.5f) : -1);

    // The rest of the enemy damage while the spike ran (first enemy cast to last hit): weapon hits
    // apart, then everything else (another skill, a condition). Part of the spike's total.
    int weaponDamage = 0, weaponHits = 0, otherDamage = 0;
    {
        auto tit = m_replayCtx.agents.find(sp.target);
        const uint8_t team = tit != m_replayCtx.agents.end() ? tit->second.teamId : 0;
        int lastCaster = -1;
        float lastTime = -1.f;
        for (size_t i = 0; i < pops.size(); ++i)
        {
            const DamagePop& q = pops[i];
            if (q.time < sp.cue - 0.02f) continue;
            if (q.time > sp.lastHit + 0.05f) break;
            if (popUsed[i] || q.heal) continue;
            auto cit = m_replayCtx.agents.find(q.casterId);
            if (cit == m_replayCtx.agents.end() || cit->second.teamId == team) continue;
            popUsed[i] = 1;
            const int amount = popAmount(q);
            if (q.weaponHit)
            {
                weaponDamage += amount;
                if (!(q.casterId == lastCaster && q.time - lastTime < 0.03f)) ++weaponHits;   // one hit, its vampiric part
                lastCaster = q.casterId;
                lastTime = q.time;
            }
            else otherDamage += amount;
        }
        if (weaponDamage + otherDamage > 0) { spikeDamage += weaponDamage + otherDamage; damageKnown = true; }
    }

    // ── Title band: SPIKE ON, the player who took it (large), the time ──
    {
        const float fs = font->FontSize, big = std::round(fs * 1.3f);
        const float bandH = std::round(10.f + fs + 4.f + std::max(big, iconSz + 4.f) + 10.f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 q(p.x + cw, p.y + bandH);
        dl->AddRectFilled(p, q, IM_COL32(0xE8, 0x50, 0x48, 0x26), 4.f);
        dl->AddRectFilled(p, ImVec2(p.x + 3.f, q.y), IM_COL32(0xFF, 0x8A, 0x80, 0xFF), 2.f);
        dl->AddText(font, fs, ImVec2(p.x + 12.f, p.y + 8.f), ImGui::ColorConvertFloat4ToU32(kPrSpike), "SPIKE ON");
        PrClock(sp.firstHit, buf, sizeof(buf));
        dl->AddText(font, fs, ImVec2(q.x - 10.f - font->CalcTextSizeA(fs, FLT_MAX, 0.f, buf).x, p.y + 8.f),
                    ImGui::ColorConvertFloat4ToU32(kPrGold), buf);
        float nx = p.x + 12.f;
        const float ny = p.y + 8.f + fs + 6.f;
        if (const AgentReplayData* a = agentOf(sp.target))
        {
            const float isz = iconSz + 4.f;
            if (ImTextureID prof = a->primaryProf > 0 ? LoadProfIcon(dev, a->primaryProf) : nullptr)
            {
                dl->AddImage(prof, ImVec2(nx, ny), ImVec2(nx + isz, ny + isz));
                nx += isz + 8.f;
            }
            const ImVec4 tc = PrTeamText(a->teamId);
            dl->AddText(PrFont(big), PrFont(big)->FontSize, ImVec2(nx, ny + (isz - big) * 0.5f), ImGui::ColorConvertFloat4ToU32(tc), a->playerName.c_str());
        }
        ImGui::Dummy(ImVec2(cw, bandH));
    }

    // ── Four facts in boxes ──
    {
        ImGui::Dummy(ImVec2(1.f, 2.f));
        constexpr float kGap = 6.f;
        const float valSz = std::round(font->FontSize * 1.35f);
        const float kBoxH = std::round(font->FontSize + valSz + 20.f);
        // Four in a row when the labels fit, else two rows of two.
        float widest = 0.f;
        for (const char* l : { "Skills", "Hit in", "Damage", "Lowest HP" })
            widest = std::max(widest, ImGui::CalcTextSize(l).x);
        const int cols = std::floor((cw - 3.f * kGap) / 4.f) >= widest + 18.f ? 4 : 2;
        const float boxW = std::floor((cw - (cols - 1) * kGap) / cols);
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        struct Fact { const char* label; const char* help; std::string value; ImVec4 col; float bar; };
        char v1[16], v2[16], v3[16], v4[16];
        if (damageKnown) snprintf(v4, sizeof(v4), "%d", spikeDamage); else snprintf(v4, sizeof(v4), "-");
        snprintf(v1, sizeof(v1), "%d", static_cast<int>(sp.hits.size()));
        snprintf(v2, sizeof(v2), "%.2f s", sp.lastHit - sp.firstHit);
        if (sp.lowHp < 0.f)       snprintf(v3, sizeof(v3), "-");
        else if (sp.lowHp <= 0.f) snprintf(v3, sizeof(v3), "died");
        else                      snprintf(v3, sizeof(v3), "%d%%", static_cast<int>(std::round(sp.lowHp * 100.f)));
        const Fact facts[4] = {
            { "Skills",    "How many enemy skills hit the player. 3 or more within 1.5 seconds is a spike.", v1, ImVec4(0.92f, 0.92f, 0.92f, 1.f), -1.f },
            { "Hit in",    "How close together the hits landed. The smaller the number, the harder the spike is to stop.", v2, kPrGold, -1.f },
            { "Damage",    "All the enemy damage the player took during the spike:\nthe skills, weapon hits and the rest.", v4, ImVec4(1.f, 0.45f, 0.40f, 1.f), -1.f },
            { "Lowest HP", "How low the player's health went. 0% means they died.", v3, ImVec4(0.92f, 0.92f, 0.92f, 1.f), std::max(0.f, sp.lowHp) },
        };
        for (int i = 0; i < 4; ++i)
        {
            const Fact& f = facts[i];
            const ImVec2 r0(b0.x + (i % cols) * (boxW + kGap), b0.y + (i / cols) * (kBoxH + kGap)), r1(r0.x + boxW, r0.y + kBoxH);
            ImGui::SetCursorScreenPos(r0);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##fact", ImVec2(boxW, kBoxH));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(r0, r1, hov ? IM_COL32(255, 255, 255, 26) : IM_COL32(255, 255, 255, 14), 4.f);
            dl->AddRect(r0, r1, hov ? IM_COL32(0xE1, 0xBE, 0x50, 0x90) : IM_COL32(255, 255, 255, 34), 4.f, 0, 1.f);
            dl->AddText(font, font->FontSize, ImVec2(r0.x + 8.f, r0.y + 5.f), ImGui::ColorConvertFloat4ToU32(kPrDim), f.label);
            dl->AddText(PrFont(valSz), PrFont(valSz)->FontSize, ImVec2(r0.x + 8.f, r0.y + 7.f + font->FontSize), ImGui::ColorConvertFloat4ToU32(f.col), f.value.c_str());
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
        const int rows = 4 / cols;
        ImGui::SetCursorScreenPos(ImVec2(b0.x, b0.y + rows * kBoxH + (rows - 1) * kGap + 4.f));
    }

    // One line per cast: icon + skill, a value on the right, "by" + caster under it.
    auto castLine = [&](const CastRef& c, const char* tag, const char* right, const ImVec4& rightCol, const char* extra = nullptr,
                        const ImVec4& tagCol = kPrDim) {
        if (ImTextureID tex = LoadSkillIcon(this, dev, c.ev->skillId, m_skillIconIndex, m_skillIconCache))
            ImGui::Image(tex, ImVec2(iconSz, iconSz));
        else
            ImGui::Dummy(ImVec2(iconSz, iconSz));
        ImGui::SameLine(0.f, 6.f);
        vText(iconSz, ImVec4(1.f, 1.f, 1.f, 1.f), GetSkillDisplayName(c.ev->skillId).c_str());
        if (tag) { ImGui::SameLine(0.f, 6.f); vText(iconSz, tagCol, tag); }
        rightText(iconSz, rightCol, right);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iconSz + 6.f);
        ImGui::TextColored(kPrDim, "by");
        ImGui::SameLine(0.f, 5.f);
        who(c.casterId, lineH);
        if (extra)
        {
            ImGui::SameLine(0.f, 8.f);
            vText(lineH, kPrDim, extra);
            if (ImGui::IsItemHovered()) PrTip(kHelpReaction);
        }
        ImGui::Dummy(ImVec2(1.f, 2.f));
    };

    // ── Hits ──
    char hitsTotal[48];
    snprintf(hitsTotal, sizeof(hitsTotal), "-%d in total", spikeDamage);
    section("HITS", nullptr, "hits at", nullptr, damageKnown ? hitsTotal : nullptr);
    {
        const ImVec2 h0 = ImGui::GetCursorScreenPos();
        for (size_t i = 0; i < sp.hits.size(); ++i)
        {
            char dmg[16];
            if (hitDamage[i] >= 0) snprintf(dmg, sizeof(dmg), "-%d", hitDamage[i]);
            PrClock(HitTime(*sp.hits[i].ev), buf, sizeof(buf));
            castLine(sp.hits[i], hitDamage[i] >= 0 ? dmg : nullptr, buf, kPrGold, nullptr, ImVec4(1.f, 0.45f, 0.40f, 1.f));
        }
        // Weapon hits and the rest during the spike, one line each.
        auto restLine = [&](const char* label, int amount) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iconSz + 6.f);
            ImGui::TextColored(ImVec4(216 / 255.f, 220 / 255.f, 226 / 255.f, 1.f), "%s", label);
            ImGui::SameLine(0.f, 6.f);
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.40f, 1.f), "-%d", amount);
        };
        if (weaponDamage > 0)
        {
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "Weapon attacks (%d hit%s)", weaponHits, weaponHits == 1 ? "" : "s");
            restLine(lbl, weaponDamage);
        }
        if (otherDamage > 0) restLine("Other damage", otherDamage);
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(h0, ImVec2(h0.x + cw, ImGui::GetCursorScreenPos().y)))
        {
            snprintf(buf, sizeof(buf), "Spike: %d damage skills hit in %.2f s", static_cast<int>(sp.hits.size()), sp.lastHit - sp.firstHit);
            DrawCastTableTooltip(sp.hits, buf);
        }
    }

    // ── Answers ──
    section("ANSWERS", "Prots and heals from teammates on the player.", "timing", kHelpTiming);
    if (sp.answers.empty())
        ImGui::TextColored(kPrDim, "No prot or heal on the player.");
    const float call = PrCall(sp.hits);
    for (size_t ai = 0; ai < sp.answers.size(); ++ai)
    {
        const CastRef& a = sp.answers[ai];
        char extra[48], tag[24];
        snprintf(buf, sizeof(buf), "%+.2f s", HitTime(*a.ev) - sp.firstHit);
        snprintf(extra, sizeof(extra), "reaction %+.2f s", a.ev->startTime - call);
        const bool heal = isHeal(a.ev->skillId);
        if (heal && answerHeal[ai] >= 0) snprintf(tag, sizeof(tag), "+%d", answerHeal[ai]);
        else                             snprintf(tag, sizeof(tag), "%s", heal ? "heal" : "prot");
        castLine(a, tag, buf, kPrGold, extra, heal && answerHeal[ai] >= 0 ? ImVec4(0.55f, 0.90f, 0.50f, 1.f) : kPrDim);
        const ProtUse pu = PianoRollProtUse(a, sp.target);
        if (pu.isProt)
        {
            const std::string note = PrProtLine(pu.hits, pu.kind, pu.reversed, pu.offset, pu.prevented, pu.endHeal,
                                                pu.healBoost, pu.capped, pu.healedBack);
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + iconSz + 6.f, ImGui::GetCursorPosY() - 2.f));
            ImGui::TextColored(ImVec4(0.74f, 0.82f, 0.70f, 1.f), "%s", note.c_str());
            if (ImGui::IsItemHovered()) PrTip(kHelpProtUse);
            ImGui::Dummy(ImVec2(1.f, 2.f));
        }
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
        if (si->attribute == 13) return true;   // Healing Prayers: Infuse Health heals without a Heal scale
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
        dl->AddText(font, font->FontSize, ImVec2(x0, p0.y + 6.f), TeamColour(team, 0xFF), buf);
        ImGui::SetCursorScreenPos(ImVec2(x0 + font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, buf).x + 6.f, p0.y + 5.f));
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
            row("covered",  "Spikes they helped on, out of their team's spikes.");
            row("reaction", "Second enemy starts casting -> they start casting.");
            row("",         "Their own cast time is not included.");
            row("timing",   "First enemy hit -> their prot or heal takes effect.");
            row("",         "Their own cast time is included.");
            row("first",    "Times they were the first teammate to help.");
            ImGui::Spacing();
            ImGui::TextColored(kPrDim, "Reaction and timing on a card are medians over their spikes.");
            ImGui::Spacing();
            ImGui::TextColored(kPrDim, "Hover a number on a card for more.");
            ImGui::Spacing();
            ImGui::TextColored(kPrDim, "Click a card to see their spikes on the bar.");
            ImGui::EndTooltip();
        }

        std::vector<int> monks;
        for (int id : ids) if (const auto* a = agentOf(id); a && a->primaryProf == 3) monks.push_back(id);
        if (monks.empty())
        {
            dl->AddText(font, font->FontSize, ImVec2(x0, p0.y + 30.f), ImGui::ColorConvertFloat4ToU32(kPrDim), "No Monk on this team.");
            ImGui::PopID();
            continue;
        }

        constexpr float kGap = 8.f;
        const float cardH = std::min(std::round(font->FontSize * 3.f + 36.f), h - 34.f);
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
            if (sel) dl->AddRect(r0, r1, IM_COL32(0xE1, 0xBE, 0x50, 0xFF), 3.f, 0, 1.5f);

            dl->PushClipRect(r0, r1, true);
            // Name
            float tx = r0.x + 7.f;
            if (ImTextureID prof = m->primaryProf > 0 ? LoadProfIcon(dev, m->primaryProf) : nullptr)
            {
                dl->AddImage(prof, ImVec2(tx, r0.y + 6.f), ImVec2(tx + 18.f, r0.y + 24.f));
                tx += 22.f;
            }
            dl->AddText(font, font->FontSize, ImVec2(tx, r0.y + 6.f), IM_COL32(216, 220, 226, 255), m->playerName.c_str());
            // Covered bar
            snprintf(buf, sizeof(buf), "%d / %d", covered, static_cast<int>(mine.size()));
            const float numW = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, buf).x;
            const float by = r0.y + 14.f + font->FontSize, barR = std::max(r1.x - 14.f - numW, r0.x + 27.f);
            dl->AddRectFilled(ImVec2(r0.x + 7.f, by), ImVec2(barR, by + 5.f), IM_COL32(255, 255, 255, 22), 2.5f);
            if (!mine.empty() && covered > 0)
                dl->AddRectFilled(ImVec2(r0.x + 7.f, by), ImVec2(r0.x + 7.f + (barR - r0.x - 7.f) * covered / mine.size(), by + 5.f), TeamColour(team, 0xD0), 2.5f);
            dl->AddText(font, font->FontSize, ImVec2(r1.x - 7.f - numW, by - 5.f), IM_COL32(216, 220, 226, 255), buf);

            // reaction (first: it is the number that matters), timing, first. Narrow cards (many
            // Monks on a team) keep reaction alone; the rest is in the hover.
            const bool compact = cardW < 170.f;
            const float colStep = compact ? cardW - 14.f : (cardW - 14.f) / 3.f, ly = by + 11.f;
            const char* labels[3] = { "reaction", "timing", "first" };
            const float tMed = PrMedian(timings), rMed = PrMedian(reactions);
            char vals[3][16];
            if (std::isnan(rMed)) snprintf(vals[0], sizeof(vals[0]), "-"); else snprintf(vals[0], sizeof(vals[0]), "%+.2f s", rMed);
            if (std::isnan(tMed)) snprintf(vals[1], sizeof(vals[1]), "-"); else snprintf(vals[1], sizeof(vals[1]), "%+.2f s", tMed);
            snprintf(vals[2], sizeof(vals[2]), "%d", first);
            const ImU32 valCol[3] = { ImGui::ColorConvertFloat4ToU32(kPrGold), IM_COL32(216, 220, 226, 255), IM_COL32(216, 220, 226, 255) };
            for (int c = 0; c < (compact ? 1 : 3); ++c)
            {
                const float cx = r0.x + 7.f + c * colStep;
                dl->AddText(font, font->FontSize, ImVec2(cx, ly), c == 0 ? ImGui::ColorConvertFloat4ToU32(kPrGold) : ImGui::ColorConvertFloat4ToU32(kPrDim), labels[c]);
                dl->AddText(font, font->FontSize, ImVec2(cx, ly + font->FontSize), valCol[c], vals[c]);
            }
            // The reaction column stands out: a thin gold underline under its value.
            if (!std::isnan(rMed))
            {
                const float vw = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, vals[0]).x;
                const float uy = ly + font->FontSize * 2.f + 1.f;
                dl->AddLine(ImVec2(r0.x + 7.f, uy), ImVec2(r0.x + 7.f + vw, uy), IM_COL32(0xE1, 0xBE, 0x50, 0xB0), 1.f);
            }
            dl->PopClipRect();

            // Hover a number: what it measures; elsewhere on the card: the summary.
            int hovNum = -1;   // 0..2 = reaction / timing / first, 3 = the covered bar
            if (hov)
            {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                if (mp.y >= ly - 2.f) hovNum = compact ? 0 : std::clamp(static_cast<int>((mp.x - r0.x - 7.f) / colStep), 0, 2);
                else if (mp.y >= by - 7.f && mp.y <= by + 12.f) hovNum = 3;
            }
            if (hovNum >= 0)
            {
                const char* help[4] = { kHelpReaction, kHelpTiming, kHelpFirst, kHelpCovered };
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(help[hovNum]);
                if (hovNum == 0 || hovNum == 1)
                {
                    ImGui::Spacing();
                    ImGui::TextColored(kPrDim, "%s", kHelpTypical);
                }
                ImGui::EndTooltip();
            }
            else if (hov && !mine.empty())
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
                    line("Median timing", kPrGold, buf);
                    ImGui::SameLine(0.f, 10.f); ImGui::TextColored(kPrDim, "first enemy hit -> their help takes effect");
                    snprintf(buf, sizeof(buf), "%+.2f s", PrMedian(reactions));
                    line("Median reaction", kPrGold, buf);
                    ImGui::SameLine(0.f, 10.f); ImGui::TextColored(kPrDim, "second enemy starts casting -> they start casting");
                }
                if (!healTimes.empty())
                {
                    snprintf(buf, sizeof(buf), "%+.2f s", PrMedian(healTimes));
                    line("Median heal time", kPrGold, buf);
                    ImGui::SameLine(0.f, 10.f); ImGui::TextColored(kPrDim, "first enemy hit -> their heal lands");
                }

                // Their prots, each cast once (one cast can answer two spikes).
                std::unordered_set<const SkillUseEvent*> seen;
                std::unordered_map<int, int> backBySkill;
                int prots = 0, protsHit = 0, back = 0;
                for (const SpikeInfo* sp : mine)
                    for (const auto& a : sp->answers)
                    {
                        if (a.casterId != monkId || !seen.insert(a.ev).second) continue;
                        const ProtUse pu = PianoRollProtUse(a, sp->target);
                        if (!pu.isProt) continue;
                        ++prots;
                        if (pu.hits > 0) ++protsHit;
                        const int spared = pu.reversed + pu.offset + pu.prevented;
                        back += spared;
                        backBySkill[a.ev->skillId] += spared;
                    }
                if (prots > 0)
                {
                    snprintf(buf, sizeof(buf), "%d of %d (%d%%)", protsHit, prots, static_cast<int>(std::round(100.f * protsHit / prots)));
                    line("Prots that took a hit", kWhite, buf);
                    if (back > 0)
                    {
                        snprintf(buf, sizeof(buf), "%d Health", back);
                        line("Spared by prots", kWhite, buf);
                        ImGui::SameLine(0.f, 10.f);
                        ImGui::TextColored(kPrDim, "reversed + offset + ~prevented");
                    }
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
                            const float tw = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, lbl).x;
                            tdl->AddText(font, font->FontSize, ImVec2(ax(g) - tw * 0.5f, cy + 6.f), ImGui::ColorConvertFloat4ToU32(kPrDim), lbl);
                        }
                    }
                    tdl->AddLine(ImVec2(ax(0.f), cy - 7.f), ImVec2(ax(0.f), cy + 4.f), IM_COL32(0xFF, 0x8A, 0x80, 0xC0), 1.f);
                    for (float r : timings)
                        tdl->AddCircleFilled(ImVec2(ax(r), cy), 3.f, TeamColour(team, 0x90));
                    const float med = ax(PrMedian(timings));
                    tdl->AddRectFilled(ImVec2(med - 1.f, cy - 7.f), ImVec2(med + 1.f, cy + 7.f), IM_COL32(0xE1, 0xBE, 0x50, 0xFF));
                    ImGui::TextColored(kPrDim, "0 = the first enemy hit. Left of 0: their help was on before the damage. Gold = their median.");
                }

                // What they answered with, most used first.
                std::vector<std::pair<int, int>> used;   // skill, count
                std::unordered_set<const SkillUseEvent*> counted;
                for (const SpikeInfo* sp : mine)
                    for (const auto& a : sp->answers)
                        if (a.casterId == monkId && counted.insert(a.ev).second)
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
                        if (const auto bit = backBySkill.find(used[u].first); bit != backBySkill.end() && bit->second > 0)
                        {
                            ImGui::SameLine(0.f, 10.f);
                            const char* verb = "spared";
                            switch (ProtEfficiency::KindOf(used[u].first))
                            {
                            case ProtEfficiency::Kind::Reversed:         verb = "reversed"; break;
                            case ProtEfficiency::Kind::Offset:           verb = "offset"; break;
                            case ProtEfficiency::Kind::FlatReduction:
                            case ProtEfficiency::Kind::PercentReduction: verb = "prevented"; break;
                            default: break;
                            }
                            ImGui::TextColored(ImVec4(0.74f, 0.82f, 0.70f, 1.f), "%d %s", bit->second, verb);
                        }
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
