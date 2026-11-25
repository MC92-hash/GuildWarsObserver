#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Spoiler-free mode: which matches may show their result.
//
// A match's result is its winner, its length and everything that gives them away (lord damage,
// kills, deaths, the Stats and Map tabs, the replay's scrub bar). One switch, the Library's
// Spoiler toggle: on, every match hides its result; off, every match shows it. There is no
// per-match reveal (owner's call, 2026-11-25). A match opened from a share link follows the
// viewer's own toggle.
//
// Stored locally in settings/spoilers.json.
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

    // A match with no folder (a file opened by hand) never hides.
    bool IsHidden(const std::string& folder) const
    {
        return m_enabled && !folder.empty();
    }

    void Load()
    {
        std::ifstream f(GetFilePath());
        if (!f.is_open()) return;
        try {
            nlohmann::json j;
            f >> j;
            m_enabled = j.value("hide_results", false);
        } catch (...) {}
        m_version++;
    }

    void Save() const
    {
        std::ofstream f(GetFilePath());
        if (!f.is_open()) return;
        nlohmann::json j;
        j["hide_results"] = m_enabled;
        f << j.dump(2) << "\n";
    }

private:
    bool m_enabled = false;
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
