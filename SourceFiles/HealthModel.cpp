#include "pch.h"
#include "HealthModel.h"
#include "ReplayMapData.h"
#include "EquipmentHealth.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Base health is 100 at level 1 and rises 20 per level: 20*level + 80, which is the documented
    // 480 at level 20 rather than 500. The +48 and -72 that recur in the packet data are exactly
    // 10% and 15% of it, which is also how we know morale applies to the base and not the total.
    constexpr int kBaseAtLevel20 = 480;

    // Primary Dervishes carry an innate 25 health on top.
    constexpr int kDervishBonus = 25;
    constexpr int kProfDervish = 10;

    // Health from level alone. Morale is a percentage of THIS, not of the total.
    int LevelHealth(const AgentReplayData& ard)
    {
        const int level = ard.playerLevel > 0 ? ard.playerLevel : 20;
        return 20 * level + 80;
    }

    int BaseHealth(const AgentReplayData& ard)
    {
        int base = LevelHealth(ard);
        if (ard.primaryProf == kProfDervish) base += kDervishBonus;
        return base;
    }

    // Morale and death penalty are a percentage of the level health, so the step is 4.8 per percent
    // at level 20. Confirmed against camera readings: one player's readings sat at +10, +19, +24 and
    // +72 above his own baseline, which is 2%, 4%, 5% and 15% of 480.
    //
    // The Dervish bonus is deliberately excluded. Two Dervishes measured at a full morale boost read
    // +48, not the +50 that 10% of their 505 would give -- so the innate 25 is added after morale is
    // worked out, not before. Including it left both of their armour totals at values no combination
    // of runes can produce.
    int MoraleHealth(const AgentReplayData& ard, int moralePercent)
    {
        return (int)std::lround((double)LevelHealth(ard) * (double)moralePercent / 100.0);
    }

    // A reading is only an anchor when it can be attributed to a state. Two kinds qualify:
    //
    //   * the recorder marked it live: the camera was on this agent at this instant
    //   * the value just changed: the server pushed it now, so it describes now
    //
    // Neither is sufficient on its own -- see MarkFreshReadings, which is where the real test is.
    bool IsUsableAnchor(const AgentSnapshot& s, uint32_t previousMaxHp)
    {
        if (s.max_hp == 0 || s.is_dead) return false;
        return s.max_hp_is_live || s.max_hp != previousMaxHp;
    }

    // Recover the unwounded maximum from a wounded reading.
    //
    // Deep Wound removes min(100, total/5) and truncates, which is invertible: a wounded reading
    // measures the player just as well as a healthy one, and discarding it throws away the readings
    // of whoever was under the most pressure. Worth 156 and 108 extra readings on the two sampled
    // matches, 14% and 28% more than the clean ones alone.
    //
    // The inverse is exact except where two totals collapse onto one wounded value: truncation puts
    // 5k+4 and 5k+5 on the same number, and the cap does the same at 499 and 500. Those are dropped
    // rather than guessed, because a reading that could be either of two totals a health point
    // apart splits the armour vote for nothing when readings are already plentiful.
    bool UnwoundHealth(uint32_t wounded, uint32_t& total)
    {
        if (wounded == 0) return false;

        uint32_t only = 0;
        int hits = 0;
        for (uint32_t t = wounded; t <= wounded + 100; ++t)
        {
            if (AgentReplayData::ApplyDeepWound(t) != wounded) continue;
            only = t;
            if (++hits > 1) return false;   // ambiguous
        }
        if (hits != 1) return false;

        total = only;
        return true;
    }

    // Deep Wound and the health it removes arrive on separate packets, and around a transition the
    // two are out of phase in no fixed direction: one measured player's flag went 0, 1, 0 across
    // three snapshots while the wounded value landed on the third, so asking which came first
    // cannot separate them. Neither is trusted for a beat afterwards.
    constexpr float kWoundSettleSeconds = 1.0f;
}

