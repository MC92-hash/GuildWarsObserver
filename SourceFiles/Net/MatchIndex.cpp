#include "pch.h"
#include "Net/MatchIndex.h"
#include "Net/HttpClient.h"
#include "json.hpp"
#include <fstream>

using json = nlohmann::json;

void MatchIndex::ParseDate(const std::string& dateStr, int& y, int& m, int& d)
{
    y = m = d = 0;
    // Expected format: "YYYY-MM-DD"
    if (dateStr.size() >= 10 && dateStr[4] == '-' && dateStr[7] == '-')
    {
        try
        {
            y = std::stoi(dateStr.substr(0, 4));
            m = std::stoi(dateStr.substr(5, 2));
            d = std::stoi(dateStr.substr(8, 2));
        }
        catch (...) {}
    }
}

bool MatchIndex::FetchFromRemote(HttpClient& http, const std::wstring& path)
{
    auto resp = http.Get(path);
    if (!resp.IsOk())
    {
        m_lastError = "Failed to fetch index: " + resp.errorMessage;
        if (resp.statusCode > 0)
            m_lastError += " (HTTP " + std::to_string(resp.statusCode) + ")";
        return false;
    }

    std::string body(resp.body.begin(), resp.body.end());
    return ParseJson(body);
}

bool MatchIndex::LoadFromCache(const std::filesystem::path& cachePath)
{
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec))
    {
        m_lastError = "Cache file not found: " + cachePath.string();
        return false;
    }

    std::ifstream f(cachePath);
    if (!f.is_open())
    {
        m_lastError = "Failed to open cache file: " + cachePath.string();
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return ParseJson(content);
}

void MatchIndex::SaveToCache(const std::filesystem::path& cachePath) const
{
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);

    json j;
    j["version"] = 1;

    json matchesArr = json::array();
    for (const auto& entry : m_entries)
    {
        json m;
        m["folder"] = entry.folder;
        m["map_id"] = entry.map_id;
        m["date"] = entry.date;
        m["occasion"] = entry.occasion;
        m["flux"] = entry.flux;
        m["duration"] = entry.duration;
        m["winner"] = entry.winner;
        m["size_bytes"] = entry.size_bytes;

        json guildsObj = json::object();
        for (const auto& [pid, g] : entry.guilds)
        {
            json gj;
            gj["name"] = g.name;
            gj["tag"] = g.tag;
            gj["cape"] = {
                {"bg_color", g.cape.bg_color}, {"detail_color", g.cape.detail_color},
                {"emblem_color", g.cape.emblem_color}, {"shape", g.cape.shape},
                {"detail", g.cape.detail}, {"emblem", g.cape.emblem},
                {"trim", g.cape.trim}
            };
            guildsObj[pid] = std::move(gj);
        }
        m["guilds"] = std::move(guildsObj);

        // Team stats
        if (!entry.team_kills.empty())
        {
            json tkObj = json::object();
            for (const auto& [k, v] : entry.team_kills) tkObj[k] = v;
            m["team_kills"] = std::move(tkObj);
        }
        if (!entry.team_damage.empty())
        {
            json tdObj = json::object();
            for (const auto& [k, v] : entry.team_damage) tdObj[k] = v;
            m["team_damage"] = std::move(tdObj);
        }

        // Parties
        if (!entry.parties.empty())
        {
            json partiesObj = json::object();
            for (const auto& [pid, party] : entry.parties)
            {
                json playersArr = json::array();
                for (const auto& p : party.players)
                {
                    json pj;
                    pj["encoded_name"] = p.encoded_name;
                    pj["primary"] = p.primary;
                    pj["secondary"] = p.secondary;
                    pj["player_number"] = p.player_number;
                    pj["skill_template_code"] = p.skill_template_code;
                    pj["kills"] = p.kills;
                    pj["deaths"] = p.deaths;
                    pj["total_damage"] = p.total_damage;
                    pj["preview_stats"] = {
                        p.interrupted_count ? json(*p.interrupted_count) : json(nullptr),
                        p.cancelled_skills_count ? json(*p.cancelled_skills_count) : json(nullptr),
                        p.skills_finished ? json(*p.skills_finished) : json(nullptr),
                        p.total_damage_received ? json(*p.total_damage_received) : json(nullptr),
                        p.total_healing_dealt ? json(*p.total_healing_dealt) : json(nullptr),
                        p.total_healing_received ? json(*p.total_healing_received) : json(nullptr)
                    };
                    json skills = json::array();
                    for (int s : p.used_skills) skills.push_back(s);
                    pj["used_skills"] = std::move(skills);
                    playersArr.push_back(std::move(pj));
                }
                json partyJ;
                partyJ["PLAYER"] = std::move(playersArr);
                partiesObj[pid] = std::move(partyJ);
            }
            m["parties"] = std::move(partiesObj);
        }

        matchesArr.push_back(std::move(m));
    }
    j["matches"] = std::move(matchesArr);

    std::ofstream f(cachePath);
    if (f.is_open())
        f << j.dump(2);
}

