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


void ReplayWindow::DrawAgentDataWindow()
{
    if (!m_replayCtx.agentsLoaded || m_replayCtx.agents.empty())
    {
        ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Agent Data", &m_showAgentDataWindow))
        {
            if (!m_replayCtx.agentsLoaded)
                ImGui::TextWrapped("Agent data is still loading...");
            else
                ImGui::TextWrapped("No agent data found in the match folder.");
        }
        ImGui::End();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(960, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Agent Data", &m_showAgentDataWindow))
    {
        ImGui::End();
        return;
    }

    // ---- Top bar: timeline + stats ----
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    char curBuf[16], totBuf[16];
    FormatTime(m_debugTimeline - m_displayTimeOffset, curBuf, sizeof(curBuf));
    FormatTime(maxT - m_displayTimeOffset, totBuf, sizeof(totBuf));
    ImGui::Text("%s / %s", curBuf, totBuf);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##timeline", &m_debugTimeline, 0.f, maxT, ""))
        m_replayCtx.isPlaying = false;

    ImGui::Text("Players:%d  Flags:%d  NPCs:%d  Spirits:%d  Gadgets:%d  Items:%d  Unknown:%d  Total:%d",
                static_cast<int>(m_playerIds.size()),
                static_cast<int>(m_flagIds.size()),
                static_cast<int>(m_npcIds.size()),
                static_cast<int>(m_spiritIds.size()),
                static_cast<int>(m_gadgetIds.size()),
                static_cast<int>(m_itemIds.size()),
                static_cast<int>(m_unknownIds.size()),
                static_cast<int>(m_replayCtx.agents.size()));

    ImGui::Checkbox("Parsed View", &m_showParsedView);
    ImGui::SameLine();
    ImGui::TextDisabled("(uncheck for raw text)");
    ImGui::Separator();

    // ---- Left pane: categorized agent list (resizable) ----
    ImGui::BeginChild("AgentList", ImVec2(m_agentListWidth, 0), true);

    // Helper lambda to draw a selectable agent row inside a category
    auto DrawAgentEntry = [&](int agentId, const AgentReplayData& ard)
    {
        ImVec4 color(1, 1, 1, 1);
        if (ard.teamId == 1) color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        else if (ard.teamId == 2) color = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
        else if (ard.teamId == 3) color = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        std::string label;
        if (ard.type == AgentType::Player)
            label = std::format("[{}] {}", agentId, ard.playerName);
        else if (!ard.categoryName.empty() && ard.categoryName != "Unknown")
            label = std::format("[{}] {}", agentId, ard.categoryName);
        else
            label = std::format("[{}] id:{}", agentId, ard.modelId);

        if (ImGui::Selectable(label.c_str(), m_selectedAgentId == agentId))
        {
            m_selectedAgentId = agentId;
            EnterFollowMode(agentId);
        }

        ImGui::PopStyleColor();
    };

    // --- PLAYERS section (grouped by team) ---
    if (!m_playerIds.empty() && ImGui::TreeNodeEx("Players", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool anyRed = false;
        for (int id : m_playerIds)
            if (m_replayCtx.agents[id].teamId == 1) { anyRed = true; break; }
        if (anyRed && ImGui::TreeNodeEx("Red Team", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_playerIds)
                if (m_replayCtx.agents[id].teamId == 1) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        bool anyBlue = false;
        for (int id : m_playerIds)
            if (m_replayCtx.agents[id].teamId == 2) { anyBlue = true; break; }
        if (anyBlue && ImGui::TreeNodeEx("Blue Team", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_playerIds)
                if (m_replayCtx.agents[id].teamId == 2) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        for (int id : m_playerIds)
            if (m_replayCtx.agents[id].teamId != 1 && m_replayCtx.agents[id].teamId != 2)
                DrawAgentEntry(id, m_replayCtx.agents[id]);

        ImGui::TreePop();
    }

    // --- Flags section ---
    if (!m_flagIds.empty() && ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int id : m_flagIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- NPCs section ---
    if (!m_npcIds.empty() && ImGui::TreeNodeEx("NPCs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int id : m_npcIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- Spirits section (grouped by team) ---
    if (!m_spiritIds.empty() && ImGui::TreeNodeEx("Spirits", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool anyT1 = false;
        for (int id : m_spiritIds)
            if (m_replayCtx.agents[id].teamId == 1) { anyT1 = true; break; }
        if (anyT1 && ImGui::TreeNodeEx("Team 1 Spirits", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_spiritIds)
                if (m_replayCtx.agents[id].teamId == 1) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        bool anyT2 = false;
        for (int id : m_spiritIds)
            if (m_replayCtx.agents[id].teamId == 2) { anyT2 = true; break; }
        if (anyT2 && ImGui::TreeNodeEx("Team 2 Spirits", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_spiritIds)
                if (m_replayCtx.agents[id].teamId == 2) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        for (int id : m_spiritIds)
            if (m_replayCtx.agents[id].teamId != 1 && m_replayCtx.agents[id].teamId != 2)
                DrawAgentEntry(id, m_replayCtx.agents[id]);

        ImGui::TreePop();
    }

    // --- Gadgets section ---
    if (!m_gadgetIds.empty() && ImGui::TreeNode("Gadgets"))
    {
        for (int id : m_gadgetIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- Items section ---
    if (!m_itemIds.empty() && ImGui::TreeNode("Items"))
    {
        for (int id : m_itemIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- Unknown section ---
    if (!m_unknownIds.empty() && ImGui::TreeNode("Unknown"))
    {
        for (int id : m_unknownIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    ImGui::EndChild();

    // Vertical drag splitter between left and right panes
    ImGui::SameLine();
    {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Button("##splitter", ImVec2(4.0f, -1));
        if (ImGui::IsItemActive())
            m_agentListWidth += ImGui::GetIO().MouseDelta.x;
        m_agentListWidth = std::clamp(m_agentListWidth, 120.f, avail - 200.f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    ImGui::SameLine();

    // ---- Right pane: agent detail ----
    ImGui::BeginChild("AgentDetail", ImVec2(0, 0), true);

    if (m_selectedAgentId >= 0 && m_replayCtx.agents.count(m_selectedAgentId))
    {
        auto& ard = m_replayCtx.agents[m_selectedAgentId];

        // Header with classification info
        ImGui::TextColored(ImVec4(1, 0.9f, 0.4f, 1), "Agent %d  [%s]",
                           ard.agent_id, AgentTypeName(ard.type));
        ImGui::SameLine();
        ImGui::Text(" |  %d snapshots  |  Model: %u  |  Team: %s (%u)",
                    static_cast<int>(ard.snapshots.size()), ard.modelId,
                    GetTeamName(ard.teamId), ard.teamId);

        if (ard.type == AgentType::Player)
        {
            ImGui::Text("Player: %s", ard.playerName.c_str());

            // Solved max HP per weapon set. Read-only listing: the equipment
            // signature, best-fit M, which channel produced it, vote counts,
            // and the time span the evidence covers.
            if (ImGui::TreeNodeEx("Solved Max-HP by Weapon Set",
                                  ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ard.solvedMaxHpByWeaponSet.empty())
                {
                    ImGui::TextDisabled("(no solved weapon sets)");
                }
                else if (ImGui::BeginTable("MaxHpSet", 7,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                    ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Weapon Set");
                    ImGui::TableSetupColumn("M");
                    ImGui::TableSetupColumn("Source");
                    ImGui::TableSetupColumn("Votes");
                    ImGui::TableSetupColumn("Median Res");
                    ImGui::TableSetupColumn("Accepted");
                    ImGui::TableSetupColumn("Seen");
                    ImGui::TableHeadersRow();

                    auto sourceName = [](AgentReplayData::MaxHpSource s) -> const char* {
                        switch (s) {
                        case AgentReplayData::MaxHpSource::Lattice:         return "lattice";
                        case AgentReplayData::MaxHpSource::SkillBreakpoint: return "skill-table";
                        case AgentReplayData::MaxHpSource::DivineFavor:     return "divine-favor";
                        default:                                           return "--";
                        }
                    };

                    for (const auto& [key, rec] : ard.solvedMaxHpByWeaponSet)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        // weapon id : offhand id : weapon type : offhand type
                        ImGui::Text("%u:%u:%u:%u",
                                    (unsigned)((key >> 32) & 0xFFFF),
                                    (unsigned)((key >> 16) & 0xFFFF),
                                    (unsigned)((key >> 8)  & 0xFF),
                                    (unsigned)( key        & 0xFF));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%u", rec.maxHp);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(sourceName(rec.source));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%d/%d", rec.supporting, rec.observations);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%.4f", rec.medianResidual);
                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextUnformatted(rec.accepted ? "yes" : "no");
                        ImGui::TableSetColumnIndex(6);
                        ImGui::Text("%.0f-%.0f", rec.firstSeen, rec.lastSeen);
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
        }
        else if (ard.type == AgentType::Spirit)
        {
            ImGui::Text("Spirit: %s  |  Skill ID: %d", ard.categoryName.c_str(), ard.spiritSkillId);
            ImGui::Text("Overlap Hidden: %s  |  Newest: %s",
                        ard.overlapHidden ? "Yes" : "No",
                        ard.overlapIsNewest ? "Yes" : "No");
            ImGui::Text("Overlap Threshold: %.0f  |  Dist to Newest: %.0f",
                        ard.overlapThreshold, ard.overlapDistNewest);
        }
        else if (ard.type == AgentType::Item)
        {
            ImGui::Text("Item: %s  |  item_id: %u", ard.categoryName.c_str(),
                        ard.snapshots.empty() ? 0u : ard.snapshots[0].item_id);
        }
        else if (ard.type == AgentType::Gadget)
        {
            ImGui::Text("Gadget: %s  |  gadget_id: %u", ard.categoryName.c_str(),
                        ard.snapshots.empty() ? 0u : ard.snapshots[0].gadget_id);
        }
        else if (!ard.categoryName.empty() && ard.categoryName != "Unknown")
        {
            ImGui::Text("Category: %s", ard.categoryName.c_str());
        }
        else
        {
            ImGui::Text("agent_model_type: 0x%X  |  model_id: %u  |  gadget_id: %u",
                        ard.agentModelType, ard.modelId,
                        ard.snapshots.empty() ? 0u : ard.snapshots[0].gadget_id);
        }

        ImGui::Separator();

        // Snapshot at current timeline
        const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
        if (snap)
        {
            ImGui::Text("Snapshot at t=%.3fs:", snap->time);
            ImGui::Separator();

            if (m_showParsedView)
            {
                if (ImGui::BeginTable("SnapFields", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 260)))
                {
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 200);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    auto Row = [](const char* field, const char* fmt, auto... args)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(field);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text(fmt, args...);
                    };

                    Row("Position", "%.3f, %.3f, %.3f", snap->x, snap->y, snap->z);
                    Row("Rotation", "%.3f rad", snap->rotation);
                    Row("Alive / Dead", "%s / %s", snap->is_alive ? "Yes" : "No", snap->is_dead ? "Yes" : "No");
                    Row("Health", "%.1f%%  (max %u)", snap->health_pct * 100.f, snap->max_hp);
                    Row("HP Pips", "%.3f", snap->hp_pips);
                    Row("Is Knocked", "%s", snap->is_knocked ? "Yes" : "No");
                    Row("Model ID", "%u", snap->model_id);
                    Row("Gadget ID", "%u", snap->gadget_id);
                    Row("Team", "%s (%u)", GetTeamName(snap->team_id), snap->team_id);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "--- Conditions ---");

                    Row("Condition", "%s", snap->has_condition ? "Yes" : "No");
                    Row("Deep Wound", "%s", snap->has_deep_wound ? "Yes" : "No");
                    Row("Bleeding", "%s", snap->has_bleeding ? "Yes" : "No");
                    Row("Crippled", "%s", snap->has_crippled ? "Yes" : "No");
                    Row("Blind", "%s", snap->has_blind ? "Yes" : "No");
                    Row("Poison", "%s", snap->has_poison ? "Yes" : "No");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.6f, 0.3f, 1, 1), "--- Hex/Enchant ---");

                    Row("Hex", "%s", snap->has_hex ? "Yes" : "No");
                    Row("Degen Hex", "%s", snap->has_degen_hex ? "Yes" : "No");
                    Row("Enchantment", "%s", snap->has_enchantment ? "Yes" : "No");
                    Row("Weapon Spell", "%s", snap->has_weapon_spell ? "Yes" : "No");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.6f, 1), "--- Casting ---");

                    Row("Is Casting", "%s", snap->is_casting ? "Yes" : "No");
                    Row("Skill ID", "%u", snap->skill_id);
                    Row("Is Holding", "%s", snap->is_holding ? "Yes" : "No");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "--- Weapon ---");

                    Row("Weapon Type", "%s (%u)", GetWeaponTypeName(snap->weapon_type), snap->weapon_type);
                    Row("Weapon Item Type", "%u", snap->weapon_item_type);
                    Row("Offhand Item Type", "%u", snap->offhand_item_type);
                    Row("Weapon Item ID", "%u", snap->weapon_item_id);
                    Row("Offhand Item ID", "%u", snap->offhand_item_id);
                    Row("Weapon Attack Spd", "%.3f", snap->weapon_attack_speed);
                    Row("Attack Spd Mod", "%.3f", snap->attack_speed_modifier);
                    Row("Dagger Status", "%s (%u)", GetDaggerStatusName(snap->dagger_status), snap->dagger_status);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1, 1), "--- Movement ---");

                    Row("Velocity", "%.3f, %.3f", snap->move_x, snap->move_y);
                    float speed = std::sqrtf(snap->move_x * snap->move_x + snap->move_y * snap->move_y);
                    Row("Speed", "%.1f", speed);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(1, 0.6f, 0.8f, 1), "--- Animation ---");

                    Row("Model State", "%u", snap->model_state);
                    Row("Animation Code", "%u", snap->animation_code);
                    Row("Animation ID", "%u", snap->animation_id);
                    Row("Animation Speed", "%.3f", snap->animation_speed);
                    Row("Animation Type", "%.3f", snap->animation_type);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "--- Other ---");

                    Row("Visual Effects", "%u", snap->visual_effects);
                    Row("In Spirit Range", "%u", snap->in_spirit_range);
                    Row("Agent Model Type", "0x%X", snap->agent_model_type);
                    Row("Item ID", "%u", snap->item_id);
                    Row("Item Extra Type", "%u", snap->item_extra_type);
                    Row("Gadget Extra Type", "%u", snap->gadget_extra_type);

                    ImGui::EndTable();
                }
            }
            else
            {
                ImGui::TextWrapped("Raw: %s", snap->raw_line.c_str());
            }
        }

        // Scrollable snapshot table
        ImGui::Separator();
        ImGui::Text("All Snapshots:");
        if (ImGui::BeginTable("SnapTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Pos X");
            ImGui::TableSetupColumn("Pos Y");
            ImGui::TableSetupColumn("HP%");
            ImGui::TableSetupColumn("Alive");
            ImGui::TableSetupColumn("Casting");
            ImGui::TableSetupColumn("Skill");
            ImGui::TableSetupColumn("Speed");
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(ard.snapshots.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& s = ard.snapshots[row];
                    ImGui::TableNextRow();

                    bool isNearTimeline = std::fabsf(s.time - m_debugTimeline) < 0.15f;
                    if (isNearTimeline)
                    {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                            ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 0.4f)));
                    }

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%.3f", s.time);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.1f", s.x);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f", s.y);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.1f%%", s.health_pct * 100.f);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(s.is_alive ? "Y" : "N");
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(s.is_casting ? "Y" : "N");
                    ImGui::TableSetColumnIndex(6);
                    if (s.skill_id > 0) ImGui::Text("%u", s.skill_id);
                    else ImGui::TextUnformatted("-");
                    ImGui::TableSetColumnIndex(7);
                    float spd = std::sqrtf(s.move_x * s.move_x + s.move_y * s.move_y);
                    ImGui::Text("%.0f", spd);
                }
            }

            ImGui::EndTable();
        }
    }
    else
    {
        ImGui::TextWrapped("Select an agent from the list on the left.");
    }

    ImGui::EndChild();
    ImGui::End();
}


void ReplayWindow::DrawStoCWindow()
{
    if (!m_replayCtx.stocLoaded)
    {
        ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("StoC Events", &m_showStoCWindow))
            ImGui::TextWrapped("StoC event data is still loading...");
        ImGui::End();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1100, 660), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("StoC Events", &m_showStoCWindow))
    {
        ImGui::End();
        return;
    }

    auto& sd = m_replayCtx.stocData;
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);

    // ---- Event timeline bar ----
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        float canvasW = ImGui::GetContentRegionAvail().x;
        float canvasH = 32.f;

        ImGui::InvisibleButton("##timeline_canvas", ImVec2(canvasW, canvasH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH),
                          IM_COL32(30, 30, 30, 255));
        dl->AddRect(canvasPos, ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH),
                    IM_COL32(80, 80, 80, 255));

        auto PlotEvents = [&](const auto& events, StoCCategory cat)
        {
            ImU32 col = StoCCategoryColor(cat);
            for (auto& ev : events)
            {
                float xp = canvasPos.x + (ev.time / maxT) * canvasW;
                dl->AddLine(ImVec2(xp, canvasPos.y), ImVec2(xp, canvasPos.y + canvasH), col, 1.0f);
            }
        };

        if (m_selectedStoCCategory == StoCCategory::AgentMovement || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.agentMovement, StoCCategory::AgentMovement);
        if (m_selectedStoCCategory == StoCCategory::Skill || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.skill, StoCCategory::Skill);
        if (m_selectedStoCCategory == StoCCategory::AttackSkill || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.attackSkill, StoCCategory::AttackSkill);
        if (m_selectedStoCCategory == StoCCategory::BasicAttack || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.basicAttack, StoCCategory::BasicAttack);
        if (m_selectedStoCCategory == StoCCategory::Combat || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.combat, StoCCategory::Combat);
        if (m_selectedStoCCategory == StoCCategory::Jumbo || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.jumbo, StoCCategory::Jumbo);
        if (m_selectedStoCCategory == StoCCategory::Unknown || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.unknown, StoCCategory::Unknown);
        if (m_selectedStoCCategory == StoCCategory::Lifecycle || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.lifecycle, StoCCategory::Lifecycle);
        if (m_selectedStoCCategory == StoCCategory::MapObject || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.mapObject, StoCCategory::MapObject);
        if (m_selectedStoCCategory == StoCCategory::DoorEvent || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.doorEvents, StoCCategory::DoorEvent);
        if (m_selectedStoCCategory == StoCCategory::FlagEvent || m_selectedStoCCategory == StoCCategory::_Count)
        {
            auto PlotFlagSub = [&](const auto& vec) {
                ImU32 col = StoCCategoryColor(StoCCategory::FlagEvent);
                for (auto& ev : vec) {
                    float xp = canvasPos.x + (ev.time / maxT) * canvasW;
                    dl->AddLine(ImVec2(xp, canvasPos.y), ImVec2(xp, canvasPos.y + canvasH), col, 1.0f);
                }
            };
            PlotFlagSub(sd.flagEvents.pickups);
            PlotFlagSub(sd.flagEvents.drops);
            PlotFlagSub(sd.flagEvents.states);
            PlotFlagSub(sd.flagEvents.items);
            PlotFlagSub(sd.flagEvents.stands);
            PlotFlagSub(sd.flagEvents.spawns);
            PlotFlagSub(sd.flagEvents.announces);
        }

        if (ImGui::IsItemClicked())
            m_debugTimeline = ((ImGui::GetIO().MousePos.x - canvasPos.x) / canvasW) * maxT;
    }

    ImGui::Checkbox("Show Raw", &m_stocShowRaw);
    ImGui::Separator();

    // ---- Left pane: category list ----
    ImGui::BeginChild("StoCCatList", ImVec2(m_stocListWidth, 0), true);

    for (int i = 0; i < static_cast<int>(StoCCategory::_Count); i++)
    {
        auto cat = static_cast<StoCCategory>(i);
        int count = StoCCategoryCount(sd, cat);
        ImU32 col = StoCCategoryColor(cat);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        auto label = std::format("{} ({})", StoCCategoryName(cat), count);
        if (ImGui::Selectable(label.c_str(), m_selectedStoCCategory == cat))
        {
            m_selectedStoCCategory = cat;
            m_selectedStoCEventIdx = -1;
        }
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    // Splitter
    ImGui::SameLine();
    {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Button("##stoc_splitter", ImVec2(4.0f, -1));
        if (ImGui::IsItemActive())
            m_stocListWidth += ImGui::GetIO().MouseDelta.x;
        m_stocListWidth = std::clamp(m_stocListWidth, 120.f, avail - 200.f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    ImGui::SameLine();

    // ---- Right pane: event table ----
    ImGui::BeginChild("StoCDetail", ImVec2(0, 0), true);

    const auto& rctx = m_replayCtx;

    switch (m_selectedStoCCategory)
    {
    // ====================== AGENT MOVEMENT ======================
    case StoCCategory::AgentMovement:
    {
        ImGui::Text("Agent Movement Events: %d", static_cast<int>(sd.agentMovement.size()));
        if (ImGui::BeginTable("AMTable", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Agent ID", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Agent Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("X",     ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Y",     ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Plane", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.agentMovement.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.agentMovement[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##am{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", ev.agent_id);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.agent_id).c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.1f", ev.x);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.1f", ev.y);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.0f", ev.plane);
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.agentMovement.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.agentMovement[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== SKILL EVENTS ======================
    case StoCCategory::Skill:
    {
        ImGui::Text("Skill Events: %d", static_cast<int>(sd.skill.size()));
        if (ImGui::BeginTable("SKTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",       ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("Skill",       ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Skill Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Caster",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.skill.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.skill[row];
                    int tid = ResolveTarget(ev.target_id, ev.caster_id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##sk{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.skill_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetSkillDisplayName(ev.skill_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%d", tid);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, tid).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.skill.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.skill[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== ATTACK SKILL EVENTS ======================
    case StoCCategory::AttackSkill:
    {
        ImGui::Text("Attack Skill Events: %d", static_cast<int>(sd.attackSkill.size()));
        if (ImGui::BeginTable("ASKTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",       ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed, 150);
            ImGui::TableSetupColumn("Skill",       ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Skill Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Caster",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.attackSkill.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.attackSkill[row];
                    int tid = ResolveTarget(ev.target_id, ev.caster_id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##ask{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.skill_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetSkillDisplayName(ev.skill_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%d", tid);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, tid).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.attackSkill.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.attackSkill[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== BASIC ATTACK EVENTS ======================
    case StoCCategory::BasicAttack:
    {
        ImGui::Text("Basic Attack Events: %d", static_cast<int>(sd.basicAttack.size()));
        if (ImGui::BeginTable("BATable", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Caster",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.basicAttack.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.basicAttack[row];
                    int tid = ResolveTarget(ev.target_id, ev.caster_id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##ba{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", tid);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, tid).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.basicAttack.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.basicAttack[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== COMBAT EVENTS ======================
    case StoCCategory::Combat:
    {
        ImGui::Text("Combat Events: %d", static_cast<int>(sd.combat.size()));
        if (ImGui::BeginTable("CMTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Caster",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value",        ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Dmg Type",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.combat.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.combat[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##cm{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.target_id);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.target_id).c_str());
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%.2f", ev.value);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::Text("%d", ev.damage_type);
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.combat.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.combat[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== JUMBO MESSAGES ======================
    case StoCCategory::Jumbo:
    {
        ImGui::Text("Jumbo Messages: %d", static_cast<int>(sd.jumbo.size()));
        if (ImGui::BeginTable("JMBTable", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Message",      ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Party Value",   ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Party",         ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.jumbo.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.jumbo[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##jmb{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.message.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.party_value);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(JumboPartyLabel(ev.party_value));
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.jumbo.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.jumbo[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== UNKNOWN EVENTS ======================
    case StoCCategory::Unknown:
    {
        ImGui::Text("Unknown Events: %d", static_cast<int>(sd.unknown.size()));
        if (ImGui::BeginTable("UNKTable", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Raw Line", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.unknown.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.unknown[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##unk{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.raw_line.c_str());
                }
            }
            ImGui::EndTable();
        }
        break;
    }

    // ====================== LIFECYCLE EVENTS ======================
    case StoCCategory::Lifecycle:
    {
        ImGui::Text("Lifecycle Events: %d", static_cast<int>(sd.lifecycle.size()));
        if (ImGui::BeginTable("LCTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",       ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Agent ID",    ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Agent Type",  ImGuiTableColumnFlags_WidthFixed, 75);
            ImGui::TableSetupColumn("Type Code",   ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("X",           ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Y",           ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Speed",       ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.lifecycle.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.lifecycle[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.3f}##lc{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.isAdd ? "ADD" : "REMOVE");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.agent_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", ev.agent_type);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.type_code);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.1f", ev.x);
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%.1f", ev.y);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::Text("%.1f", ev.speed);
                }
            }
            ImGui::EndTable();
        }
        break;
    }

    // ====================== MAP OBJECT EVENTS ======================
    case StoCCategory::MapObject:
    {
        ImGui::Text("Map Object Events: %d", static_cast<int>(sd.mapObject.size()));
        if (ImGui::BeginTable("MOTable", 7,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Object ID",    ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Anim Type",    ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Anim Stage",   ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("State",         ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Unk1",          ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.mapObject.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.mapObject[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.3f}##mo{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.isState ? "STATE" : "MAP_OBJECT");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", ev.object_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d", ev.animation_type);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.animation_stage);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%d", ev.state);
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%d", ev.unk1);
                }
            }
            ImGui::EndTable();
        }
        break;
    }

    // ====================== DOOR EVENTS ======================
    case StoCCategory::DoorEvent:
    {
        ImGui::Text("Door Events: %d", static_cast<int>(sd.doorEvents.size()));
        if (sd.doorEvents.empty())
        {
            ImGui::TextWrapped("No door_events.txt found for this replay.");
            break;
        }
        if (ImGui::BeginTable("DETable", 7,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Object ID",    ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Anim Type",    ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Anim Stage",   ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Status",        ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("State",         ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.doorEvents.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.doorEvents[row];
                    int sec = static_cast<int>(ev.time);
                    int ms  = static_cast<int>((ev.time - static_cast<float>(sec)) * 1000.f);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:02d}:{:02d}.{:03d}##de{}", sec / 60, sec % 60, ms, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                    {
                        m_selectedStoCEventIdx = row;
                        m_debugTimeline = ev.time;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.isState ? "DOOR_STATE" : "DOOR_ANIMATION");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", ev.object_id);
                    ImGui::TableSetColumnIndex(3);
                    if (!ev.isState) ImGui::Text("%d", ev.animation_type); else ImGui::TextUnformatted("-");
                    ImGui::TableSetColumnIndex(4);
                    if (!ev.isState) ImGui::Text("%d", ev.animation_stage); else ImGui::TextUnformatted("-");
                    ImGui::TableSetColumnIndex(5);
                    if (!ev.isState)
                    {
                        const char* statusText = ev.status == 1 ? "OPENING" : ev.status == 2 ? "CLOSING" : "UNKNOWN";
                        ImGui::TextUnformatted(statusText);
                    }
                    else
                        ImGui::TextUnformatted("-");
                    ImGui::TableSetColumnIndex(6);
                    if (ev.isState)
                    {
                        const char* stateText = ev.state == 0 ? "CLOSED" : ev.state == 1 ? "OPEN" : "UNKNOWN";
                        ImGui::TextUnformatted(stateText);
                    }
                    else
                        ImGui::TextUnformatted("-");
                }
            }
            ImGui::EndTable();
        }
        break;
    }

    // ====================== FLAG EVENTS ======================
    case StoCCategory::FlagEvent:
    {
        auto& fe = sd.flagEvents;
        ImGui::Text("Flag Events: %d", fe.totalCount());

        struct MergedFlagRow { float time; int code; std::string detail; const std::string* raw; };
        static std::vector<MergedFlagRow> mergedRows;
        static int lastTotalCount = -1;
        int curTotal = fe.totalCount();
        if (curTotal != lastTotalCount)
        {
            lastTotalCount = curTotal;
            mergedRows.clear();
            mergedRows.reserve(curTotal);
            for (auto& e : fe.pickups)
                mergedRows.push_back({ e.time, 0,
                    std::format("item={} player={} tc={}", e.item_id, e.player_agent_id, e.team_code), &e.raw_line });
            for (auto& e : fe.drops)
                mergedRows.push_back({ e.time, 1,
                    std::format("player={} tc={}", e.player_agent_id, e.team_code), &e.raw_line });
            for (auto& e : fe.states)
                mergedRows.push_back({ e.time, 2,
                    std::format("tc={} item={} state={}", e.team_code, e.item_id, e.state), &e.raw_line });
            for (auto& e : fe.items)
                mergedRows.push_back({ e.time, 3,
                    std::format("item={} model={} extra={} type={}", e.item_id, e.model_id, e.extra_id, e.type), &e.raw_line });
            for (auto& e : fe.stands)
                mergedRows.push_back({ e.time, 4,
                    std::format("stand={} sub={} val={}", e.stand_agent_id, e.sub_field, e.value), &e.raw_line });
            for (auto& e : fe.spawns)
                mergedRows.push_back({ e.time, 5,
                    std::format("agent={} unk={} obj={}", e.agent_id, e.unk, e.object_id), &e.raw_line });
            for (auto& e : fe.announces)
                mergedRows.push_back({ e.time, 6,
                    std::format("action={} tmpl={} team={}", e.action == 0 ? "RETURN" : "STICK", e.template_id, e.team), &e.raw_line });
            std::stable_sort(mergedRows.begin(), mergedRows.end(),
                [](const MergedFlagRow& a, const MergedFlagRow& b) { return a.time < b.time; });
        }

        static const char* kFlagCodeNames[] = {
            "PICKUP", "DROP", "STATE", "ITEM", "STAND", "SPAWN", "ANNOUNCE"
        };

        if (ImGui::BeginTable("FETable", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",   ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Code",   ImGuiTableColumnFlags_WidthFixed, 75);
            ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(mergedRows.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& r = mergedRows[row];
                    ImGui::TableNextRow();

                    bool nearCurrent = (r.time >= m_debugTimeline - 0.5f && r.time <= m_debugTimeline + 0.5f);
                    if (nearCurrent)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(80, 80, 40, 100));

                    ImGui::TableSetColumnIndex(0);
                    int sec = static_cast<int>(r.time);
                    int ms  = static_cast<int>((r.time - sec) * 1000.f);
                    if (ImGui::Selectable(std::format("{:02d}:{:02d}.{:03d}##fe{}", sec / 60, sec % 60, ms, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                    {
                        m_selectedStoCEventIdx = row;
                        m_debugTimeline = r.time;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", r.code);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted((r.code >= 0 && r.code <= 6) ? kFlagCodeNames[r.code] : "?");
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(r.detail.c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(mergedRows.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", mergedRows[m_selectedStoCEventIdx].raw->c_str());
        }
        break;
    }

    default:
        ImGui::TextWrapped("Select a category from the left.");
        break;
    }

    ImGui::EndChild();
    ImGui::End();
}
