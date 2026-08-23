#pragma once

// Health contributed by an equipped weapon set: the W(t) term of the max-HP model.
//
// This is the part of a player's maximum health that moves when they swap sets, and the largest
// remaining source of error once the camera has anchored a reading. Measured on the last recorded
// match: of the combat packets that no camera-measured state could explain, 31 of 35 were a weapon
// or shield mod the camera never happened to catch.
//
// Read from the effect words themselves rather than from the English in EquipmentComponents.h.
// The words carry the magnitude directly, and unlike the component identity word they are present
// on every item -- only 92 of 221 item definitions in a sampled match carried an identity word at
// all, so a table lookup keyed on it would miss more than half the equipment.
//
// Header-only and free of ImGui/D3D, matching EquipmentData.h.

#include "EquipmentData.h"

namespace Equipment
{
    // A weapon set's health mods, kept apart by the condition each one hangs on rather than
    // summed, because the conditions come and go without a weapon swap: a shield of Valor moves
    // its bearer's maximum by 60 the instant a hex lands. That is exactly the case a solver keyed
    // on the weapon set alone cannot see, and reads as noise.
    struct HealthMods
    {
        int flat = 0;
        int whenEnchanted = 0;
        int whenHexed = 0;
        int whenInStance = 0;

        // False when an item's mods never arrived. The set's contribution is then UNKNOWN rather
        // than zero, and a caller must not feed it to the model as if it were measured.
        bool known = false;

        // Weapon and offhand mods stack, so a +30 pommel with a +30 shield handle gives +60. The
        // non-stacking rule belongs to runes, which live on armour and never in these two slots.
        int At(bool enchanted, bool hexed, bool inStance) const
        {
            return flat
                 + (enchanted ? whenEnchanted : 0)
                 + (hexed ? whenHexed : 0)
                 + (inStance ? whenInStance : 0);
        }

        int Min() const { return flat; }
        int Max() const { return flat + whenEnchanted + whenHexed + whenInStance; }

        // True when this set's contribution depends on the wearer's state, so a single number
        // cannot describe it.
        bool Conditional() const { return Max() != Min(); }
    };

    // The four words that grant maximum health. Health_extra_per_piece (the Survivor insignia) is
    // deliberately absent: it belongs to armour, whose mod list the recording never carries.
    inline void AccumulateHealth(const ItemDef& item, HealthMods& out)
    {
        if (!item.resolved) return;

        for (const Mod& m : item.mods)
        {
            const int v = SingleValue(m.arg1, m.arg2);
            switch (m.id)
            {
            case ModId::Health_extra:                out.flat += v;          break;
            case ModId::Health_extra_when_enchanted: out.whenEnchanted += v; break;
            case ModId::Health_extra_when_hexed:     out.whenHexed += v;     break;
            case ModId::Health_extra_when_in_stance: out.whenInStance += v;  break;
            default: break;
            }
        }
    }

    // Health from the two hand slots as they were at one instant, joined against the ids the agent
    // snapshot already carries. An empty slot contributes nothing and is not a gap in knowledge;
    // an item whose mods never arrived is, and clears `known`.
    inline HealthMods HealthFromWeaponSet(const Data& equipment,
                                          uint16_t weaponItemId,
                                          uint16_t offhandItemId)
    {
        HealthMods out;
        if (!equipment.loaded) return out;

        out.known = true;
        for (uint16_t id : { weaponItemId, offhandItemId })
        {
            if (!id) continue;
            const ItemDef* item = equipment.FindByAgentItemId(id);
            if (!item) { out.known = false; continue; }
            if (!item->resolved) { out.known = false; continue; }
            AccumulateHealth(*item, out);
        }
        return out;
    }
}
