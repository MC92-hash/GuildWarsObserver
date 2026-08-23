#pragma once

// Equipment recorded by the Toolbox GW Observer Recorder (StoC/equipment_events.txt).
//
// The recorder stores each item's mod words *raw* rather than as decoded text, so the decoding
// below can be corrected without re-recording anything: improving this file fixes every replay
// that already exists. See GWToolboxpp/docs/ITEM_MOD_IDENTIFIERS.md for where the identifier
// table came from (the game client's own PvP upgrade table) and EXPORTS_CONVENTIONS.md for the
// file format.
//
// Header-only and free of ImGui/D3D, matching Parsers/ and Cache/ — colours come out as plain
// RGB so the renderer decides how to use them.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "EquipmentComponents.h"

namespace Equipment
{
    // Slot indices are fixed by GW::NPCEquipment::items[].
    enum Slot : uint8_t
    {
        Slot_Weapon = 0, Slot_Offhand, Slot_Chest, Slot_Legs,
        Slot_Head, Slot_Feet, Slot_Hands, Slot_CostumeBody, Slot_CostumeHead,
        Slot_Count
    };

    struct Mod
    {
        uint32_t raw  = 0;
        uint16_t id   = 0;   // raw >> 16
        uint8_t  arg1 = 0;   // (raw >> 8) & 0xFF
        uint8_t  arg2 = 0;   // raw & 0xFF
    };

    struct ItemDef
    {
        uint32_t ref          = 0;
        uint32_t itemId       = 0;
        uint16_t agentItemId  = 0;  // matches agent snapshot fields 28/29
        uint32_t modelFileId  = 0;  // the skin; resolves against Gw.dat
        // Gw.dat file holding the inventory icon. Same as modelFileId for a weapon, but armour is
        // a composite item whose icon can only be resolved inside the game client, so the recorder
        // does it and writes the answer here. 0 in recordings made before that change.
        uint32_t iconFileId   = 0;
        uint32_t itemType     = 0;
        uint32_t interaction  = 0;
        uint8_t  dyes[4]      = {};
        uint8_t  dyeTint      = 0;
        uint32_t modelId      = 0;
        bool     resolved     = false; // false => mods are UNKNOWN, not absent
        std::vector<Mod> mods;
        std::string name;
        float    firstSeen    = 0.f;
    };

    // ref < 0 means the slot was emptied.
    struct SlotEvent
    {
        float   time    = 0.f;
        int     agentId = 0;
        uint8_t slot    = 0;
        int     ref     = -1;
    };

    struct Data
    {
        std::vector<ItemDef>  items;   // index == ref
        std::vector<SlotEvent> events; // time-ordered
        // Join key for the weapon ids already present in the agent snapshot stream, which are
        // 16-bit where the equipment record's item id is 32-bit.
        std::unordered_map<uint16_t, uint32_t> byAgentItemId;
        bool loaded = false;

        const ItemDef* Find(int ref) const
        {
            return (ref >= 0 && ref < static_cast<int>(items.size())) ? &items[ref] : nullptr;
        }

        const ItemDef* FindByAgentItemId(uint16_t id) const
        {
            if (!id) return nullptr;
            auto it = byAgentItemId.find(id);
            return it != byAgentItemId.end() ? Find(static_cast<int>(it->second)) : nullptr;
        }

        // Item in a slot at time t, for agents whose armour is only in the event stream.
        const ItemDef* FindAtTime(int agentId, uint8_t slot, float t) const
        {
            int best = -1;
            for (const auto& e : events)
            {
                if (e.time > t) break;
                if (e.agentId == agentId && e.slot == slot) best = e.ref;
            }
            return Find(best);
        }
    };

    // ─── Rarity ──────────────────────────────────────────────────────────────────────────
    // Derived from the raw interaction flags, mirroring ToolboxUtils::Items::GetRarity.
    //
    // White and Blue are NOT separable here: the client tells them apart via the item's
    // single_item_name colour token, which is not in the recording. Blue weapons are vanishingly
    // rare in GvG, so everything unflagged reports White. Adding an explicit rarity byte to
    // ITEM_DEF would resolve it if it ever matters.

    enum class Rarity : uint8_t { White, Blue, Purple, Gold, Green, Special };

    struct Rgb { uint8_t r, g, b; };

    // Bits shared by sampled PvP items and absent from a real green. Tested as a conjunction:
    // two of them mean something else on their own, so only the full set is specific enough.
    constexpr uint32_t kPvpItemFlags = 0x1C101100;

    inline bool LooksLikePvPItem(uint32_t interaction)
    {
        return (interaction & kPvpItemFlags) == kPvpItemFlags;
    }

    inline bool IsInscribable(uint32_t interaction) { return (interaction & 0x08000000) != 0; }
    // A *free* slot is one the item does not already have an upgrade in.
    inline bool PrefixSlotFree(uint32_t interaction) { return ((interaction >> 14) & 1) == 0; }
    inline bool SuffixSlotFree(uint32_t interaction) { return ((interaction >> 15) & 1) == 0; }

    inline Rarity RarityFromInteraction(uint32_t interaction)
    {
        // A PvP tournament weapon carries the same unique-item bits as a real green, so the
        // green test alone cannot separate them — only the PvP flags can.
        if (interaction & 0x10)
            return LooksLikePvPItem(interaction) ? Rarity::Special : Rarity::Green;
        if (interaction & 0x400000) return Rarity::Purple;
        if (interaction & 0x20000)  return Rarity::Gold;
        // Nothing flagged is White or Blue, and the recording cannot tell them apart. Nobody
        // brings a white weapon to a GvG, so report Blue.
        return Rarity::Blue;
    }

    inline Rgb RarityColor(Rarity r)
    {
        switch (r)
        {
            case Rarity::Gold:    return {255, 204,  85};
            case Rarity::Green:   return {  0, 255,   0};
            case Rarity::Blue:    return {127, 207, 207};
            case Rarity::Purple:  return {153, 119, 187};
            case Rarity::Special: return {223,  33,  33};
            default:              return {255, 255, 255};
        }
    }

    constexpr Rgb kStatColor  = {255, 255, 255}; // damage line
    constexpr Rgb kMutedColor = {124, 126, 126}; // conditions, requirements, empty slots, armour

    // An item's upgrade lines read in its own rarity colour, the same one its name uses. The
    // headline stat (damage, armor, energy) and the customisation bonus stay white, and every
    // bracketed qualifier stays muted, so those are set by the renderer rather than here.
    inline Rgb ModTextColor(Rarity r) { return RarityColor(r); }

