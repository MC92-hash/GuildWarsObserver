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
            guildsObj[pid] = std::move(gj);
        }
        m["guilds"] = std::move(guildsObj);

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
                entry.guilds[pid] = std::move(gi);
            }
        }

        m_entries.push_back(std::move(entry));
    }

    m_loaded = true;
    m_lastError.clear();
    return true;
}
