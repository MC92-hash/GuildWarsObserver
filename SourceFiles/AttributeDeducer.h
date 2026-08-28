#pragma once
#include <vector>
#include <unordered_map>
#include <functional>

#include "AttributeModel.h"

// Forward declarations
struct AgentReplayData;
struct CombatLogRow;
class SkillDatabaseView;

// The combat-log evidence provider (genres G1-G3 plus Divine Favor).
//
// This was the whole of attribute deduction in v1: it scanned the combat log, kept the packets a
// skill's breakpoint table could explain, voted per (caster, attribute) and reported the mode.
// Everything it knows is still true, so none of it was thrown away - but a vote is a decision,
// and deciding one attribute at a time is what kept the old pass from using the 200-point rule
// or the runes. So it now reports what it saw instead of what it concluded: one Evidence per
// accepted packet, carrying the FULL set of ranks that fit rather than the nearest one, and
// AttributeModel::SolveAll decides.
//
// What it still owns, unchanged: the armour-ignoring (type 55) damage gate, the Deep Wound skip
// on heal targets, the requirement that the target's max HP be authoritative rather than
// estimated, the profession gates, and the two-step Divine Favor pass that reads a primary
// Monk's flat bonus packet before aligning that Monk's own heals.
void CollectCombatEvidence(
    const std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatLogRow>& combatLog,
    const SkillDatabaseView& skillView,
    const std::function<std::pair<uint32_t, bool>(int agentId, float t)>& resolveMaxHp,
    std::unordered_map<int, std::vector<AttributeModel::Evidence>>& out);
