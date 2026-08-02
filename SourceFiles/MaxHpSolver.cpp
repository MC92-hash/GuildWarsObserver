#include "pch.h"
#include "MaxHpSolver.h"
#include "ReplayMapData.h"

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
    constexpr int    kMinEvents = 4;         // minimum events to solve/accept a segment
    constexpr double kAcceptMedianResid = 0.02; // segment accept gate

    // Integer-fit residual of fraction f against candidate max_hp M.
    inline double Residual(double f, uint32_t M)
    {
        double x = f * static_cast<double>(M);
        return std::fabs(x - std::round(x));
    }

    struct Hit { float time; double f; };
}

void SolveMaxHpTimelines(
    std::unordered_map<int, AgentReplayData>& agents,
    const std::vector<CombatEvent>& combatEvents)
{
    const int R = static_cast<int>(kMMax - kMMin) + 1;

    // Step 1: collect per-target hits (only Player targets, valid fractions).
    std::unordered_map<int, std::vector<Hit>> perTarget;
    for (const auto& ce : combatEvents)
    {
        if (!ce.IsDamageOrHeal()) continue;
        double f = std::fabs(static_cast<double>(ce.value));
        if (f < kMinFrac || f > kMaxFrac) continue;
        auto ta = agents.find(ce.target_id);
        if (ta == agents.end() || ta->second.type != AgentType::Player) continue;
        perTarget[ce.target_id].push_back({ ce.time, f });
    }

    for (auto& [targetId, hits] : perTarget)
    {
        auto ait = agents.find(targetId);
        if (ait == agents.end()) continue;
        AgentReplayData& ard = ait->second;
        ard.solvedMaxHp.clear();

        if (static_cast<int>(hits.size()) < kMinEvents) continue;

        // Defensive sort by time.
        std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) { return a.time < b.time; });

        // Per-segment state.
        std::vector<char>   viable(R, 1);   // currently viable candidate mask
        std::vector<double> score(R, 0.0);  // sum of r^2 over segment (viable only)
        std::vector<double> segFs;          // segment event fractions (for median)
        float  segStart    = 0.f;           // first segment covers from t=0
        float  segLastTime = 0.f;
        int    outliers    = 0;

        auto resetSeg = [&]()
        {
            std::fill(viable.begin(), viable.end(), (char)1);
            std::fill(score.begin(), score.end(), 0.0);
            segFs.clear();
            outliers = 0;
        };

        // True if at least one currently-viable M stays consistent with f.
        auto consistentWith = [&](double f) -> bool
        {
            for (int i = 0; i < R; ++i)
                if (viable[i] && Residual(f, kMMin + i) <= kResidThresh)
                    return true;
            return false;
        };

        // Apply f to the segment: drop inconsistent Ms, accumulate score.
        auto applyEvent = [&](double f)
        {
            for (int i = 0; i < R; ++i)
            {
                if (!viable[i]) continue;
                double r = Residual(f, kMMin + i);
                if (r > kResidThresh) viable[i] = 0;
                else                  score[i] += r * r;
            }
        };

        // Close the current segment, choose M*, and store it.
        auto closeSegment = [&](float tEndVal)
        {
            if (segFs.empty()) return;

            double minScore = std::numeric_limits<double>::max();
            for (int i = 0; i < R; ++i)
                if (viable[i] && score[i] < minScore) minScore = score[i];

            // Collect ties within 1e-9 of the minimum score.
            std::vector<int> tied;
            for (int i = 0; i < R; ++i)
                if (viable[i] && score[i] <= minScore + 1e-9) tied.push_back(i);

            int bestIdx = tied.empty() ? -1 : tied.front();
            if (tied.size() > 1)
            {
                // Tie-break to the candidate nearest the camera-observed
                // max_hp at the segment midpoint (if any).
                float mid = (segLastTime > segStart) ? (segStart + segLastTime) * 0.5f
                                                      : segStart;
                uint32_t cam = ard.maxHpAtTime(mid);
                if (cam > 0)
                {
                    uint32_t bestDist = 0xFFFFFFFFu;
                    for (int i : tied)
                    {
                        uint32_t M = kMMin + i;
                        uint32_t d = (M > cam) ? (M - cam) : (cam - M);
                        if (d < bestDist) { bestDist = d; bestIdx = i; }
                    }
                }
            }
            if (bestIdx < 0) return;

            uint32_t bestM = kMMin + bestIdx;

            // Median residual over the segment at M*.
            std::vector<double> resids;
            resids.reserve(segFs.size());
            for (double f : segFs) resids.push_back(Residual(f, bestM));
            std::sort(resids.begin(), resids.end());
            size_t n = resids.size();
            double median = (n & 1) ? resids[n / 2]
                                    : 0.5 * (resids[n / 2 - 1] + resids[n / 2]);

            AgentReplayData::MaxHpSegment seg;
            seg.tStart         = segStart;
            seg.tEnd           = tEndVal;
            seg.maxHp          = bestM;
            seg.eventCount     = static_cast<int>(segFs.size());
            seg.medianResidual = static_cast<float>(median);
            seg.accepted       = (seg.eventCount >= kMinEvents) &&
                                 (median <= kAcceptMedianResid);
            ard.solvedMaxHp.push_back(seg);
        };

        // Steps 2-3: greedy viability change-point with outlier confirmation.
        size_t i = 0;
        while (i < hits.size())
        {
            double f = hits[i].f;
            if (consistentWith(f))
            {
                applyEvent(f);
                segFs.push_back(f);
                segLastTime = hits[i].time;
                ++i;
                continue;
            }

            // Breaker: this event would empty the viable set.
            if (i + 1 < hits.size() && consistentWith(hits[i + 1].f))
            {
                // Next event still fits the pre-breaker set -> outlier.
                ++outliers;
                int allowed = std::max(1, static_cast<int>(segFs.size()) / 20);
                if (outliers > allowed)
                {
                    // Too many outliers -> treat as a real change point.
                    closeSegment(hits[i].time);
                    resetSeg();
                    segStart    = hits[i].time;
                    segLastTime = hits[i].time;
                    applyEvent(f);
                    segFs.push_back(f);
                }
                // Otherwise discard the breaker and keep the segment.
                ++i;
                continue;
            }

            // Next event also inconsistent (or no next) -> confirmed change point.
            closeSegment(hits[i].time);
            resetSeg();
            segStart    = hits[i].time;
            segLastTime = hits[i].time;
            applyEvent(f);
            segFs.push_back(f);
            ++i;
        }

        // Close the final open segment (extends to the end of the match).
        closeSegment(FLT_MAX);
    }
}