    // ─── Mod identifiers ─────────────────────────────────────────────────────────────────
    // Ported from GWToolboxpp/GWToolboxdll/Constants/ModIdentifier.h. A mod word is packed
    // [identifier:16][arg1:8][arg2:8].

    namespace ModId
    {
        constexpr uint16_t Armor_minus                        = 0x2018;
        constexpr uint16_t Damage_reduction_chance            = 0x2078;
        constexpr uint16_t Damage_reduction_when_enchanted    = 0x2088;
        constexpr uint16_t Damage_reduction_when_hexed        = 0x2098;
        constexpr uint16_t Damage_reduction_when_in_stance    = 0x20A8;
        constexpr uint16_t Energy_minus                       = 0x20B8;
        constexpr uint16_t Energy_degen                       = 0x20C8;
        constexpr uint16_t Health_minus_rune                  = 0x20D8;
        constexpr uint16_t Life_degen                         = 0x20E8;
        constexpr uint16_t Armor_plus                         = 0x2108;
        constexpr uint16_t Armor_elemental_plus               = 0x2128;
        constexpr uint16_t Armor_physical_plus                = 0x2158;
        constexpr uint16_t Armor_when_attacking               = 0x2178;
        constexpr uint16_t Armor_when_casting                 = 0x2188;
        constexpr uint16_t Armor_when_enchanted               = 0x2198;
        constexpr uint16_t Armor_when_over_hp                 = 0x21A8;
        constexpr uint16_t Armor_when_under_hp                = 0x21B8;
        constexpr uint16_t Armor_when_hexed                   = 0x21C8;
        constexpr uint16_t Attribute_plus_rune                = 0x21E8;
        constexpr uint16_t Halves_casting_chance_of_spells    = 0x2208;
        constexpr uint16_t Halves_casting_chance_of_attribute = 0x2218;
        constexpr uint16_t Damage_plus_unconditional          = 0x2238;
        constexpr uint16_t Damage_vs_hexed_foes               = 0x2258;
        constexpr uint16_t Damage_when_enchanted              = 0x2268;
        constexpr uint16_t Damage_extra_over_50               = 0x2278;
        constexpr uint16_t Damage_when_under_hp               = 0x2288;
        constexpr uint16_t Damage_when_hexed                  = 0x2298;
        constexpr uint16_t Damage_when_in_stance              = 0x22A8;
        constexpr uint16_t Enchanting                         = 0x22B8;
        constexpr uint16_t Energy_plus                        = 0x22D8;
        constexpr uint16_t Energy_extra_when_enchanted        = 0x22F8;
        constexpr uint16_t Energy_extra_when_over_hp          = 0x2308;
        constexpr uint16_t Energy_extra_when_under_hp         = 0x2318;
        constexpr uint16_t Energy_extra_when_hexed            = 0x2328;
        constexpr uint16_t Health_extra                       = 0x2348;
        constexpr uint16_t Health_extra_when_enchanted        = 0x2368;
        constexpr uint16_t Health_extra_when_hexed            = 0x2378;
        constexpr uint16_t Health_extra_when_in_stance        = 0x2388;
        constexpr uint16_t Halves_recharge_chance_of_attribute= 0x2398;
        constexpr uint16_t Halves_recharge_chance_of_spells   = 0x23A8;
        constexpr uint16_t Adrenaline_double_chance           = 0x23B8;
        constexpr uint16_t Armor_penetration_chance           = 0x23F8;
        constexpr uint16_t Component_identity                 = 0x2408;
        constexpr uint16_t Attribute_plus_chance              = 0x2418;
        constexpr uint16_t Condition_duration_increase        = 0x2468;
        constexpr uint16_t Damage_type                        = 0x24B8;
        constexpr uint16_t Energy_gain_on_hit                 = 0x2518;
        constexpr uint16_t Lifesteal                          = 0x2528;
        constexpr uint16_t Effect_marker                      = 0x2530;
        constexpr uint16_t Effect_marker_alt                  = 0x2532;
        constexpr uint16_t Energy_extra_per_piece             = 0x26C8;
        constexpr uint16_t Health_extra_per_piece             = 0x26D8;
        constexpr uint16_t Condition_duration_reduction_rune  = 0x2778;
        constexpr uint16_t Requirement                        = 0x2798;
        constexpr uint16_t Rune_tiered_minor                  = 0x27E8;
        constexpr uint16_t Rune_tiered_major                  = 0x27E9;
        constexpr uint16_t Rune_tiered_superior               = 0x27EA;
        constexpr uint16_t Halves_casting_chance_of_req       = 0x2808;
        constexpr uint16_t Halves_recharge_chance_of_req      = 0x2828;
        constexpr uint16_t Item_attribute_plus_chance         = 0x2838;
        constexpr uint16_t Condition_duration_reduction       = 0x2858;
        constexpr uint16_t Holy_damage_taken_extra            = 0x2868;
        constexpr uint16_t Base_Energy_extra                  = 0x62C8;
        constexpr uint16_t Energy_extra_from_requirement      = 0x67C8;

        constexpr uint16_t Condition_requires_attribute       = 0x8010;
        constexpr uint16_t Condition_marker                   = 0x8070;
        constexpr uint16_t Condition_while_attacking          = 0x8090;
        constexpr uint16_t Condition_while_activating_skills  = 0x80A0;
        constexpr uint16_t Condition_while_affected_by        = 0x80B0;
        constexpr uint16_t Condition_while_holding_item       = 0x80C0;
        constexpr uint16_t Condition_while_using_preparation  = 0x80D0;
        constexpr uint16_t Condition_while_in_stance          = 0x8110;
        constexpr uint16_t Condition_while_pet_alive          = 0x8120;
        constexpr uint16_t Condition_while_shout_or_chant     = 0x8130;
        constexpr uint16_t Condition_while_not_enchanted      = 0x8140;

        constexpr uint16_t Insignia_armor_conditional         = 0xA0F8;
        constexpr uint16_t Insignia_armor_conditional_signed  = 0xA0FB;
        constexpr uint16_t Armor_plus_vs_type                 = 0xA118;
        constexpr uint16_t Armor_elemental_plus_armor         = 0xA128;
        constexpr uint16_t Armor_physical_plus_armor          = 0xA158;
        constexpr uint16_t Armor_base                         = 0xA3C8;
        constexpr uint16_t Customized_damage_modifier         = 0xA498;
        constexpr uint16_t Effect_marker_armor                = 0xA530;
        constexpr uint16_t Effect_marker_armor_alt            = 0xA532;
        constexpr uint16_t Insignia_armor_per_minion          = 0xA6E8;
        constexpr uint16_t Insignia_armor_per_spirit          = 0xA6F8;
        constexpr uint16_t Insignia_armor_with_requirement    = 0xA708;
        constexpr uint16_t Insignia_armor_per_enchantment     = 0xA718;
        constexpr uint16_t Insignia_armor_while_recharging    = 0xA728;
        constexpr uint16_t Insignia_armor_when_under_hp       = 0xA738;
        constexpr uint16_t Insignia_armor_per_signet          = 0xA748;
        constexpr uint16_t Weapon_damage                      = 0xA7A8;
        constexpr uint16_t Armor_extra_from_requirement       = 0xA7B8;
        constexpr uint16_t Damage_reduction_flat              = 0xA7F8;
    }

