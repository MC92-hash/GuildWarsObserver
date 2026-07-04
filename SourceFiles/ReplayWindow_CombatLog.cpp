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
// Combat Log panel
// ---------------------------------------------------------------------------

void ReplayWindow::DrawCombatLog()
{
    if (!m_showCombatLog || !m_combatLogBuilt) return;

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    float minW = 360.f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 200.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("combat_log"))
        m_panelLayout.ApplySize("combat_log");
    else
        ImGui::SetNextWindowSize(ImVec2(720.f, 440.f), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("combat_log");

    // Gold-accented dark panel styling
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    ImVec4(1.f, 1.f, 1.f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);

    if (!ImGui::Begin("Combat Log", &m_showCombatLog))
    {
        m_panelLayout.TrackWindow("combat_log");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
        return;
    }

    m_panelLayout.TrackWindow("combat_log");

    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    const ImU32 uBlue    = IM_COL32(74, 200, 255, 255);
    const ImU32 uRed     = IM_COL32(255, 107, 107, 255);
    const ImU32 uGray    = IM_COL32(154, 164, 177, 255);
    const ImU32 uWhite   = IM_COL32(232, 236, 242, 255);
    const ImU32 uGreen   = IM_COL32(64, 224, 128, 255);
    const ImU32 uOrange  = IM_COL32(255, 159, 64, 255);
    const ImU32 uPurple  = IM_COL32(191, 97, 255, 255);
    const ImU32 uMuted   = IM_COL32(200, 176, 128, 255);
    const ImU32 uKillRed = IM_COL32(255, 80, 80, 255);
    const ImU32 uKillBg  = IM_COL32(208, 72, 72, 31);
    const ImU32 uKillBdr = IM_COL32(204, 48, 48, 255);
    const ImU32 uHoverBg   = IM_COL32(255, 215, 100, 15);
    const ImU32 uSelectBg  = IM_COL32(255, 230, 120, 38);
    const ImU32 uSelectBdr = IM_COL32(255, 215, 100, 200);
    const ImU32 uGoldDim   = IM_COL32(255, 215, 100, 60);
    const ImU32 uTsCol     = IM_COL32(200, 176, 128, 255);
    const ImU32 uDmgRed    = IM_COL32(255, 128, 128, 255);

    auto teamColorU32 = [&](int agentId) -> ImU32 {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return uGray;
        if (it->second.teamId == 1) return uRed;
        if (it->second.teamId == 2) return uBlue;
        return uGray;
    };

    auto agentNameStr = [&](int agentId) -> std::string {
        if (agentId <= 0) return "";
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return std::format("#{}", agentId);
        auto& a = it->second;
        if (a.type == AgentType::Player) return a.playerName;
        if (!a.categoryName.empty()) return a.categoryName;
        return std::format("#{}", agentId);
    };

    // --- Filter bar ---
    {
        // Snapshot to detect changes
        bool prevDmg = m_clFilterDamage, prevHeal = m_clFilterHeals;
        bool prevSkill = m_clFilterSkills, prevIntr = m_clFilterInterrupt, prevCanc = m_clFilterCancel;
        bool prevDeath = m_clFilterDeaths, prevAtk = m_clFilterAttacks, prevJumbo = m_clFilterJumbo;
        int  prevPlayer = m_clFilterPlayerId;

        auto FilterPill = [](const char* label, bool active) -> bool {
            ImVec4 bg  = active ? ImVec4(0.14f, 0.11f, 0.04f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.05f);
            ImVec4 tx  = active ? ImVec4(1.f, 0.91f, 0.69f, 1.f)
                                : ImVec4(0.60f, 0.64f, 0.69f, 1.f);
            ImVec4 hov = active ? ImVec4(0.20f, 0.16f, 0.06f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.12f);
            ImVec4 bdr = active ? ImVec4(1.f, 0.84f, 0.39f, 0.80f)
                                : ImVec4(1.f, 1.f, 1.f, 0.08f);
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 1.f : 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        float availW = ImGui::GetContentRegionAvail().x;
        float lineX = 0.f;

        auto MaybeSameLine = [&](const char* nextLabel) {
            float est = ImGui::CalcTextSize(nextLabel).x + 24.f;
            if (lineX + est < availW) {
                ImGui::SameLine();
            } else {
                lineX = 0.f;
            }
        };

        // ALL is visually active when every category filter is on
        bool allOn = m_clFilterDamage && m_clFilterHeals && m_clFilterSkills &&
                     m_clFilterInterrupt && m_clFilterCancel &&
                     m_clFilterDeaths && m_clFilterAttacks && m_clFilterJumbo;
        if (FilterPill("All", allOn))
        {
            bool target = !allOn;
            m_clFilterDamage = m_clFilterHeals = m_clFilterSkills = target;
            m_clFilterInterrupt = m_clFilterCancel = m_clFilterDeaths = m_clFilterAttacks = m_clFilterJumbo = target;
        }
        lineX = ImGui::GetItemRectSize().x;

        MaybeSameLine("Damage");
        if (FilterPill("Damage", m_clFilterDamage)) m_clFilterDamage = !m_clFilterDamage;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Heals");
        if (FilterPill("Heals", m_clFilterHeals)) m_clFilterHeals = !m_clFilterHeals;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Skills");
        if (FilterPill("Skills", m_clFilterSkills)) m_clFilterSkills = !m_clFilterSkills;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Interrupt");
        if (FilterPill("Interrupt", m_clFilterInterrupt)) m_clFilterInterrupt = !m_clFilterInterrupt;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Cancel");
        if (FilterPill("Cancel", m_clFilterCancel)) m_clFilterCancel = !m_clFilterCancel;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Deaths");
        if (FilterPill("Deaths", m_clFilterDeaths)) m_clFilterDeaths = !m_clFilterDeaths;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Attacks");
        if (FilterPill("Attacks", m_clFilterAttacks)) m_clFilterAttacks = !m_clFilterAttacks;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Jumbo");
        if (FilterPill("Jumbo", m_clFilterJumbo)) m_clFilterJumbo = !m_clFilterJumbo;

        if (m_clFilterPlayerId >= 0)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.f, 1.f));
            std::string filterLabel = std::format("Filtering: {} [x]",
                agentNameStr(m_clFilterPlayerId));
            if (ImGui::SmallButton(filterLabel.c_str()))
                m_clFilterPlayerId = -1;
            ImGui::PopStyleColor();
        }

        bool filterChanged =
            m_clFilterDamage != prevDmg || m_clFilterHeals != prevHeal ||
            m_clFilterSkills != prevSkill || m_clFilterInterrupt != prevIntr ||
            m_clFilterCancel != prevCanc || m_clFilterDeaths != prevDeath ||
            m_clFilterAttacks != prevAtk || m_clFilterJumbo != prevJumbo ||
            m_clFilterPlayerId != prevPlayer;
        if (filterChanged)
            m_clScrollToSelected = true;
    }

    // --- Skill name filter (autocomplete multi-select) ---
    bool inputFocused = false, inputHovered = false;
    {
        EnsureSkillIconIndex();
        ID3D11Device* sDev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;

        // Show selected skill chips
        if (!m_clFilterSkillIds.empty())
        {
            for (int idx = 0; idx < (int)m_clFilterSkillIds.size(); ++idx)
            {
                int sid = m_clFilterSkillIds[idx];
                std::string chipLabel = GetSkillDisplayName(sid) + " x##sk" + std::to_string(idx);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.82f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 2));
                if (ImGui::SmallButton(chipLabel.c_str()))
                {
                    m_clFilterSkillIds.erase(m_clFilterSkillIds.begin() + idx);
                    m_clFilterSkillSet.erase(sid);
                    --idx;
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Clear all##clsk"))
            {
                m_clFilterSkillIds.clear();
                m_clFilterSkillSet.clear();
            }
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##skillsearch",
            "Filter by skill name...", m_clSkillSearchBuf, sizeof(m_clSkillSearchBuf));
        inputFocused = ImGui::IsItemActive();
        inputHovered = ImGui::IsItemHovered();
        ImVec2 inputMin = ImGui::GetItemRectMin();
        ImVec2 inputMax = ImGui::GetItemRectMax();
        float dropdownW = inputMax.x - inputMin.x;

        if (inputHovered || inputFocused)
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        if (ImGui::IsItemActivated()) m_clSkillSearchFocused = true;

        bool showDropdown = m_clSkillSearchFocused && m_clSkillSearchBuf[0] != '\0';
        bool dropdownHovered = false;

        if (showDropdown)
        {
            std::string query(m_clSkillSearchBuf);
            for (auto& c : query) c = (char)std::tolower((unsigned char)c);

            struct Match { int id; std::string name; };
            std::vector<Match> matches;

            const auto& db = m_skillView;
            if (db.IsLoaded())
            {
                db.ForEachSkill([&](const SkillInfo& si) {
                    if (si.name.empty()) return;
                    if (m_clFilterSkillSet.count(si.id)) return;
                    std::string lower = si.name;
                    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                    if (lower.find(query) != std::string::npos)
                        matches.push_back({si.id, si.name});
                });
            }

            for (auto& row : m_combatLog)
            {
                if (row.skillId <= 0) continue;
                if (m_clFilterSkillSet.count(row.skillId)) continue;
                if (db.IsLoaded() && db.Get(row.skillId)) continue;
                std::string sn = GetSkillDisplayName(row.skillId);
                if (sn.empty() || sn.rfind("Skill ", 0) == 0) continue;
                std::string lower = sn;
                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.find(query) != std::string::npos)
                {
                    bool dup = false;
                    for (auto& m : matches) if (m.id == row.skillId) { dup = true; break; }
                    if (!dup) matches.push_back({row.skillId, sn});
                }
            }

            std::sort(matches.begin(), matches.end(),
                [](const Match& a, const Match& b) { return a.name < b.name; });

            if (!matches.empty())
            {
                int maxShow = std::min((int)matches.size(), 10);
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float popH = rowH * (float)maxShow + 12.f;
                if ((int)matches.size() > maxShow) popH += rowH;

                ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y + 2.f));
                ImGui::SetNextWindowSize(ImVec2(dropdownW, popH));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.97f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.f));

                ImGui::Begin("##skill_dropdown", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing);

                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

                dropdownHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

                int shown = 0;
                for (auto& m : matches)
                {
                    if (shown >= 10) { ImGui::TextDisabled("... %d more", (int)matches.size() - 10); break; }
                    ImGui::PushID(m.id);

                    if (sDev)
                    {
                        ImTextureID tex = LoadSkillIcon(this, sDev, m.id,
                            m_skillIconIndex, m_skillIconCache);
                        if (tex)
                        {
                            ImGui::Image(tex, ImVec2(16, 16));
                            ImGui::SameLine();
                        }
                    }

                    if (ImGui::Selectable(m.name.c_str()))
                    {
                        m_clFilterSkillIds.push_back(m.id);
                        m_clFilterSkillSet.insert(m.id);
                        m_clSkillSearchBuf[0] = '\0';
                        m_clSkillSearchFocused = false;
                    }

                    ImGui::PopID();
                    ++shown;
                }

                ImGui::End();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }
        }

        if (!inputFocused && !dropdownHovered)
            m_clSkillSearchFocused = false;
    }

    ImGui::Separator();

    // --- Pre-filter visible rows ---
    bool skillFilterActive = !m_clFilterSkillSet.empty();
    std::vector<int> filtered;
    filtered.reserve(m_combatLog.size());
    for (int i = 0; i < (int)m_combatLog.size(); ++i)
    {
        auto& row = m_combatLog[i];
        if (row.time > m_debugTimeline) break;

        if (row.category != CombatLogCategory::Jumbo &&
            m_clFilterPlayerId >= 0 &&
            row.casterId != m_clFilterPlayerId &&
            row.targetId != m_clFilterPlayerId)
            continue;

        if (row.category != CombatLogCategory::Jumbo &&
            skillFilterActive && !m_clFilterSkillSet.count(row.skillId))
            continue;

        // No filters active → show everything
        bool noFilter = !m_clFilterDamage && !m_clFilterHeals && !m_clFilterSkills &&
                        !m_clFilterInterrupt && !m_clFilterCancel &&
                        !m_clFilterDeaths && !m_clFilterAttacks && !m_clFilterJumbo;
        bool show = noFilter;
        if (!show) {
            if (row.category == CombatLogCategory::Skill && row.interrupted)
                show = m_clFilterInterrupt || m_clFilterSkills;
            else if (row.category == CombatLogCategory::Skill && row.cancelled)
                show = m_clFilterCancel || m_clFilterSkills;
            else switch (row.category) {
            case CombatLogCategory::Damage:      show = m_clFilterDamage || (row.skillId > 0 && m_clFilterSkills); break;
            case CombatLogCategory::Heal:        show = m_clFilterHeals;  break;
            case CombatLogCategory::Skill:       show = m_clFilterSkills; break;
            case CombatLogCategory::Death:       show = m_clFilterDeaths; break;
            case CombatLogCategory::Interrupt:   show = m_clFilterInterrupt; break;
            case CombatLogCategory::KnockDown:   show = m_clFilterSkills; break;
            case CombatLogCategory::Block:       show = m_clFilterSkills; break;
            case CombatLogCategory::BasicAttack: show = m_clFilterAttacks; break;
            case CombatLogCategory::Jumbo:       show = m_clFilterJumbo;  break;
            case CombatLogCategory::Other:       show = true;             break;
            }
        }
        if (show) filtered.push_back(i);
    }

    // --- Column layout ---
    constexpr float kRowH      = 20.f;
    constexpr float kColTs     = 0.f;
    constexpr float kTsW       = 92.f;
    constexpr float kColCProf  = 92.f;    // caster profession icon
    constexpr float kProfSz    = 16.f;
    constexpr float kColCast   = 110.f;   // 92 + 16 + 2 gap
    constexpr float kCastW     = 130.f;
    constexpr float kColArr1   = 240.f;
    constexpr float kArr1W     = 16.f;
    constexpr float kColIcon   = 256.f;
    constexpr float kIconSz    = 18.f;
    constexpr float kColSkill  = 278.f;   // 256 + 18 + 4 gap
    constexpr float kSkillW    = 140.f;
    constexpr float kColArr2   = 418.f;
    constexpr float kArr2W     = 16.f;
    constexpr float kColTProf  = 434.f;   // target profession icon
    constexpr float kColTgt    = 452.f;   // 434 + 16 + 2 gap
    constexpr float kTgtW      = 130.f;
    constexpr float kColVal    = 582.f;
    constexpr float kValW      = 100.f;
    constexpr float kRowW      = 682.f;

    // --- Column headers ---
    {
        ImDrawList* hdl = ImGui::GetWindowDrawList();
        float hx = ImGui::GetCursorScreenPos().x;
        float hy = ImGui::GetCursorScreenPos().y;
        const ImU32 hdrCol = IM_COL32(200, 176, 128, 220);
        float hdrY = hy + 1.f;

        hdl->AddText(ImVec2(hx + kColTs,    hdrY), hdrCol, "Time");
        hdl->AddText(ImVec2(hx + kColCast,  hdrY), hdrCol, "Caster");
        hdl->AddText(ImVec2(hx + kColSkill, hdrY), hdrCol, "Skill");
        hdl->AddText(ImVec2(hx + kColTgt,   hdrY), hdrCol, "Target");
        hdl->AddText(ImVec2(hx + kColVal,   hdrY), hdrCol, "Value");

        float lineY = hy + ImGui::GetTextLineHeightWithSpacing();
        hdl->AddLine(ImVec2(hx, lineY), ImVec2(hx + kRowW, lineY),
            IM_COL32(255, 215, 100, 40));

        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 2.f));
    }

    // --- Scrolling log region ---
    float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##logscroll", ImVec2(0, -footerH), false,
        ImGuiWindowFlags_HorizontalScrollbar);

    EnsureSkillIconIndex();
    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    ImGuiListClipper clipper;
    clipper.Begin((int)filtered.size(), kRowH);
    while (clipper.Step())
    {
        for (int idx = clipper.DisplayStart; idx < clipper.DisplayEnd; ++idx)
        {
            auto& row = m_combatLog[filtered[idx]];
            ImGui::PushID(idx);

            float startX = ImGui::GetCursorScreenPos().x;
            float startY = ImGui::GetCursorScreenPos().y;

            if (ImGui::InvisibleButton("##r", ImVec2(kRowW, kRowH)))
            {
                m_clSelectedRowIdx = filtered[idx];
                float relX = io.MousePos.x - startX;
                if (relX >= kColCast && relX < kColCast + kCastW)
                    m_clFilterPlayerId = row.casterId;
                else
                {
                    m_debugTimeline = row.time;
                    if (row.casterId > 0)
                        EnterFollowMode(row.casterId);
                }
            }
            bool hovered = ImGui::IsItemHovered();
            bool selected = (filtered[idx] == m_clSelectedRowIdx);

            if (row.category == CombatLogCategory::Death)
            {
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), uKillBg);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + 2.f, startY + kRowH), uKillBdr);
            }
            else if (row.category == CombatLogCategory::Jumbo)
            {
                const bool isNeutShrine = (row.eventType == "NEUTRALIZED_SHRINE");
                ImU32 jbBg = isNeutShrine
                    ? IM_COL32(180, 180, 180, 18)
                    : (row.jumboTeam == 1)
                        ? IM_COL32(255, 107, 107, 20)
                        : (row.jumboTeam == 2)
                            ? IM_COL32(74, 200, 255, 20)
                            : IM_COL32(255, 215, 100, 20);
                ImU32 jbBdr = isNeutShrine
                    ? IM_COL32(180, 180, 180, 140)
                    : (row.jumboTeam == 1)
                        ? IM_COL32(255, 107, 107, 160)
                        : (row.jumboTeam == 2)
                            ? IM_COL32(74, 200, 255, 160)
                            : IM_COL32(255, 215, 100, 160);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), jbBg);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + 2.f, startY + kRowH), jbBdr);
            }

            if (selected)
            {
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), uSelectBg);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + 2.f, startY + kRowH), uSelectBdr);
            }

            if (hovered)
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), uHoverBg);

            float textY = startY + (kRowH - fontSize) * 0.5f;

            // Timestamp (left-aligned, bracketed)
            {
                float matchTime = row.time - m_displayTimeOffset;
                int totalMs = (int)(std::abs(matchTime) * 1000.f);
                char tsBuf[24];
                if (matchTime < 0.f)
                    snprintf(tsBuf, sizeof(tsBuf), "[-%02d:%02d.%03d]",
                             totalMs / 60000, (totalMs / 1000) % 60, totalMs % 1000);
                else
                    snprintf(tsBuf, sizeof(tsBuf), "[%02d:%02d.%03d]",
                             totalMs / 60000, (totalMs / 1000) % 60, totalMs % 1000);
                dl->PushClipRect(
                    ImVec2(startX + kColTs, startY),
                    ImVec2(startX + kColTs + kTsW, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColTs, textY), uTsCol, tsBuf);
                dl->PopClipRect();
            }

            // --- Death row: [Skull] [ProfIcon] PlayerName died ---
            if (row.category == CombatLogCategory::Death)
            {
                float cx = startX + kColCProf;

                ImTextureID skullTex = LoadFlagIcon(dev, "death.png");
                if (skullTex)
                {
                    float iy = startY + (kRowH - kIconSz) * 0.5f;
                    dl->AddImage(skullTex,
                        ImVec2(cx, iy),
                        ImVec2(cx + kIconSz, iy + kIconSz));
                    cx += kIconSz + 4.f;
                }

                auto cIt = m_replayCtx.agents.find(row.casterId);
                if (cIt != m_replayCtx.agents.end() && cIt->second.primaryProf > 0 && dev)
                {
                    ImTextureID pTex = LoadProfIcon(dev, cIt->second.primaryProf);
                    if (pTex)
                    {
                        float iy = startY + (kRowH - kProfSz) * 0.5f;
                        dl->AddImage(pTex,
                            ImVec2(cx, iy),
                            ImVec2(cx + kProfSz, iy + kProfSz));
                        cx += kProfSz + 4.f;
                    }
                }

                std::string name = agentNameStr(row.casterId);
                ImU32 col = teamColorU32(row.casterId);
                std::string deathText = name + " died";
                dl->PushClipRect(
                    ImVec2(cx, startY),
                    ImVec2(startX + kColVal + kValW, startY + kRowH), true);
                dl->AddText(ImVec2(cx, textY), col, deathText.c_str());
                dl->PopClipRect();

                ImGui::PopID();
                continue;
            }

            // --- Jumbo row: [Icon] Message ---
            if (row.category == CombatLogCategory::Jumbo)
            {
                float cx = startX + kColCProf;

                const char* jIcon = nullptr;
                if (row.eventType == "BASE_UNDER_ATTACK")
                    jIcon = "damagedone.png";
                else if (row.eventType == "GUILD_LORD_UNDER_ATTACK")
                    jIcon = "kill.png";
                else if (row.eventType == "CAPTURED_SHRINE")
                    jIcon = "Health_Shrine_Bonus.jpg";
                else if (row.eventType == "NEUTRALIZED_SHRINE")
                    jIcon = "Health_Shrine_Bonus.jpg";
                else if (row.eventType == "CAPTURED_TOWER")
                    jIcon = (row.jumboTeam == 1) ? "Red_flag_waving.svg.png"
                                                 : "Blue_flag_waving.svg.png";
                else if (row.eventType == "PARTY_DEFEATED")
                    jIcon = "death2.png";
                else if (row.eventType == "MORALE_BOOST")
                    jIcon = "Morale_10.png";
                else if (row.eventType == "VICTORY" || row.eventType == "FLAWLESS_VICTORY")
                    jIcon = "cup.webp";

                if (jIcon)
                {
                    ImTextureID jTex = LoadFlagIcon(dev, jIcon);
                    if (jTex)
                    {
                        float iy = startY + (kRowH - kIconSz) * 0.5f;
                        dl->AddImage(jTex,
                            ImVec2(cx, iy),
                            ImVec2(cx + kIconSz, iy + kIconSz));
                        cx += kIconSz + 6.f;
                    }
                }

                const bool isNeutShrine = (row.eventType == "NEUTRALIZED_SHRINE");
                ImU32 jCol = isNeutShrine ? IM_COL32(0xBB, 0xBB, 0xBB, 0xFF)
                           : (row.jumboTeam == 1) ? uRed
                           : (row.jumboTeam == 2) ? uBlue
                           : uTsCol;
                const char* jText = JumboMessageDisplayText(row.eventType, row.jumboTeam);

                dl->PushClipRect(
                    ImVec2(cx, startY),
                    ImVec2(startX + kColVal + kValW, startY + kRowH), true);
                dl->AddText(ImVec2(cx, textY), jCol, jText);
                dl->PopClipRect();

                ImGui::PopID();
                continue;
            }

            // Caster profession icon (16x16)
            {
                auto cIt = m_replayCtx.agents.find(row.casterId);
                if (cIt != m_replayCtx.agents.end() && cIt->second.primaryProf > 0 && dev)
                {
                    ImTextureID pTex = LoadProfIcon(dev, cIt->second.primaryProf);
                    if (pTex)
                    {
                        float iy = startY + (kRowH - kProfSz) * 0.5f;
                        dl->AddImage(pTex,
                            ImVec2(startX + kColCProf, iy),
                            ImVec2(startX + kColCProf + kProfSz, iy + kProfSz));
                    }
                }
            }

            // Caster name (team-colored, 130px)
            {
                std::string name = agentNameStr(row.casterId);
                ImU32 col = teamColorU32(row.casterId);
                dl->PushClipRect(
                    ImVec2(startX + kColCast, startY),
                    ImVec2(startX + kColCast + kCastW, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColCast, textY), col, name.c_str());
                dl->PopClipRect();
            }

            // Arrow 1 (16px)
            {
                dl->PushClipRect(
                    ImVec2(startX + kColArr1, startY),
                    ImVec2(startX + kColArr1 + kArr1W, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColArr1, textY), uMuted,
                    "\xe2\x86\x92");
                dl->PopClipRect();
            }

            // Skill icon (18x18) with gold border
            if (row.skillId > 0 && dev)
            {
                ImTextureID tex = LoadSkillIcon(this, dev, row.skillId,
                    m_skillIconIndex, m_skillIconCache);
                if (tex)
                {
                    float iy = startY + (kRowH - kIconSz) * 0.5f;
                    dl->AddImage(tex,
                        ImVec2(startX + kColIcon, iy),
                        ImVec2(startX + kColIcon + kIconSz, iy + kIconSz));
                    dl->AddRect(
                        ImVec2(startX + kColIcon - 0.5f, iy - 0.5f),
                        ImVec2(startX + kColIcon + kIconSz + 0.5f, iy + kIconSz + 0.5f),
                        uGoldDim);
                }
            }
            else if (row.category == CombatLogCategory::BasicAttack)
            {
                dl->PushClipRect(
                    ImVec2(startX + kColIcon, startY),
                    ImVec2(startX + kColIcon + kIconSz, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColIcon, textY), uGray,
                    "\xe2\x9a\x94");
                dl->PopClipRect();
            }

            bool selfCast = (row.targetId <= 0 || row.targetId == row.casterId);
            float skillColW = selfCast
                ? (kColVal - kColSkill)
                : kSkillW;

            // Skill name (140px, or extended for self-cast)
            {
                const char* snText = nullptr;
                std::string snBuf;
                ImU32 snCol = uWhite;
                if (row.skillId > 0) {
                    snBuf = GetSkillDisplayName(row.skillId);
                    snText = snBuf.c_str();
                    if (row.interrupted)     snCol = uOrange;
                    else if (row.cancelled)  snCol = uPurple;
                } else if (row.category == CombatLogCategory::BasicAttack) {
                    snText = "Attack";
                    snCol = uGray;
                } else if (!row.eventType.empty()) {
                    snText = row.eventType.c_str();
                    snCol = uGray;
                }
                if (snText) {
                    dl->PushClipRect(
                        ImVec2(startX + kColSkill, startY),
                        ImVec2(startX + kColSkill + skillColW, startY + kRowH),
                        true);
                    dl->AddText(
                        ImVec2(startX + kColSkill, textY), snCol, snText);
                    dl->PopClipRect();
                }
            }

            if (!selfCast)
            {
                // Arrow 2 (16px)
                dl->PushClipRect(
                    ImVec2(startX + kColArr2, startY),
                    ImVec2(startX + kColArr2 + kArr2W, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColArr2, textY), uMuted,
                    "\xe2\x86\x92");
                dl->PopClipRect();

                // Target profession icon (16x16)
                {
                    auto tIt = m_replayCtx.agents.find(row.targetId);
                    if (tIt != m_replayCtx.agents.end() && tIt->second.primaryProf > 0 && dev)
                    {
                        ImTextureID pTex = LoadProfIcon(dev, tIt->second.primaryProf);
                        if (pTex)
                        {
                            float iy = startY + (kRowH - kProfSz) * 0.5f;
                            dl->AddImage(pTex,
                                ImVec2(startX + kColTProf, iy),
                                ImVec2(startX + kColTProf + kProfSz, iy + kProfSz));
                        }
                    }
                }

                // Target name (team-colored, 130px)
                std::string tn = agentNameStr(row.targetId);
                ImU32 tCol = teamColorU32(row.targetId);
                dl->PushClipRect(
                    ImVec2(startX + kColTgt, startY),
                    ImVec2(startX + kColTgt + kTgtW, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColTgt, textY), tCol, tn.c_str());
                dl->PopClipRect();
            }

            // Value (right-aligned, 60px)
            {
                char valBuf[32] = {};
                ImU32 valCol = uWhite;

                switch (row.category) {
                case CombatLogCategory::Damage:
                    if (row.valueAbs != 0)
                        snprintf(valBuf, sizeof(valBuf), "-%d%% (%d)",
                            (int)(std::abs(row.valuePct) * 100.f), std::abs(row.valueAbs));
                    else
                        snprintf(valBuf, sizeof(valBuf), "-%d%%",
                            (int)(std::abs(row.valuePct) * 100.f));
                    valCol = uDmgRed;
                    break;
                case CombatLogCategory::Heal:
                    if (row.valueAbs != 0)
                        snprintf(valBuf, sizeof(valBuf), "+%d%% (+%d)",
                            (int)(std::abs(row.valuePct) * 100.f), std::abs(row.valueAbs));
                    else
                        snprintf(valBuf, sizeof(valBuf), "+%d%%",
                            (int)(std::abs(row.valuePct) * 100.f));
                    valCol = uGreen;
                    break;
                case CombatLogCategory::Interrupt:
                    snprintf(valBuf, sizeof(valBuf), "INTERRUPTED");
                    valCol = uKillRed;
                    break;
                case CombatLogCategory::KnockDown:
                    snprintf(valBuf, sizeof(valBuf), "KNOCKED DOWN");
                    valCol = uOrange;
                    break;
                case CombatLogCategory::Death:
                    snprintf(valBuf, sizeof(valBuf), "KILLED");
                    valCol = uKillRed;
                    break;
                case CombatLogCategory::Block:
                    snprintf(valBuf, sizeof(valBuf), "BLOCKED");
                    valCol = uGray;
                    break;
                case CombatLogCategory::Skill:
                    if (row.interrupted) {
                        snprintf(valBuf, sizeof(valBuf), "INTERRUPTED");
                        valCol = uKillRed;
                    } else if (row.cancelled) {
                        snprintf(valBuf, sizeof(valBuf), "CANCELLED");
                        valCol = uOrange;
                    }
                    break;
                default:
                    break;
                }

                if (valBuf[0])
                {
                    float tw = font->CalcTextSizeA(
                        fontSize, FLT_MAX, 0.f, valBuf).x;
                    float tx = (tw <= kValW)
                        ? (startX + kColVal + kValW - tw)
                        : (startX + kColVal);
                    dl->PushClipRect(
                        ImVec2(startX + kColVal, startY),
                        ImVec2(startX + kColVal + kValW, startY + kRowH),
                        true);
                    dl->AddText(ImVec2(tx, textY), valCol, valBuf);
                    dl->PopClipRect();
                }
            }

            ImGui::PopID();
        }
    }
    clipper.End();

    // Scroll to selected row when filters change
    if (m_clScrollToSelected)
    {
        bool found = false;
        if (m_clSelectedRowIdx >= 0)
        {
            for (int i = 0; i < (int)filtered.size(); ++i)
            {
                if (filtered[i] == m_clSelectedRowIdx)
                {
                    float targetY = (float)i * kRowH;
                    float viewH = ImGui::GetWindowHeight();
                    ImGui::SetScrollY(targetY - viewH * 0.5f);
                    m_clAutoScroll = false;
                    found = true;
                    break;
                }
            }
            if (!found)
                m_clSelectedRowIdx = -1;
        }
        m_clScrollToSelected = false;
    }

    // Re-enable auto-scroll whenever playback is active
    if (m_replayCtx.isPlaying)
        m_clAutoScroll = true;

    // Auto-scroll to bottom
    if (m_clAutoScroll)
        ImGui::SetScrollHereY(1.0f);

    // Pause auto-scroll when user scrolls up while NOT playing
    if (!m_replayCtx.isPlaying &&
        ImGui::GetScrollMaxY() > 0.f &&
        ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 20.f)
        m_clAutoScroll = false;

    bool scrollHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();

    if (scrollHovered && !inputHovered && !inputFocused)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

    // Footer: resume button when auto-scroll paused
    if (!m_clAutoScroll)
    {
        if (ImGui::Button("Resume"))
            m_clAutoScroll = true;
    }
    else
    {
        ImGui::TextDisabled("Auto-scrolling...");
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(9);
}
