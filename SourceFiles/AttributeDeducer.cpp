#include "pch.h"
#include "AttributeDeducer.h"
#include "ReplayMapData.h"
#include "SkillDatabase.h"

#include <unordered_set>
#include <cmath>
#include <algorithm>

namespace
{
    // The former hand-curated whitelist is gone. Usable skills are now derived
    // automatically at load time by SkillDatabase::ClassifyDeductionUsability
    // (see the rationale comment there). This file only consumes the resulting
    // per-skill flags: deductionUsable, deductionKind, dedV0/dedV15, dfConfounded.

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

    // Cumulative Guild Wars attribute-point cost to reach a given rank.
    // Ranks above 12 come from runes/headgear and cost no extra points, so the
    // table is capped at 12 for budget purposes.
    int AttributePointCost(int rank)
    {
        static const int kCumulative[] = {
            0, 1, 3, 6, 10, 15, 21, 28, 37, 48, 61, 77, 97
        };
        if (rank < 0) return 0;
        if (rank > 12) rank = 12;
        return kCumulative[rank];
    }

    // Divine Favor flat companion-heal bonus per rank: round(3.2 * rank),
    // r in [0..16] (source: GWW). The game logs this as its OWN separate HEAL
    // combat event, co-timed with the spell, on every Monk spell cast on an
    // ally target -- it is not baked into the spell's own heal value.
    const int kDfBonus[17] = {
        0, 3, 6, 10, 13, 16, 19, 22, 26, 29, 32, 35, 38, 42, 45, 48, 51
    };

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

    // Match a value to the nearest attribute breakpoint over r in [0..16], where
    // breakpoint(r) = round(v0 + r*(v15-v0)/15). Returns the rank and abs error.
    void NearestBreakpoint(double value, float v0, float v15, int& rank, double& err)
    {
        rank = -1;
        err = 1e30;
        for (int r = 0; r <= 16; ++r) {
            double bp = std::round((double)v0 + (double)r * ((double)v15 - (double)v0) / 15.0);
            double e = std::fabs(value - bp);
            if (e < err) { err = e; rank = r; }
        }
    }

    // A single deduction-usable Monk heal observation cast by a PRIMARY Monk,
    // collected for PASS B step 2 (Healing / Protection alignment).
    struct DfHealObs
    {
        int    attr = 0;   // spell's own attribute (Healing 13 or Protection 15, etc.)
        float  v0 = 0;
        float  v15 = 0;
        double obs = 0;    // observed healed amount (valuePct * authoritative maxHp)
    };
}