    // ─── Lookup tables ───────────────────────────────────────────────────────────────────

    inline const char* AttributeName(uint32_t id)
    {
        // Indices are GW::Constants::Attribute values; 26-28 are unused in that enum.
        static const char* const names[] = {
            "Fast Casting", "Illusion Magic", "Domination Magic", "Inspiration Magic",
            "Blood Magic", "Death Magic", "Soul Reaping", "Curses",
            "Air Magic", "Earth Magic", "Fire Magic", "Water Magic", "Energy Storage",
            "Healing Prayers", "Smiting Prayers", "Protection Prayers", "Divine Favor",
            "Strength", "Axe Mastery", "Hammer Mastery", "Swordsmanship", "Tactics",
            "Beast Mastery", "Expertise", "Wilderness Survival", "Marksmanship",
            nullptr, nullptr, nullptr,
            "Dagger Mastery", "Deadly Arts", "Shadow Arts",
            "Communing", "Restoration Magic", "Channeling Magic",
            "Critical Strikes", "Spawning Power",
            "Spear Mastery", "Command", "Motivation", "Leadership",
            "Scythe Mastery", "Wind Prayers", "Earth Prayers", "Mysticism"
        };
        return id < std::size(names) ? names[id] : nullptr;
    }

    inline const char* DamageTypeName(uint32_t t)
    {
        // Sparse: 9 and 10 are unassigned between holy (8) and earth (11).
        static const char* const names[] = {
            "Blunt", "Piercing", "Slashing", "Cold", "Lightning",
            "Fire", "Chaos", "Dark", "Holy", nullptr, nullptr, "Earth"
        };
        return t < std::size(names) ? names[t] : nullptr;
    }

    inline std::string AttributeLabel(uint8_t id)
    {
        const char* n = AttributeName(id);
        return n ? n : std::format("attribute {}", id);
    }

    inline std::string DamageTypeLabel(uint8_t t)
    {
        const char* n = DamageTypeName(t);
        return n ? n : std::format("damage type {}", t);
    }

    // Numbering used by the -20% reduction mods. Burning is the only condition with no
    // reduction component, which is what leaves the gap at 2.
    inline const char* ConditionName(uint8_t c)
    {
        switch (c)
        {
            case 0: return "Bleeding";   case 1: return "Blind";
            case 2: return "Burning";    case 3: return "Crippled";
            case 4: return "Deep Wound"; case 5: return "Disease";
            case 6: return "Poison";     case 7: return "Dazed";
            case 8: return "Weakness";   default: return nullptr;
        }
    }

    // The +33% prefixes reference conditions by their skill id instead.
    inline const char* ConditionSkillName(uint8_t c)
    {
        switch (c)
        {
            case 222: return "Bleeding";  case 225: return "Crippled";
            case 226: return "Deep Wound"; case 228: return "Poison";
            case 229: return "Dazed";     case 230: return "Weakness";
            default: return nullptr;
        }
    }

    inline const char* RuneEffectName(uint8_t e)
    {
        switch (e)
        {
            case 194: return "Health";
            case 234: return "physical damage reduction";
            default:  return nullptr;
        }
    }

    inline const char* AffectedByName(uint8_t e)
    {
        switch (e)
        {
            case 4:  return "a hex";        case 6:  return "an enchantment";
            case 8:  return "a condition";  case 25: return "a weapon spell";
            default: return nullptr;
        }
    }

    inline std::string ConditionLabel(uint8_t c)
    {
        const char* n = ConditionName(c);
        return n ? n : std::format("condition {}", c);
    }

    // ─── Mod decoding ────────────────────────────────────────────────────────────────────

    struct ModText
    {
        std::string effect;      // the gold half: "Damage +15%"
        std::string condition;   // the grey half: "while Health is above 50%" (often empty)
        bool structural = false; // bookkeeping word, not an effect — hide by default
        bool known      = true;  // identifier is in the table above
    };

    // Single-magnitude mods disagree about which byte carries the value, and only ever populate
    // one of the two, so taking whichever is non-zero is correct for both layouts.
    inline uint8_t SingleValue(uint8_t a1, uint8_t a2) { return a1 ? a1 : a2; }

    // "Health +30" / "Health -20" -- a maximum-health line with a magnitude in it. Deliberately
    // strict: "Health regeneration -1" and "Armor +5 (while Health is below 50%)" both mention
    // health and neither is one of these.
    inline bool IsHealthMagnitudeLine(std::string_view line)
    {
        constexpr std::string_view kPrefix = "Health ";
        if (!line.starts_with(kPrefix)) return false;
        size_t i = kPrefix.size();
        if (i >= line.size() || (line[i] != '+' && line[i] != '-')) return false;
        return i + 1 < line.size() && line[i + 1] >= '0' && line[i + 1] <= '9';
    }

    // Replaces the magnitude in such a line with the value the item actually rolled, keeping
    // whatever follows it. No-op on anything that is not a health magnitude line.
    inline void RewriteHealthMagnitude(std::string& effect, int rolled)
    {
        if (!IsHealthMagnitudeLine(effect)) return;
        constexpr size_t kNumberStart = 7; // strlen("Health ")
        size_t end = kNumberStart + 1;
        while (end < effect.size() && effect[end] >= '0' && effect[end] <= '9') end++;
        const char sign = effect[kNumberStart];
        effect = std::format("Health {}{}{}", sign, rolled, effect.substr(end));
    }