namespace HealthModel
{
    int WeaponSetHealth(const Equipment::Data& equipment, const AgentSnapshot& snap, bool* known)
    {
        const Equipment::HealthMods mods =
            Equipment::HealthFromWeaponSet(equipment, snap.weapon_item_id, snap.offhand_item_id);

        if (known) *known = mods.known;

        // Stances are not in the snapshot -- they have to be reconstructed from cast events, which
        // is Phase 3 work. Until then a "+45 while in a Stance" mod is simply not counted, so the
        // model under-reports rather than inventing health the player may not have had.
        return mods.At(snap.has_enchantment, snap.has_hex || snap.has_degen_hex, false);
    }

    // Which recorded readings describe the moment they sit on.
    //
    // One rule, applied to every term at once: a recorded value describes now when every term of
    // the recipe that could have moved it reads the same now as it read when the server pushed the
    // value. Comparing the terms rather than the times is deliberate -- swapping between two sets
    // with identical health, or taking a death and a boost that cancel, changes nothing the value
    // could have noticed, and there is no reason to throw such a reading away.
    //
    // This replaces three separate tests, each with its own settle window and all of them scanning
    // back a fixed eight seconds. That window was sound while anchors were only ever values the
    // server had just pushed, which are fresh by construction. The camera sampler broke it: it
    // marks hundreds of readings per player live, and `live` says where the camera was, not that
    // the number was refreshed. A value minutes old would sail through, because nothing had
    // changed inside the last eight seconds of it.
    //
    // Measured on the two matches recorded with the sampler, as the share of reading groups that
    // disagree with themselves on one weapon set -- where armour and mods cancel, so only staleness
    // can move the number:
    //
    //             Warrior's Isle    Corrupted Isle
    //   before        30%               27%
    //   after         15%                3%     ... and all but one of the survivors differ by 1,
    //
    // which is rounding, not staleness. The errors removed were exactly one morale step (48 for a
    // boost, 72 for a death, 268 readings across the two matches), a stale weapon set (30, the
    // shield most of the field runs) and a stale Deep Wound (99 or 100, the cap).
    void MarkFreshReadings(AgentReplayData& ard, const Inputs& inputs)
    {
        ard.maxHpDescribesNow.assign(ard.snapshots.size(), 0);
        if (!inputs.equipment || ard.snapshots.empty()) return;

        // Index where the current value's run began: the instant the server last pushed it.
        int runStart = 0;
        float woundChangedAt = -1.f;

        for (int i = 0; i < (int)ard.snapshots.size(); ++i)
        {
            const AgentSnapshot& now = ard.snapshots[i];
            if (i > 0)
            {
                if (now.max_hp != ard.snapshots[i - 1].max_hp) runStart = i;
                if (now.has_deep_wound != ard.snapshots[i - 1].has_deep_wound)
                    woundChangedAt = now.time;
            }
            if (woundChangedAt >= 0.f && now.time - woundChangedAt < kWoundSettleSeconds) continue;

            const AgentSnapshot& then = ard.snapshots[runStart];
            if (then.has_deep_wound != now.has_deep_wound) continue;

            bool thenKnown = false, nowKnown = false;
            if (WeaponSetHealth(*inputs.equipment, then, &thenKnown) !=
                WeaponSetHealth(*inputs.equipment, now, &nowKnown)) continue;
            if (!thenKnown || !nowKnown) continue;

            if (inputs.moralePercent &&
                inputs.moralePercent(ard, then.time) != inputs.moralePercent(ard, now.time)) continue;
            if (inputs.shrineBonus &&
                inputs.shrineBonus(ard, then.time) != inputs.shrineBonus(ard, now.time)) continue;

            ard.maxHpDescribesNow[i] = 1;
        }
    }

    bool DescribesNow(const AgentReplayData& ard, int idx)
    {
        return idx >= 0 && idx < (int)ard.maxHpDescribesNow.size() && ard.maxHpDescribesNow[idx];
    }

