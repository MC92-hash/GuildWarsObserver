#include "pch.h"
#include "MaxHpSolver.h"
#include "ReplayMapData.h"
#include "SkillDatabase.h"
#include "DivineFavor.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace
{
    // Tunable constants.
    constexpr uint32_t kMMin = 380;   // lowest plausible player max_hp
    constexpr uint32_t kMMax = 750;   // highest plausible; 750 < 2*380 excludes harmonics
    constexpr double kMinFrac = 0.003; // ignore tiny fractions (rounding noise)
    constexpr double kMaxFrac = 1.0;   // ignore out-of-range fractions
    constexpr double kResidThresh = 0.03;    // per-event viability residual
    constexpr int    kMinEvents = 4;         // minimum events to solve/accept a bucket
    constexpr double kAcceptMedianResid = 0.02; // bucket accept gate

    // Integer-fit residual of fraction f against candidate max_hp M.
    inline double Residual(double f, uint32_t M)
    {
        double x = f * static_cast<double>(M);
        return std::fabs(x - std::round(x));
    }

    struct Hit { float time; double f; };

    // Some packets carry a fraction by construction rather than by division,
    // and so stay consistent with almost any M. They must never reach the
    // sieve or they bias it toward round numbers:
    //   * Protective Spirit caps damage at 10% of max health -> f == 0.10
    //   * Necromancer sacrifice costs are a fixed % of max health
    // Rejects the small-denominator rationals these produce.
    bool IsConstructedFraction(double f)
    {
        for (int den : { 3, 4, 5, 6, 10, 20 })
        {
            double x = f * den;
            if (std::fabs(x - std::round(x)) < 1e-3 && std::round(x) >= 1.0)
                return true;
        }
        return false;
    }
}

void SolveMaxHpTimelines(
    std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatEvent>& combatEvents)
{
    const int R = static_cast<int>(kMMax - kMMin) + 1;

    for (auto& [id, ard] : agents)
        ard.solvedMaxHpByWeaponSet.clear();

    // Step 1: bucket per-target hits by the weapon set equipped at the time of
    // the hit. Deep Wound observations are dropped rather than bucketed: it
    // scales max health by 20% (capped at 100), so those packets belong to a
    // different effective max HP than the rest of the set's evidence.
    std::unordered_map<int, std::unordered_map<uint64_t, std::vector<Hit>>> perTarget;

    for (const auto& ce : combatEvents)
    {
        if (!ce.IsDamageOrHeal()) continue;
        double f = std::fabs(static_cast<double>(ce.value));
        if (f < kMinFrac || f > kMaxFrac) continue;
        if (IsConstructedFraction(f)) continue;

        auto ta = agents.find(ce.target_id);
        if (ta == agents.end() || ta->second.type != AgentType::Player) continue;

        const AgentReplayData& ard = ta->second;
        int idx = ard.snapshotIndexAtTime(ce.time);
        if (idx < 0) continue;
        if (ard.snapshots[idx].has_deep_wound) continue;

        uint64_t key = AgentReplayData::WeaponSetKey(ard.snapshots[idx]);
        perTarget[ce.target_id][key].push_back({ ce.time, f });
    }

    // Step 2: independent solve per (target, weapon set).
    std::vector<int>    consistent(R);
    std::vector<double> score(R);

    for (auto& [targetId, buckets] : perTarget)
    {
        auto ait = agents.find(targetId);
        if (ait == agents.end()) continue;
        AgentReplayData& ard = ait->second;

        for (auto& [key, hits] : buckets)
        {
            if (static_cast<int>(hits.size()) < kMinEvents) continue;

            std::sort(hits.begin(), hits.end(),
                [](const Hit& a, const Hit& b) { return a.time < b.time; });

            std::fill(consistent.begin(), consistent.end(), 0);
            std::fill(score.begin(), score.end(), 0.0);

            // Count how many hits each candidate explains rather than striking
            // candidates off permanently. A bucket is homogeneous by
            // construction now, so the correct M explains essentially all of
            // it -- but one corrupt packet must not be able to eliminate the
            // right answer outright, which is what a strike-off sieve does.
            for (const Hit& h : hits)
            {
                for (int i = 0; i < R; ++i)
                {
                    double r = Residual(h.f, kMMin + i);
                    if (r <= kResidThresh) { ++consistent[i]; score[i] += r * r; }
                }
            }

            int bestIdx = -1, bestCount = 0;
            for (int i = 0; i < R; ++i)
            {
                if (consistent[i] > bestCount ||
                    (consistent[i] == bestCount && bestIdx >= 0 && score[i] < score[bestIdx]))
                { bestCount = consistent[i]; bestIdx = i; }
            }
            if (bestIdx < 0 || bestCount < kMinEvents) continue;

            // Tie-break to the candidate nearest the camera-observed max_hp
            // inside this bucket (if the recording carries one).
            uint32_t cam = ard.maxHpAtTime(hits[hits.size() / 2].time);
            if (cam > 0)
            {
                uint32_t bestDist = 0xFFFFFFFFu;
                for (int i = 0; i < R; ++i)
                {
                    if (consistent[i] != bestCount) continue;
                    uint32_t M = kMMin + i;
                    uint32_t d = (M > cam) ? (M - cam) : (cam - M);
                    if (d < bestDist) { bestDist = d; bestIdx = i; }
                }
            }

            uint32_t bestM = kMMin + bestIdx;

            std::vector<double> resids;
            resids.reserve(hits.size());
            for (const Hit& h : hits) resids.push_back(Residual(h.f, bestM));
            std::sort(resids.begin(), resids.end());
            size_t n = resids.size();
            double median = (n & 1) ? resids[n / 2]
                                    : 0.5 * (resids[n / 2 - 1] + resids[n / 2]);

            AgentReplayData::SolvedMaxHp rec;
            rec.maxHp          = bestM;
            rec.observations   = static_cast<int>(hits.size());
            rec.supporting     = bestCount;
            rec.medianResidual = static_cast<float>(median);
            rec.source         = AgentReplayData::MaxHpSource::Lattice;
            rec.firstSeen      = hits.front().time;
            rec.lastSeen       = hits.back().time;
            // The median is taken over every hit in the bucket, so it stays
            // meaningful even when a few outliers were not counted above.
            rec.accepted       = (bestCount >= kMinEvents) &&
                                 (median <= kAcceptMedianResid);
            ard.solvedMaxHpByWeaponSet[key] = rec;
        }
    }
}