    inline ModText DecodeMod(uint16_t id, uint8_t arg1, uint8_t arg2)
    {
        const uint8_t v = SingleValue(arg1, arg2);
        // Chance percentages are taken as written. Upgrade *components* store half the figure
        // the game prints (a 10% haft reads 5), but every component resolves through the
        // component table and never reaches this function - what does reach it is an item's
        // inherent words, and those hold the real percentage.
        const int chance = v;

        auto plain  = [](std::string e) { return ModText{std::move(e), {}, false, true}; };
        // Qualifiers are bracketed here so every caller renders them the same way the game does.
        auto cond   = [](std::string e, const std::string& c) { return ModText{std::move(e), "(" + c + ")", false, true}; };
        auto marker = [](std::string e) { return ModText{std::move(e), {}, true, true}; };

        switch (id)
        {
            case ModId::Armor_minus:        return plain(std::format("Armor -{}", v));
            case ModId::Energy_minus:       return plain(std::format("Energy -{}", v));
            case ModId::Energy_degen:       return plain(std::format("Energy regeneration -{}", v));
            case ModId::Health_minus_rune:  return plain(std::format("Health -{}", v));
            case ModId::Life_degen:         return plain(std::format("Health regeneration -{}", v));
            case ModId::Armor_plus:         return plain(std::format("Armor +{}", v));

            case ModId::Armor_elemental_plus:
            case ModId::Armor_elemental_plus_armor:
                return cond(std::format("Armor +{}", v), "vs. elemental damage");
            case ModId::Armor_physical_plus:
            case ModId::Armor_physical_plus_armor:
                return cond(std::format("Armor +{}", v), "vs. physical damage");
            case ModId::Armor_when_attacking:
                return cond(std::format("Armor +{}", v), "while attacking");
            case ModId::Armor_when_casting:
                return cond(std::format("Armor +{}", v), "while casting");
            case ModId::Armor_when_enchanted:
                return cond(std::format("Armor +{}", v), "while enchanted");
            case ModId::Armor_when_over_hp:
                return cond(std::format("Armor +{}", arg2), std::format("while Health is above {}%", arg1));
            case ModId::Armor_when_under_hp:
                return cond(std::format("Armor +{}", arg2), std::format("while Health is below {}%", arg1));
            case ModId::Armor_when_hexed:
                return cond(std::format("Armor +{}", v), "while hexed");
            case ModId::Armor_plus_vs_type:
                return cond(std::format("Armor +{}", arg2), std::format("vs. {} damage", DamageTypeLabel(arg1)));
            case ModId::Armor_extra_from_requirement:
                return cond(std::format("Armor +{}", arg1), std::format("requirement met; +{} otherwise", arg2));
            case ModId::Armor_base:
                return plain(std::format("Armor: {}", v));

            case ModId::Damage_plus_unconditional:
                return plain(std::format("Damage +{}%", v));
            case ModId::Damage_extra_over_50:
                return cond(std::format("Damage +{}%", arg2), std::format("while Health is above {}%", arg1));
            case ModId::Damage_when_under_hp:
                return cond(std::format("Damage +{}%", arg2), std::format("while Health is below {}%", arg1));
            case ModId::Damage_vs_hexed_foes:
                return cond(std::format("Damage +{}%", arg2), "vs. hexed foes");
            case ModId::Damage_when_enchanted:
                return cond(std::format("Damage +{}%", arg2), "while enchanted");
            case ModId::Damage_when_hexed:
                return cond(std::format("Damage +{}%", arg2), "while hexed");
            case ModId::Damage_when_in_stance:
                return cond(std::format("Damage +{}%", arg2), "while in a stance");

            case ModId::Enchanting:
                return plain(std::format("Enchantments last {}% longer", v));
            case ModId::Energy_plus:
                return plain(std::format("Energy +{}", v));
            case ModId::Energy_extra_when_enchanted:
                return cond(std::format("Energy +{}", v), "while enchanted");
            case ModId::Energy_extra_when_over_hp:
                return cond(std::format("Energy +{}", arg2), std::format("while Health is above {}%", arg1));
            case ModId::Energy_extra_when_under_hp:
                return cond(std::format("Energy +{}", arg2), std::format("while Health is below {}%", arg1));
            case ModId::Energy_extra_when_hexed:
                return cond(std::format("Energy +{}", v), "while hexed");
            case ModId::Base_Energy_extra:
                return cond(std::format("Energy +{}", v), "base");
            case ModId::Energy_extra_from_requirement:
                return cond(std::format("Energy +{}", arg1), std::format("requirement met; +{} otherwise", arg2));
            case ModId::Energy_gain_on_hit:
                return plain(std::format("Energy gain on hit: {}", v));
            case ModId::Energy_extra_per_piece:
                return cond(std::format("Energy +{}", v), "scaled by armor piece");

            case ModId::Health_extra:
                return plain(std::format("Health +{}", v));
            case ModId::Health_extra_when_enchanted:
                return cond(std::format("Health +{}", v), "while enchanted");
            case ModId::Health_extra_when_in_stance:
                return cond(std::format("Health +{}", v), "while in a stance");
            case ModId::Health_extra_when_hexed:
                return cond(std::format("Health +{}", v), "while hexed");
            case ModId::Health_extra_per_piece:
                return cond(std::format("Health +{}", v), "scaled by armor piece");

            case ModId::Lifesteal:
                return plain(std::format("Life Draining: {}", v));
            case ModId::Adrenaline_double_chance:
                return cond("Double adrenaline gain", std::format("Chance: {}%", chance));
            case ModId::Armor_penetration_chance:
                return cond(std::format("Armor penetration +{}%", arg2), std::format("Chance: {}%", arg1));
            case ModId::Damage_type:
                return plain(std::format("Damage type: {}", DamageTypeLabel(v)));
            case ModId::Requirement:
                return plain(std::format("Requires {} {}", arg2, AttributeLabel(arg1)));
            case ModId::Weapon_damage:
                // The args are not consistently ordered low-to-high, so sort rather than
                // trusting arg1 to be the minimum.
                return plain(std::format("Damage: {}-{}", std::min(arg1, arg2), std::max(arg1, arg2)));
            case ModId::Customized_damage_modifier:
            {
                // Stored as a percentage *of* base damage: a customised weapon reads 120,
                // meaning 120% -> +20%.
                return plain(v >= 100 ? std::format("Damage +{}%", v - 100)
                                      : std::format("Damage {}%", v));
            }

            // Wording follows the game's own component descriptions.
            case ModId::Halves_casting_chance_of_spells:
                return cond("Halves casting time of spells", std::format("Chance: {}%", chance));
            case ModId::Halves_recharge_chance_of_spells:
                return cond("Halves skill recharge of spells", std::format("Chance: {}%", chance));
            case ModId::Halves_casting_chance_of_req:
                return cond("Halves casting time on spells of item's attribute",
                            std::format("Chance: {}%", chance));
            case ModId::Halves_recharge_chance_of_req:
                return cond("Halves skill recharge on spells of item's attribute",
                            std::format("Chance: {}%", chance));
            case ModId::Halves_casting_chance_of_attribute:
                return cond(std::format("Halves casting time of {} spells", AttributeLabel(arg2)),
                            std::format("Chance: {}%", arg1));
            case ModId::Halves_recharge_chance_of_attribute:
                return cond(std::format("Halves skill recharge of {} spells", AttributeLabel(arg2)),
                            std::format("Chance: {}%", arg1));

            case ModId::Attribute_plus_rune:
                return plain(std::format("{} +{}", AttributeLabel(arg1), arg2));
            case ModId::Attribute_plus_chance:
                return cond(std::format("{} +1", AttributeLabel(arg1)), "20% chance while using skills");
            case ModId::Item_attribute_plus_chance:
                return cond("Item's attribute +1", std::format("Chance: {}%", chance));

            case ModId::Condition_duration_increase:
            {
                const char* c = ConditionSkillName(arg2);
                return cond(std::format("Lengthens {} duration by 33%", c ? c : "condition"), "on foes");
            }
            case ModId::Condition_duration_reduction:
                return cond(std::format("Reduces {} duration by 20%", ConditionLabel(arg1)), "on you");
            case ModId::Condition_duration_reduction_rune:
                return cond(std::format("Reduces {} and {} duration by 20%",
                                        ConditionLabel(arg1), ConditionLabel(arg2)), "on you");

            case ModId::Rune_tiered_minor:
            case ModId::Rune_tiered_major:
            case ModId::Rune_tiered_superior:
            {
                const char* tier = id == ModId::Rune_tiered_minor ? "Minor"
                                 : id == ModId::Rune_tiered_major ? "Major" : "Superior";
                const char* eff = RuneEffectName(arg2);
                return eff ? plain(std::format("{} rune: {}", tier, eff))
                           : plain(std::format("{} rune effect {}", tier, arg2));
            }

            case ModId::Damage_reduction_chance:
                return cond(std::format("Received physical damage -{}", arg2),
                            std::format("Chance: {}%", arg1));
            case ModId::Damage_reduction_when_enchanted:
                return cond(std::format("Received physical damage -{}", v), "while enchanted");
            case ModId::Damage_reduction_when_hexed:
                return cond(std::format("Received physical damage -{}", v), "while hexed");
            case ModId::Damage_reduction_when_in_stance:
                return cond(std::format("Received physical damage -{}", v), "while in a stance");
            case ModId::Damage_reduction_flat:
                return plain(std::format("Received physical damage -{}", v));

            case ModId::Insignia_armor_per_signet:
                return cond(std::format("Armor +{}", v), "for each equipped signet");
            case ModId::Insignia_armor_while_recharging:
                return cond(std::format("Armor +{}", v), "while recharging skills");
            case ModId::Insignia_armor_when_under_hp:
                return cond(std::format("Armor +{}", v), "while health is low");
            case ModId::Insignia_armor_with_requirement:
                return cond(std::format("Armor +{}", arg1), std::format("requires {} in the attribute", arg2));
            case ModId::Insignia_armor_per_enchantment:
                return cond(std::format("Armor +{}", arg1), "per enchantment");
            case ModId::Insignia_armor_per_minion:
                return cond(std::format("Armor +{}", v), "per minion controlled");
            case ModId::Insignia_armor_per_spirit:
                return cond(std::format("Armor +{}", v), "per spirit controlled");
            case ModId::Insignia_armor_conditional:
                return cond(std::format("Armor +{}", v), "conditional");
            case ModId::Insignia_armor_conditional_signed:
                // Stored two's complement, so 236 is -20.
                return cond(std::format("Armor {}", static_cast<int>(arg1) - 256), "conditional");
            case ModId::Holy_damage_taken_extra:
                return cond(std::format("Holy damage received +{}", arg2), "scaled by armor piece");

            // ── Structural: bookkeeping every upgrade component carries ──────────────────
            case ModId::Component_identity:
                return marker(std::format("[component {}]", (static_cast<uint32_t>(arg1) << 8) | arg2));
            case ModId::Effect_marker:
            case ModId::Effect_marker_alt:
            case ModId::Effect_marker_armor:
            case ModId::Effect_marker_armor_alt:
                return marker("[effect marker]");

            // A 0x80xx word states the condition under which the *following* armor mod applies.
            case ModId::Condition_marker:
                return marker("[condition marker]");
            case ModId::Condition_requires_attribute:
                return marker(std::format("[requires {} {}]", arg2, AttributeLabel(arg1)));
            case ModId::Condition_while_attacking:
                return marker("[while attacking]");
            case ModId::Condition_while_activating_skills:
                return marker("[while activating skills]");
            case ModId::Condition_while_affected_by:
            {
                const char* e = AffectedByName(arg1);
                return marker(e ? std::format("[while affected by {}]", e)
                                : std::format("[while affected by effect {}]", arg1));
            }
            case ModId::Condition_while_holding_item:
                return marker("[while holding an item]");
            case ModId::Condition_while_using_preparation:
                return marker("[while using a preparation]");
            case ModId::Condition_while_in_stance:
                return marker("[while in a stance]");
            case ModId::Condition_while_pet_alive:
                return marker("[while your pet is alive]");
            case ModId::Condition_while_shout_or_chant:
                return marker("[while affected by a shout, echo or chant]");
            case ModId::Condition_while_not_enchanted:
                return marker("[while not enchanted]");

            default:
                return ModText{std::format("Unknown mod 0x{:04X}", id),
                               std::format("arg1={} arg2={}", arg1, arg2), false, false};
        }
    }

