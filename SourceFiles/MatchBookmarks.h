#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Persistent per-match bookmarks, stored locally in settings/bookmarks.json
// ---------------------------------------------------------------------------

class MatchBookmarks
{
public:
    struct Entry
    {
        uint32_t    time_ms = 0;
        std::string title;
    };

    static MatchBookmarks& Get()
    {
        static MatchBookmarks instance;
        static bool loaded = false;
        if (!loaded) { instance.Load(); loaded = true; }
        return instance;
    }

    const std::vector<Entry>& GetBookmarks(const std::string& folderName) const
    {
        auto it = m_bookmarks.find(folderName);
        return (it != m_bookmarks.end()) ? it->second : s_empty;
    }

    void SetBookmarks(const std::string& folderName, const std::vector<Entry>& entries)
    {
        if (entries.empty())
            m_bookmarks.erase(folderName);
        else
            m_bookmarks[folderName] = entries;
        Save();
    }

    void Load()
    {
        auto path = GetFilePath();
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            nlohmann::json j;
            f >> j;
            for (auto& [key, arr] : j.items())
            {
                if (!arr.is_array()) continue;
                std::vector<Entry> entries;
                for (auto& obj : arr)
                {
                    Entry e;
                    if (obj.contains("time_ms")) e.time_ms = obj["time_ms"].get<uint32_t>();
                    if (obj.contains("title"))   e.title   = obj["title"].get<std::string>();
                    entries.push_back(std::move(e));
                }
                if (!entries.empty())
                    m_bookmarks[key] = std::move(entries);
            }
        } catch (...) {}
    }

    void Save() const
    {
        auto path = GetFilePath();
        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j;
        for (auto& [key, entries] : m_bookmarks)
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& e : entries)
                arr.push_back({ {"time_ms", e.time_ms}, {"title", e.title} });
            j[key] = std::move(arr);
        }
        f << j.dump(2) << "\n";
    }

private:
    std::unordered_map<std::string, std::vector<Entry>> m_bookmarks;
    inline static const std::vector<Entry> s_empty;

    static std::filesystem::path GetFilePath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        auto settingsDir = dir / "settings";
        if (!std::filesystem::exists(settingsDir))
            std::filesystem::create_directories(settingsDir);
        return settingsDir / "bookmarks.json";
    }
};
