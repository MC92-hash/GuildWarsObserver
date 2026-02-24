#include "pch.h"
#include "draw_debug_match_metadata.h"
#include "ReplayLibrary.h"
#include "GuiGlobalConstants.h"

static const char* GetProfessionName(int id)
{
    switch (id)
    {
    case 0:  return "None";
    case 1:  return "Warrior";
    case 2:  return "Ranger";
    case 3:  return "Monk";
    case 4:  return "Necromancer";
    case 5:  return "Mesmer";
    case 6:  return "Elementalist";
    case 7:  return "Assassin";
    case 8:  return "Ritualist";
    case 9:  return "Paragon";
    case 10: return "Dervish";
    default: return "Unknown";
    }
}

static void DrawPlayerDetails(const PlayerMeta& p)
{
    ImGui::PushID(p.id);

    const char* displayName = p.encoded_name.empty() ? "(unnamed)" : p.encoded_name.c_str();
    char nodeLabel[128];
    snprintf(nodeLabel, sizeof(nodeLabel), "[%d] %s  %s/%s",
        p.id, displayName, GetProfessionName(p.primary), GetProfessionName(p.secondary));

    if (ImGui::TreeNode("player_detail", "%s", nodeLabel))
    {
        if (ImGui::BeginTable("##pdetail", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            auto Row = [](const char* label, const char* fmt, auto... args) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text(fmt, args...);
            };
            auto RowStr = [](const char* label, const std::string& val) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(val.c_str());
            };

            Row("id", "%d", p.id);
            RowStr("encoded_name", p.encoded_name);
            Row("primary", "%s (%d)", GetProfessionName(p.primary), p.primary);
            Row("secondary", "%s (%d)", GetProfessionName(p.secondary), p.secondary);
            Row("level", "%d", p.level);
            Row("team_id", "%d", p.team_id);
            Row("player_number", "%d", p.player_number);
            Row("guild_id", "%d", p.guild_id);
            Row("model_id", "%d", p.model_id);
            Row("gadget_id", "%d", p.gadget_id);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Separator();
            ImGui::TableNextColumn(); ImGui::Separator();

            Row("total_damage", "%d", p.total_damage);
            Row("kills", "%d", p.kills);
            Row("deaths", "%d", p.deaths);
            Row("crits_dealt", "%d", p.crits_dealt);
            Row("crits_received", "%d", p.crits_received);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Separator();
            ImGui::TableNextColumn(); ImGui::Separator();

            Row("attacks_started", "%d", p.attacks_started);
            Row("attacks_finished", "%d", p.attacks_finished);
            Row("attacks_stopped", "%d", p.attacks_stopped);
            Row("cancelled_attacks_count", "%d", p.cancelled_attacks_count);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Separator();
            ImGui::TableNextColumn(); ImGui::Separator();

            Row("skills_activated", "%d", p.skills_activated);
            Row("skills_finished", "%d", p.skills_finished);
            Row("skills_stopped", "%d", p.skills_stopped);
            Row("cancelled_skills_count", "%d", p.cancelled_skills_count);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Separator();
            ImGui::TableNextColumn(); ImGui::Separator();

            Row("attack_skills_activated", "%d", p.attack_skills_activated);
            Row("attack_skills_finished", "%d", p.attack_skills_finished);
            Row("attack_skills_stopped", "%d", p.attack_skills_stopped);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Separator();
            ImGui::TableNextColumn(); ImGui::Separator();

            Row("interrupted_count", "%d", p.interrupted_count);
            Row("interrupted_skills_count", "%d", p.interrupted_skills_count);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Separator();
            ImGui::TableNextColumn(); ImGui::Separator();

            RowStr("skill_template_code", p.skill_template_code);

            // Used skills as a single row
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("used_skills");
            ImGui::TableNextColumn();
            if (p.used_skills.empty())
            {
                ImGui::TextUnformatted("[]");
            }
            else
            {
                std::string skillStr;
                for (size_t i = 0; i < p.used_skills.size(); i++)
                {
                    if (i > 0) skillStr += ", ";
                    skillStr += std::to_string(p.used_skills[i]);
                }
                ImGui::TextWrapped("%s", skillStr.c_str());
            }

            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

static void DrawPlayerTable(const std::vector<PlayerMeta>& players, const char* label)
{
    if (players.empty()) return;

    if (!ImGui::TreeNode(label, "%s (%d)", label, (int)players.size()))
        return;

    // Summary table with key columns
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##psummary", 12, flags))
    {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Pri/Sec");
        ImGui::TableSetupColumn("Lvl");
        ImGui::TableSetupColumn("Player#");
        ImGui::TableSetupColumn("Guild");
        ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("K/D");
        ImGui::TableSetupColumn("Damage");
        ImGui::TableSetupColumn("Crits D/R");
        ImGui::TableSetupColumn("Atk Start/Fin/Stop");
        ImGui::TableSetupColumn("Skl Act/Fin/Stop");
        ImGui::TableHeadersRow();

        for (const auto& p : players)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", p.id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.encoded_name.empty() ? "(unnamed)" : p.encoded_name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s/%s", GetProfessionName(p.primary), GetProfessionName(p.secondary));
            ImGui::TableNextColumn(); ImGui::Text("%d", p.level);
            ImGui::TableNextColumn(); ImGui::Text("%d", p.player_number);
            ImGui::TableNextColumn(); ImGui::Text("%d", p.guild_id);
            ImGui::TableNextColumn(); ImGui::Text("%d", p.model_id);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", p.kills, p.deaths);
            ImGui::TableNextColumn(); ImGui::Text("%d", p.total_damage);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", p.crits_dealt, p.crits_received);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d / %d", p.attacks_started, p.attacks_finished, p.attacks_stopped);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d / %d", p.skills_activated, p.skills_finished, p.skills_stopped);
        }
        ImGui::EndTable();
    }

    // Expandable detail view per player (all fields)
    if (ImGui::TreeNode("all_fields", "Expand All Fields"))
    {
        for (const auto& p : players)
            DrawPlayerDetails(p);
        ImGui::TreePop();
    }

    ImGui::TreePop();
}