    // ─── Tooltip model ───────────────────────────────────────────────────────────────────

    struct TooltipLine
    {
        enum class Kind : uint8_t
        {
            Name,   // rarity-coloured, larger
            Stat,   // white damage/armour line, grey requirement in `condition`
            Mod,    // gold effect, grey `condition`
            Muted,  // entirely grey (customized, empty slots, warnings)
        };
        Kind        kind = Kind::Mod;
        std::string text;
        std::string condition;
    };

    struct Tooltip
    {
        std::string  name;
        Rarity       rarity   = Rarity::White;
        bool         resolved = true;
        std::vector<TooltipLine> lines;
    };

    // How many prefix/suffix slots the item type has. Inscriptions sit in their own slot and
    // are not counted here. Values are GW::Constants::ItemType.
    inline int UpgradeSlotCount(uint32_t itemType)
    {
        switch (itemType)
        {
            case 2:  case 5:  case 15: case 26:  // Axe, Bow, Hammer, Staff
            case 27: case 32: case 35: case 36:  // Sword, Daggers, Scythe, Spear
                return 2;
            case 24:                             // Shield
                return 1;
            // Wands and focus items are left at 0: their upgrade does not register as a
            // component here, so counting one would report an empty slot on every one of them.
            default:
                return 0;
        }
    }

