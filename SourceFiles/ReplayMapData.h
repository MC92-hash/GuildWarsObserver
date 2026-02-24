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
};

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

    float maxReplayTime = 0.f;
};