    void SolveArmour(std::unordered_map<int, AgentReplayData>& agents, const Inputs& inputs)
    {
        for (auto& [id, ard] : agents)
        {
            ard.solvedArmourHealth = 0;
            ard.armourSolved = false;
            ard.armourSupport = 0;
            ard.armourObservations = 0;
            ard.maxHpDescribesNow.clear();
        }

        if (!inputs.equipment || !inputs.equipment->loaded) return;

        for (auto& [id, ard] : agents)
        {
            if (ard.type != AgentType::Player) continue;

            MarkFreshReadings(ard, inputs);

            // Armour never changes during a match, so every anchor is a measurement of the same
            // integer. Disagreement means one of the other terms was wrong at that instant, not
            // that the armour changed -- so take the value the readings agree on, not an average.
            std::unordered_map<int, int> votes;

            uint32_t previousMaxHp = 0;
            for (int idx = 0; idx < (int)ard.snapshots.size(); ++idx)
            {
                const AgentSnapshot& snap = ard.snapshots[idx];
                const uint32_t seenBefore = previousMaxHp;
                previousMaxHp = snap.max_hp;

                if (!IsUsableAnchor(snap, seenBefore)) continue;
                if (!DescribesNow(ard, idx)) continue;

                // A wounded reading measures the same recipe with a known amount taken off the
                // end, so put it back before solving rather than dropping the reading.
                uint32_t measured = snap.max_hp;
                if (snap.has_deep_wound && !UnwoundHealth(snap.max_hp, measured)) continue;

                bool weaponKnown = false;
                const int weapon = WeaponSetHealth(*inputs.equipment, snap, &weaponKnown);
                if (!weaponKnown) continue;

                const int base = BaseHealth(ard);
                const int morale = inputs.moralePercent
                    ? MoraleHealth(ard, inputs.moralePercent(ard, snap.time)) : 0;
                const int shrine = inputs.shrineBonus ? inputs.shrineBonus(ard, snap.time) : 0;

                votes[(int)measured - base - weapon - shrine - morale]++;
                ard.armourObservations++;
            }

            if (votes.empty()) continue;

            int bestValue = 0, bestCount = 0;
            for (const auto& [value, count] : votes)
            {
                // Ties break toward the smaller armour value: over-reporting health is the more
                // misleading error, since it makes a player look tankier than they were.
                if (count > bestCount || (count == bestCount && value < bestValue))
                {
                    bestCount = count;
                    bestValue = value;
                }
            }

            ard.solvedArmourHealth = bestValue;
            ard.armourSupport = bestCount;
            ard.armourSolved = true;
        }
    }

    Breakdown At(const AgentReplayData& ard, float t, const Inputs& inputs)
    {
        Breakdown out;

        const int idx = ard.snapshotIndexAtTime(t);
        if (idx < 0 || !inputs.equipment || !inputs.equipment->loaded)
        {
            out.source = Source::Fallback;
            return out;
        }
        const AgentSnapshot& snap = ard.snapshots[idx];

        out.base = BaseHealth(ard);
        out.weaponSet = WeaponSetHealth(*inputs.equipment, snap, &out.weaponSetKnown);
        out.shrine = inputs.shrineBonus ? inputs.shrineBonus(ard, t) : 0;
        out.morale = inputs.moralePercent
            ? MoraleHealth(ard, inputs.moralePercent(ard, t)) : 0;
        out.armourSolved = ard.armourSolved;
        out.armour = ard.armourSolved ? ard.solvedArmourHealth : 0;

        if (!ard.armourSolved)
        {
            out.source = Source::Fallback;
            return out;
        }

        int total = out.base + out.armour + out.weaponSet + out.shrine + out.morale;
        if (total < 1) total = 1;

        // Deep Wound is applied last and truncates its 20%, capped at 100. Settled on two
        // camera-fresh transitions that separate truncation from rounding: 458 -> 367 (cut 91) and
        // 488 -> 391 (cut 97).
        if (snap.has_deep_wound)
        {
            const int cut = std::min(100, total / 5);
            out.deepWound = -cut;
            total -= cut;
            if (total < 1) total = 1;
        }

        out.total = (uint32_t)total;

        // A live reading beats the model: it is the server's own number, not our reconstruction of
        // it. Reported as an anchor so the UI can say so, and so a mismatch is visible rather than
        // quietly averaged away.
        // Only defer to the recorded value when it can be attributed to the state at this instant.
        // A live reading that predates the current weapon set describes the previous one, and
        // showing it is exactly the bug the model exists to fix.
        out.source = (snap.max_hp_is_live && snap.max_hp > 0 && DescribesNow(ard, idx))
                        ? Source::Anchor : Source::Modelled;
        if (out.source == Source::Anchor) out.total = snap.max_hp;

        return out;
    }
}

