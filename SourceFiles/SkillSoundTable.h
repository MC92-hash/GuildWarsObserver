#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// One layered sound of a skill cast, captured at a fixed offset from the moment the cast started.
// A single cast emits several of these at once or in sequence (the game stacks a cast loop, an
// impact, an ambient tail, ...), which is why a skill maps to a list rather than one file id.
struct SkillSoundCue {
    uint32_t file_id = 0;
    float    offset  = 0.f;   // seconds after cast start (settings/skill_sounds.json "ms" / 1000)
    bool     afterCast = false;   // "after_cast": captured once the cast had completed
};

// A skill's full captured sound timeline, from settings/skill_sounds.json. Cues are split by
// where they are emitted from: the caster's position or the target's. "after_cast" and
// "cast_completed_ms" are kept so a cue captured after the cast can be placed after the
// replay's own release (the private sound schedule decides how they are played).
struct SkillSoundEntry {
    std::string name;
    float castDuration = 0.f;                 // "cast_duration" seconds (0 for instants)
    float castCompletedMs = 0.f;              // "cast_completed_ms": when the captured cast completed
    std::vector<SkillSoundCue> casterCues;    // JSON "caster" + "other" (ambient, emitted at caster)
    std::vector<SkillSoundCue> targetCues;    // JSON "target"
};

// skill_id -> captured sound timeline. Replaces StoC/sound_events.txt as the source of skill
// audio entirely: the recorder only ever wrote a sound_events line for skills cast within
// earshot of the recording camera, so most casts on the map were never captured at all.
class SkillSoundTable {
public:
    bool Load(const std::string& jsonPath);
    const SkillSoundEntry* Get(uint32_t skillId) const;
    bool IsLoaded() const { return m_loaded; }
    size_t Size() const { return m_entries.size(); }

private:
    std::unordered_map<uint32_t, SkillSoundEntry> m_entries;
    bool m_loaded = false;
};
