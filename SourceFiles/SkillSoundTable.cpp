#include "pch.h"
#include "SkillSoundTable.h"
#include "json.hpp"
#include <fstream>

static void ParseFileIdArray(const nlohmann::json& arr, std::vector<uint32_t>& out)
{
    for (auto& fid : arr) {
        if (fid.is_number()) {
            out.push_back(fid.get<uint32_t>());
        }
        else if (fid.is_string()) {
            std::string s = fid.get<std::string>();
            unsigned int id0 = 0, id1 = 0;
            if (sscanf_s(s.c_str(), "%x %x", &id0, &id1) == 2) {
                int decoded = (static_cast<int>(id0) - 0xFF00FF) + (static_cast<int>(id1) * 0xFF00);
                out.push_back(static_cast<uint32_t>(decoded));
                OutputDebugStringA(std::format("[SkillSoundTable] Decoded '{}' -> hash {}\n", s, decoded).c_str());
            }
        }
    }
}

bool SkillSoundTable::Load(const std::string& jsonPath)
{
    m_entries.clear();
    m_loaded = false;

    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        OutputDebugStringA(("[SkillSoundTable] Failed to open: " + jsonPath + "\n").c_str());
        return false;
    }

    try {
        nlohmann::json j;
        f >> j;

        for (auto& [key, val] : j.items()) {
            uint32_t skillId = static_cast<uint32_t>(std::stoul(key));
            SkillSoundEntry entry;
            if (val.contains("name"))
                entry.name = val["name"].get<std::string>();

            if (val.contains("caster_sounds") && val["caster_sounds"].is_array())
                ParseFileIdArray(val["caster_sounds"], entry.caster_sounds);
            if (val.contains("target_sounds") && val["target_sounds"].is_array())
                ParseFileIdArray(val["target_sounds"], entry.target_sounds);

            // Legacy: "file_ids" maps to caster_sounds for backward compat
            if (val.contains("file_ids") && val["file_ids"].is_array())
                ParseFileIdArray(val["file_ids"], entry.caster_sounds);

            if (!entry.caster_sounds.empty() || !entry.target_sounds.empty())
                m_entries[skillId] = std::move(entry);
        }

        m_loaded = true;
        OutputDebugStringA(std::format("[SkillSoundTable] Loaded {} skill sound entries\n", m_entries.size()).c_str());
        return true;
    }
    catch (const std::exception& ex) {
        OutputDebugStringA(std::format("[SkillSoundTable] Parse error: {}\n", ex.what()).c_str());
        return false;
    }
}

const SkillSoundEntry* SkillSoundTable::Get(uint32_t skillId) const
{
    auto it = m_entries.find(skillId);
    return (it != m_entries.end()) ? &it->second : nullptr;
}