std::unordered_map<int, PlayerAttributeProfile> DeduceAttributes(
    const std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatLogRow>& combatLog,
    const SkillDatabaseView& skillView,
    const std::function<std::pair<uint32_t, bool>(int agentId, float t)>& resolveMaxHp)
{
    std::unordered_map<int, PlayerAttributeProfile> result;
    if (!skillView.IsLoaded() || !resolveMaxHp) return result;

    // Accepted ranks grouped per caster, per attribute (PASS A + PASS B).
    // casterId -> (attributeId -> list of accepted ranks)
    std::unordered_map<int, std::unordered_map<int, std::vector<int>>> accepted;

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

    // Estimates that must be flagged low-confidence regardless of internal
    // agreement (populated by a non-confident Divine Favor detection).
    // casterId -> set of attributeIds
    std::unordered_map<int, std::unordered_set<int>> forcedLowConf;

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
        if (!si || !si->deductionUsable) continue;

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
            o.v0 = si->dedV0;
            o.v15 = si->dedV15;
            o.obs = obs;
            monkSkillHealObs[row.casterId].push_back(o);
            continue;
        }

        // -------------------------------------------------------------------
        // PASS A: direct nearest-integer-rank match over r in [0, 16].
        // -------------------------------------------------------------------
        if (si->dedV15 == si->dedV0) continue; // degenerate, cannot rank-match

        int bestRank = -1;
        double bestErr = 1e30;
        NearestBreakpoint(obs, si->dedV0, si->dedV15, bestRank, bestErr);
        if (bestRank < 0 || bestErr > 0.75) continue;

        // Profession gate.
        const int attr = si->attribute;
        const int attrProf = SkillDatabase::GetProfessionForAttribute(attr);
        if (attrProf == 0) continue; // no-attribute / title track -> not deducible
        const bool matchesPrimary   = (attrProf == caster.primaryProf);
        const bool matchesSecondary = (attrProf == caster.secondaryProf);
        if (!matchesPrimary && !matchesSecondary) continue;
        if (IsPrimaryOnlyAttribute(attr) && !matchesPrimary) continue;

        accepted[row.casterId][attr].push_back(bestRank);
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
    // casterId -> detected DF-bonus absolute value (kDfBonus[rank]), used by
    // step 2 to exclude bonus-event rows from the skill-heal alignment.
    std::unordered_map<int, int> dfBonusValue;

    for (auto& [casterId, roundedVals] : monkAllHealRounded)
    {
        if (roundedVals.empty()) continue;

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
                int diff = v - kDfBonus[d];
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

        const bool confident = (bestCount >= 5 && tieCount == 1);

        auto& casterAccepted = accepted[casterId];
        for (int i = 0; i < bestCount; ++i)
            casterAccepted[16].push_back(bestDf);
        if (!confident) forcedLowConf[casterId].insert(16);

        dfBonusValue[casterId] = kDfBonus[bestDf];
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

        auto& casterAccepted = accepted[casterId];
        for (const DfHealObs& o : obsList)
        {
            if (o.v15 == o.v0) continue; // degenerate, cannot rank-match

            // This row's skill-matched combat event actually turned out to be
            // the Divine Favor bonus (not the spell's own base heal) -- skip
            // it here, it was already counted in step 1's pool.
            const int roundedObs = (int)std::llround(o.obs);
            int diffToBonus = roundedObs - bonusVal;
            if (diffToBonus < 0) diffToBonus = -diffToBonus;
            if (haveBonus && diffToBonus <= 1) continue;

            int r = -1;
            double e = 1e30;
            NearestBreakpoint(o.obs, o.v0, o.v15, r, e);
            if (r < 0 || e > 0.75) continue;

            // Profession gate, same as PASS A.
            const int attrProf = SkillDatabase::GetProfessionForAttribute(o.attr);
            if (attrProf == 0) continue;
            const bool matchesPrimary   = (attrProf == caster.primaryProf);
            const bool matchesSecondary = (attrProf == caster.secondaryProf);
            if (!matchesPrimary && !matchesSecondary) continue;
            if (IsPrimaryOnlyAttribute(o.attr) && !matchesPrimary) continue;

            casterAccepted[o.attr].push_back(r);
        }
    }

    // -----------------------------------------------------------------------
    // Aggregate.
    // -----------------------------------------------------------------------
    for (auto& [casterId, byAttr] : accepted)
    {
        PlayerAttributeProfile profile;
        int totalCost = 0;

        const auto flcIt = forcedLowConf.find(casterId);

        for (auto& [attr, ranks] : byAttr)
        {
            if (ranks.empty()) continue;

            // Mode of ranks; ties resolved toward the higher rank.
            std::unordered_map<int, int> counts;
            for (int r : ranks) counts[r]++;
            int modeRank = ranks.front();
            int modeCount = 0;
            for (auto& [r, c] : counts) {
                if (c > modeCount || (c == modeCount && r > modeRank)) {
                    modeCount = c;
                    modeRank = r;
                }
            }

            int agreeing = 0;
            for (int r : ranks) if (r == modeRank) ++agreeing;

            // Standard deviation of accepted ranks.
            double mean = 0.0;
            for (int r : ranks) mean += r;
            mean /= (double)ranks.size();
            double var = 0.0;
            for (int r : ranks) { double d = r - mean; var += d * d; }
            var /= (double)ranks.size();

            AttributeEstimate est;
            est.attributeId = attr;
            est.rank = modeRank;
            est.observations = (int)ranks.size();
            est.agreeing = agreeing;
            est.spread = (float)std::sqrt(var);
            est.lowConfidence = (est.agreeing * 2 < est.observations);
            if (flcIt != forcedLowConf.end() && flcIt->second.count(attr))
                est.lowConfidence = true;
            profile.attributes.push_back(est);

            profile.totalCleanObservations += est.observations;
            totalCost += AttributePointCost(modeRank);
        }

        if (profile.attributes.empty()) continue;

        std::sort(profile.attributes.begin(), profile.attributes.end(),
                  [](const AttributeEstimate& a, const AttributeEstimate& b) {
                      return a.attributeId < b.attributeId;
                  });

        // Soft budget sanity: a real level-20 character has ~200 attribute
        // points. A larger implied spend means our ranks are inconsistent.
        profile.budgetPlausible = (totalCost <= 200);

        result[casterId] = std::move(profile);
    }

    return result;
}
