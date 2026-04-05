#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Persistent per-match notes, stored locally in settings/notes.json
// ---------------------------------------------------------------------------

class MatchNotes
{
public:
    static MatchNotes& Get()
    {
        static MatchNotes instance;
        static bool loaded = false;
        if (!loaded) { instance.Load(); loaded = true; }
        return instance;
    }

    const std::string& GetNote(const std::string& folderName) const
    {
        auto it = m_notes.find(folderName);
        return (it != m_notes.end()) ? it->second : s_empty;
    }

    bool HasNote(const std::string& folderName) const
    {
        auto it = m_notes.find(folderName);
        return it != m_notes.end() && !it->second.empty();
    }

    void SetNote(const std::string& folderName, const std::string& text)
    {
        if (text.empty())
            m_notes.erase(folderName);
        else
            m_notes[folderName] = text;
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
            for (auto& [key, val] : j.items())
            {
                std::string v = val.get<std::string>();
                if (!v.empty())
                    m_notes[key] = std::move(v);
            }
        } catch (...) {}
    }

    void Save() const
    {
        auto path = GetFilePath();
        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j;
        for (auto& [key, val] : m_notes)
            j[key] = val;
        f << j.dump(2) << "\n";
    }

private:
    std::unordered_map<std::string, std::string> m_notes;
    inline static const std::string s_empty;

    static std::filesystem::path GetFilePath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        auto settingsDir = dir / "settings";
        if (!std::filesystem::exists(settingsDir))
            std::filesystem::create_directories(settingsDir);
        return settingsDir / "notes.json";
    }
};