namespace HealthModel
{
    std::vector<ArmourBuild> ArmourCandidates(int totalHealth, size_t maxResults)
    {
        // Five armour pieces, so five rune slots and five insignia slots. Vigor does not stack
        // with itself, so at most one of it; the rest of the slots go to attribute runes (bought
        // with health) and Runes of Vitae (10 health each).
        constexpr int kPieces = 5;
        static const int kVigorTiers[] = { 50, 41, 30, 0 };

        std::vector<ArmourBuild> out;

        for (int vigor : kVigorTiers)
        {
            for (int sup = 0; sup <= kPieces; ++sup)
            {
                for (int maj = 0; maj + sup <= kPieces; ++maj)
                {
                    const int used = (vigor ? 1 : 0) + sup + maj;
                    if (used > kPieces) continue;

                    for (int vitae = 0; vitae <= kPieces - used; ++vitae)
                    {
                        const int withoutSurvivor = vigor - sup * 75 - maj * 35 + vitae * 10;

                        // Survivor reaches 40 across the five pieces in steps of 5 (15 on the
                        // chest, 10 on the legs, 5 on each of the other three), so every multiple
                        // of 5 up to 40 is attainable. 0 is tried first and ranked first.
                        for (int survivor = 0; survivor <= 40; survivor += 5)
                        {
                            if (withoutSurvivor + survivor != totalHealth) continue;

                            ArmourBuild b;
                            b.vigor = vigor;
                            b.vitae = vitae;
                            b.majorRunes = maj;
                            b.superiorRunes = sup;
                            b.survivor = survivor;

                            // Plain English, as a reader would say it out loud.
                            std::vector<std::string> parts;
                            if (vigor == 50)      parts.push_back("Superior Rune of Vigor");
                            else if (vigor == 41) parts.push_back("Major Rune of Vigor");
                            else if (vigor == 30) parts.push_back("Minor Rune of Vigor");
                            else                  parts.push_back("no vigor rune");
                            if (vitae) parts.push_back(vitae == 1 ? "1 Rune of Vitae"
                                                                  : std::format("{} Runes of Vitae", vitae));
                            if (sup)   parts.push_back(sup == 1 ? "1 superior rune"
                                                                : std::format("{} superior runes", sup));
                            if (maj)   parts.push_back(maj == 1 ? "1 major rune"
                                                                : std::format("{} major runes", maj));
                            if (survivor) parts.push_back(std::format("Survivor insignias (+{})", survivor));

                            for (size_t i = 0; i < parts.size(); ++i)
                                b.label += (i ? ", " : "") + parts[i];

                            out.push_back(std::move(b));
                        }
                    }
                }
            }
        }

        // Most plausible first: no Survivor, then superior vigor, then the fewest runes.
        std::stable_sort(out.begin(), out.end(), [](const ArmourBuild& a, const ArmourBuild& b) {
            const auto rank = [](const ArmourBuild& x) {
                return std::tuple{ x.survivor != 0, x.vigor != 50,
                                   x.vitae + x.majorRunes + x.superiorRunes };
            };
            return rank(a) < rank(b);
        });

        if (out.size() > maxResults) out.resize(maxResults);
        return out;
    }
}
