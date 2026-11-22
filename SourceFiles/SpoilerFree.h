#pragma once
#include <string>
#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Spoiler-free mode: which matches may show their result.
//
// A match's result is its winner, its length and everything that gives them away (lord damage,
// kills, deaths, the Stats and Map tabs, the replay's scrub bar). One switch, the Library's
// Spoiler-free toggle, hides it for every match at once; a match then stays hidden until it is
// REVEALED: by hand (a Reveal button) or by watching the replay to the end. A match opened from a
// share link follows the viewer's own toggle.
//
// Keyed by the match folder name, as the ratings and notes are. Stored locally in
// settings/spoilers.json.
// ---------------------------------------------------------------------------

class SpoilerFree
{
public:
    static SpoilerFree& Get()
    {
        static SpoilerFree instance;
        static bool loaded = false;
        if (!loaded) { instance.Load(); loaded = true; }
        return instance;
    }

    // Bumped on every change, for the views that cache what they computed from results.
    unsigned Version() const { return m_version; }

    // The Library's toggle.
    bool Enabled() const { return m_enabled; }
    void SetEnabled(bool on)
    {
        if (on == m_enabled) return;
        m_enabled = on;
        m_version++;
        Save();
    }

    bool IsRevealed(const std::string& folder) const { return m_revealed.count(folder) != 0; }

    // A match with no folder (a file opened by hand) has nowhere to remember a reveal, so it
    // never hides.
    bool IsHidden(const std::string& folder) const
    {
        return m_enabled && !folder.empty() && !IsRevealed(folder);
    }

    // Recorded even while the toggle is off: a match watched to the end before the toggle goes on
    // stays revealed afterwards.
    void Reveal(const std::string& folder)
    {
        if (folder.empty() || IsRevealed(folder)) return;
        m_revealed.insert(folder);
        m_version++;
        Save();
    }

    void Load()
    {
        std::ifstream f(GetFilePath());
        if (!f.is_open()) return;
        try {
            nlohmann::json j;
            f >> j;
            m_enabled = j.value("hide_results", false);
            auto readSet = [&](const char* key, std::unordered_set<std::string>& out) {
                if (!j.contains(key) || !j[key].is_array()) return;
                for (const auto& v : j[key])
                    if (v.is_string() && !v.get<std::string>().empty())
                        out.insert(v.get<std::string>());
            };
            readSet("revealed", m_revealed);
        } catch (...) {}
        m_version++;
    }

    void Save() const
    {
        std::ofstream f(GetFilePath());
        if (!f.is_open()) return;
        nlohmann::json j;
        j["hide_results"] = m_enabled;
        j["revealed"]     = nlohmann::json::array();
        for (const auto& s : m_revealed) j["revealed"].push_back(s);
        f << j.dump(2) << "\n";
    }

private:
    bool m_enabled = false;
    std::unordered_set<std::string> m_revealed;
    unsigned m_version = 0;

    static std::filesystem::path GetFilePath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto settingsDir = std::filesystem::path(exePath).parent_path() / "settings";
        std::error_code ec;
        if (!std::filesystem::exists(settingsDir, ec))
            std::filesystem::create_directories(settingsDir, ec);
        return settingsDir / "spoilers.json";
    }
};