    // Bookkeeping words. 0x8xxx states the condition under which the *following* armor word
    // applies; the component's own description already spells that out, so it is not shown.
    inline bool IsStructuralWord(uint16_t id)
    {
        return id == ModId::Component_identity
            || id == ModId::Effect_marker      || id == ModId::Effect_marker_alt
            || id == ModId::Effect_marker_armor || id == ModId::Effect_marker_armor_alt
            || (id >= 0x8000 && id < 0x9000);
    }

    // Words that belong on the header lines rather than in the upgrade list. These are inherent
    // to the item wherever they sit in the array, so they are recognised by id rather than by
    // position - on a staff the energy pair trails the upgrade components.
    inline bool IsHeaderWord(uint16_t id)
    {
        return id == ModId::Weapon_damage  || id == ModId::Damage_type
            || id == ModId::Armor_base     || id == ModId::Requirement
            || id == ModId::Customized_damage_modifier
            || id == ModId::Base_Energy_extra
            || id == ModId::Energy_extra_from_requirement
            || id == ModId::Armor_extra_from_requirement;
    }

    // "Armor +7 (vs. elemental damage)" -> "Armor +7" + "(vs. elemental damage)".
    // The brackets stay on the qualifier, matching how the game writes it.
    inline void SplitCondition(const std::string& text, std::string& effect, std::string& condition)
    {
        const size_t open = text.rfind(" (");
        if (open != std::string::npos && !text.empty() && text.back() == ')')
        {
            effect    = text.substr(0, open);
            condition = text.substr(open + 1);
            return;
        }
        effect = text;
        condition.clear();
    }

    // Wintergreen weapons carry fixed stats that their mod words do not describe, so they are
    // recognised by name instead. https://wiki.guildwars.com/wiki/Category:Wintergreen_weapons
    struct WintergreenStats
    {
        int min = -1, max = -1;   // damage range
        int damageType = -1;      // 0 blunt, 1 piercing, 2 slashing, 3 cold
        int armor = -1;           // shield only
    };

    inline bool WintergreenOverride(const std::string& name, WintergreenStats& out)
    {
        if (name.find("Wintergreen") == std::string::npos) return false;

        struct Row { const char* kind; WintergreenStats stats; };
        static const Row rows[] = {
            {"Daggers", {  5,  5, 1, -1}},
            {"Hammer",  { 15, 15, 0, -1}},
            {"Bow",     { 15, 15, 1, -1}},
            {"Axe",     { 10, 10, 2, -1}},
            {"Scythe",  { 10, 10, 2, -1}},
            {"Sword",   { 10, 10, 2, -1}},
            {"Spear",   { 10, 10, 1, -1}},
            {"Staff",   { 10, 10, 3, -1}},
            {"Wand",    { 10, 10, 3, -1}},
            {"Shield",  { -1, -1, -1,  8}},
        };
        for (const auto& row : rows)
        {
            if (name.find(row.kind) != std::string::npos) { out = row.stats; return true; }
        }
        return false;
    }

    // One upgrade component's worth of the mod array: the identity word, its markers and the
    // effect words between it and the next identity word.
    struct ModGroup
    {
        uint16_t             componentId = 0;
        const ComponentInfo* info        = nullptr;
        ComponentClass       cls         = ComponentClass::Upgrade;
        std::vector<Mod>     effects;
    };

