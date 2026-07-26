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
    Loading,   // arm winding back; derived from how long ago the load event was
    Loaded,
    Fired,
    Impact,
};

struct CatapultEvent {
    float         time  = 0.f;
    CatapultState state = CatapultState::Unknown;
};

// Border tint shared by every place a catapult's state is shown as an icon, so
// the event timeline, the minimap and the world marker always agree.
inline ImU32 CatapultStateBorderColor(CatapultState s)
{
    switch (s) {
    case CatapultState::Fired:
    case CatapultState::Impact:   return IM_COL32(220,  60,  55, 255);  // red
    case CatapultState::Loading:  return IM_COL32(250, 225,  60, 255);  // yellow
    case CatapultState::Loaded:   return IM_COL32(240, 170,  40, 255);  // amber
    case CatapultState::Repaired: return IM_COL32( 70, 200,  90, 255);  // green
    default:                      return IM_COL32(160, 160, 160, 255);
    }
}

inline const char* CatapultStateName(CatapultState s)
{
    switch (s) {
    case CatapultState::Repaired: return "Catapult Repaired";
    case CatapultState::Loading:  return "Catapult Loading";
    case CatapultState::Loaded:   return "Catapult Loaded";
    case CatapultState::Fired:    return "Catapult Fired";
    case CatapultState::Impact:   return "Catapult Impact";
    default:                      return "Catapult";
    }
}

class CatapultLeverState {
public:
    void AddEvent(float time, CatapultState state);
    void Finalize();

    CatapultState GetState(float t)        const;
    // The raw state change at or before t. GetState smooths over the reload the
    // server sends mid-flight, which is right for the readout but wrong for the
    // model: the arm really does start reloading then.
    bool          LastEvent(float t, CatapultState& outState, float& outTime) const;
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

    // How long the arm takes to wind back, matching the load segment of the
    // catapult model (0x220F6) so the readout and the animation finish together.
    static constexpr float kLoadAnimSec = 9.67f;

    // Every pulsing state breathes at the same rate; colour alone tells them apart.
    static constexpr float kPulseCycleSec = 1.2f;

    CatapultState EvaluateState(float t) const;
    float         FiredElapsed(float t)  const;
    float         FindLastFiredTime(float t) const;
};
