#pragma once
#include <vector>
#include <unordered_map>

// Forward declarations
struct AgentReplayData;
struct CombatEvent;

// Solves per-player max_hp timelines from combat DAMAGE/HEAL packet decimals.
//
// A DAMAGE/HEAL value is a fraction of the target's max_hp; the real HP delta
// is an integer, so fraction * max_hp is integer-exact only when max_hp is
// correct. This derives an authoritative max_hp per stable time window and also
// reveals step changes (Deep Wound, weapon swaps). Results are written into
// each player agent's solvedMaxHp vector; camera-observed max_hp remains a
// seed/fallback. Additive and backward-compatible: players with too few hits
// simply get no solved segments and fall through to the existing fallback.
void SolveMaxHpTimelines(
    std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatEvent>& combatEvents);
