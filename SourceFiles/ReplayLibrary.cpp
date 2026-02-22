#include "pch.h"
#include "ReplayLibrary.h"
#include <json.hpp>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

static bool IsStandaloneCommaLine(const std::string& line)
{
    bool foundComma = false;
    for (auto c : line)
    {
        if (c == ' ' || c == '\t' || c == '\r') continue;
        if (c == ',' && !foundComma) { foundComma = true; continue; }
        return false;
    }
    return foundComma;
}

static std::string SanitizeJson(const std::string& raw)
{
    // The recorder produces infos.json with commas on their own lines.
    // Two cases:
    //   1) Previous line already ends with ',' -> standalone comma is duplicate -> remove it
    //   2) Previous line does NOT end with ',' -> standalone comma is a field separator -> append to prev line
    std::vector<std::string> lines;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);

    std::string result;
    result.reserve(raw.size());

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (IsStandaloneCommaLine(lines[i]))
        {
            // Check if previous content line already ends with a comma
            // If so, this is a duplicate comma -> skip entirely
            // If not, append comma to the previous line
            if (!result.empty())
            {
                // Find the last non-whitespace char in result before the trailing newline
                size_t pos = result.size();
                while (pos > 0 && (result[pos - 1] == '\n' || result[pos - 1] == '\r'
                    || result[pos - 1] == ' ' || result[pos - 1] == '\t'))
                    pos--;

                if (pos > 0 && result[pos - 1] == ',')
                {
                    // Previous line already has trailing comma -> skip this duplicate
                    continue;
                }
                else
                {
                    // Insert comma after previous content, before the trailing newline
                    result.insert(pos, ",");
                    continue;
                }
            }
            continue;
        }

        result += lines[i];
        result += '\n';
    }

    return result;
}

// --- LocalReplayProvider ---

void LocalReplayProvider::SetFolder(const std::string& path)
{
    m_folder_path = path;
}

void LocalReplayProvider::ParsePlayerArray(const void* jsonArrayPtr, std::vector<PlayerMeta>& out)
{
    const json& arr = *static_cast<const json*>(jsonArrayPtr);
    if (!arr.is_array()) return;

    out.reserve(arr.size());
    for (const auto& p : arr)
    {
        PlayerMeta pm;
        pm.id = p.value("id", 0);
        pm.primary = p.value("primary", 0);
        pm.secondary = p.value("secondary", 0);
        pm.level = p.value("level", 0);
        pm.team_id = p.value("team_id", 0);
        pm.player_number = p.value("player_number", 0);
        pm.guild_id = p.value("guild_id", 0);
        pm.model_id = p.value("model_id", 0);
        pm.gadget_id = p.value("gadget_id", 0);
        pm.encoded_name = p.value("encoded_name", std::string());
        pm.total_damage = p.value("total_damage", 0);
        pm.attacks_started = p.value("attacks_started", 0);
        pm.attacks_finished = p.value("attacks_finished", 0);
        pm.attacks_stopped = p.value("attacks_stopped", 0);
        pm.skills_activated = p.value("skills_activated", 0);
        pm.skills_finished = p.value("skills_finished", 0);
        pm.skills_stopped = p.value("skills_stopped", 0);
        pm.attack_skills_activated = p.value("attack_skills_activated", 0);
        pm.attack_skills_finished = p.value("attack_skills_finished", 0);
        pm.attack_skills_stopped = p.value("attack_skills_stopped", 0);
        pm.interrupted_count = p.value("interrupted_count", 0);
        pm.interrupted_skills_count = p.value("interrupted_skills_count", 0);
        pm.cancelled_attacks_count = p.value("cancelled_attacks_count", 0);
        pm.cancelled_skills_count = p.value("cancelled_skills_count", 0);
        pm.crits_dealt = p.value("crits_dealt", 0);
        pm.crits_received = p.value("crits_received", 0);
        pm.deaths = p.value("deaths", 0);
        pm.kills = p.value("kills", 0);
        pm.skill_template_code = p.value("skill_template_code", std::string());

        if (p.contains("used_skills") && p["used_skills"].is_array())
        {
            for (const auto& s : p["used_skills"])
                pm.used_skills.push_back(s.get<int>());
        }

        out.push_back(std::move(pm));
    }
}

