#pragma once
#include <algorithm>
#include <vector>
#include <utility>

// Repairs for the recorder's saved BAR metadata, not for the cast-event stream.
// Verified against original skill_events / attack_skill_events in archived
// matches. Limits are the observed recording periods, inclusive: the same bad
// ID can refer to a different skill in another period (notably 3448 and 3492).
// See scripts/audit_historical_skill_ids.py and scripts/data/historical_skill_id_evidence.json.
inline int ResolveHistoricalBarSkillId(int id, int dateKey)
{
    struct Repair { int id, first, last, original; };
    static constexpr Repair repairs[] = {
        {3441, 20260522, 20260522, 15},  // Mantra of Inscriptions
        {3443, 20260608, 20260608, 15},
        {3446, 20260321, 20260401, 2},   // Resurrection Signet
        {3448, 20260429, 20260504, 2},
        {3448, 20260607, 20260607, 13},  // Mantra of Recovery
        {3449, 20260401, 20260426, 28},  // Hex Breaker
        {3450, 20260626, 20260708, 13},
        {3455, 20260712, 20260801, 28},
        {3458, 20260709, 20260802, 2},
        {3460, 20260401, 20260428, 69},  // Shatter Enchantment
        {3466, 20260709, 20260803, 69},
        {3488, 20260512, 20260512, 1},   // Healing Signet
        {3490, 20260403, 20260403, 1},
        {3492, 20260401, 20260428, 16},  // Mantra of Concentration
        {3492, 20260630, 20260630, 1},
        {3493, 20260429, 20260502, 177}, // Ward Against Foes
        {3495, 20260730, 20260730, 1},
    };
    for (const auto& repair : repairs)
        if (id == repair.id && dateKey >= repair.first && dateKey <= repair.last)
            return repair.original;
    return id;
}

// Preserve recorded ordering, collapsing a duplicate if the correct ID was
// already present. Return whether a saved template must be regenerated.
inline bool RepairHistoricalSkillBar(std::vector<int>& skills, int dateKey)
{
    bool changed = false;
    for (int& id : skills)
    {
        const int repaired = ResolveHistoricalBarSkillId(id, dateKey);
        changed |= repaired != id;
        id = repaired;
    }
    if (changed)
    {
        std::vector<int> unique;
        for (int id : skills)
            if (std::find(unique.begin(), unique.end(), id) == unique.end())
                unique.push_back(id);
        skills = std::move(unique);
    }
    return changed;
}