    // Turns one item into the lines a tooltip should show, in the order the game itself uses:
    // name, damage/armour with its requirement, inherent effects, inscription, then the
    // prefix/suffix upgrades, then the customisation bonus.
    //
    // Text comes from the component table wherever the identity word resolves, because the
    // effect words alone do not carry the magnitude the game displays - a Vampiric Axe Haft and
    // a Vampiric Hammer Haft share one Lifesteal word but drain 3 and 5 respectively. Only
    // words with no component behind them are decoded individually.
    inline Tooltip BuildTooltip(const ItemDef& item)
    {
        using Kind = TooltipLine::Kind;

        Tooltip tip;
        tip.name     = item.name.empty() ? std::string("(unnamed item)") : item.name;
        tip.rarity   = RarityFromInteraction(item.interaction);
        tip.resolved = item.resolved;
        tip.lines.push_back({Kind::Name, tip.name, {}});

        if (!item.resolved)
        {
            tip.lines.push_back({Kind::Muted, "Mods unknown - item data never arrived", {}});
            return tip;
        }

        int dmgMin = -1, dmgMax = -1, dmgType = -1, armor = -1, energy = -1;
        int reqAttr = -1, reqRank = -1, customized = -1;

        for (const auto& m : item.mods)
        {
            switch (m.id)
            {
                case ModId::Weapon_damage:
                    dmgMin = std::min(m.arg1, m.arg2);
                    dmgMax = std::max(m.arg1, m.arg2);
                    break;
                // Also emitted by elemental prefixes, and theirs is the one that wins.
                case ModId::Damage_type: dmgType    = SingleValue(m.arg1, m.arg2); break;
                case ModId::Armor_base:  armor      = std::max(armor, int(SingleValue(m.arg1, m.arg2))); break;
                case ModId::Requirement: reqAttr    = m.arg1; reqRank = m.arg2;    break;
                case ModId::Customized_damage_modifier:
                                         customized = SingleValue(m.arg1, m.arg2); break;

                // A staff's or focus's inherent energy, and a shield's inherent armor, are both
                // stored as a met/unmet pair: arg1 with the attribute requirement satisfied,
                // arg2 without. The tooltip shows the satisfied value, as the game does.
                case ModId::Base_Energy_extra:
                    energy = std::max(energy, int(SingleValue(m.arg1, m.arg2)));
                    break;
                case ModId::Energy_extra_from_requirement:
                    energy = std::max(energy, int(m.arg1));
                    break;
                case ModId::Armor_extra_from_requirement:
                    armor = std::max(armor, int(m.arg1));
                    break;

                // Fallback for items that state their requirement as a condition word instead
                // of a Requirement word.
                case ModId::Condition_requires_attribute:
                    if (reqAttr < 0) { reqAttr = m.arg1; reqRank = m.arg2; }
                    break;
                default: break;
            }
        }

        // Fixed-stat holiday weapons: the mod words do not carry these numbers at all.
        WintergreenStats wintergreen;
        if (WintergreenOverride(item.name, wintergreen))
        {
            dmgMin  = wintergreen.min;
            dmgMax  = wintergreen.max;
            dmgType = wintergreen.damageType;
            if (wintergreen.armor >= 0) armor = wintergreen.armor;
            reqAttr = -1; // wintergreen weapons have no attribute requirement
        }

        std::string requirement;
        if (reqAttr >= 0)
            requirement = std::format("(Requires {} {})", reqRank, AttributeLabel(static_cast<uint8_t>(reqAttr)));

        if (dmgMin >= 0)
        {
            const char* type = dmgType >= 0 ? DamageTypeName(static_cast<uint32_t>(dmgType)) : nullptr;
            tip.lines.push_back({Kind::Stat,
                                 std::format("{} Dmg: {}-{}", type ? type : "Damage", dmgMin, dmgMax),
                                 requirement});
            if (energy >= 0) tip.lines.push_back({Kind::Stat, std::format("Energy +{}", energy), {}});
        }
        else if (armor >= 0)
        {
            // A shield's armor takes the place of the damage line, requirement alongside.
            tip.lines.push_back({Kind::Stat, std::format("Armor: {}", armor), requirement});
            if (energy >= 0) tip.lines.push_back({Kind::Stat, std::format("Energy +{}", energy), {}});
        }
        else if (energy >= 0)
        {
            // A focus has no damage line; its energy is the headline stat.
            tip.lines.push_back({Kind::Stat, std::format("Energy +{}", energy), requirement});
        }
        else if (!requirement.empty())
        {
            tip.lines.push_back({Kind::Stat, {}, requirement});
        }

        // Split the mod array on identity words. Anything before the first one is inherent to
        // the item rather than contributed by an upgrade.
        ModGroup inherent;
        std::vector<ModGroup> groups;

        for (const auto& m : item.mods)
        {
            if (m.id == ModId::Component_identity)
            {
                ModGroup g;
                g.componentId = static_cast<uint16_t>(m.arg1 << 8 | m.arg2);
                g.info = FindComponent(g.componentId);
                if (g.info) g.cls = g.info->cls;
                groups.push_back(std::move(g));
                continue;
            }

            ModGroup& target = groups.empty() ? inherent : groups.back();

            // The marker also tells us what kind of component this is, which matters for
            // ordering and for the empty-slot count when the id is not in the table.
            if (m.id == ModId::Effect_marker_alt || m.id == ModId::Effect_marker_armor_alt)
            {
                if (!groups.empty() && !target.info) target.cls = ComponentClass::Inscription;
                continue;
            }
            if (m.id == ModId::Effect_marker_armor)
            {
                if (!groups.empty() && !target.info) target.cls = ComponentClass::Insignia;
                continue;
            }
            if (IsStructuralWord(m.id) || IsHeaderWord(m.id)) continue;

            target.effects.push_back(m);
        }

        // Effects with no component behind them: decode the words themselves. Anything the
        // identifier table does not recognise is dropped rather than shown as a hex dump.
        auto emitDecoded = [&tip](const ModGroup& group) {
            for (const auto& m : group.effects)
            {
                ModText text = DecodeMod(m.id, m.arg1, m.arg2);
                if (text.structural || !text.known) continue;
                tip.lines.push_back({Kind::Mod, std::move(text.effect), std::move(text.condition)});
            }
        };

        // The component table stores each mod's MAXIMUM roll, because that is the name the game
        // gives it: a "Sword Pommel of Fortitude" is always called Health +30. The item in hand may
        // have rolled lower, and the effect word carries what it actually rolled.
        //
        // That difference is real health, not a display detail. One measured player's Fortitude
        // sword granted 29 and his Bronze Shield 28, which is why his maximum read 519 where two
        // full mods would have given 520 -- and why the model must read the word, not the table.
        auto rolledHealthFor = [](const ModGroup& group) -> int {
            int rolled = -1;
            for (const Mod& m : group.effects)
            {
                switch (m.id)
                {
                case ModId::Health_extra:
                case ModId::Health_extra_when_enchanted:
                case ModId::Health_extra_when_hexed:
                case ModId::Health_extra_when_in_stance:
                    rolled = SingleValue(m.arg1, m.arg2);
                    break;
                default: break;
                }
            }
            if (rolled < 0 || !group.info) return -1;

            // Only safe when the component grants health on exactly one line. The Survivor
            // insignia describes three (15 chest / 10 legs / 5 elsewhere) against a single word,
            // so rewriting them all with one value would be worse than leaving them alone.
            const std::string_view desc = group.info->description;
            int healthLines = 0;
            for (size_t start = 0; start <= desc.size();)
            {
                const size_t nl = desc.find('\n', start);
                const std::string_view line = desc.substr(start, nl == std::string_view::npos ? nl : nl - start);
                if (IsHealthMagnitudeLine(line)) healthLines++;
                if (nl == std::string_view::npos) break;
                start = nl + 1;
            }
            return healthLines == 1 ? rolled : -1;
        };

        auto emitGroup = [&tip, &emitDecoded, &rolledHealthFor](const ModGroup& group) {
            if (!group.info)
            {
                emitDecoded(group);
                return;
            }
            const int rolledHealth = rolledHealthFor(group);

            // Inscriptions are named in game (Inscription: "I have the power!"); prefixes and
            // suffixes are already spelled out by the item's own name, so only their effects show.
            if (group.cls == ComponentClass::Inscription && !group.info->name.empty())
            {
                tip.lines.push_back({Kind::Mod, std::string(group.info->name), {}});
            }

            const std::string_view desc = group.info->description;
            for (size_t start = 0; start <= desc.size();)
            {
                const size_t nl = desc.find('\n', start);
                const std::string line{desc.substr(start, nl == std::string_view::npos ? nl : nl - start)};
                if (!line.empty())
                {
                    std::string effect, condition;
                    SplitCondition(line, effect, condition);
                    if (rolledHealth >= 0) RewriteHealthMagnitude(effect, rolledHealth);
                    tip.lines.push_back({Kind::Mod, std::move(effect), std::move(condition)});
                }
                if (nl == std::string_view::npos) break;
                start = nl + 1;
            }
        };

        emitDecoded(inherent);

        for (const auto& g : groups) if (g.cls == ComponentClass::Inscription) emitGroup(g);
        for (const auto& g : groups) if (g.cls == ComponentClass::Upgrade)     emitGroup(g);
        for (const auto& g : groups) if (g.cls == ComponentClass::Insignia)    emitGroup(g);

        // Slots the item was never given an upgrade for. Counted from the components actually
        // present rather than from the interaction flags, which report a PvP item's slots as
        // free even when they are filled.
        int filled = 0;
        for (const auto& g : groups) if (g.cls == ComponentClass::Upgrade) filled++;
        for (int i = filled; i < UpgradeSlotCount(item.itemType); i++)
            tip.lines.push_back({Kind::Muted, "<empty prefix / suffix>", {}});

        if (customized >= 0)
        {
            tip.lines.push_back({Kind::Stat,
                                 customized >= 100 ? std::format("Damage +{}%", customized - 100)
                                                   : std::format("Damage {}%", customized),
                                 "(customized)"});
        }

        return tip;
    }

