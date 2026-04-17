#pragma once
#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>
#include <span>
#include <unordered_map>
#include <atomic>
#include <mutex>

// map_id (from match metadata infos.json) -> FFNA file ID inside gw.dat
// Isle of Wurms (metadata map_id 532 / 534): South Health Shrine capture radius (game units).
inline constexpr float kWurmsShrineCaptureRadius = 1010.f;

inline bool IsIsleOfWurmsMap(int metadataMapId)
{
    return metadataMapId == 532 || metadataMapId == 534;
}

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
        { 5766, { 1730, "Spirit of Infuriating Heat" } },
        { 4286, { 1213, "Spirit of Tranquility" } },
        { 2932, { 470,  "Spirit of Predatory Season" } },
    };
    for (auto& e : table)
        if (e.modelId == modelId) return &e.info;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Spirit overwrite distance (game units). If two spirits of the same
// model_id and team are within this distance, the older one is destroyed.
//
// Ranger Nature Rituals:  overwrite within 3500 units
// Ritualist Binding Rituals: overwrite within 2512 units
// ---------------------------------------------------------------------------

inline float GetSpiritOverwriteDist(uint32_t modelId)
{
    switch (modelId) {
    // --- Ranger Nature Rituals ---
    case 2927: // Edge of Extinction
    case 2929: // Fertile Season
    case 2932: // Predatory Season
    case 2936: // Energizing Wind
    case 2937: // Quickening Zephyr
    case 2938: // Nature's Renewal
    case 2939: // Muddy Terrain
    case 4289: // Famine
    case 5767: // Toxicity
    case 5766: // Infuriating Heat
    case 4286: // Tranquility
        return 3500.f;

    // --- Ritualist Binding Rituals ---
    default:
        return 2512.f;
    }
}

// ---------------------------------------------------------------------------
// Spirit type classification: nature ritual vs binding ritual
// ---------------------------------------------------------------------------

