#include "pch.h"
#include "SkillSoundTable.h"
#include "json.hpp"
#include <fstream>
#include <algorithm>

namespace {

// Reads one cue array ("caster" / "target" / "other"). Entries are {"id": <file id>, "ms":
// <offset from cast start>}; "after_cast" is ignored because "ms" is already absolute.
// File ids are normally plain integers, but the capture tooling has also emitted them as a
// "<hex> <hex>" pair encoding a GW hash, so that form is still decoded here.
void ParseCueArray(const nlohmann::json& arr, std::vector<SkillSoundCue>& out)
{
    for (const auto& e : arr) {
        if (!e.is_object() || !e.contains("id"))
            continue;

        SkillSoundCue cue;
        const auto& id = e["id"];
        if (id.is_number()) {
            cue.file_id = id.get<uint32_t>();
        }
        else if (id.is_string()) {
            std::string s = id.get<std::string>();
            unsigned int id0 = 0, id1 = 0;
            if (sscanf_s(s.c_str(), "%x %x", &id0, &id1) == 2)
                cue.file_id = static_cast<uint32_t>((static_cast<int>(id0) - 0xFF00FF) +
                                                    (static_cast<int>(id1) * 0xFF00));
            else
                continue;
        }
        else {
            continue;
        }

        if (cue.file_id == 0)
            continue;

        if (e.contains("ms") && e["ms"].is_number())
            cue.offset = e["ms"].get<float>() / 1000.f;

        out.push_back(cue);
    }
}

} // namespace

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

        size_t cueCount = 0;
        for (auto& [key, val] : j.items()) {
            if (!val.is_object())
                continue;

            uint32_t skillId = 0;
            try {
                skillId = static_cast<uint32_t>(std::stoul(key));
            }
            catch (const std::exception&) {
                continue; // non-numeric key
            }

            SkillSoundEntry entry;
            if (val.contains("name") && val["name"].is_string())
                entry.name = val["name"].get<std::string>();
            if (val.contains("cast_duration") && val["cast_duration"].is_number())
                entry.castDuration = val["cast_duration"].get<float>();

            // "other" is ambient/environment audio with no target of its own - emitted from the
            // caster, so it rides along with the caster cues.
            if (val.contains("caster") && val["caster"].is_array())
                ParseCueArray(val["caster"], entry.casterCues);
            if (val.contains("other") && val["other"].is_array())
                ParseCueArray(val["other"], entry.casterCues);
            if (val.contains("target") && val["target"].is_array())
                ParseCueArray(val["target"], entry.targetCues);

            if (entry.casterCues.empty() && entry.targetCues.empty())
                continue;

            // "other" repeats some of "caster" verbatim for a handful of skills; playing the same
            // file at the same offset from the same point twice only adds noise.
            auto dedupe = [](std::vector<SkillSoundCue>& cues) {
                std::sort(cues.begin(), cues.end(), [](const SkillSoundCue& a, const SkillSoundCue& b) {
                    if (a.offset != b.offset) return a.offset < b.offset;
                    return a.file_id < b.file_id;
                });
                cues.erase(std::unique(cues.begin(), cues.end(),
                    [](const SkillSoundCue& a, const SkillSoundCue& b) {
                        return a.file_id == b.file_id && a.offset == b.offset;
                    }), cues.end());
            };
            dedupe(entry.casterCues);
            dedupe(entry.targetCues);

            cueCount += entry.casterCues.size() + entry.targetCues.size();
            m_entries[skillId] = std::move(entry);
        }

        m_loaded = true;
        OutputDebugStringA(std::format("[SkillSoundTable] Loaded {} skills / {} cues\n",
                                       m_entries.size(), cueCount).c_str());
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
