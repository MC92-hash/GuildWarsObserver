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

// Snaps a recorded max_hp to the nearby value that makes this packet's
// fraction resolve to a whole number of health.
//
// The recorded max_hp is the correct denominator only about 58% of the time.
// Measured over the local match archive, the error is not noise -- it is a
// small set of known modifiers the snapshot field fails to track:
//
//     -30 / +30   33% of failures   weapon / shield health mod
//     +48         14%               morale boost   (+10% of the 480 base)
//     -72          7%               death penalty  (-15% of the 480 base)
//     -60 / +60    8%               two mods at once
//     +100         5%               Deep Wound cap
//   plus sums of those (+78, +18, -12, -102, ...)
//
// The +48 and -72 are what confirm the base is 480 rather than 500: they are
// exactly 10% and 15% of it.
//
// So the candidate set is a handful of offsets rather than a search, and the
// packet's own fraction picks which one is right. Returns `recorded`
// unchanged when it already works, or when nothing plausible fits.
//
// Measured over the local archive (204k packets): the share of packets that
// resolve to a whole number of health goes from 55.3% to 85.1%. A null model
// feeding random fractions through the same code scores 8.0%, so the gain is
// signal rather than the candidate set absorbing anything put in front of it.
uint32_t CorrectMaxHpForPacket(uint32_t recorded, double fraction);
