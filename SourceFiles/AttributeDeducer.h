#pragma once
#include <vector>
#include <unordered_map>
#include <functional>

// Forward declarations
struct AgentReplayData;
struct CombatLogRow;
class SkillDatabaseView;

struct AttributeEstimate
{
    int attributeId = -1;
    int rank = -1;
    int observations = 0;
    int agreeing = 0;
    float spread = 0;
    bool lowConfidence = false;
};

struct PlayerAttributeProfile
{
    std::vector<AttributeEstimate> attributes;
    int totalCleanObservations = 0;
    bool budgetPlausible = true;
};

std::unordered_map<int, PlayerAttributeProfile> DeduceAttributes(
    const std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatLogRow>& combatLog,
    const SkillDatabaseView& skillView,
    const std::function<std::pair<uint32_t, bool>(int agentId, float t)>& resolveMaxHp);
