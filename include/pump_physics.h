//  pump_physics.h - Vibratory pump P-Q head characteristic and virtual flow calculation
//  Universal Espresso Controller & Hardware-In-The-Loop Simulator
//
//  Header-only and free of Arduino dependencies on purpose: the controller, the HIL simulator and
//  the host regression tests (simulator-esp32s3/test/host/test_pump_flow.cpp) all run this code.

#pragma once

#include <math.h>

// The P-Q shape exponent every preset used before it became a per-pump field. Kept as the default
// so callers that do not pass an exponent (the HIL simulator) behave exactly as before.
#define SB_PUMP_PQ_EXPONENT_DEFAULT 1.35f

/**
 * Calculates estimated instantaneous water flow (ml/s) through an electromagnetic
 * vibratory pump (e.g. Ulka EX5/EP5 or Olab Silent Green) based on pump pressure head
 * and AC duty cycle.
 *
 * Derived empirically from published Ulka P-Q pump performance curves:
 *   Q_pump = (dutyPct / 100) * maxFlowMlS * max(0, 1 - (measuredBar / stallBar)^exponent)
 *
 * Flow is linear in duty because pulse skipping fires whole half-cycles: every fired half-cycle is
 * one full pump stroke, so the delivered volume scales with the number of strokes.
 *
 * @param measuredBar Current measured hydraulic line pressure in bar
 * @param dutyPct     Current PSM/triac power output percentage [0..100]
 * @param stallBar    Maximum zero-flow stall pressure of the pump in bar
 * @param maxFlowMlS  Maximum open-discharge flow rate in ml/s at 0 bar
 * @param exponent    Shape of the P-Q curve (how fast flow falls with back-pressure)
 * @return Estimated flow rate in ml/s
 */
inline float pumpPressureShape(float measuredBar, float stallBar, float exponent) {
    if (stallBar <= 0.0f) return 0.0f;
    if (exponent <= 0.0f) exponent = SB_PUMP_PQ_EXPONENT_DEFAULT;
    const float ratio = measuredBar / stallBar;
    float shape = 1.0f - powf(ratio > 0.0f ? ratio : 0.0f, exponent);
    if (shape < 0.0f) shape = 0.0f;
    if (shape > 1.0f) shape = 1.0f;
    return shape;
}

inline float calculateVirtualFlow(float measuredBar, float dutyPct, float stallBar, float maxFlowMlS,
                                  float exponent = SB_PUMP_PQ_EXPONENT_DEFAULT) {
    if (stallBar <= 0.0f || maxFlowMlS <= 0.0f || dutyPct <= 0.0f) {
        return 0.0f;
    }
    const float q = (dutyPct / 100.0f) * maxFlowMlS * pumpPressureShape(measuredBar, stallBar, exponent);
    return q > 0.0f ? q : 0.0f;
}

/**
 * The inverse of calculateVirtualFlow(): the duty that makes the pump deliver `targetFlowMlS` at
 * the current back-pressure. This is the feed-forward of a FLOW phase.
 *
 * Near the stall pressure the pump cannot deliver the target at any duty, so the full duty is
 * returned and the caller's phase ceiling decides what is actually allowed.
 *
 * @return Duty in percent, 0..100
 */
inline float calculateDutyForFlow(float targetFlowMlS, float measuredBar, float stallBar,
                                  float maxFlowMlS, float exponent = SB_PUMP_PQ_EXPONENT_DEFAULT) {
    if (targetFlowMlS <= 0.0f || maxFlowMlS <= 0.0f || stallBar <= 0.0f) return 0.0f;
    const float deliverable = maxFlowMlS * pumpPressureShape(measuredBar, stallBar, exponent);
    if (deliverable <= 0.01f) return 100.0f;   // at or past stall - nothing short of full power
    const float duty = (targetFlowMlS / deliverable) * 100.0f;
    return duty > 100.0f ? 100.0f : duty;
}

/**
 * Hydraulic compliance correction: the flow that actually passes the puck.
 *
 * While the pressure rises, part of what the pump delivers is stored in the system (the line,
 * the group, the gas in the headspace) instead of passing the puck. While it falls, that stored
 * water is released through the puck. So
 *
 *     Q_puck = Q_pump - C * dP/dt
 *
 * with C the system compliance in ml/bar. C = 0 turns the correction off.
 *
 * @return Puck-side flow in ml/s, never negative
 */
inline float complianceCorrectedFlow(float pumpFlowMlS, float dPdtBarPerS, float complianceMlPerBar) {
    if (complianceMlPerBar <= 0.0f) return pumpFlowMlS > 0.0f ? pumpFlowMlS : 0.0f;
    const float q = pumpFlowMlS - complianceMlPerBar * dPdtBarPerS;
    return q > 0.0f ? q : 0.0f;
}

/**
 * The pressure rate that the compliance correction is allowed to use.
 *
 * A pressure trace that is steady at 9 bar still ripples by a few tenths of a bar (pump strokes,
 * sensor noise, the PID trimming). Differentiated, that ripple is +/-1-2 bar/s, and multiplied by
 * the compliance it swung the puck-flow estimate between ~0.4 and ~3.7 ml/s while the cup was
 * filling at a steady 2 g/s. Two things keep the ripple out while still correcting a real ramp:
 *
 *   - a slow low-pass on dP/dt (alpha 0.02 at 100 Hz, tau ~ 0.5 s): ripple averages towards zero,
 *     a ramp that lasts seconds does not;
 *   - a dead band of 1 bar/s: whatever ripple survives the filter is ignored, while a profile
 *     ramp (typically 2-4 bar/s) keeps everything above the band.
 *
 * On a 0 -> 9 bar ramp in 3 s this keeps ~2/3 of the correction; on +/-0.3 bar ripple it keeps
 * the estimate within ~0.1 ml/s of the uncorrected pump flow.
 */
class PuckFlowRate {
public:
    static constexpr float ALPHA         = 0.02f;   // per 10 ms tick
    static constexpr float DEADBAND_BARS = 1.0f;    // bar/s

    void reset(float pressureBar) {
        _prevP = pressureBar;
        _dPdt  = 0.0f;
    }

    // Feeds one control tick; returns the dead-banded rate in bar/s.
    float update(float pressureBar, float dtSec) {
        const float raw = (dtSec > 0.0f) ? (pressureBar - _prevP) / dtSec : 0.0f;
        _prevP = pressureBar;
        _dPdt  = ALPHA * raw + (1.0f - ALPHA) * _dPdt;
        if (_dPdt >  DEADBAND_BARS) return _dPdt - DEADBAND_BARS;
        if (_dPdt < -DEADBAND_BARS) return _dPdt + DEADBAND_BARS;
        return 0.0f;
    }

    float filteredRate() const { return _dPdt; }

private:
    float _prevP = 0.0f;
    float _dPdt  = 0.0f;
};