// ---------------------------------------------------------------------------
// Breakpoint inversion
// ---------------------------------------------------------------------------

namespace
{
    constexpr double kInvTolerance = 0.35; // HP; how close amount/f must sit to an integer
    constexpr int    kMinDfSupport = 6;    // packets needed to trust a DF rank
    constexpr int    kMinSetVotes  = 3;    // votes needed to accept a weapon set

    inline int Breakpoint(float v0, float v15, int r)
    {
        return (int)std::lround((double)v0 + (double)r * ((double)v15 - (double)v0) / 15.0);
    }

    // A vote for one (target, weapon set) bucket.
    struct Vote { int targetId; uint64_t key; uint32_t maxHp; float time; };

    // Turns "known absolute amount N, observed fraction f" into a max HP,
    // accepting only results that land close enough to a whole number to be a
    // real health total rather than a coincidence.
    bool InvertToMaxHp(int amount, double f, uint32_t& out)
    {
        if (amount <= 0 || f <= 0.0) return false;
        double m = (double)amount / f;
        if (m < (double)kMMin || m > (double)kMMax) return false;
        if (std::fabs(m - std::round(m)) > kInvTolerance) return false;
        out = (uint32_t)std::lround(m);
        return true;
    }

    // Collapses votes to the most-voted value per bucket and writes it in,
    // overwriting a weaker source but never a stronger one.
    void ApplyVotes(std::unordered_map<int, AgentReplayData>& agents,
                    const std::vector<Vote>& votes,
                    AgentReplayData::MaxHpSource source)
    {
        // targetId -> weapon set -> value -> count
        std::unordered_map<int, std::unordered_map<uint64_t,
            std::unordered_map<uint32_t, int>>> tally;
        // targetId -> weapon set -> (first, last) vote time
        std::unordered_map<int, std::unordered_map<uint64_t,
            std::pair<float, float>>> span;

        for (const Vote& v : votes)
        {
            tally[v.targetId][v.key][v.maxHp]++;
            auto& s = span[v.targetId];
            auto it = s.find(v.key);
            if (it == s.end()) s[v.key] = { v.time, v.time };
            else {
                it->second.first  = std::min(it->second.first,  v.time);
                it->second.second = std::max(it->second.second, v.time);
            }
        }

        for (auto& [targetId, byKey] : tally)
        {
            auto ait = agents.find(targetId);
            if (ait == agents.end()) continue;
            AgentReplayData& ard = ait->second;

            for (auto& [key, counts] : byKey)
            {
                int total = 0, bestCount = 0;
                uint32_t bestVal = 0;
                for (auto& [val, c] : counts)
                {
                    total += c;
                    if (c > bestCount || (c == bestCount && val > bestVal))
                    { bestCount = c; bestVal = val; }
                }
                if (bestCount < kMinSetVotes) continue;

                auto& slot = ard.solvedMaxHpByWeaponSet[key];
                // Exact channels outrank the lattice; DF outranks a single
                // skill table because its rank is corroborated across the
                // whole party rather than against one target.
                if (slot.accepted && slot.source > source) continue;

                slot.maxHp          = bestVal;
                slot.observations   = total;
                slot.supporting     = bestCount;
                slot.medianResidual = 0.f;
                slot.accepted       = true;
                slot.source         = source;
                slot.firstSeen      = span[targetId][key].first;
                slot.lastSeen       = span[targetId][key].second;
            }
        }
    }
}

