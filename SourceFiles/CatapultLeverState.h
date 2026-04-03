#pragma once

#include "pch.h"

// ---------------------------------------------------------------------------
// Catapult lever state machine for Warrior's Isle (map_id 171).
//
// Stores a sorted event log per lever (keyed by object_id). All query
// methods take the current replay time as parameter and derive state
// deterministically, so scrubbing / seeking always produces the correct
// visual without accumulated mutable state.
// ---------------------------------------------------------------------------

enum class CatapultState : uint8_t {
    Unknown,
    Repaired,
    Loaded,
    Fired,
    Impact,
};

struct CatapultEvent {
    float         time  = 0.f;
    CatapultState state = CatapultState::Unknown;
};

class CatapultLeverState {
public:
    void AddEvent(float time, CatapultState state);
    void Finalize();

    CatapultState GetState(float t)        const;
    std::string   GetLabelText(float t)    const;
    ImU32         GetLabelColor(float t)   const;
    float         GetGlowOpacity(float t)  const;
    ImU32         GetGlowColor(float t)    const;

private:
    std::vector<CatapultEvent> events_;

    static constexpr float kFiredToImpactSec    = 7.0f;
    static constexpr float kImpactDurationSec   = 0.5f;
    static constexpr float kReturnFlashSec      = 0.3f;
    static constexpr float kTotalCycleSec       = kFiredToImpactSec + kImpactDurationSec;

    static constexpr float kLoadedPulseCycleSec = 1.2f;
    static constexpr float kFiredPulseCycleSec  = 0.4f;

    CatapultState EvaluateState(float t) const;
    float         FiredElapsed(float t)  const;
    float         FindLastFiredTime(float t) const;
};
