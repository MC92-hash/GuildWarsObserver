#pragma once
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Persistent match ratings (1-5 stars), stored locally in settings/ratings.json
// ---------------------------------------------------------------------------

class MatchRatings
{
public:
    static MatchRatings& Get()
    {
        static MatchRatings instance;
        static bool loaded = false;
        if (!loaded) { instance.Load(); loaded = true; }
        return instance;
    }

    int GetRating(const std::string& folderName) const
    {
        auto it = m_ratings.find(folderName);
        return (it != m_ratings.end()) ? it->second : 0;
    }

    void SetRating(const std::string& folderName, int stars)
    {
        stars = std::clamp(stars, 0, 5);
        if (stars == 0)
            m_ratings.erase(folderName);
        else
            m_ratings[folderName] = stars;
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
                int v = val.get<int>();
                if (v >= 1 && v <= 5)
                    m_ratings[key] = v;
            }
        } catch (...) {}
    }

    void Save() const
    {
        auto path = GetFilePath();
        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j;
        for (auto& [key, val] : m_ratings)
            j[key] = val;
        f << j.dump(2) << "\n";
    }

private:
    std::unordered_map<std::string, int> m_ratings;

    static std::filesystem::path GetFilePath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        auto settingsDir = dir / "settings";
        if (!std::filesystem::exists(settingsDir))
            std::filesystem::create_directories(settingsDir);
        return settingsDir / "ratings.json";
    }
};

// ---------------------------------------------------------------------------
// Star drawing helpers (5-pointed star via ImDrawList)
// ---------------------------------------------------------------------------

inline void DrawStarFilled(ImDrawList* dl, float cx, float cy, float r, ImU32 col)
{
    ImVec2 verts[10];
    for (int i = 0; i < 10; i++)
    {
        float angle = (float)i * 3.14159265f / 5.f - 3.14159265f / 2.f;
        float rad = (i % 2 == 0) ? r : r * 0.4f;
        verts[i] = ImVec2(cx + cosf(angle) * rad, cy + sinf(angle) * rad);
    }
    dl->AddConvexPolyFilled(verts, 10, col);
}

inline void DrawStarOutline(ImDrawList* dl, float cx, float cy, float r, ImU32 col)
{
    ImVec2 verts[10];
    for (int i = 0; i < 10; i++)
    {
        float angle = (float)i * 3.14159265f / 5.f - 3.14159265f / 2.f;
        float rad = (i % 2 == 0) ? r : r * 0.4f;
        verts[i] = ImVec2(cx + cosf(angle) * rad, cy + sinf(angle) * rad);
    }
    dl->AddPolyline(verts, 10, col, ImDrawFlags_Closed, 1.0f);
}

// ---------------------------------------------------------------------------
// Star rating ImGui widget
// Returns the new rating if user clicked a star, or 0 if no interaction.
// Pass readOnly=true for non-interactive display (e.g. table cells).
// ---------------------------------------------------------------------------

inline int DrawStarRating(const char* id, int currentRating, bool readOnly = false)
{
    ImGui::PushID(id);

    const float starSize = 12.f;
    const float spacing  = 2.f;
    const float totalW   = 5 * starSize + 4 * spacing;
    const ImVec2 cursor  = ImGui::GetCursorScreenPos();
    const float  cy      = cursor.y + ImGui::GetTextLineHeight() * 0.5f;
    ImDrawList* dl       = ImGui::GetWindowDrawList();

    const ImU32 colFilled = IM_COL32(230, 180, 30, 255);  // gold
    const ImU32 colEmpty  = IM_COL32(100, 100, 100, 140);  // gray outline
    const ImU32 colHover  = IM_COL32(255, 210, 60, 255);  // bright gold

    int clicked = 0;

    // Invisible button for the whole star area
    if (!readOnly)
    {
        ImGui::InvisibleButton("##stars", ImVec2(totalW, ImGui::GetTextLineHeight()));
        if (ImGui::IsItemHovered())
        {
            float mx = ImGui::GetIO().MousePos.x - cursor.x;
            int hoverStar = std::clamp((int)(mx / (starSize + spacing)) + 1, 1, 5);

            // Draw hover preview
            for (int i = 1; i <= 5; i++)
            {
                float cx = cursor.x + (i - 1) * (starSize + spacing) + starSize * 0.5f;
                ImU32 col = (i <= hoverStar) ? colHover : colEmpty;
                if (i <= hoverStar)
                    DrawStarFilled(dl, cx, cy, starSize * 0.5f, col);
                else
                    DrawStarOutline(dl, cx, cy, starSize * 0.5f, col);
            }

            if (ImGui::IsItemClicked())
                clicked = (hoverStar == currentRating) ? -1 : hoverStar;  // -1 = clear
        }
        else
        {
            // Draw current rating
            for (int i = 1; i <= 5; i++)
            {
                float cx = cursor.x + (i - 1) * (starSize + spacing) + starSize * 0.5f;
                if (i <= currentRating)
                    DrawStarFilled(dl, cx, cy, starSize * 0.5f, colFilled);
                else
                    DrawStarOutline(dl, cx, cy, starSize * 0.5f, colEmpty);
            }
        }
    }
    else
    {
        // Read-only: just draw and advance cursor
        for (int i = 1; i <= 5; i++)
        {
            float cx = cursor.x + (i - 1) * (starSize + spacing) + starSize * 0.5f;
            if (i <= currentRating)
                DrawStarFilled(dl, cx, cy, starSize * 0.5f, colFilled);
            else
                DrawStarOutline(dl, cx, cy, starSize * 0.5f, colEmpty);
        }
        ImGui::Dummy(ImVec2(totalW, ImGui::GetTextLineHeight()));
    }

    ImGui::PopID();
    return clicked;  // >0 = new rating, -1 = clear, 0 = no click
}
