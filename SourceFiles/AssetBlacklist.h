#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <json.hpp>

// ---------------------------------------------------------------------------
// Persistent per-map asset blacklist and transparency overrides.
// Stored in settings/asset_blacklists/<map_id>.json
//
// hidden_assets:      prop_index -> bool (true = hidden)
// transparent_assets: prop_index -> float alpha in [0,1]
// ---------------------------------------------------------------------------

class AssetBlacklist
{
public:
    static AssetBlacklist& Get()
    {
        static AssetBlacklist instance;
        return instance;
    }

    void LoadForMap(int map_id)
    {
        m_entries.clear();
        m_alphaEntries.clear();
        m_currentMapId = map_id;
        if (map_id == 0) return;

        auto path = GetFilePath(map_id);
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            nlohmann::json j;
            f >> j;
            if (j.contains("hidden_assets") && j["hidden_assets"].is_object())
            {
                for (auto& [key, val] : j["hidden_assets"].items())
                {
                    uint32_t propIndex = static_cast<uint32_t>(std::stoul(key));
                    m_entries[propIndex] = val.get<bool>();
                }
            }
            if (j.contains("transparent_assets") && j["transparent_assets"].is_object())
            {
                for (auto& [key, val] : j["transparent_assets"].items())
                {
                    uint32_t propIndex = static_cast<uint32_t>(std::stoul(key));
                    m_alphaEntries[propIndex] = std::clamp(val.get<float>(), 0.0f, 1.0f);
                }
            }
        } catch (...) {}
    }

    void Save() const
    {
        if (m_currentMapId == 0) return;

        auto path = GetFilePath(m_currentMapId);
        if (m_entries.empty() && m_alphaEntries.empty())
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return;
        }

        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j;
        if (!m_entries.empty()) {
            nlohmann::json& ha = j["hidden_assets"];
            for (auto& [propIdx, hidden] : m_entries)
                ha[std::to_string(propIdx)] = hidden;
        }
        if (!m_alphaEntries.empty()) {
            nlohmann::json& ta = j["transparent_assets"];
            for (auto& [propIdx, alpha] : m_alphaEntries)
                ta[std::to_string(propIdx)] = alpha;
        }
        f << j.dump(2) << "\n";
    }

    // --- Blacklist (hidden) ---

    bool IsHidden(uint32_t prop_index) const
    {
        auto it = m_entries.find(prop_index);
        return it != m_entries.end() && it->second;
    }

    bool HasEntry(uint32_t prop_index) const
    {
        return m_entries.find(prop_index) != m_entries.end();
    }

    void SetEntry(uint32_t prop_index, bool hidden)
    {
        m_entries[prop_index] = hidden;
        Save();
    }

    void RemoveEntry(uint32_t prop_index)
    {
        m_entries.erase(prop_index);
        Save();
    }

    const std::unordered_map<uint32_t, bool>& GetEntries() const { return m_entries; }

    // --- Transparency overrides ---

    bool HasAlphaOverride(uint32_t prop_index) const
    {
        return m_alphaEntries.find(prop_index) != m_alphaEntries.end();
    }

    float GetAlpha(uint32_t prop_index) const
    {
        auto it = m_alphaEntries.find(prop_index);
        return (it != m_alphaEntries.end()) ? it->second : 1.0f;
    }

    void SetAlpha(uint32_t prop_index, float alpha)
    {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        m_alphaEntries[prop_index] = alpha;
        Save();
    }

    void RemoveAlpha(uint32_t prop_index)
    {
        m_alphaEntries.erase(prop_index);
        Save();
    }

    const std::unordered_map<uint32_t, float>& GetAlphaEntries() const { return m_alphaEntries; }

    int GetCurrentMapId() const { return m_currentMapId; }

private:
    std::unordered_map<uint32_t, bool> m_entries;
    std::unordered_map<uint32_t, float> m_alphaEntries;
    int m_currentMapId = 0;

    static std::filesystem::path GetFilePath(int map_id)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        auto blacklistDir = dir / "settings" / "asset_blacklists";
        if (!std::filesystem::exists(blacklistDir))
            std::filesystem::create_directories(blacklistDir);
        return blacklistDir / (std::to_string(map_id) + ".json");
    }
};