const RemoteMatchEntry* MatchIndex::FindEntry(const std::string& folder) const
{
    for (const auto& entry : m_entries)
    {
        if (entry.folder == folder)
            return &entry;
    }
    return nullptr;
}

std::vector<const RemoteMatchEntry*> MatchIndex::GetNewEntries(
    const std::set<std::string>& localFolders) const
{
    std::vector<const RemoteMatchEntry*> result;
    for (const auto& entry : m_entries)
    {
        if (localFolders.find(entry.folder) == localFolders.end())
            result.push_back(&entry);
    }
    return result;
}

bool MatchIndex::ParseJson(const std::string& jsonStr)
{
    json j = json::parse(jsonStr, nullptr, false, true);
    if (j.is_discarded())
    {
        m_lastError = "Failed to parse index JSON";
        return false;
    }

    if (!j.contains("matches") || !j["matches"].is_array())
    {
        m_lastError = "index.json missing 'matches' array";
        return false;
    }

    m_entries.clear();
    for (const auto& item : j["matches"])
    {
        RemoteMatchEntry entry;
        entry.folder = item.value("folder", "");
        if (entry.folder.empty())
            continue;

        entry.map_id = item.value("map_id", 0);
        entry.date = item.value("date", "");
        ParseDate(entry.date, entry.year, entry.month, entry.day);
        entry.occasion = item.value("occasion", "");
        entry.flux = item.value("flux", "");
        entry.duration = item.value("duration", "");
        entry.winner = item.value("winner", 0);
        entry.size_bytes = item.value("size_bytes", (uint64_t)0);

        if (item.contains("guilds") && item["guilds"].is_object())
        {
            for (auto& [pid, gObj] : item["guilds"].items())
            {
                RemoteGuildInfo gi;
                gi.name = gObj.value("name", "");
                gi.tag = gObj.value("tag", "");
                if (gObj.contains("cape") && gObj["cape"].is_object())
                {
                    const auto& c = gObj["cape"];
                    gi.cape.bg_color = c.value("bg_color", 0);
                    gi.cape.detail_color = c.value("detail_color", 0);
                    gi.cape.emblem_color = c.value("emblem_color", 0);
                    gi.cape.shape = c.value("shape", 0);
                    gi.cape.detail = c.value("detail", 0);
                    gi.cape.emblem = c.value("emblem", 0);
                    gi.cape.trim = c.value("trim", 0);
                }
                entry.guilds[pid] = std::move(gi);
            }
        }

        if (item.contains("team_kills") && item["team_kills"].is_object())
        {
            for (auto& [k, v] : item["team_kills"].items())
                entry.team_kills[k] = v.get<int>();
        }

        if (item.contains("team_damage") && item["team_damage"].is_object())
        {
            for (auto& [k, v] : item["team_damage"].items())
                entry.team_damage[k] = v.get<int>();
        }

        if (item.contains("parties") && item["parties"].is_object())
        {
            for (auto& [pid, partyObj] : item["parties"].items())
            {
                RemotePartyInfo party;
                if (partyObj.contains("PLAYER") && partyObj["PLAYER"].is_array())
                {
                    for (const auto& pj : partyObj["PLAYER"])
                    {
                        RemotePlayerInfo pi;
                        pi.encoded_name = pj.value("encoded_name", "");
                        pi.primary = pj.value("primary", 0);
                        pi.secondary = pj.value("secondary", 0);
                        pi.player_number = pj.value("player_number", 0);
                        pi.skill_template_code = pj.value("skill_template_code", "");
                        pi.kills = pj.value("kills", 0);
                        pi.deaths = pj.value("deaths", 0);
                        pi.total_damage = pj.value("total_damage", 0);
                        if (pj.contains("preview_stats") && pj["preview_stats"].is_array())
                        {
                            const auto& ps = pj["preview_stats"];
                            if (ps.size() > 0 && ps[0].is_number_integer())
                                pi.interrupted_count = ps[0].get<int>();
                            if (ps.size() > 1 && ps[1].is_number_integer())
                                pi.cancelled_skills_count = ps[1].get<int>();
                            if (ps.size() > 2 && ps[2].is_number_integer())
                                pi.skills_finished = ps[2].get<int>();
                            if (ps.size() > 3 && ps[3].is_number_integer())
                                pi.total_damage_received = ps[3].get<int>();
                            if (ps.size() > 4 && ps[4].is_number_integer())
                                pi.total_healing_dealt = ps[4].get<int>();
                            if (ps.size() > 5 && ps[5].is_number_integer())
                                pi.total_healing_received = ps[5].get<int>();
                        }
                        if (pj.contains("used_skills") && pj["used_skills"].is_array())
                        {
                            for (const auto& s : pj["used_skills"])
                                pi.used_skills.push_back(s.get<int>());
                        }
                        party.players.push_back(std::move(pi));
                    }
                }
                entry.parties[pid] = std::move(party);
            }
        }

        m_entries.push_back(std::move(entry));
    }

    m_loaded = true;
    m_lastError.clear();
    return true;
}
