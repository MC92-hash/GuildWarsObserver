#pragma once
#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>

// map_id (from match metadata infos.json) -> FFNA file ID inside gw.dat
inline uint32_t GetDatMapId(int metadataMapId)
{
    switch (metadataMapId)
    {
    case 171: return 0x1F1FC;  // Warrior's Isle
    case 172: return 0x1F208;  // Hunter's Isle
    case 173: return 0x1F22C;  // Wizard's Isle
    case 167: return 0x1F24D;  // Burning Isle
    case 170: return 0x1F265;  // Frozen Isle
    case 174: return 0x1F268;  // Nomad's Isle
    case 168: return 0x1F27A;  // Druid's Isle
    case 175: return 0x1F29B;  // Isle of the Dead
    case 358: return 0x28784;  // Isle of Meditation
    case 355: return 0x2661F;  // Isle of Weeping Stone
    case 356: return 0x26625;  // Isle of Jade
    case 357: return 0x28736;  // Imperial Isle
    case 533: return 0x33056;  // Uncharted Isle
    case 534: return 0x3321C;  // Isle of Wurms
    case 541: return 0x3314E;  // Corrupted Isle
    case 542: return 0x334A2;  // Isle of Solitude
    default:  return 0;
    }
}

enum class AgentType : uint8_t { Unknown, Player, NPC, Gadget, Flag, Spirit, Item };

inline const char* AgentTypeName(AgentType t)
{
    switch (t) {
    case AgentType::Player:  return "Player";
    case AgentType::NPC:     return "NPC";
    case AgentType::Gadget:  return "Gadget";
    case AgentType::Flag:    return "Flag";
    case AgentType::Spirit:  return "Spirit";
    case AgentType::Item:    return "Item";
    default:                 return "Unknown";
    }
}

// Per-map flag item_id pairs. Returns true if the given item_id is a flag
// on the specified map. Flags must never be interpolated — they snap to
// their exact recorded snapshot positions.
inline bool IsFlagItemId(int mapId, uint32_t itemId)
{
    if (itemId == 0) return false;
    switch (mapId) {
    case 167: return itemId == 57 || itemId == 58;   // Burning Isle
    case 168: return itemId == 45 || itemId == 46;   // Druid's Isle
    case 170: return itemId == 49 || itemId == 50;   // Frozen Isle
    case 171: return itemId == 67 || itemId == 68;   // Warrior's Isle
    case 174: return itemId == 45 || itemId == 46;   // Nomad's Isle
    case 175: return itemId == 45 || itemId == 46;   // Isle of the Dead
    case 355: return itemId == 61 || itemId == 62;   // Isle of Weeping Stone
    case 356: return itemId == 61 || itemId == 62;   // Isle of Jade
    case 357: return itemId == 65 || itemId == 66;   // Imperial Isle
    case 358: return itemId == 61 || itemId == 62;   // Isle of Meditation
    case 533: return itemId == 69 || itemId == 70;   // Uncharted Isle
    case 534: return itemId == 61 || itemId == 62;   // Isle of Wurms
    case 541: return itemId == 73 || itemId == 74;   // Corrupted Isle
    case 542: return itemId == 61 || itemId == 62;   // Isle of Solitude
    default:  return false;
    }
}

// ---------------------------------------------------------------------------
// Ritualist spirit lookup: model_id → { skill_id, display_name }
// ---------------------------------------------------------------------------

struct SpiritInfo
{
    int         skillId;
    const char* name;
};

