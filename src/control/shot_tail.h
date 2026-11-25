// =================================================================================================
//  shot_tail.h - The rules that decide where a post-shot tail sample belongs
//
//  When the pump stops, the shot is not over: the last of the coffee is still arriving in the cup
//  and the scale keeps climbing for a second or two. Those samples have to end up in the curve of
//  the shot that just ended, or the stored graph stops short of the weight the ledger records.
//
//  The rules are tiny but easy to get wrong, so they live here - header-only and free of any
//  Arduino dependency - and the host regression tests drive them directly.
// =================================================================================================
#pragma once

#include <stdint.h>

// ------------------------------------------------------------------------------------------------
// Where a tail sample goes
//
// The tail always belongs to the shot that ended. It is routed explicitly rather than guessed from
// the capture state, so it can never leak into the next shot's capture.
// ------------------------------------------------------------------------------------------------
enum class ShotTailRoute : uint8_t {
    LIVE_CAPTURE = 0,   // the shot has not been frozen yet - the freeze will pick the tail up
    FROZEN_SHOT  = 1    // the shot is frozen and is waiting to be written to flash
};

// True when the shot that just ended has already been frozen into its own record.
inline ShotTailRoute shotTailRoute(bool frozenShotValid) {
    return frozenShotValid ? ShotTailRoute::FROZEN_SHOT : ShotTailRoute::LIVE_CAPTURE;
}

// ------------------------------------------------------------------------------------------------
// What a tail sample contributes to the shot
//
// The tail can only ever RAISE what the shot recorded. A noisy reading must not walk the recorded
// weight or the peak pressure back down, and the settled yield in the cup must be the highest
// weight the shot ever showed - which is exactly what makes the graph agree with the ledger.
// ------------------------------------------------------------------------------------------------
inline void shotTailAccumulate(float weightG, float pBar, float& cutoffG, float& peakBar) {
    if (weightG > cutoffG) cutoffG = weightG;
    if (pBar > peakBar)   peakBar = pBar;
}

// ------------------------------------------------------------------------------------------------
// The gauges the tail records
//
// When the pump stops, the line pressure falls away and the flow reads zero. Writing that collapse
// into the stored curve would trail a tail of dying gauges behind the shot - it says nothing the
// reader wants to know and it makes the graph look worse than the shot actually was.
//
// A tail sample therefore records the gauges as they were when the pump stopped. The weight keeps
// moving, because the last of the coffee is still arriving, so the stored curve ends at the settled
// yield. The clients stop the pressure and the flow curves at the pump stop, so what the reader
// sees is a shot that ended, not a shot that died.
//
// The rule is deliberately shaped so that it can be driven on every tick: while the pump is running
// the live readings pass straight through and the held values follow them, and the moment the pump
// stops the held values simply stop moving. The caller does not have to know when the stop
// happened, and it cannot forget to freeze them.
// ------------------------------------------------------------------------------------------------
inline void shotTailHoldGauges(float livePBar, float liveFlowVirt, bool pumpRunning,
                                float& heldPBar, float& heldFlowVirt) {
    if (!pumpRunning) return;   // the pump is off - the gauges keep the reading they had
    heldPBar     = livePBar;
    heldFlowVirt = liveFlowVirt;
}

// ------------------------------------------------------------------------------------------------
// How long the shot was
//
// The brewing time is how long the pump ran. The tail samples sit beyond the end of the shot, so
// they must never stretch it. The caller's own brewing time wins; the last sample is only a
// fallback for a shot whose brewing time was not passed in.
// ------------------------------------------------------------------------------------------------
inline uint32_t shotFrozenDurationMs(uint32_t shotDurationMs, uint32_t lastSampleMs) {
    return shotDurationMs ? shotDurationMs : lastSampleMs;
}
