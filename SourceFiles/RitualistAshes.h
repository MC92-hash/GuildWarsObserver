#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Ritualist item spells ("ashes").
//
// Casting one puts a bundle in the caster's hands. Dropping it destroys the
// ashes, but the server still spawns a world item for the moment they are on
// the ground, and that item is indistinguishable from a flag or a map bundle:
// the carrier shows the same held-item marker, and the item itself gets an
// ordinary recycled id with no field saying what it is. Left alone, a
// ritualist's ashes are therefore reconstructed as a repair kit or a vine seed
// lying wherever they were dropped, for the rest of the replay — one phantom
// marker per cast, and a Kaolai monk casts a few dozen times a match.
//
// What identifies them is the cast: ashes only ever enter a player's hands
// through one of the skills below, never through a pickup. Cast time cannot be
// used as the correlation window on its own — item spells run up to six seconds
// and other skills lengthen them — so the hold is read from the carrier's own
// snapshots and only its *start* is matched back to a cast.
//
// The urn model is shared across each family; the game does not model the
// individual ashes separately.
// ---------------------------------------------------------------------------

enum class AshesKind : uint8_t { Offensive, Defensive, Resurrect };

struct AshesSkill
{
    int         skillId;
    AshesKind   kind;
    const char* droppedName;   // shown while the urn is on the ground
};

inline uint32_t AshesModelFileId(AshesKind kind)
{
    switch (kind) {
    case AshesKind::Offensive: return 175522;
    case AshesKind::Defensive: return 175519;
    case AshesKind::Resurrect: return 175523;
    }
    return 0;
}

// Every urn model carries a submesh that is not part of the urn and must not be
// drawn. It is a different one for the resurrection urn.
inline int AshesHiddenSubmesh(AshesKind kind)
{
    return (kind == AshesKind::Resurrect) ? 2 : 1;
}

// Names drop the "Was" the skill titles carry: "Cruel Was Daoshen" is shown as
// "Ashes of Cruel Daoshen" once the urn is on the ground.
inline const AshesSkill* LookupAshesSkill(int skillId)
{
    static constexpr AshesSkill table[] = {
        // --- Offensive ---
        { 1223, AshesKind::Offensive, "Ashes of Anguished Lingwah" },
        {  788, AshesKind::Offensive, "Ashes of Blind Mingson"     },
        { 1218, AshesKind::Offensive, "Ashes of Cruel Daoshen"     },
        {  812, AshesKind::Offensive, "Ashes of Defiant Xinrae"    },
        { 1732, AshesKind::Offensive, "Ashes of Destructive Glaive" },
        { 3157, AshesKind::Offensive, "Ashes of Destructive Glaive" },  // PvP split
        {  789, AshesKind::Offensive, "Ashes of Grasping Kuurong"  },
        {  790, AshesKind::Offensive, "Ashes of Vengeful Khanhei"  },

        // --- Defensive ---
        { 1220, AshesKind::Defensive, "Ashes of Attuned Songkai"   },
        { 2016, AshesKind::Defensive, "Ashes of Energetic Lee Sa"  },
        {  772, AshesKind::Defensive, "Ashes of Generous Tsungrai" },
        {  773, AshesKind::Defensive, "Ashes of Mighty Vorizun"    },
        { 1219, AshesKind::Defensive, "Ashes of Protective Kaolai" },
        { 2072, AshesKind::Defensive, "Ashes of Pure Li Ming"      },
        { 1221, AshesKind::Defensive, "Ashes of Resilient Xiko"    },
        {  913, AshesKind::Defensive, "Ashes of Tranquil Tanasen"  },
        { 1731, AshesKind::Defensive, "Ashes of Vocal Sogolon"     },

        // --- Resurrection ---
        { 1222, AshesKind::Resurrect, "Ashes of Lively Naomei"     },
    };

    for (const auto& e : table)
        if (e.skillId == skillId) return &e;
    return nullptr;
}

// The held-item marker a carrier shows in weapon_item_type while holding any
// bundle at all — ashes, a flag, a repair kit or a vine seed.
inline constexpr uint8_t kBundleHeldItemType = 46;
