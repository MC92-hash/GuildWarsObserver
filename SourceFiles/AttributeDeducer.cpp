#include "pch.h"
#include "AttributeDeducer.h"
#include "ReplayMapData.h"
#include "SkillDatabase.h"
#include "DivineFavor.h"

#include <unordered_set>
#include <cmath>
#include <algorithm>
#include <map>
#include <tuple>

using AttributeModel::Evidence;
using AttributeModel::Genre;

namespace
{
    // The former hand-curated whitelist is gone. Usable skills are now derived
    // automatically at load time by SkillDatabase::ClassifyDeductionUsability
    // (see the rationale comment there). This file only consumes the resulting
    // per-skill flags: deductionUsable, deductionKind, dedV0/dedV15,
    // dedTwoScale/dedV0b/dedV15b, dfConfounded.

    // Attributes that only benefit a character's PRIMARY profession.
    bool IsPrimaryOnlyAttribute(int attr)
    {
        switch (attr) {
        case 0:  // Fast Casting
        case 6:  // Soul Reaping
        case 12: // Energy Storage
        case 16: // Divine Favor
        case 17: // Strength
        case 23: // Expertise
        case 35: // Critical Strikes
        case 36: // Spawning Power
        case 40: // Leadership
        case 44: // Mysticism
            return true;
        default:
            return false;
        }
    }

    // Nearest snapshot to time t (by absolute time distance).
    const AgentSnapshot* NearestSnapshot(const AgentReplayData& ard, float t)
    {
        if (ard.snapshots.empty()) return nullptr;
        int lo = 0, hi = (int)ard.snapshots.size() - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (ard.snapshots[mid].time < t) lo = mid + 1; else hi = mid;
        }
        int idx = lo;
        if (idx > 0) {
            float dPrev = std::fabs(ard.snapshots[idx - 1].time - t);
            float dCur  = std::fabs(ard.snapshots[idx].time - t);
            if (dPrev < dCur) idx = idx - 1;
        }
        return &ard.snapshots[idx];
    }

    // Which of the model's genres a skill scale kind reports under. LifeLoss arrives on the same
    // damage row as Damage does, so the two share a genre.
    Genre GenreForKind(SkillScaleKind kind)
    {
        switch (kind) {
        case SkillScaleKind::Heal:      return Genre::CombatHeal;
        case SkillScaleKind::LifeSteal: return Genre::CombatLifeSteal;
        default:                        return Genre::CombatDamage;
        }
    }

    // A single deduction-usable Monk heal observation cast by a PRIMARY Monk,
    // collected for PASS B step 2 (Healing / Protection alignment).
    struct DfHealObs
    {
        int    attr = 0;   // spell's own attribute (Healing 13 or Protection 15, etc.)
        int    skillId = 0;
        float  time = 0;
        float  v0 = 0;
        float  v15 = 0;
        bool   twoScale = false;   // the packet may be bp1(r) or bp1(r) + bp2(r)
        float  v0b = 0;
        float  v15b = 0;
        double obs = 0;    // observed healed amount (valuePct * authoritative maxHp)
    };

    // A packet is accepted when its absolute value is within 0.75 of a breakpoint - the same
    // tolerance v1 used, and for the same reason: the game rounds the number it sends, and two
    // roundings (its own and ours) can drift a health point apart.
    constexpr double kPacketTolerance = 0.75;

    // Ether Feast: a heal on the caster, worth 1, 2 or 3 breakpoints. See AttributeRules.cpp.
    constexpr int kSkillEtherFeast = 40;

    // One Energy Surge / Energy Burn cast, reduced to the energy its largest packet implies.
    struct PerEnergyCast
    {
        int      energy = 0;
        uint32_t ranks = 0;
        int      attribute = -1;
        double   damage = 0.0;
        float    time = 0.f;
    };

    // Every rank a packet of this magnitude could have come from, for one skill.
    uint32_t RanksForSkill(float v0, float v15, bool twoScale, float v0b, float v15b, double obs)
    {
        if (v15 == v0) return 0;   // degenerate, cannot rank-match
        return twoScale
            ? AttributeModel::RanksMatchingPairWithin(v0, v15, v0b, v15b, obs, kPacketTolerance)
            : AttributeModel::RanksMatchingWithin(v0, v15, obs, kPacketTolerance);
    }

    // What one packet of this skill is worth.
    //
    // A life-steal scale narrower than 15 points has a step below one health point per rank, so
    // neighbouring ranks land on the same number and the rank set it yields is genuinely wide -
    // Avatar of Grenth's 0...12 cannot separate 11 from 12. The set already says that; the half
    // weight stops a Dervish's six hundred scythe hits from outvoting the sharper genres.
    float PacketWeight(const SkillInfo& si)
    {
        const bool coarseSteal = (si.deductionKind == SkillScaleKind::LifeSteal &&
                                  std::fabs(si.dedV15 - si.dedV0) < 15.f);
        return coarseSteal ? 0.5f : 1.f;
    }

    // Skills whose recorded packets do not follow the breakpoint table their description
    // advertises, so no rank can honestly be read from them.
    //
    // Shield Guardian heals "all allies in earshot" for 10...40 when its block fires. On the
    // reference match it paid out 26, 32 and 34 on three Monks whose other spells measure
    // Protection Prayers 14 - where round(10 + rank * 2) wants 38. Whatever the party-wide heal
    // does on its way into the log it is not that table, and left in it dragged all three Monks
    // down to Protection 8-11 and produced a contradiction against the rest of their bars.
    bool IsUnreliableCombatSkill(int skillId)
    {
        return skillId == 885;   // Shield Guardian
    }

    bool ProfessionGateOk(int attr, const AgentReplayData& caster)
    {
        const int attrProf = SkillDatabase::GetProfessionForAttribute(attr);
        if (attrProf == 0) return false;   // No Attribute / title track -> not deducible
        const bool matchesPrimary   = (attrProf == caster.primaryProf);
        const bool matchesSecondary = (attrProf == caster.secondaryProf);
        if (!matchesPrimary && !matchesSecondary) return false;
        if (IsPrimaryOnlyAttribute(attr) && !matchesPrimary) return false;
        return true;
    }
}

