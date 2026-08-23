#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations
struct AgentReplayData;
struct AgentSnapshot;
namespace Equipment { struct Data; }

// Builds a player's maximum health forward, from its parts, instead of guessing it backwards from
// packet decimals.
//
//     raw   = 480                     base health at level 20 (20*level + 80)
//           + 25                      primary Dervish only
//           + A                       armour: runes and insignias, one unknown per player
//           + W(t)                    weapon-set mods, from the recorded item words
//           + 120                     Southern Health Shrine, Isle of Wurms, while the team holds it
//     total = raw + morale% * 480     morale and death penalty apply to the BASE, not the total
//     total = total - min(100, total/5)   Deep Wound, truncated
//
// Why forward. The old path (MaxHpSolver) asks which max HP turns a damage percentage into a whole
// number, then patches the misses against a hand-made offset list. It only knows a number where a
// packet lands, it cannot say why the number is what it is, and it keys everything on the weapon
// set -- so a shield that grants health only while hexed reads as noise, because the same set
// yields two different answers. Building the number from its parts fixes all three: there is an
// answer at every instant, it can be explained term by term, and the offsets stop being magic
// (-30 is a weapon mod, +48 is a morale boost, -72 is a death penalty).
//
// Every term above is known or measurable except A, and A is a single integer per player for the
// whole match, because nobody changes armour mid-GvG. So one camera reading, minus the terms we
// already know, gives A -- and from then on the model can state the player's maximum at any
// instant, including the long stretches when the camera is looking elsewhere.
//
// Design reference: gwobserver-private/MaxHpModelV2Plan.md
namespace HealthModel
{
    // Where a number came from. Callers should show uncertainty rather than hide it, and must
    // never let a modelled value overwrite a measured one.
    enum class Source : uint8_t
    {
        None,
        Anchor,    // a camera-fresh reading at this very instant: measured, not inferred
        Modelled,  // built from the recipe with a solved armour value
        Fallback,  // the model could not run; the caller should use its old chain
    };

    struct Breakdown
    {
        uint32_t total = 0;

        int base = 0;        // 480, or 505 for a primary Dervish
        int armour = 0;      // A, solved per player
        int weaponSet = 0;   // W(t) at this instant, conditionals resolved
        int shrine = 0;      // +120 while the team holds the Isle of Wurms shrine
        int morale = 0;      // signed health delta from morale or death penalty
        int deepWound = 0;   // negative when Deep Wound is active

        Source source = Source::None;
        bool armourSolved = false;   // false => `armour` is 0 because we do not know it
        bool weaponSetKnown = false; // false => an item's mods never arrived
    };

    // Everything the model needs that lives outside the agent data. Passed as callbacks for the
    // same reason AttributeDeducer takes its max-HP lookup that way: the morale and shrine
    // timelines belong to ReplayWindow, and the model has no business reaching into it.
    struct Inputs
    {
        const Equipment::Data* equipment = nullptr;

        // Morale in percent, -60..+10, for this agent at this time.
        std::function<int(const AgentReplayData&, float)> moralePercent;

        // Health granted by a captured shrine, 0 on every map but the Isle of Wurms.
        std::function<int(const AgentReplayData&, float)> shrineBonus;
    };

    // Health a weapon set contributes at one instant, with the conditional mods resolved against
    // the snapshot's own flags. `known` is false when an item's mods never arrived, which is a gap
    // in knowledge rather than an absence of mods.
    int WeaponSetHealth(const Equipment::Data& equipment, const AgentSnapshot& snap, bool* known);

    // Solves one armour value per player from the camera-fresh readings, and stores it on the
    // agent. A reading is usable only when the recorder marked it live: a stale max_hp belongs to
    // an earlier weapon set, morale state or death-penalty rank, and would poison the solve.
    //
    // Needs recordings that carry both the equipment stream and the live flag. Older ones simply
    // solve nothing, and every caller falls through to the existing chain.
    void SolveArmour(std::unordered_map<int, AgentReplayData>& agents, const Inputs& inputs);

    // The player's maximum health at time t, with its terms. Returns Source::Fallback when armour
    // was never solved, in which case `total` is 0 and the caller should use its own chain.
    Breakdown At(const AgentReplayData& ard, float t, const Inputs& inputs);

    // ─── Reading armour back as runes and insignias ──────────────────────────────────────────
    //
    // The recording carries a player's armour pieces -- their skin and their dye -- but no mods on
    // them at all: the server never sends another player's runes or insignias, and the item
    // definitions arrive with an empty mod list. So the health is measured and the build that
    // produced it is not. All we can honestly do is enumerate the builds that reach the measured
    // total, and say how many there are.
    struct ArmourBuild
    {
        int vigor          = 0;  // 0, 30, 41 or 50 -- vigor does not stack, so at most one
        int vitae          = 0;  // Runes of Vitae, 10 health each
        int majorRunes     = 0;  // attribute runes bought with 35 health each
        int superiorRunes  = 0;  // attribute runes bought with 75 health each
        int survivor       = 0;  // health from Survivor insignias, 5 to 40 across five pieces
        std::string label;       // one-line summary, e.g. "superior vigor + 2 vitae"

        // Health given up to attribute runes, always negative or zero.
        int AttributePenalty() const { return -(majorRunes * 35 + superiorRunes * 75); }
    };

    // Builds that reach `totalHealth` exactly, most plausible first, capped at `maxResults`.
    //
    // Ordered rather than merely listed, because the arithmetic alone leaves twenty-odd answers and
    // most of them are builds nobody runs. Three preferences, in order:
    //
    //   * no Survivor insignia. Every armour piece has an insignia slot, and in competitive play it
    //     holds a profession insignia -- worth far more than the 5 to 15 health Survivor gives.
    //     Assuming no Survivor takes the mean number of candidates from 21.7 to 2.3 across the 32
    //     players sampled, and only three of them need one to be explained at all.
    //   * superior vigor, the near-universal choice when health is the point of the rune.
    //   * fewer runes, since a build needing four of them to land on a number is less likely than
    //     one needing a single rune.
    //
    // Empty means nothing reaches the total, which is a statement about the model being wrong at
    // this player's readings rather than about his armour.
    std::vector<ArmourBuild> ArmourCandidates(int totalHealth, size_t maxResults);
}