static void DrawGuildsSection(const std::map<std::string, GuildMeta>& guilds)
{
    if (guilds.empty()) return;

    if (!ImGui::TreeNode("Guilds"))
        return;

    for (const auto& [guildId, g] : guilds)
    {
        ImGui::PushID(g.id);
        if (ImGui::TreeNode("guild_node", "[%s] %s (ID: %d)", g.tag.c_str(), g.name.c_str(), g.id))
        {
            ImGui::Text("Rank: %d  |  Rating: %d", g.rank, g.rating);
            ImGui::Text("Faction: %d  |  Faction Pts: %d  |  Qualifier Pts: %d",
                g.faction, g.faction_points, g.qualifier_points);
            ImGui::Text("Features: %d", g.features);

            if (ImGui::TreeNode("Cape Data"))
            {
                ImGui::Text("BG Color: %d", g.cape.bg_color);
                ImGui::Text("Detail Color: %d", g.cape.detail_color);
                ImGui::Text("Emblem Color: %d", g.cape.emblem_color);
                ImGui::Text("Shape: %d  |  Detail: %d  |  Emblem: %d  |  Trim: %d",
                    g.cape.shape, g.cape.detail, g.cape.emblem, g.cape.trim);
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::TreePop();
}

static const char* GetMapName(int mapId)
{
    switch (mapId)
    {
    case 7:             return "Warrior's Isle";
    case 8:             return "Hunter's Isle";
    case 6: case 9:     return "Wizard's Isle";
    case 52: case 67:   return "Burning Isle";
    case 68:            return "Frozen Isle";
    case 69:            return "Nomad's Isle";
    case 70:            return "Druid's Isle";
    case 71:            return "Isle of the Dead";
    case 360: case 364: return "Isle of Meditation";
    case 361:           return "Isle of Weeping Stone";
    case 362:           return "Isle of Jade";
    case 363:           return "Imperial Isle";
    case 531:           return "Uncharted Isle";
    case 532:           return "Isle of Wurms";
    case 537: case 539: return "Corrupted Isle";
    case 540:           return "Isle of Solitude";
    default:            return nullptr;
    }
}

static std::string GetPartyGuildLabel(const MatchMeta& m, const std::string& partyId)
{
    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end()) return "?";

    std::map<int, int> guildCounts;
    for (const auto& p : pit->second.players)
    {
        if (p.guild_id > 0)
            guildCounts[p.guild_id]++;
    }

    int bestGuildId = 0;
    int bestCount = 0;
    for (const auto& [gid, cnt] : guildCounts)
    {
        if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }
    }

    if (bestGuildId == 0) return "?";

    std::string guildIdStr = std::to_string(bestGuildId);
    auto git = m.guilds.find(guildIdStr);
    if (git != m.guilds.end())
        return git->second.name + " [" + git->second.tag + "]";

    return "Guild #" + guildIdStr;
}

static void DrawSingleMatch(const MatchMeta& m, int index)
{
    std::string guild1 = GetPartyGuildLabel(m, "1");
    std::string guild2 = GetPartyGuildLabel(m, "2");

    // Winner marker
    const char* w1 = (m.winner_party_id == 1) ? " *" : "";
    const char* w2 = (m.winner_party_id == 2) ? " *" : "";

    const char* mapName = GetMapName(m.map_id);
    char mapStr[64];
    if (mapName)
        snprintf(mapStr, sizeof(mapStr), "%s", mapName);
    else
        snprintf(mapStr, sizeof(mapStr), "Map %d", m.map_id);

    char header[512];
    snprintf(header, sizeof(header), "Match #%d:  %04d/%02d/%02d - %s - %s%s vs %s%s - %s",
        index, m.year, m.month, m.day, m.occasion.c_str(),
        guild1.c_str(), w1, guild2.c_str(), w2, mapStr);

    ImGui::PushID(index);
    if (ImGui::CollapsingHeader(header))
    {
        ImGui::Indent(8.0f);

        if (ImGui::TreeNodeEx("Match Info", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Folder: %s", m.folder_path.c_str());
            ImGui::Text("Map ID: %d", m.map_id);
            ImGui::Text("Date: %04d/%02d/%02d", m.year, m.month, m.day);
            ImGui::Text("Occasion: %s", m.occasion.c_str());
            ImGui::Text("Flux: %s", m.flux.c_str());
            ImGui::Text("Duration: %s  (original: %s)", m.match_duration.c_str(), m.match_original_duration.c_str());
            ImGui::Text("End Time: %s  (%d ms)", m.match_end_time_formatted.c_str(), m.match_end_time_ms);
            ImGui::Text("Winner: Party %d", m.winner_party_id);

            ImGui::Separator();
            ImGui::Text("Team Kills:");
            for (const auto& [team, kills] : m.team_kills)
                ImGui::Text("  Team %s: %d", team.c_str(), kills);

            ImGui::Text("Team Damage:");
            for (const auto& [team, damage] : m.team_damage)
                ImGui::Text("  Team %s: %d", team.c_str(), damage);

            ImGui::TreePop();
        }

        for (const auto& [partyId, party] : m.parties)
        {
            char partyLabel[64];
            snprintf(partyLabel, sizeof(partyLabel), "Party %s", partyId.c_str());
            if (ImGui::TreeNode(partyLabel))
            {
                DrawPlayerTable(party.players, "Players");
                DrawPlayerTable(party.others, "NPCs/Others");
                ImGui::TreePop();
            }
        }

        DrawGuildsSection(m.guilds);

        if (ImGui::TreeNode("StoC"))
        {
            if (ImGui::TreeNode("Lord Damage"))
            {
                const auto& ld = m.lord_damage;
                if (!ld.has_data)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        "No lord damage data available.");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                        "Total Lord Damage  Blue: %ld", ld.total_lord_damage_blue);
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        "Total Lord Damage  Red:  %ld", ld.total_lord_damage_red);
                    ImGui::Separator();

                    ImGui::Text("Events: %d", (int)ld.events.size());

                    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;

                    if (ImGui::BeginTable("##lord_dmg_events", 5, flags))
                    {
                        ImGui::TableSetupColumn("Timestamp");
                        ImGui::TableSetupColumn("Caster ID");
                        ImGui::TableSetupColumn("Damage");
                        ImGui::TableSetupColumn("Team");
                        ImGui::TableSetupColumn("Cumulative After");
                        ImGui::TableHeadersRow();

                        for (const auto& evt : ld.events)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(evt.timestamp.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%d", evt.caster_id);
                            ImGui::TableNextColumn(); ImGui::Text("%ld", evt.damage);
                            ImGui::TableNextColumn();
                            if (evt.attacking_team == 1)
                                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Blue (1)");
                            else if (evt.attacking_team == 2)
                                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Red (2)");
                            else
                                ImGui::Text("%d", evt.attacking_team);
                            ImGui::TableNextColumn(); ImGui::Text("%ld", evt.damage_after);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }

        ImGui::Unindent(8.0f);
    }
    ImGui::PopID();
}

void draw_debug_match_metadata_panel(ReplayLibrary& library)
{
    if (!GuiGlobalConstants::is_debug_match_metadata_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Debug: Match Metadata", &GuiGlobalConstants::is_debug_match_metadata_open))
    {
        ImGui::End();
        return;
    }

    GuiGlobalConstants::ClampWindowToScreen();

    ImGui::Text("Match Data Folder: %s",
        library.GetFolderPath().empty() ? "(not set)" : library.GetFolderPath().c_str());

    if (!library.IsLoaded())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
            "No matches loaded. Use File > Load Match Data Folder to select a folder.");
        ImGui::End();
        return;
    }

    const auto& matches = library.GetMatches();
    ImGui::Text("Total matches loaded: %d", (int)matches.size());
    ImGui::Separator();

    if (matches.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
            "Folder scanned but no infos.json files found.");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Rescan Folder"))
    {
        library.ScanFolder();
    }

    ImGui::SameLine();
    static char filterBuf[128] = "";
    ImGui::SetNextItemWidth(250);
    ImGui::InputTextWithHint("##filter", "Filter by folder name / guild / occasion...", filterBuf, sizeof(filterBuf));

    ImGui::Separator();

    ImGui::BeginChild("##match_list", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

    std::string filterStr(filterBuf);
    for (auto& c : filterStr) c = (char)tolower((unsigned char)c);

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];

        if (!filterStr.empty())
        {
            auto toLower = [](const std::string& s) {
                std::string r = s;
                for (auto& c : r) c = (char)tolower((unsigned char)c);
                return r;
            };

            bool found = false;
            found |= toLower(m.folder_name).find(filterStr) != std::string::npos;
            found |= toLower(m.occasion).find(filterStr) != std::string::npos;
            found |= toLower(m.flux).find(filterStr) != std::string::npos;
            for (const auto& [gid, g] : m.guilds)
            {
                found |= toLower(g.name).find(filterStr) != std::string::npos;
                found |= toLower(g.tag).find(filterStr) != std::string::npos;
            }
            if (!found) continue;
        }

        DrawSingleMatch(m, i);
    }

    ImGui::EndChild();
    ImGui::End();
}
