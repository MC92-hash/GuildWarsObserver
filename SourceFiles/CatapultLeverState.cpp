#include "pch.h"
#include "CatapultLeverState.h"

// ---------------------------------------------------------------------------
// Event log construction
// ---------------------------------------------------------------------------

void CatapultLeverState::AddEvent(float time, CatapultState state)
{
    events_.push_back({ time, state });
}

void CatapultLeverState::Finalize()
{
    std::sort(events_.begin(), events_.end(),
              [](const CatapultEvent& a, const CatapultEvent& b) {
                  return a.time < b.time;
              });
}

// ---------------------------------------------------------------------------
// Core state evaluation (scrub-safe)
// ---------------------------------------------------------------------------

float CatapultLeverState::FindLastFiredTime(float t) const
{
    float lastFired = -1.f;
    for (auto& ev : events_)
    {
        if (ev.time > t) break;
        if (ev.state == CatapultState::Fired)
            lastFired = ev.time;
    }
    return lastFired;
}

float CatapultLeverState::FiredElapsed(float t) const
{
    float firedTime = FindLastFiredTime(t);
    if (firedTime < 0.f) return -1.f;
    return t - firedTime;
}

CatapultState CatapultLeverState::EvaluateState(float t) const
{
    if (events_.empty()) return CatapultState::Unknown;

    // Find the last event at or before time t
    CatapultState base = CatapultState::Unknown;
    float lastEventTime = -1.f;
    for (auto& ev : events_)
    {
        if (ev.time > t) break;
        base = ev.state;
        lastEventTime = ev.time;
    }

    if (base == CatapultState::Unknown)
        return CatapultState::Unknown;

    // If base state is Fired, derive Impact / return-to-Repaired from elapsed time
    if (base == CatapultState::Fired)
    {
        float elapsed = t - lastEventTime;
        if (elapsed >= kTotalCycleSec)
            return CatapultState::Repaired;
        if (elapsed >= kFiredToImpactSec)
            return CatapultState::Impact;
        return CatapultState::Fired;
    }

    return base;
}

CatapultState CatapultLeverState::GetState(float t) const
{
    return EvaluateState(t);
}

// ---------------------------------------------------------------------------
// Label text
// ---------------------------------------------------------------------------

std::string CatapultLeverState::GetLabelText(float t) const
{
    CatapultState s = EvaluateState(t);
    switch (s)
    {
    case CatapultState::Unknown:
        return "Lever";
    case CatapultState::Repaired:
        return "Catapult Repaired";
    case CatapultState::Loaded:
        return "Catapult Loaded";
    case CatapultState::Fired:
    {
        float firedTime = FindLastFiredTime(t);
        float remaining = kFiredToImpactSec - (t - firedTime);
        if (remaining < 0.f) remaining = 0.f;
        return std::format("Catapult Fired \xe2\x80\x94 {:.1f}s", remaining);
    }
    case CatapultState::Impact:
        return "IMPACT";
    }
    return "Lever";
}

// ---------------------------------------------------------------------------
// Label color
// ---------------------------------------------------------------------------

ImU32 CatapultLeverState::GetLabelColor(float t) const
{
    CatapultState s = EvaluateState(t);
    switch (s)
    {
    case CatapultState::Unknown:   return IM_COL32(255, 255, 255, 230);
    case CatapultState::Repaired:  return IM_COL32(255, 255, 255, 230);
    case CatapultState::Loaded:    return IM_COL32(255, 165, 0,   255);
    case CatapultState::Fired:     return IM_COL32(255, 60,  60,  255);
    case CatapultState::Impact:    return IM_COL32(255, 0,   0,   255);
    }
    return IM_COL32(255, 255, 255, 230);
}

// ---------------------------------------------------------------------------
// Glow color
// ---------------------------------------------------------------------------

ImU32 CatapultLeverState::GetGlowColor(float t) const
{
    CatapultState s = EvaluateState(t);
    switch (s)
    {
    case CatapultState::Repaired:
    {
        // Brief green flash during the return-flash window after Impact
        float firedTime = FindLastFiredTime(t);
        if (firedTime >= 0.f)
        {
            float elapsed = t - firedTime;
            if (elapsed >= kTotalCycleSec && elapsed < kTotalCycleSec + kReturnFlashSec)
                return IM_COL32(0, 255, 0, 255);
        }
        return IM_COL32(255, 255, 255, 255);
    }
    case CatapultState::Loaded:  return IM_COL32(255, 180, 40,  255);
    case CatapultState::Fired:   return IM_COL32(255, 50,  50,  255);
    case CatapultState::Impact:  return IM_COL32(255, 0,   0,   255);
    default:                     return IM_COL32(255, 255, 255, 255);
    }
}

// ---------------------------------------------------------------------------
// Glow opacity (animated pulse using replay time)
// ---------------------------------------------------------------------------

static float PulseValue(float t, float cycleSec)
{
    float phase = std::fmod(t, cycleSec) / cycleSec;
    constexpr float kTwoPi = 6.2831853f;
    return 0.5f + 0.5f * sinf(phase * kTwoPi);
}

float CatapultLeverState::GetGlowOpacity(float t) const
{
    CatapultState s = EvaluateState(t);
    switch (s)
    {
    case CatapultState::Unknown:
        return 0.f;
    case CatapultState::Repaired:
    {
        // Green flash during return window, then subtle static glow
        float firedTime = FindLastFiredTime(t);
        if (firedTime >= 0.f)
        {
            float elapsed = t - firedTime;
            if (elapsed >= kTotalCycleSec && elapsed < kTotalCycleSec + kReturnFlashSec)
            {
                float flashProgress = (elapsed - kTotalCycleSec) / kReturnFlashSec;
                return 0.8f * (1.f - flashProgress);
            }
        }
        return 0.15f;
    }
    case CatapultState::Loaded:
        return 0.6f * PulseValue(t, kLoadedPulseCycleSec);
    case CatapultState::Fired:
        return 0.85f * PulseValue(t, kFiredPulseCycleSec);
    case CatapultState::Impact:
        return 1.0f;
    }
    return 0.f;
}