void CollectCombatEvidence(
    const std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatLogRow>& combatLog,
    const SkillDatabaseView& skillView,
    const std::function<std::pair<uint32_t, bool>(int agentId, float t)>& resolveMaxHp,
    std::unordered_map<int, std::vector<Evidence>>& out)
{
    if (!skillView.IsLoaded() || !resolveMaxHp) return;

    // PASS B, step 1 input: every Heal-category observation from a primary
    // Monk caster, regardless of which skill produced it (rounded to the
    // nearest int). Used to directly detect the recurring Divine Favor
    // companion-heal constant. See the row-scan comment below for why this
    // must be collected independently of skill usability / row.skillId.
    // casterId -> rounded abs heal amounts
    std::unordered_map<int, std::vector<int>> monkAllHealRounded;

    // PASS B, step 2 input: single-target Monk heals cast by a PRIMARY Monk
    // on a deduction-usable Heal-kind skill.
    // casterId -> list of {attr, v0, v15, obs}
    std::unordered_map<int, std::vector<DfHealObs>> monkSkillHealObs;

    // R4 input: one entry per Energy Surge / Energy Burn cast, keyed by
    // (caster, skill, millisecond) because every packet of one cast shares a
    // timestamp and only the largest of them is the primary target's.
    std::map<std::tuple<int, int, int>, PerEnergyCast> perEnergyCasts;

    // -----------------------------------------------------------------------
    // Row scan.
    // -----------------------------------------------------------------------
    for (const CombatLogRow& row : combatLog)
    {
        if (row.category != CombatLogCategory::Damage &&
            row.category != CombatLogCategory::Heal)
            continue;

        // Caster must be a known player agent.
        auto casterIt = agents.find(row.casterId);
        if (casterIt == agents.end()) continue;
        const AgentReplayData& caster = casterIt->second;
        if (caster.type != AgentType::Player) continue;

        const bool isDamageRow = (row.category == CombatLogCategory::Damage);

        // ---------------------------------------------------------------
        // PASS B, step 1 collection: every Heal-category row from a primary
        // Monk caster, independent of row.skillId / skill usability.
        //
        // Divine Favor fires its own separate HEAL combat event on every
        // Monk spell cast on an ally, with a constant value of
        // round(3.2 * rank). The merge pass in ReplayWindow.cpp (Pass 2 of
        // combat-log construction) pairs each HEAL/DAMAGE combat event to
        // the nearest still-open Skill row; whichever of the base heal or
        // the DF-bonus event arrives first in the stream claims that row
        // (and gets its skillId), while the OTHER one falls through to
        // become a standalone row with skillId == 0. Both rows still carry
        // category == Heal with a correct caster/target/valuePct, so all
        // Heal rows have to be gathered here -- ahead of the
        // row.skillId <= 0 gate below -- or the bonus events that land in
        // skillId == 0 rows would be invisible to Divine Favor detection.
        // ---------------------------------------------------------------
        if (!isDamageRow && caster.primaryProf == 3)
        {
            bool dwSkip = false;
            auto targetIt = agents.find(row.targetId);
            if (targetIt != agents.end())
            {
                const AgentSnapshot* snap = NearestSnapshot(targetIt->second, row.time);
                if (snap && snap->has_deep_wound) dwSkip = true;
            }
            if (!dwSkip)
            {
                std::pair<uint32_t, bool> mhp = resolveMaxHp(row.targetId, row.time);
                if (mhp.first != 0 && !mhp.second)
                {
                    double absVal = std::fabs((double)row.valuePct) * (double)mhp.first;
                    monkAllHealRounded[row.casterId].push_back((int)std::llround(absVal));
                }
            }
        }

        if (row.skillId <= 0) continue;

        const int sid = skillView.ResolvePvpSkillId(row.skillId);
        const SkillInfo* si = skillView.Get(sid);
        if (!si) continue;

        // -------------------------------------------------------------------
        // R4: the skills whose packet is an energy reading and not a damage
        // table. Energy Surge and Energy Burn describe no damage range at all,
        // so deductionUsable is false for both and the generic pass below
        // never sees them - but 7 (or 9) times the energy the foe lost is the
        // most-repeated measurement of a Mesmer's Domination Magic in the
        // whole recording, and the energy stream only catches a handful of the
        // same casts. So they are read here, ahead of the usability gate,
        // by the rule written for them in AttributeRules.cpp.
        // -------------------------------------------------------------------
        if (isDamageRow && row.damageType == 55 &&
            AttributeModel::IsDamagePerEnergySkill(sid))
        {
            const std::pair<uint32_t, bool> mhp = resolveMaxHp(row.targetId, row.time);
            if (mhp.first == 0 || mhp.second) continue;
            const double obs = std::fabs((double)row.valuePct) * (double)mhp.first;

            int energy = 0;
            const uint32_t ranks =
                AttributeModel::DamagePerEnergyRanks(sid, *si, obs, energy);
            if (ranks == 0) continue;
            if (!ProfessionGateOk(si->attribute, caster)) continue;

            // One cast pays the primary target in full and everyone nearby 75%
            // of that, and all of it arrives on the same timestamp - so the
            // largest packet of the instant is the one that measures the loss.
            const auto key = std::make_tuple(row.casterId, sid,
                                             (int)std::llround(row.time * 1000.0));
            PerEnergyCast& slot = perEnergyCasts[key];
            if (energy > slot.energy)
            {
                slot.energy = energy;
                slot.ranks = ranks;
                slot.attribute = si->attribute;
                slot.damage = obs;
                slot.time = row.time;
            }
            continue;
        }

        // Ether Feast heals the caster for its breakpoint once per point of
        // energy drained, and the foe does not always have the three points
        // the description assumes. The multiplier is decided per packet.
        if (!isDamageRow && sid == kSkillEtherFeast && row.casterId == row.targetId)
        {
            const std::pair<uint32_t, bool> mhp = resolveMaxHp(row.targetId, row.time);
            if (mhp.first == 0 || mhp.second) continue;
            const double obs = std::fabs((double)row.valuePct) * (double)mhp.first;

            int multiplier = 0;
            const uint32_t ranks = AttributeModel::EtherFeastRanks(*si, obs, multiplier);
            if (ranks == 0) continue;
            if (!ProfessionGateOk(si->attribute, caster)) continue;

            Evidence ev;
            ev.attribute = si->attribute;
            ev.ranks = ranks;
            ev.weight = 0.7f;   // the multiplier was inferred, not recorded
            ev.genre = Genre::DamagePerEnergy;
            ev.skillId = sid;
            ev.time = row.time;
            ev.count = 1;
            ev.value = (float)obs;
            out[row.casterId].push_back(ev);
            continue;
        }

        if (!si->deductionUsable) continue;

        // Both ids are tested because the resolve maps a skill onto its PvP split, and a split
        // that does not exist today may exist tomorrow.
        if (IsUnreliableCombatSkill(sid) || IsUnreliableCombatSkill(row.skillId)) continue;

        // Kind / category compatibility gate.
        bool routeToPassB = false;
        switch (si->deductionKind) {
        case SkillScaleKind::Damage:
        case SkillScaleKind::LifeLoss:
            if (!isDamageRow) continue; // damage-only kinds
            break;
        case SkillScaleKind::LifeSteal:
            break; // contributes on both the foe damage row and the caster heal row
        case SkillScaleKind::Heal:
            if (isDamageRow) continue;
            // A single-target Monk heal cast by a PRIMARY Monk is co-timed
            // with a Divine Favor bonus event -> route to PASS B step 2.
            // Secondary monks (X/Mo) have DF rank 0, so their heals stay
            // clean in PASS A.
            if (si->dfConfounded && caster.primaryProf == 3)
                routeToPassB = true;
            break;
        default:
            continue;
        }

        // Armor-ignoring damage only: everything else is armor/skill modified.
        if (isDamageRow && row.damageType != 55) continue;

        // Heals on a Deep Wounded target are reduced by 20% -> unreliable.
        if (!isDamageRow) {
            auto targetIt = agents.find(row.targetId);
            if (targetIt != agents.end()) {
                const AgentSnapshot* snap = NearestSnapshot(targetIt->second, row.time);
                if (snap && snap->has_deep_wound) continue;
            }
        }

        // Resolve the target's max HP; require an authoritative (non-estimated)
        // value so legacy recordings never contribute garbage.
        std::pair<uint32_t, bool> mhp = resolveMaxHp(row.targetId, row.time);
        if (mhp.first == 0 || mhp.second) continue;

        const double maxHp = (double)mhp.first;
        const double obs = std::fabs((double)row.valuePct) * maxHp;

        // Route Divine-Favor-confounded heals to PASS B step 2 instead of
        // ranking here. The spell's own heal event is clean (no DF baked in)
        // -- it just needs the occasional row where the DF-bonus event (not
        // the base heal) ended up paired to this skill filtered back out,
        // which step 2 does once Divine Favor has been detected.
        if (routeToPassB)
        {
            DfHealObs o;
            o.attr = si->attribute;
            o.skillId = sid;
            o.time = row.time;
            o.v0 = si->dedV0;
            o.v15 = si->dedV15;
            o.twoScale = si->dedTwoScale;
            o.v0b = si->dedV0b;
            o.v15b = si->dedV15b;
            o.obs = obs;
            monkSkillHealObs[row.casterId].push_back(o);
            continue;
        }

        // -------------------------------------------------------------------
        // PASS A: every rank whose breakpoint this packet could be.
        // -------------------------------------------------------------------
        const uint32_t ranks = RanksForSkill(si->dedV0, si->dedV15, si->dedTwoScale,
                                             si->dedV0b, si->dedV15b, obs);
        if (ranks == 0) continue;

        if (!ProfessionGateOk(si->attribute, caster)) continue;

        Evidence ev;
        ev.attribute = si->attribute;
        ev.ranks = ranks;
        ev.weight = PacketWeight(*si);
        ev.genre = GenreForKind(si->deductionKind);
        ev.skillId = sid;
        ev.time = row.time;
        ev.count = 1;
        ev.value = (float)obs;
        out[row.casterId].push_back(ev);
    }

    // -----------------------------------------------------------------------
    // R4 emit: one observation per Energy Surge / Energy Burn cast, now that
    // the packets of each cast have been reduced to the largest of them.
    // -----------------------------------------------------------------------
    for (const auto& [key, cast] : perEnergyCasts)
    {
        Evidence ev;
        ev.attribute = cast.attribute;
        ev.ranks = cast.ranks;
        ev.weight = 1.f;   // the packet is exact: an integer times a fixed multiplier
        ev.genre = Genre::DamagePerEnergy;
        ev.skillId = std::get<1>(key);
        ev.time = cast.time;
        ev.count = 1;
        ev.value = (float)cast.damage;
        out[std::get<0>(key)].push_back(ev);
    }

    // -----------------------------------------------------------------------
    // PASS B: Divine Favor + Healing/Protection, per primary-Monk caster.
    //
    // Divine Favor's companion heal is logged by the game as its own HEAL
    // event with a CONSTANT value of round(3.2 * rank), emitted alongside
    // every Monk spell cast on an ally. Because it recurs on every heal cast
    // regardless of which skill was used, it dominates the rounded-value
    // frequency distribution of this caster's heals and can be read directly
    // (step 1) instead of being jointly solved against the spell's own value.
    // The spell's own base heal event is clean (Divine Favor is not baked
    // into it), so it aligns directly to the skill's breakpoints with no
    // subtraction (step 2), once any row that actually captured the
    // DF-bonus event (instead of the base heal) is excluded.
    // -----------------------------------------------------------------------

    // Step 1: Divine Favor detection.
    // casterId -> detected DF-bonus absolute value (kDivineFavorBonus[rank]), used by
    // step 2 to exclude bonus-event rows from the skill-heal alignment.
    std::unordered_map<int, int> dfBonusValue;

    // Step 1a: adopt the rank MaxHpSolver already solved, where it has one.
    //
    // That pass scores every candidate rank by how many of the Monk's packets
    // it turns into a whole-number health total, which is a sharper test than
    // the frequency mode below -- and it ran against the raw fractions, before
    // any max-HP resolution, so it cannot inherit an error from it. Deriving
    // the rank twice from the same events risked the two disagreeing, with
    // nothing to reconcile them.
    for (const auto& [agentId, ard] : agents)
    {
        if (ard.solvedDivineFavorRank < 0) continue;
        if (ard.type != AgentType::Player || ard.primaryProf != 3) continue;

        const int rank = std::min(16, ard.solvedDivineFavorRank);
        const int votes = std::max(1, ard.solvedDivineFavorSupport);

        Evidence ev;
        ev.attribute = 16;                    // Divine Favor
        ev.ranks = 1u << rank;
        ev.weight = 1.f;
        ev.genre = Genre::DivineFavor;
        ev.skillId = 0;
        ev.time = 0.f;
        ev.count = votes;
        ev.value = (float)kDivineFavorBonus[rank];
        out[agentId].push_back(ev);

        dfBonusValue[agentId] = kDivineFavorBonus[rank];
    }

    for (auto& [casterId, roundedVals] : monkAllHealRounded)
    {
        if (roundedVals.empty()) continue;
        if (dfBonusValue.count(casterId)) continue; // already solved above

        // Count occurrences of each rounded value, then fold each value into
        // the nearest Divine Favor rank (if within +/-1) to get a per-rank
        // vote total. This tolerates the DF-bonus value landing on either of
        // two adjacent integers across different casts (rounding noise).
        std::unordered_map<int, int> freq;
        for (int v : roundedVals) freq[v]++;

        int dfCount[17] = { 0 };
        for (auto& [v, c] : freq)
        {
            int bestD = -1, bestDiff = 2;
            for (int d = 0; d <= 16; ++d)
            {
                int diff = v - kDivineFavorBonus[d];
                if (diff < 0) diff = -diff;
                if (diff < bestDiff) { bestDiff = diff; bestD = d; }
            }
            if (bestD >= 0) dfCount[bestD] += c;
        }

        int bestDf = -1, bestCount = 0, tieCount = 0;
        for (int d = 0; d <= 16; ++d)
        {
            if (dfCount[d] > bestCount) { bestCount = dfCount[d]; bestDf = d; tieCount = 1; }
            else if (dfCount[d] > 0 && dfCount[d] == bestCount) { ++tieCount; }
        }

        if (bestDf < 0 || bestCount < 3) continue; // no clear Divine Favor signal

        // A tie, or a thin distribution, is a weaker reading than the solver's - say so with the
        // weight rather than by hiding the observation.
        const bool confident = (bestCount >= 5 && tieCount == 1);

        Evidence ev;
        ev.attribute = 16;
        ev.ranks = 1u << bestDf;
        ev.weight = confident ? 1.f : 0.5f;
        ev.genre = Genre::DivineFavor;
        ev.skillId = 0;
        ev.time = 0.f;
        ev.count = bestCount;
        ev.value = (float)kDivineFavorBonus[bestDf];
        out[casterId].push_back(ev);

        dfBonusValue[casterId] = kDivineFavorBonus[bestDf];
    }

    // Step 2: Healing / Protection alignment for this caster's deduction-
    // usable Monk heals, now that Divine Favor is known for this caster (or
    // was not detected, in which case no bonus-event exclusion is applied).
    for (auto& [casterId, obsList] : monkSkillHealObs)
    {
        if (obsList.empty()) continue;

        auto casterIt = agents.find(casterId);
        if (casterIt == agents.end()) continue;
        const AgentReplayData& caster = casterIt->second;

        const auto bonusIt = dfBonusValue.find(casterId);
        const bool haveBonus = (bonusIt != dfBonusValue.end());
        const int bonusVal = haveBonus ? bonusIt->second : -1000;

        for (const DfHealObs& o : obsList)
        {
            // This row's skill-matched combat event actually turned out to be
            // the Divine Favor bonus (not the spell's own base heal) -- skip
            // it here, it was already counted in step 1's pool.
            const int roundedObs = (int)std::llround(o.obs);
            int diffToBonus = roundedObs - bonusVal;
            if (diffToBonus < 0) diffToBonus = -diffToBonus;
            if (haveBonus && diffToBonus <= 1) continue;

            const uint32_t ranks =
                RanksForSkill(o.v0, o.v15, o.twoScale, o.v0b, o.v15b, o.obs);
            if (ranks == 0) continue;

            if (!ProfessionGateOk(o.attr, caster)) continue;

            Evidence ev;
            ev.attribute = o.attr;
            ev.ranks = ranks;
            ev.weight = 1.f;
            ev.genre = Genre::CombatHeal;
            ev.skillId = o.skillId;
            ev.time = o.time;
            ev.count = 1;
            ev.value = (float)o.obs;
            out[casterId].push_back(ev);
        }
    }
}
