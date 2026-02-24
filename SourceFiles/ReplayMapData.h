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

enum class AgentType : uint8_t { Unknown, Player, NPC, Gadget };

inline const char* AgentTypeName(AgentType t)
{
    switch (t) {
    case AgentType::Player:  return "Player";
    case AgentType::NPC:     return "NPC";
    case AgentType::Gadget:  return "Gadget";
    default:                 return "Unknown";
    }
}

inline const char* LookupNpcName(uint32_t modelId)
{
    switch (modelId) {
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
    case 4334: return "Acid Trap";
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
};