bool LocalReplayProvider::ParseInfosJson(const std::filesystem::path& jsonPath, MatchMeta& out)
{
    try
    {
        std::ifstream file(jsonPath);
        if (!file.is_open()) return false;

        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        std::string sanitized = SanitizeJson(buffer.str());

        json j = json::parse(sanitized, nullptr, false, true);
        if (j.is_discarded()) return false;

        out.map_id = j.value("map_id", 0);
        out.flux = j.value("flux", std::string());
        out.day = j.value("day", 0);
        out.month = j.value("month", 0);
        out.year = j.value("year", 0);
        out.occasion = j.value("occasion", std::string());
        out.match_duration = j.value("match_duration", std::string());
        out.match_original_duration = j.value("match_original_duration", std::string());
        out.match_end_time_ms = j.value("match_end_time_ms", 0);
        out.match_end_time_formatted = j.value("match_end_time_formatted", std::string());
        out.winner_party_id = j.value("winner_party_id", 0);

        if (j.contains("team_kills") && j["team_kills"].is_object())
        {
            for (auto& [key, val] : j["team_kills"].items())
                out.team_kills[key] = val.get<int>();
        }

        if (j.contains("team_damage") && j["team_damage"].is_object())
        {
            for (auto& [key, val] : j["team_damage"].items())
                out.team_damage[key] = val.get<int>();
        }

        if (j.contains("parties") && j["parties"].is_object())
        {
            for (auto& [partyId, partyObj] : j["parties"].items())
            {
                PartyMeta party;
                if (partyObj.contains("PLAYER"))
                    ParsePlayerArray(static_cast<const void*>(&partyObj["PLAYER"]), party.players);
                if (partyObj.contains("OTHER"))
                    ParsePlayerArray(static_cast<const void*>(&partyObj["OTHER"]), party.others);
                out.parties[partyId] = std::move(party);
            }
        }

        if (j.contains("guilds") && j["guilds"].is_object())
        {
            for (auto& [guildId, gObj] : j["guilds"].items())
            {
                GuildMeta gm;
                gm.id = gObj.value("id", 0);
                gm.name = gObj.value("name", std::string());
                gm.tag = gObj.value("tag", std::string());
                gm.rank = gObj.value("rank", 0);
                gm.features = gObj.value("features", 0);
                gm.rating = gObj.value("rating", 0);
                gm.faction = gObj.value("faction", 0);
                gm.faction_points = gObj.value("faction_points", 0);
                gm.qualifier_points = gObj.value("qualifier_points", 0);

                if (gObj.contains("cape") && gObj["cape"].is_object())
                {
                    const auto& c = gObj["cape"];
                    gm.cape.bg_color = c.value("bg_color", 0);
                    gm.cape.detail_color = c.value("detail_color", 0);
                    gm.cape.emblem_color = c.value("emblem_color", 0);
                    gm.cape.shape = c.value("shape", 0);
                    gm.cape.detail = c.value("detail", 0);
                    gm.cape.emblem = c.value("emblem", 0);
                    gm.cape.trim = c.value("trim", 0);
                }

                out.guilds[guildId] = std::move(gm);
            }
        }

        out.folder_name = jsonPath.parent_path().filename().string();
        out.folder_path = jsonPath.parent_path().string();

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::vector<MatchMeta> LocalReplayProvider::GetAvailableReplays()
{
    std::vector<MatchMeta> results;

    if (m_folder_path.empty() || !std::filesystem::exists(m_folder_path))
        return results;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_folder_path, ec))
    {
        if (!entry.is_directory()) continue;

        auto infosPath = entry.path() / "infos.json";
        if (!std::filesystem::exists(infosPath)) continue;

        MatchMeta meta;
        if (ParseInfosJson(infosPath, meta))
            results.push_back(std::move(meta));
    }

    return results;
}

// --- ReplayLibrary ---

void ReplayLibrary::SetMatchDataFolder(const std::string& path)
{
    m_folder_path = path;
    m_provider.SetFolder(path);
}

void ReplayLibrary::ScanFolder()
{
    m_matches.clear();
    m_loaded = false;

    if (m_folder_path.empty()) return;

    m_matches = m_provider.GetAvailableReplays();
    m_loaded = true;
}

void ReplayLibrary::Clear()
{
    m_matches.clear();
    m_loaded = false;
    m_folder_path.clear();
}