inline bool IsNatureRitual(uint32_t modelId)
{
    switch (modelId) {
    case 2927: case 2929: case 2932: case 2936: case 2937:
    case 2938: case 2939: case 4289: case 5767: case 5766:
    case 4286:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Offensive binding ritual classification (attack/disrupt spirits)
// ---------------------------------------------------------------------------

inline bool IsOffensiveBindingRitual(uint32_t modelId)
{
    switch (modelId) {
    case 5905: // Agony
    case 5771: // Anguish
    case 4278: // Bloodsong
    case 4265: // Pain
    case 4279: // Wanderlust
    case 4264: // Shadowsong
    case 4266: // Destruction
    case 4272: // Dissonance
    case 4276: // Disenchantment
    case 5773: // Gaze of Fury
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Agent model_id + type -> .dat file hash for 3D model lookup
// Returns 0 if no model is available for this agent.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Player model variant tables — multiple models per profession+gender
// ---------------------------------------------------------------------------

inline std::span<const uint32_t> GetPlayerModelVariants(int primaryProf, bool isFemale)
{
    // Female variants
    static constexpr uint32_t kWarF[]  = { 0x3BD9E, 0x26C53, 0x1FBC4 }; // Timera, Zaishen Fighter, Devona
    static constexpr uint32_t kRanF[]  = { 0x1FC35 };                    // Reyna
    static constexpr uint32_t kMonF[]  = { 0x1C7EE, 0x1FC32, 0x2D22C }; // Lina, Alesia, Sister Tai
    static constexpr uint32_t kNecF[]  = { 0x2D225, 0x1FB82 };          // Su, Eve
    static constexpr uint32_t kEleF[]  = { 0x26C50, 0x1FBBF };          // Zaishen Mage, Cynn
    static constexpr uint32_t kMesF[]  = { 0x4C460, 0x3BD99 };          // Gwen, (unnamed)
    static constexpr uint32_t kAssF[]  = { 0x3BC80, 0x2D15C };          // Zenmai, Nika
    static constexpr uint32_t kRitF[]  = { 0x4C476 };                   // Xandra
    static constexpr uint32_t kParF[]  = { 0x4C449 };                   // Hayda
    static constexpr uint32_t kDerF[]  = { 0x3BD6A };                   // Melonni

    // Male variants
    static constexpr uint32_t kWarM[]  = { 0x1FC11, 0x2D2A4 };          // Stefan, Lukas
    static constexpr uint32_t kRanM[]  = { 0x1FBBA, 0x26C56 };          // Aidan, Zaishen Archer
    static constexpr uint32_t kMonM[]  = { 0x26C4D };                    // Zaishen Healer
    static constexpr uint32_t kNecM[]  = { 0x3BBC6 };                   // Olias
    static constexpr uint32_t kEleM[]  = { 0x1FC2F, 0x2D236 };          // Orion, Headmaster Vhang
    static constexpr uint32_t kMesM[]  = { 0x2D21E, 0x1C7CD };          // Lo Sha, Dunham
    static constexpr uint32_t kAssM[]  = { 0x2D217 };                   // Panaku
    static constexpr uint32_t kRitM[]  = { 0x2D2F3, 0x2D2A9 };          // Professor Gai, Aeson
    static constexpr uint32_t kParM[]  = { 0x3BD8E };                   // Sogolon
    static constexpr uint32_t kDerM[]  = { 0x4C454 };                   // Kahmu

    // GW internal profession IDs: 1=W 2=R 3=Mo 4=N 5=Me 6=E 7=A 8=Rt 9=P 10=D
    if (isFemale) {
        switch (primaryProf) {
        case 1:  return kWarF;  case 2:  return kRanF;  case 3:  return kMonF;
        case 4:  return kNecF;  case 5:  return kMesF;  case 6:  return kEleF;
        case 7:  return kAssF;  case 8:  return kRitF;  case 9:  return kParF;
        case 10: return kDerF;
        default: return {};
        }
    } else {
        switch (primaryProf) {
        case 1:  return kWarM;  case 2:  return kRanM;  case 3:  return kMonM;
        case 4:  return kNecM;  case 5:  return kMesM;  case 6:  return kEleM;
        case 7:  return kAssM;  case 8:  return kRitM;  case 9:  return kParM;
        case 10: return kDerM;
        default: return {};
        }
    }
}

inline uint32_t LookupPlayerFileHash(int primaryProf, bool isFemale, int variantIndex = 0)
{
    auto variants = GetPlayerModelVariants(primaryProf, isFemale);
    if (variants.empty()) return 0;
    return variants[variantIndex % variants.size()];
}

inline uint32_t LookupAgentFileHash(AgentType type, uint32_t modelId,
                                     int primaryProf = 0, bool isFemale = false)
{
    if (type == AgentType::Player)
        return LookupPlayerFileHash(primaryProf, isFemale);
    if (type == AgentType::Spirit) {
        if (IsNatureRitual(modelId)) return 0x22A34;
        if (IsOffensiveBindingRitual(modelId)) return 0x2D408;
        return 0x2D44E; // Defensive binding rituals (all remaining)
    }
    if (type == AgentType::NPC) {
        switch (modelId) {
        case 170: return 0x2D161; // Guild Lord
        case 172: return 0x2D236; // Bodyguard
        case 173: return 0x26C4A; // Footman (same model as Knight)
        case 174: return 0x26C4A; // Knight
        case 175: case 176: return 0x2D18A; // Archer
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Combined model info: file hash + GWCA height + NPC adjustment scale
// Used to compute accurate per-model-type scaling in the replay window.
// ---------------------------------------------------------------------------

struct AgentModelInfo {
    uint32_t fileHash;
    float    targetHeight;   // game-unit height from GWCA (0 = unknown)
    float    npcAdjustment;  // scale multiplier (1.0 = 100%, 1.3 = 130%)
};

inline AgentModelInfo LookupPlayerModelInfo(int primaryProf, bool isFemale)
{
    uint32_t hash = LookupPlayerFileHash(primaryProf, isFemale);
    if (hash == 0) return { 0, 0.f, 1.0f };
    float height = isFemale ? 73.640617f : 75.844055f;
    if (!isFemale && (primaryProf == 1 || primaryProf == 3 || primaryProf == 5))
        height = 75.734184f;
    return { hash, height, 1.0f };
}

inline AgentModelInfo LookupAgentModelInfo(AgentType type, uint32_t modelId,
                                            int primaryProf = 0, bool isFemale = false)
{
    if (type == AgentType::Player)
        return LookupPlayerModelInfo(primaryProf, isFemale);
    if (type == AgentType::Spirit) {
        if (IsNatureRitual(modelId))
            return { 0x22A34, 73.917145f, 0.8f };   // Nature rituals (0x50 = 80%)
        if (IsOffensiveBindingRitual(modelId))
            return { 0x2D408, 95.705956f, 1.0f };   // Offensive binding rituals
        return { 0x2D44E, 84.404671f, 1.0f };       // Defensive binding rituals
    }
    if (type == AgentType::NPC) {
        switch (modelId) {
        case 170: return { 0x2D161, 98.454437f, 1.3f };  // Guild Lord (0x82 = 130%)
        case 172: return { 0x2D236, 75.844055f, 1.0f };  // Bodyguard
        case 173: return { 0x26C4A, 75.734184f, 1.0f };  // Footman
        case 174: return { 0x26C4A, 75.734184f, 1.0f };  // Knight
        case 175:
        case 176: return { 0x2D18A, 72.0f,      1.0f };  // Archer
        }
    }
    return { 0, 0.f, 1.0f };
}

// ---------------------------------------------------------------------------
// Spirit effect range (game units) for drawing range circles
// ---------------------------------------------------------------------------

inline float GetSpiritRange(uint32_t modelId)
{
    switch (modelId) {
    // Binding Rituals
    case 5905: return 1012.f;  // Agony
    case 5771: return 322.f;   // Anguish
    case 4278: return 322.f;   // Bloodsong
    case 4266: return 322.f;   // Destruction
    case 4276: return 322.f;   // Disenchantment
    case 4268: return 2512.f;  // Displacement
    case 4272: return 322.f;   // Dissonance
    case 4273: return 2512.f;  // Earthbind
    case 5773: return 322.f;   // Gaze of Fury
    case 4269: return 2512.f;  // Life
    case 4270: return 322.f;   // Preservation
    case 5770: return 2512.f;  // Recovery
    case 4271: return 2512.f;  // Recuperation
    case 5904: return 1012.f;  // Rejuvenation
    case 4277: return 2512.f;  // Restoration
    case 4264: return 322.f;   // Shadowsong
    case 4274: return 2512.f;  // Shelter
    case 4267: return 2512.f;  // Soothing
    case 4275: return 2512.f;  // Union
    case 4279: return 322.f;   // Wanderlust
    case 4265: return 322.f;   // Pain

    // Nature Rituals (all 3500)
    case 2927: case 2929: case 2932: case 2936: case 2937:
    case 2938: case 2939: case 4289: case 5767: case 5766:
    case 4286:
        return 3500.f;

    default: return 0.f;
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
    // TODO: Hunter's Isle (172) — flag and repair kit item_ids unknown
    // TODO: Wizard's Isle (173) — flag and repair kit item_ids unknown
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

// A single skill use event for driving the floating skill icon display.
// Covers both cast (activated→finished) and instant skills.
struct SkillUseEvent
{
    float startTime = 0.f;
    float endTime   = 0.f;   // same as startTime for instant skills
    float fullCastDuration = 0.f; // theoretical full cast time (from successful casts)
    int   skillId   = 0;
    int   targetId  = -1;    // resolved target agent id (-1 = self/none)
    bool  isInstant      = false;
    bool  wasCancelled   = false;  // true if SKILL_STOPPED / ATTACK_SKILL_STOPPED
    bool  wasInterrupted = false;  // true if a matching INTERRUPTED event was found
    float rechargeDuration = 0.f;  // precomputed actual recharge (may differ from DB if fast recast)
    bool  wasFastRecast    = false; // true if next cast happened before normal recharge expired
};

// Tracks one lifecycle incarnation of an agent (agent IDs are recycled by the game)
struct AgentIncarnation {
    int originalAgentId;
    float startTime;
    float endTime;   // FLT_MAX if no AGENT_REMOVE observed
};

struct AgentReplayData
{
    int agent_id = 0;
    int originalAgentId = -1; // -1 = this IS the original; >= 0 = split from that agent
    float lifecycleStart = -1.f; // AGENT_ADD time (-1 if unknown)
    float lifecycleEnd   = -1.f; // AGENT_REMOVE time (-1 if unknown)
    std::vector<AgentSnapshot> snapshots;

    AgentType type = AgentType::Unknown;
    std::string categoryName;
    std::string playerName;
    uint8_t  teamId = 0;
    uint32_t modelId = 0;
    uint16_t agentModelType = 0;
    int      playerNumber = 0;
    int      primaryProf  = 0;
    int      secondaryProf = 0;
    int      playerLevel  = 0;
    bool     isFemale     = false;
    std::string partyBarLabel;

    // Spirit-specific metadata
    int      spiritSkillId = 0;
    std::string spiritSkillName;

    // Incarnation break indices (snapshot indices where INCARNATION_BREAK was found)
    std::vector<int> incarnationBreaks;

    // Per-agent MOVE_TO_POINT events (built from StoC after both parsers finish)
    std::vector<MoveToPointEvent> moveEvents;

    // Transient spirit overlap state (recomputed each frame, not serialized)
    bool  overlapHidden       = false;
    float overlapDistNewest   = 0.f;   // distance to the newest spirit of same type+team
    float overlapThreshold    = 0.f;   // 2.7 × spirit radius
    bool  overlapIsNewest     = false;  // true if this is the newest of its group

    // Transient LOD state (set by DrawAgentCylinders, read by DrawAgentOverlay)
    // 0=Dot, 1=Pillar, 2=Cylinder
    int currentLOD = 2;

    // Per-agent casting intervals (built from StoC skill events)
    std::vector<CastInterval> castHistory;

    // Per-agent skill use timeline for floating icon display (includes instants)
    std::vector<SkillUseEvent> skillUseHistory;

    // Knockdown intervals (built from is_knocked snapshot transitions)
    struct KnockdownInterval { float start; float end; };
    std::vector<KnockdownInterval> knockdownIntervals;

    float knockdownTiltAtTime(float t) const
    {
        constexpr float FALL = 0.15f;
        constexpr float RISE = 0.15f;
        if (knockdownIntervals.empty()) return 0.f;
        // Binary search for the interval closest to t
        int lo = 0, hi = static_cast<int>(knockdownIntervals.size()) - 1, best = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (knockdownIntervals[mid].start <= t + FALL) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (best < 0) return 0.f;
        const auto& kd = knockdownIntervals[best];
        if (t < kd.start) return 0.f;
        if (t > kd.end + RISE) return 0.f;
        float elapsed = t - kd.start;
        if (elapsed < FALL) {
            float p = elapsed / FALL;
            p = p * p * (3.f - 2.f * p);
            return 90.f * p;
        }
        if (t <= kd.end) return 90.f;
        float riseT = (t - kd.end) / RISE;
        if (riseT < 1.f) {
            riseT = riseT * riseT * (3.f - 2.f * riseT);
            return 90.f * (1.f - riseT);
        }
        return 0.f;
    }

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

    bool isKnockedDownAtTime(float t) const
    {
        for (auto& kd : knockdownIntervals)
            if (t >= kd.start && t <= kd.end) return true;
        return false;
    }

    const KnockdownInterval* knockdownIntervalAtTime(float t) const
    {
        for (auto& kd : knockdownIntervals)
            if (t >= kd.start && t <= kd.end) return &kd;
        return nullptr;
    }

    const CastInterval* castIntervalAtTime(float t) const
    {
        for (auto& ci : castHistory)
            if (t >= ci.start && t <= ci.end) return &ci;
        return nullptr;
    }

    // Combined visual state for skill icon + cast bar (always in sync).
    struct SkillVisual {
        int   skillId     = 0;
        float alpha       = 0.f;   // shared icon+bar opacity
        bool  isCasting   = false;  // currently filling the bar
        float progress    = 0.f;   // 0..1 bar fill
        bool  cancelled   = false;  // self-cancel (yellow)
        bool  interrupted = false;  // enemy interrupt (purple)
    };

    SkillVisual skillVisualAtTime(float t) const
    {
        constexpr float LINGER = 2.0f;
        constexpr float FADE   = 0.8f;
        if (skillUseHistory.empty()) return {};
        int lo = 0, hi = static_cast<int>(skillUseHistory.size()) - 1, best = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (skillUseHistory[mid].startTime <= t) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (best < 0) return {};
        const auto& ev = skillUseHistory[best];
        float showEnd = ev.endTime + LINGER;
        float fadeEnd = showEnd + FADE;
        if (t < ev.startTime) return {};
        if (t > fadeEnd) return {};

        SkillVisual sv;
        sv.skillId = ev.skillId;

        // Alpha (shared by icon + bar)
        if (t <= showEnd) sv.alpha = 1.0f;
        else              sv.alpha = 1.0f - (t - showEnd) / FADE;

        // Cast bar progress + state
        float dur     = ev.endTime - ev.startTime;   // actual cast time
        float fullDur = (ev.fullCastDuration > 0.001f) ? ev.fullCastDuration : dur;
        if (!ev.isInstant && dur > 0.001f) {
            if (t < ev.endTime) {
                sv.isCasting = true;
                sv.cancelled = false;
                sv.progress  = std::min((t - ev.startTime) / dur, 1.f);
            } else {
                sv.isCasting   = false;
                sv.cancelled   = ev.wasCancelled && !ev.wasInterrupted;
                sv.interrupted = ev.wasInterrupted;
                sv.progress    = ev.wasCancelled
                    ? std::min(dur / fullDur, 1.f)
                    : 1.0f;
            }
        } else {
            // Instant skill — no cast bar
            sv.isCasting = false;
            sv.cancelled = false;
            sv.progress  = 1.0f;
        }
        return sv;
    }

    // Convenience wrapper for code that only needs skillId + alpha
    std::pair<int, float> skillIconAtTime(float t) const
    {
        auto sv = skillVisualAtTime(t);
        return { sv.skillId, sv.alpha };
    }

    // Returns {targetId, alpha} for the laser line.
    // Laser visible during cast time only (+ 0.3s fade after cast end for non-instant).
    // For instant skills: 0.5s flash then 0.3s fade.
    struct LaserInfo { int targetId; float alpha; };
    LaserInfo skillLaserAtTime(float t) const
    {
        if (skillUseHistory.empty()) return { -1, 0.f };
        int lo = 0, hi = static_cast<int>(skillUseHistory.size()) - 1, best = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (skillUseHistory[mid].startTime <= t) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (best < 0) return { -1, 0.f };
        const auto& ev = skillUseHistory[best];
        if (ev.targetId <= 0 || ev.targetId == agent_id) return { -1, 0.f };
        if (t < ev.startTime) return { -1, 0.f };

        constexpr float FADE = 0.3f;
        if (ev.isInstant) {
            constexpr float FLASH = 0.5f;
            float age = t - ev.startTime;
            if (age < FLASH) return { ev.targetId, 1.0f };
            if (age < FLASH + FADE) return { ev.targetId, 1.0f - (age - FLASH) / FADE };
            return { -1, 0.f };
        }
        if (t <= ev.endTime) return { ev.targetId, 1.0f };
        float age = t - ev.endTime;
        if (age < FADE) return { ev.targetId, 1.0f - age / FADE };
        return { -1, 0.f };
    }

    // Returns true if the agent is dead at time t, based on the nearest
    // snapshot's is_dead flag. Resurrection is handled automatically because
    // a new snapshot with is_dead=false will appear at the res location.
    bool isDeadAtTime(float t) const
    {
        if (snapshots.empty()) return false;
        if (t <= snapshots.front().time) return snapshots.front().is_dead;
        if (t >= snapshots.back().time)  return snapshots.back().is_dead;
        int lo = 0, hi = static_cast<int>(snapshots.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snapshots[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        return snapshots[lo].is_dead;
    }

    bool isAliveAtTime(float t) const
    {
        if (snapshots.empty()) return false;
        if (t <= snapshots.front().time) return snapshots.front().is_alive;
        if (t >= snapshots.back().time)  return snapshots.back().is_alive;
        int lo = 0, hi = static_cast<int>(snapshots.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snapshots[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        return snapshots[lo].is_alive;
    }

    float healthPctAtTime(float t) const
    {
        if (snapshots.empty()) return 0.f;
        if (t <= snapshots.front().time) return snapshots.front().health_pct;
        if (t >= snapshots.back().time)  return snapshots.back().health_pct;
        int lo = 0, hi = static_cast<int>(snapshots.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snapshots[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        return snapshots[lo].health_pct;
    }

    // Returns the time at which the current death sequence began.
    // Walk backwards from the snapshot at time t to find the first is_dead
    // snapshot in this contiguous death run. Used to freeze position at death.
    float deathTransitionTime(float t) const
    {
        if (snapshots.empty()) return t;
        int lo = 0, hi = static_cast<int>(snapshots.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snapshots[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        while (lo > 0 && snapshots[lo - 1].is_dead) lo--;
        return snapshots[lo].time;
    }

    // Returns the time at which is_alive first became false in the current
    // contiguous not-alive run ending at snapshot time t.  Used by spirits
    // where is_alive may go false without is_dead going true (overwrite/despawn).
    float notAliveTransitionTime(float t) const
    {
        if (snapshots.empty()) return t;
        int lo = 0, hi = static_cast<int>(snapshots.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snapshots[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        while (lo > 0 && !snapshots[lo - 1].is_alive) lo--;
        return snapshots[lo].time;
    }
};

// ---------------------------------------------------------------------------
// Combat log row (built from merged StoC streams)
// ---------------------------------------------------------------------------

enum class CombatLogCategory : uint8_t {
    Skill, Damage, Heal, Interrupt, KnockDown, Death, Block, BasicAttack, Jumbo, Other
};

struct CombatLogRow {
    float       time        = 0.f;
    int         casterId    = 0;
    int         targetId    = 0;
    int         skillId     = 0;
    bool        cancelled   = false;
    bool        interrupted = false;
    int         interrupterId = 0;
    float       valuePct    = 0.f;
    int         valueAbs    = 0;
    int         jumboTeam   = 0;
    CombatLogCategory category = CombatLogCategory::Skill;
    std::string eventType;
};

// ---------------------------------------------------------------------------
// StoC event structures
// ---------------------------------------------------------------------------

enum class StoCCategory : uint8_t
{
    AgentMovement, Skill, AttackSkill, BasicAttack, Combat, Jumbo, Unknown, Lifecycle, MapObject, FlagEvent, _Count
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
    case StoCCategory::Lifecycle:     return "Lifecycle Events";
    case StoCCategory::MapObject:     return "Map Object Events";
    case StoCCategory::FlagEvent:     return "Flag Events";
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

struct StoCLordDamageEvent
{
    float time            = 0.f;
    int   caster_id       = 0;
    int   target_id       = 0;
    float value           = 0.f;
    int   damage_type     = 0;
    int   attacking_team  = 0;
    int   damage          = 0;
    int   damage_after    = 0;
};

// ---------------------------------------------------------------------------
// Bundle type classification for items that can be picked up as bundles
// ---------------------------------------------------------------------------

enum class BundleType : uint8_t { Unknown, Flag, RepairKit, VineSeed };

inline BundleType LookupBundleType(int mapId, uint32_t itemId)
{
    if (IsFlagItemId(mapId, itemId)) return BundleType::Flag;
    const char* name = LookupMapItem(mapId, itemId);
    if (!name) return BundleType::Unknown;
    if (strcmp(name, "Repair Kit") == 0) return BundleType::RepairKit;
    if (strcmp(name, "Vine Seed") == 0)  return BundleType::VineSeed;
    return BundleType::Unknown;
}

// ---------------------------------------------------------------------------
// Lifecycle events (AGENT_ADD / AGENT_REMOVE from lifecycle_events.txt)
// ---------------------------------------------------------------------------

struct LifecycleEvent
{
    float    time = 0.f;
    int      agent_id = 0;
    bool     isAdd = true;
    uint32_t agent_type = 0;
    int      type_code = 0;
    float    x = 0, y = 0, z = 0;
    float    speed = 0;
};

// ---------------------------------------------------------------------------
// Map object manipulation events (from manipulate_map_object_events.txt)
// ---------------------------------------------------------------------------

struct MapObjectEvent
{
    float    time = 0.f;
    uint32_t object_id = 0;
    int      animation_type = 0;
    int      animation_stage = 0;
    bool     isState = false;
    int      state = 0;
    int      unk1 = 0;
};

// ---------------------------------------------------------------------------
// Flag events (from flag_events.txt — GvG flag StoC packets)
// ---------------------------------------------------------------------------

struct FlagPickupEvent
{
    float time = 0.f;
    int   item_id = 0;
    int   player_agent_id = 0;
    int   team_code = 0;
    std::string raw_line;
};

struct FlagDropEvent
{
    float time = 0.f;
    int   player_agent_id = 0;
    int   team_code = 0;
    std::string raw_line;
};

struct FlagStateEvent
{
    float    time = 0.f;
    int      team_code = 0;
    int      item_id = 0;
    uint32_t state = 0;
    std::string raw_line;
};

struct FlagItemEvent
{
    float    time = 0.f;
    int      item_id = 0;
    int      model_id = 0;
    uint32_t extra_id = 0;
    int      type = 0;
    std::string raw_line;
};

struct FlagStandEvent
{
    float time = 0.f;
    int   stand_agent_id = 0;
    int   sub_field = 0;
    int   value = 0;
    std::string raw_line;
};

struct FlagSpawnEvent
{
    float time = 0.f;
    int   agent_id = 0;
    int   unk = 0;
    int   object_id = 0;
    std::string raw_line;
};

struct FlagAnnounceEvent
{
    float time = 0.f;
    int   action = 0;      // 0=RETURN, 1=STICK
    int   template_id = 0;
    int   team = 0;        // 0=unknown, 1=blue, 2=red
    std::string raw_line;
};

struct FlagEventData
{
    std::vector<FlagPickupEvent>   pickups;
    std::vector<FlagDropEvent>     drops;
    std::vector<FlagStateEvent>    states;
    std::vector<FlagItemEvent>     items;
    std::vector<FlagStandEvent>    stands;
    std::vector<FlagSpawnEvent>    spawns;
    std::vector<FlagAnnounceEvent> announces;

    bool empty() const {
        return pickups.empty() && drops.empty() && states.empty()
            && items.empty() && stands.empty() && spawns.empty()
            && announces.empty();
    }
    int totalCount() const {
        return static_cast<int>(pickups.size() + drops.size() + states.size()
            + items.size() + stands.size() + spawns.size() + announces.size());
    }
};

// ---------------------------------------------------------------------------
// Bundle carry interval — tracks what a player is holding over time
// ---------------------------------------------------------------------------

struct BundleCarryInterval
{
    float      startTime = 0.f;
    float      endTime   = FLT_MAX;
    BundleType type      = BundleType::Unknown;
    int        itemAgentId = -1;
};

struct StoCData
{
    std::vector<AgentMovementEvent>     agentMovement;
    std::vector<SkillActivationEvent>   skill;
    std::vector<AttackSkillEvent>       attackSkill;
    std::vector<BasicAttackEvent>       basicAttack;
    std::vector<CombatEvent>            combat;
    std::vector<JumboMessageEvent>      jumbo;
    std::vector<UnknownEvent>           unknown;
    std::vector<StoCLordDamageEvent>    lordDamage;
    std::vector<LifecycleEvent>         lifecycle;
    std::vector<MapObjectEvent>         mapObject;
    FlagEventData                       flagEvents;
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
    InterpolationMode mode = InterpolationMode::OriginalLinear;
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

    // Playback state (Phase 3 timeline controller)
    bool  isPlaying      = false;
    bool  loopPlayback   = false;
    float playbackSpeed  = 1.0f;
    int   speedIndex     = 2;        // index into {0.25, 0.5, 1, 2, 4, 8}
};
