#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <cstdint>

class HttpClient;

struct RemoteGuildInfo
{
    std::string name;
    std::string tag;
};

struct RemotePlayerInfo
{
    std::string encoded_name;
    int primary = 0;
    int secondary = 0;
    std::vector<int> used_skills;
    std::string skill_template_code;
    int kills = 0;
    int deaths = 0;
    int total_damage = 0;
};

struct RemotePartyInfo
{
    std::vector<RemotePlayerInfo> players;
};

struct RemoteMatchEntry
{
    std::string folder;                             // S3 folder name (with spaces)
    int map_id = 0;
    std::string date;                               // "YYYY-MM-DD"
    int year = 0, month = 0, day = 0;              // parsed from date
    std::string occasion;
    std::string flux;
    std::string duration;
    int winner = 0;                                 // party id (1 or 2)
    uint64_t size_bytes = 0;
    std::map<std::string, RemoteGuildInfo> guilds;  // keyed by party id "1", "2"
    std::map<std::string, int> team_kills;
    std::map<std::string, int> team_damage;
    std::map<std::string, RemotePartyInfo> parties; // keyed by party id "1", "2"
};

class MatchIndex
{
public:
    // Fetch index.json from the remote S3 bucket.
    // path: e.g. L"/index.json"
    bool FetchFromRemote(HttpClient& http, const std::wstring& path = L"/index.json");

    // Load/save a local cache for offline fallback.
    bool LoadFromCache(const std::filesystem::path& cachePath);
    void SaveToCache(const std::filesystem::path& cachePath) const;

    const std::vector<RemoteMatchEntry>& GetEntries() const { return m_entries; }
    int GetEntryCount() const { return static_cast<int>(m_entries.size()); }

    // Find the entry for a given folder name. Returns nullptr if not found.
    const RemoteMatchEntry* FindEntry(const std::string& folder) const;

    // Returns entries whose folder is NOT in localFolders.
    std::vector<const RemoteMatchEntry*> GetNewEntries(
        const std::set<std::string>& localFolders) const;

    std::string GetLastError() const { return m_lastError; }
    bool IsLoaded() const { return m_loaded; }

private:
    bool ParseJson(const std::string& jsonStr);
    static void ParseDate(const std::string& dateStr, int& y, int& m, int& d);

    std::vector<RemoteMatchEntry> m_entries;
    std::string m_lastError;
    bool m_loaded = false;
};