void SolveMaxHpFromSkillBreakpoints(
    std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatLogRow>& combatLog,
    const SkillDatabaseView& skillView)
{
    if (!skillView.IsLoaded()) return;

    // -----------------------------------------------------------------------
    // Channel 1: Divine Favor.
    //
    // Every Monk spell cast on an ally emits a separate heal packet worth
    // round(3.2 * DF rank). The rank is one small integer per Monk, constant
    // for the match and shared across every ally they touch -- so a rank that
    // explains this Monk's packets explains them for eight different targets
    // at once. That fan-out makes it the strongest constraint available.
    // -----------------------------------------------------------------------
    struct HealObs { int targetId; uint64_t key; double f; float time; };
    std::unordered_map<int, std::vector<HealObs>> monkHeals;

    for (const CombatLogRow& row : combatLog)
    {
        if (row.category != CombatLogCategory::Heal) continue;
        if (row.valuePct == 0.f) continue;

        auto casterIt = agents.find(row.casterId);
        if (casterIt == agents.end()) continue;
        if (casterIt->second.type != AgentType::Player) continue;
        if (casterIt->second.primaryProf != 3) continue; // primary Monk only

        auto targetIt = agents.find(row.targetId);
        if (targetIt == agents.end()) continue;
        const AgentReplayData& tard = targetIt->second;
        if (tard.type != AgentType::Player) continue;

        int idx = tard.snapshotIndexAtTime(row.time);
        if (idx < 0) continue;
        if (tard.snapshots[idx].has_deep_wound) continue; // heals reduced by 20%

        monkHeals[row.casterId].push_back({
            row.targetId,
            AgentReplayData::WeaponSetKey(tard.snapshots[idx]),
            std::fabs((double)row.valuePct),
            row.time });
    }

    std::vector<Vote> dfVotes;
    for (auto& [casterId, obs] : monkHeals)
    {
        if ((int)obs.size() < kMinDfSupport) continue;

        // Score every candidate rank by how many packets it turns into a
        // plausible whole-number health total.
        int bestRank = -1, bestSupport = 0, runnerUp = 0;
        for (int r = 1; r <= 16; ++r)
        {
            int support = 0;
            for (const HealObs& o : obs)
            {
                uint32_t m = 0;
                if (InvertToMaxHp(kDivineFavorBonus[r], o.f, m)) ++support;
            }
            if (support > bestSupport) { runnerUp = bestSupport; bestSupport = support; bestRank = r; }
            else if (support > runnerUp) { runnerUp = support; }
        }

        // Require both an absolute floor and a clear margin over the
        // second-best rank, so an ambiguous Monk contributes nothing.
        if (bestRank < 0 || bestSupport < kMinDfSupport) continue;
        if (bestSupport < runnerUp * 2) continue;

        // Publish the rank so AttributeDeducer can consume it instead of
        // deriving Divine Favor a second time from the same events.
        if (auto casterIt = agents.find(casterId); casterIt != agents.end())
        {
            casterIt->second.solvedDivineFavorRank    = bestRank;
            casterIt->second.solvedDivineFavorSupport = bestSupport;
        }

        for (const HealObs& o : obs)
        {
            uint32_t m = 0;
            if (InvertToMaxHp(kDivineFavorBonus[bestRank], o.f, m))
                dfVotes.push_back({ o.targetId, o.key, m, o.time });
        }
    }

    // -----------------------------------------------------------------------
    // Channel 2: skill attribute-rank tables.
    //
    // For a heal from a skill whose rank table is known, the healed amount has
    // to be one of the 17 breakpoint values. Each packet therefore admits at
    // most 17 candidate max HPs -- against 371 for the lattice sieve. The true
    // value is voted by every packet; wrong candidates scatter, so the mode
    // wins even though any single packet is ambiguous.
    // -----------------------------------------------------------------------
    std::vector<Vote> skillVotes;
    for (const CombatLogRow& row : combatLog)
    {
        if (row.category != CombatLogCategory::Heal) continue;
        if (row.valuePct == 0.f || row.skillId <= 0) continue;

        const SkillInfo* si = skillView.Get(skillView.ResolvePvpSkillId(row.skillId));
        if (!si || !si->deductionUsable) continue;
        if (si->deductionKind != SkillScaleKind::Heal) continue;
        if (si->dedV15 == si->dedV0) continue;

        // A Divine-Favor-confounded heal carries the spell value plus the DF
        // bonus; two unknowns, so it cannot be inverted cleanly here.
        auto casterIt = agents.find(row.casterId);
        if (casterIt == agents.end()) continue;
        if (si->dfConfounded && casterIt->second.primaryProf == 3) continue;

        auto targetIt = agents.find(row.targetId);
        if (targetIt == agents.end()) continue;
        const AgentReplayData& tard = targetIt->second;
        if (tard.type != AgentType::Player) continue;

        int idx = tard.snapshotIndexAtTime(row.time);
        if (idx < 0) continue;
        if (tard.snapshots[idx].has_deep_wound) continue;

        const uint64_t key = AgentReplayData::WeaponSetKey(tard.snapshots[idx]);
        const double f = std::fabs((double)row.valuePct);

        for (int r = 0; r <= 16; ++r)
        {
            uint32_t m = 0;
            if (InvertToMaxHp(Breakpoint(si->dedV0, si->dedV15, r), f, m))
                skillVotes.push_back({ row.targetId, key, m, row.time });
        }
    }

    // Weakest first so the stronger channel overwrites on conflict.
    ApplyVotes(agents, skillVotes, AgentReplayData::MaxHpSource::SkillBreakpoint);
    ApplyVotes(agents, dfVotes,    AgentReplayData::MaxHpSource::DivineFavor);
}

