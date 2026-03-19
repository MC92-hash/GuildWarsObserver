#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct SkillSoundEntry {
    std::string name;
    std::vector<uint32_t> file_ids;        // legacy: played at cast start (caster pos)
    std::vector<uint32_t> caster_sounds;   // played when cast starts (caster position)
    std::vector<uint32_t> target_sounds;   // played when cast completes (target position)
};

class SkillSoundTable {
public:
    bool Load(const std::string& jsonPath);
    const SkillSoundEntry* Get(uint32_t skillId) const;
    bool IsLoaded() const { return m_loaded; }

private:
    std::unordered_map<uint32_t, SkillSoundEntry> m_entries;
    bool m_loaded = false;
};