inline const SpiritInfo* LookupSpirit(uint32_t modelId)
{
    static const struct { uint32_t modelId; SpiritInfo info; } table[] = {
        { 4275, { 305,  "Spirit of Union" } },
        { 4279, { 3020, "Spirit of Wanderlust" } },
        { 4264, { 3006, "Spirit of Shadowsong" } },
        { 4265, { 3007, "Spirit of Pain" } },
        { 4274, { 3016, "Spirit of Shelter" } },
        { 4273, { 3015, "Spirit of Earthbind" } },
        { 4267, { 3009, "Spirit of Soothing" } },
        { 5770, { 3025, "Spirit of Recovery" } },
        { 4271, { 3013, "Spirit of Recuperation" } },
        { 5904, { 3099, "Spirit of Rejuvenation" } },
        { 4269, { 3012, "Spirit of Life" } },
        { 4270, { 3011, "Spirit of Preservation" } },
        { 4277, { 3018, "Spirit of Restoration" } },
        { 4272, { 3014, "Spirit of Dissonance" } },
        { 4268, { 3010, "Spirit of Displacement" } },
        { 4276, { 3017, "Spirit of Disenchantment" } },
        { 5771, { 3023, "Spirit of Anguish" } },
        { 4289, { 997,  "Spirit of Famine" } },
        { 2937, { 475,  "Spirit of Quickening Zephyr" } },
        { 2938, { 476,  "Spirit of Nature's Renewal" } },
        { 2939, { 477,  "Spirit of Muddy Terrain" } },
        { 2927, { 464,  "Spirit of Edge of Extinction" } },
        { 2929, { 467,  "Spirit of Fertile Season" } },
        { 5767, { 1472, "Spirit of Toxicity" } },
        { 2936, { 474,  "Spirit of Energizing Wind" } },
        { 4266, { 3008, "Spirit of Destruction" } },
        { 5773, { 3022, "Spirit of Gaze of Fury" } },
        { 5905, { 3038, "Spirit of Agony" } },
        { 4278, { 3019, "Spirit of Bloodsong" } },
    };
    for (auto& e : table)
        if (e.modelId == modelId) return &e.info;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Spirit danger-zone radius (game units). Used for the overlap / coexistence
// rule: spirits of the same model_id and team within 2.7 × radius of each
// other cannot coexist — the oldest one is hidden.
//
// Binding Rituals (Ritualist):  effect range ≈ 2500  (spirit range / earshot)
// Nature Rituals  (Ranger):     effect range ≈ 5000  (larger area)
// ---------------------------------------------------------------------------

inline float GetSpiritRadius(uint32_t modelId)
{
    switch (modelId) {
    // --- Ranger Nature Rituals (large area) ---
    case 2927: // Edge of Extinction
    case 2929: // Fertile Season
    case 2936: // Energizing Wind
    case 2937: // Quickening Zephyr
    case 2938: // Nature's Renewal
    case 2939: // Muddy Terrain
    case 4289: // Famine
    case 5767: // Toxicity
        return 5000.f;

    // --- Ritualist Binding Rituals (spirit range) ---
    default:
        return 2500.f;
    }
}

// ---------------------------------------------------------------------------
// Map-specific item_id lookup (non-flag items like Vine Seed, Repair Kit)
// ---------------------------------------------------------------------------

inline const char* LookupMapItem(int mapId, uint32_t itemId)
{
    if (itemId == 0) return nullptr;
    switch (mapId) {
    case 168: // Druid's Isle
        if (itemId == 47 || itemId == 48) return "Vine Seed";
        break;
    case 171: // Warrior's Isle
        if (itemId == 59 || itemId == 60) return "Repair Kit";
        break;
    }
    return nullptr;
}

inline const char* LookupNpcName(uint32_t modelId)
{
    switch (modelId) {
    case 168: return "Lesser Flame Sentinel";
    case 170: return "Guild Lord";
    case 172: return "Bodyguard";
    case 173: return "Footman";
    case 174: return "Knight";
    case 175: return "Archer";
    case 176: return "Archer";
    default:  return nullptr;
    }
}

inline const char* LookupGadgetName(uint32_t gadgetId)
{
    switch (gadgetId) {
    case 1:    return "Resurrection Shrine";
    case 2:    return "Chest";
    case 5:    return "Signpost";
    case 7:    return "Gate";
    case 8:    return "Catapult";
    case 9:    return "Door";
    case 10:   return "Gate";
    case 12:   return "Merchant";
    case 13:   return "Banner";
    case 17:   return "Morale Boost";
    case 19:   return "Victory/Defeat Point";
    case 26:   return "Altar";
    case 29:   return "Siege Weapon";
    case 31:   return "Spectral Essence";
    case 32:   return "Light of Deldrimor";
    case 33:   return "Dwarven Resurrection Shrine";
    case 34:   return "Beacon of Droknar";
    case 111:  return "Tower Flag Stand";
    case 321:  return "Lever";
    case 323:  return "Lever";
    case 1323: return "Lever";
    case 1324: return "Lever";
    case 2675: return "Miasma";
    case 3873: return "Lever";
    case 3877: return "Obelisk Flag Stand";
    case 3879: return "Tower Flag Stand";
    case 4203: return "Tower Flag Stand";
    case 4217: return "Resurrection Shrine";
    case 4218: return "Resurrection Shrine";
    case 1299: return "Vine Bridge";
    case 4334: return "Acid Trap";
    case 4558: return "Stone Spores";
    case 4645: return "Gate Lock";
    case 4646: return "Gate Lock";
    case 4647: return "Gate Lock";
    case 4648: return "Gate Lock";
    case 4649: return "Gate Lock";
    case 4650: return "Gate Lock";
    case 4651: return "Gate Lock";
    case 4652: return "Gate Lock";
    case 4720: return "Obelisk Flag Stand";
    case 4721: return "Gate Lock";
    case 4722: return "Gate Lock";
    case 4725: return "Lever";
    case 5988: return "Southern Health Shrine";
    default:   return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Full calibration transform for aligning GWCA agent positions with GWMB meshes.
//
// Pipeline (applied in order):
//   0. Base remap  – GWCA (x,y,z_height) → GWMB (x, z_height, y)
//   1. Axis swaps  – swap pairs of axes (debug exploration)
//   2. Axis flips  – negate X / Y / Z independently
//   3. Rotation    – around Y axis (up)
//   4. Offset      – translate
//   5. Scale
// ---------------------------------------------------------------------------

struct MapTransform
{
    float offsetX = 0.f, offsetY = 0.f, offsetZ = 0.f;
    float scaleX  = 1.f, scaleY  = 1.f, scaleZ  = 1.f;
    float rotationDegrees = 0.f;
    bool  flipX = false;
    bool  flipY = false;
    bool  flipZ = false;
    bool  swapYZ = false;
    bool  swapXZ = false;
    bool  swapXY = false;
};

// Calibration result: only Flip Y is needed to align GWCA agent positions
// with GWMB map meshes. No offsets, scaling, or rotation required.
inline MapTransform GetDefaultMapTransform()
{
    MapTransform t;
    t.flipY = true;
    return t;
}

// Per-snapshot data for a single agent at a single point in time.
// Field order matches EXPORTS_CONVENTIONS.md "Agent State Snapshots".
struct AgentSnapshot
{
    float time = 0.f;
    float x = 0.f, y = 0.f, z = 0.f;
    float rotation = 0.f;
    uint32_t weapon_id = 0;
    uint32_t model_id = 0;
    uint32_t gadget_id = 0;
    bool is_alive = false;
    bool is_dead = false;
    float health_pct = 0.f;
    bool is_knocked = false;
    uint32_t max_hp = 0;
    bool has_condition = false;
    bool has_deep_wound = false;
    bool has_bleeding = false;
    bool has_crippled = false;
    bool has_blind = false;
    bool has_poison = false;
    bool has_hex = false;
    bool has_degen_hex = false;
    bool has_enchantment = false;
    bool has_weapon_spell = false;
    bool is_holding = false;
    bool is_casting = false;
    uint32_t skill_id = 0;
    uint8_t weapon_item_type = 0;
    uint8_t offhand_item_type = 0;
    uint16_t weapon_item_id = 0;
    uint16_t offhand_item_id = 0;
    float move_x = 0.f;
    float move_y = 0.f;
    uint16_t visual_effects = 0;
    uint8_t team_id = 0;
    uint16_t weapon_type = 0;
    float weapon_attack_speed = 0.f;
    float attack_speed_modifier = 0.f;
    uint8_t dagger_status = 0;
    float hp_pips = 0.f;
    uint32_t model_state = 0;
    uint32_t animation_code = 0;
    uint32_t animation_id = 0;
    float animation_speed = 0.f;
    float animation_type = 0.f;
    uint32_t in_spirit_range = 0;
    uint16_t agent_model_type = 0;
    uint32_t item_id = 0;
    uint32_t item_extra_type = 0;
    uint32_t gadget_extra_type = 0;

    std::string raw_line;
};

// MOVE_TO_POINT target extracted from StoC agent movement events.
// Sorted by time; used as authoritative movement anchors in interpolation.
struct MoveToPointEvent
{
    float time = 0.f;
    float targetX = 0.f;
    float targetY = 0.f;
};

// A time interval during which an agent was casting a skill.
// Built from StoC SKILL_ACTIVATED / SKILL_FINISHED / SKILL_STOPPED events.
struct CastInterval
{
    float start = 0.f;
    float end   = 0.f;
    int   skillId = 0;
};

struct AgentReplayData
{
    int agent_id = 0;
    std::vector<AgentSnapshot> snapshots;

    AgentType type = AgentType::Unknown;
    std::string categoryName;
    std::string playerName;
    uint8_t  teamId = 0;
    uint32_t modelId = 0;
    uint16_t agentModelType = 0;

    // Spirit-specific metadata
    int      spiritSkillId = 0;
    std::string spiritSkillName;

    // Per-agent MOVE_TO_POINT events (built from StoC after both parsers finish)
    std::vector<MoveToPointEvent> moveEvents;

    // Transient spirit overlap state (recomputed each frame, not serialized)
    bool  overlapHidden       = false;
    float overlapDistNewest   = 0.f;   // distance to the newest spirit of same type+team
    float overlapThreshold    = 0.f;   // 2.7 × spirit radius
    bool  overlapIsNewest     = false;  // true if this is the newest of its group

    // Per-agent casting intervals (built from StoC skill events)
    std::vector<CastInterval> castHistory;

    bool isCastingAtTime(float t) const
    {
        for (auto& ci : castHistory)
            if (t >= ci.start && t <= ci.end) return true;
        return false;
    }

    int castingSkillAtTime(float t) const
    {
        for (auto& ci : castHistory)
            if (t >= ci.start && t <= ci.end) return ci.skillId;
        return 0;
    }

    // Returns true if the agent is dead at time t, based on the nearest
    // snapshot's is_dead flag. Resurrection is handled automatically because
    // a new snapshot with is_dead=false will appear at the res location.
    bool isDeadAtTime(float t) const
    {
        if (snapshots.empty()) return false;
        if (t <= snapshots.front().time) return snapshots.front().is_dead;
        if (t >= snapshots.back().time)  return snapshots.back().is_dead;
        // Binary search for last snapshot with time <= t
        int lo = 0, hi = static_cast<int>(snapshots.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snapshots[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        return snapshots[lo].is_dead;
    }
};

// ---------------------------------------------------------------------------
// StoC event structures
// ---------------------------------------------------------------------------

enum class StoCCategory : uint8_t
{
    AgentMovement, Skill, AttackSkill, BasicAttack, Combat, Jumbo, Unknown, _Count
};

inline const char* StoCCategoryName(StoCCategory c)
{
    switch (c) {
    case StoCCategory::AgentMovement: return "Agent Movement";
    case StoCCategory::Skill:         return "Skill Events";
    case StoCCategory::AttackSkill:   return "Attack Skill Events";
    case StoCCategory::BasicAttack:   return "Basic Attack Events";
    case StoCCategory::Combat:        return "Combat Events";
    case StoCCategory::Jumbo:         return "Jumbo Messages";
    case StoCCategory::Unknown:       return "Unknown Events";
    default:                          return "?";
    }
}

struct AgentMovementEvent
{
    float time = 0.f;
    int   agent_id = 0;
    float x = 0.f;
    float y = 0.f;
    float plane = 0.f;
    std::string raw_line;
};

struct SkillActivationEvent
{
    float       time = 0.f;
    std::string type;
    int         skill_id = 0;
    int         caster_id = 0;
    int         target_id = 0;
    std::string raw_line;
};

struct AttackSkillEvent
{
    float       time = 0.f;
    std::string type;
    int         skill_id = 0;
    int         caster_id = 0;
    int         target_id = 0;
    std::string raw_line;
};

struct BasicAttackEvent
{
    float       time = 0.f;
    std::string type;
    int         caster_id = 0;
    int         target_id = 0;
    int         skill_id = 0;
    std::string raw_line;
};

struct CombatEvent
{
    float       time = 0.f;
    std::string type;
    int         caster_id = 0;
    int         target_id = 0;
    float       value = 0.f;
    int         damage_type = 0;
    std::string raw_line;
};

struct JumboMessageEvent
{
    float       time = 0.f;
    std::string message;
    int         party_value = 0;
    std::string raw_line;
};

struct UnknownEvent
{
    float       time = 0.f;
    std::string raw_line;
};

struct StoCData
{
    std::vector<AgentMovementEvent>   agentMovement;
    std::vector<SkillActivationEvent> skill;
    std::vector<AttackSkillEvent>     attackSkill;
    std::vector<BasicAttackEvent>     basicAttack;
    std::vector<CombatEvent>          combat;
    std::vector<JumboMessageEvent>    jumbo;
    std::vector<UnknownEvent>         unknown;
};

struct StoCParseProgress
{
    std::atomic<int>  files_done{ 0 };
    std::atomic<int>  files_total{ 0 };
    std::atomic<bool> finished{ false };
    std::atomic<bool> has_error{ false };

    std::mutex mutex;
    StoCData   data;
    std::vector<std::string> errors;
};

// ---------------------------------------------------------------------------

struct AgentParseProgress
{
    std::atomic<int> files_done{ 0 };
    std::atomic<int> files_total{ 0 };
    std::atomic<bool> finished{ false };
    std::atomic<bool> has_error{ false };

    std::mutex mutex;
    std::unordered_map<int, AgentReplayData> agents;
    std::vector<std::string> errors;
};

// ---------------------------------------------------------------------------
// Interpolation settings
// ---------------------------------------------------------------------------

enum class InterpolationMode : uint8_t { OriginalLinear, Improved };

struct InterpolationSettings
{
    InterpolationMode mode = InterpolationMode::Improved;
    bool  enabled              = true;   // master on/off (off = snap to nearest)
    bool  showRawSnapshots     = false;  // grey dots at raw snapshot positions
    bool  showInterpolatedLine = false;  // line between raw and interpolated
    bool  showMoveAnchors      = false;  // yellow dots at MOVE_TO_POINT targets
    bool  showCastingFreeze    = false;  // purple ring when agent is frozen by casting
    bool  showDeadFreeze       = false;  // black dot when agent is frozen by death
    float gapThreshold         = 0.4f;   // seconds; gaps larger than this trigger prediction
    float velocityInfluence    = 1.0f;   // 0..1 blending weight for MOVE_TO_POINT prediction
};

struct ReplayContext
{
    std::filesystem::path matchFolderPath;
    int mapId = 0;
    uint32_t datMapId = 0;
    bool mapLoaded = false;

    // Agent snapshot data (populated asynchronously)
    std::unordered_map<int, AgentReplayData> agents;
    bool agentsLoaded = false;
    std::shared_ptr<AgentParseProgress> agentParseProgress;

    // StoC event data (populated asynchronously)
    StoCData stocData;
    bool stocLoaded = false;
    std::shared_ptr<StoCParseProgress> stocParseProgress;

    float maxReplayTime = 0.f;

    // Per-map calibration transform (loaded from file, tunable at runtime)
    MapTransform mapTransform;

    // Interpolation configuration
    InterpolationSettings interpSettings;
};