// ---------------------------------------------------------------------------
// Per-packet correction of the recorded max_hp
// ---------------------------------------------------------------------------

namespace
{
    // Offsets the recorded max_hp is observed to be wrong by, in descending
    // order of measured frequency across the local match archive. Tried in
    // this order, so the most likely explanation wins.
    //
    // Recognisable modifiers: -30/+30 and -60/+60 are weapon and shield
    // health mods, +48 is a full morale boost (+10% of the 480 base), -72 is
    // one rank of death penalty (-15% of 480), +100 is the Deep Wound cap,
    // and +78/+18/-12/-102 are sums of those. The remainder (+10, +70, -62)
    // have no clean derivation but recur often enough in the data to earn a
    // place; they are kept empirical rather than invented.
    //
    // The list length is not arbitrary. Every extra candidate is another
    // chance to fit a fraction by accident, so both the length and the
    // tolerance below were chosen by sweeping them against a null model of
    // random fractions and maximising the separation between the two:
    //
    //     offsets  tol      real     null
    //        6     0.0015   78.6%     5.7%
    //       14     0.0015   85.1%     8.0%   <-- chosen
    //       22     0.0015   86.4%     9.7%
    //       22     0.0060   88.2%    25.6%
    //
    // Without correction the same measurement reads 55.3% real / 4.0% null.
    const int kCorrectionOffsets[] = {
        -30, 48, 30, -72, 100, -60, 60, 78, 10, 18, -102, 70, -12, -62
    };

    // Deliberately tight. A looser threshold buys a little more coverage and
    // a great deal more coincidence, as the sweep above shows.
    constexpr double kPacketTol = 0.0015;
}

uint32_t CorrectMaxHpForPacket(uint32_t recorded, double fraction)
{
    if (recorded == 0) return recorded;

    const double f = std::fabs(fraction);
    if (f <= 0.0 || f > 1.0) return recorded;

    auto integral = [f](long m) {
        if (m <= 0) return false;
        double x = f * (double)m;
        return x >= 0.5 && std::fabs(x - std::round(x)) <= kPacketTol;
    };

    // The recorded value is right most of the time; never move off it when it
    // already explains the packet.
    if (integral((long)recorded)) return recorded;

    // Ordered by measured frequency, so the likeliest explanation wins.
    for (int d : kCorrectionOffsets)
    {
        long m = (long)recorded + d;
        if (integral(m)) return (uint32_t)m;
    }

    // Nothing plausible fits -- leave the caller with the recorded value
    // rather than inventing one.
    return recorded;
}
