#pragma once
#include <string>
#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Favourite matches: one star, on or off, kept on this PC in settings/favourites.json as an
// array of match folder names. The Library page's row star and its Favorites view
// (draw_library_page.inl). Separate from the 1-5 rating (MatchRatings.h): a favourite is one
// click to keep a match at hand, the rating is a judgement of it.
// ---------------------------------------------------------------------------

class MatchFavourites
{
public:
    static MatchFavourites& Get()
    {
        static MatchFavourites instance;
        static bool loaded = false;
        if (!loaded) { instance.Load(); loaded = true; }
        return instance;
    }

    // Bumped on every change, for views that cache what they filtered.
    unsigned Version() const { return m_version; }

    size_t Count() const { return m_folders.size(); }

    bool IsFavourite(const std::string& folderName) const
    {
        return m_folders.count(folderName) > 0;
    }

    void SetFavourite(const std::string& folderName, bool on)
    {
        if (folderName.empty() || on == IsFavourite(folderName)) return;
        if (on) m_folders.insert(folderName);
        else    m_folders.erase(folderName);
        m_version++;
        Save();
    }

    void Toggle(const std::string& folderName) { SetFavourite(folderName, !IsFavourite(folderName)); }

private:
    void Load()
    {
        std::ifstream f(GetFilePath());
        if (!f.is_open()) return;
        try
        {
            nlohmann::json j;
            f >> j;
            if (j.is_array())
                for (const auto& v : j)
                    if (v.is_string()) m_folders.insert(v.get<std::string>());
        }
        catch (...) {}
        m_version++;
    }

    void Save() const
    {
        std::ofstream f(GetFilePath());
        if (!f.is_open()) return;
        nlohmann::json j = nlohmann::json::array();
        for (const auto& folder : m_folders) j.push_back(folder);
        f << j.dump(2) << "\n";
    }

    static std::filesystem::path GetFilePath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const auto settingsDir = std::filesystem::path(exePath).parent_path() / "settings";
        std::error_code ec;
        if (!std::filesystem::exists(settingsDir, ec))
            std::filesystem::create_directories(settingsDir, ec);
        return settingsDir / "favourites.json";
    }

    std::unordered_set<std::string> m_folders;
    unsigned m_version = 0;
};
