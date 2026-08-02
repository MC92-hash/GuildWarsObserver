#pragma once
#include <vector>
#include <unordered_map>

// Forward declarations
struct AgentReplayData;
struct CombatEvent;
struct CombatLogRow;
class SkillDatabaseView;

// Solves per-player max_hp from combat DAMAGE/HEAL packet decimals, keyed by
// the equipped weapon set.
//
// A DAMAGE/HEAL value is a fraction of the target's max_hp; the real HP delta
// is an integer, so fraction * max_hp is integer-exact only when max_hp is
// correct. Results are written into each player agent's solvedMaxHpByWeaponSet
// map; camera-observed max_hp remains the preferred source when the recording
// carries it. Additive and backward-compatible: weapon sets with too few hits
// simply get no entry and fall through to the existing fallback.
void SolveMaxHpTimelines(
    std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatEvent>& combatEvents);

// Refines the same map using the inverse of the attribute breakpoint tables.
//
// The lattice sieve above asks "is fraction * M near an integer?", which every
// candidate M satisfies fairly often. This asks the far sharper question "is
// fraction * M equal to a value this skill can actually produce?" -- the skill
// tables are sparse, so a single clean packet narrows M to a handful of
// candidates and two packets from different skills pin it exactly.
//
// Two channels, strongest first:
//   * Divine Favor -- a Monk's DF bonus arrives as its own heal packet worth
//     round(3.2 * rank). One unknown rank, shared across every ally that Monk
//     touches, so solving it once pins max HP for the whole party at once.
//   * Skill rank tables -- breakpoint(r) = round(v0 + r*(v15-v0)/15) for the
//     skill that produced the packet.
//
// Heals only: healing is not reduced by armour, damage is.
void SolveMaxHpFromSkillBreakpoints(
    std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatLogRow>& combatLog,
    const SkillDatabaseView& skillView);