    // ─── Parsing ─────────────────────────────────────────────────────────────────────────
    // Definitions come first in the file, so one sequential pass is enough.

    inline void ParseContent(const std::string& content, Data& out)
    {
        auto toU32 = [](const char* b, const char* e) {
            uint32_t v = 0; std::from_chars(b, e, v); return v;
        };
        auto toHex = [](const char* b, const char* e) {
            uint32_t v = 0; std::from_chars(b, e, v, 16); return v;
        };

        const char* p   = content.data();
        const char* end = p + content.size();

        while (p < end)
        {
            const char* lineEnd = static_cast<const char*>(memchr(p, '\n', end - p));
            if (!lineEnd) lineEnd = end;
            const char* stop = lineEnd;
            if (stop > p && *(stop - 1) == '\r') stop--;

            // Skip past "[MM:SS.mmm] "
            const char* close = static_cast<const char*>(memchr(p, ']', stop - p));
            if (!close) { p = lineEnd + 1; continue; }

            float time = 0.f;
            {
                const char* b = p + 1;
                const char* colon = static_cast<const char*>(memchr(b, ':', close - b));
                if (colon)
                {
                    int mm = 0, ss = 0, ms = 0;
                    std::from_chars(b, colon, mm);
                    const char* dot = static_cast<const char*>(memchr(colon, '.', close - colon));
                    if (dot) { std::from_chars(colon + 1, dot, ss); std::from_chars(dot + 1, close, ms); }
                    else     { std::from_chars(colon + 1, close, ss); }
                    time = mm * 60.f + ss + ms / 1000.f;
                }
            }

            const char* d = close + 1;
            while (d < stop && (*d == ' ' || *d == '\t')) d++;

            // Split on ';'. The name is last and the recorder strips ';' from it, so a plain
            // split is safe.
            constexpr int kMaxFields = 16;
            struct Field { const char* b; const char* e; } f[kMaxFields];
            int n = 0;
            {
                const char* start = d;
                for (const char* q = d; q < stop; ++q)
                {
                    if (*q == ';' && n < kMaxFields) { f[n++] = {start, q}; start = q + 1; }
                }
                if (n < kMaxFields) f[n++] = {start, stop};
            }
            if (n < 3) { p = lineEnd + 1; continue; }

            const size_t tagLen = static_cast<size_t>(f[0].e - f[0].b);

            if (tagLen == 8 && memcmp(f[0].b, "ITEM_DEF", 8) == 0 && n >= 12)
            {
                ItemDef item;
                item.ref         = toU32(f[1].b, f[1].e);
                item.itemId      = toU32(f[2].b, f[2].e);
                item.agentItemId = static_cast<uint16_t>(toU32(f[3].b, f[3].e));
                item.modelFileId = toU32(f[4].b, f[4].e);
                item.itemType    = toU32(f[5].b, f[5].e);
                item.interaction = toU32(f[6].b, f[6].e);

                // dyes: "d1,d2,d3,d4"
                {
                    const char* b = f[7].b;
                    for (int i = 0; i < 4 && b < f[7].e; i++)
                    {
                        const char* comma = static_cast<const char*>(memchr(b, ',', f[7].e - b));
                        const char* stopAt = comma ? comma : f[7].e;
                        item.dyes[i] = static_cast<uint8_t>(toU32(b, stopAt));
                        if (!comma) break;
                        b = comma + 1;
                    }
                }
                item.dyeTint  = static_cast<uint8_t>(toU32(f[8].b, f[8].e));
                item.modelId  = toU32(f[9].b, f[9].e);
                item.resolved = (f[10].e - f[10].b) == 8 && memcmp(f[10].b, "resolved", 8) == 0;

                // mods: 8-digit hex words separated by '|'
                {
                    const char* b = f[11].b;
                    while (b < f[11].e)
                    {
                        const char* bar = static_cast<const char*>(memchr(b, '|', f[11].e - b));
                        const char* stopAt = bar ? bar : f[11].e;
                        if (stopAt > b)
                        {
                            Mod m;
                            m.raw  = toHex(b, stopAt);
                            m.id   = static_cast<uint16_t>(m.raw >> 16);
                            m.arg1 = static_cast<uint8_t>((m.raw >> 8) & 0xFF);
                            m.arg2 = static_cast<uint8_t>(m.raw & 0xFF);
                            item.mods.push_back(m);
                        }
                        if (!bar) break;
                        b = bar + 1;
                    }
                }
                if (n >= 13) item.name.assign(f[12].b, f[12].e);
                if (n >= 14) item.iconFileId = toU32(f[13].b, f[13].e);
                // Older recordings stop at the name. A weapon's icon is its skin, so fall back to
                // that and leave armour without one rather than pointing it at the wrong file.
                if (!item.iconFileId && !(item.interaction & 4u)) item.iconFileId = item.modelFileId;
                item.firstSeen = time;

                if (item.ref >= out.items.size()) out.items.resize(item.ref + 1);
                if (item.agentItemId) out.byAgentItemId[item.agentItemId] = item.ref;
                out.items[item.ref] = std::move(item);
            }
            else if (tagLen == 9 && memcmp(f[0].b, "EQUIP_SET", 9) == 0 && n >= 4)
            {
                SlotEvent e;
                e.time    = time;
                e.agentId = static_cast<int>(toU32(f[1].b, f[1].e));
                e.slot    = static_cast<uint8_t>(toU32(f[2].b, f[2].e));
                e.ref     = static_cast<int>(toU32(f[3].b, f[3].e));
                out.events.push_back(e);
            }
            else if (tagLen == 11 && memcmp(f[0].b, "EQUIP_CLEAR", 11) == 0 && n >= 3)
            {
                SlotEvent e;
                e.time    = time;
                e.agentId = static_cast<int>(toU32(f[1].b, f[1].e));
                e.slot    = static_cast<uint8_t>(toU32(f[2].b, f[2].e));
                e.ref     = -1;
                out.events.push_back(e);
            }

            p = lineEnd + 1;
        }

        out.loaded = !out.items.empty() || !out.events.empty();
    }
}
