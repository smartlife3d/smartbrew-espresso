// =================================================================================================
//  flow_calibration.h - calibrating the pump flow model against the scale
//
//  The virtual flow comes from a pump model with factory numbers (include/pump_presets.h). No two
//  pumps, mains supplies and machines are alike, so the estimate is typically 10-30 % off. A scale
//  measures what actually reaches the cup, so while the shot is in a steady state the two can be
//  compared directly and the model scaled to match:
//
//      flowScale_new = flowScale * (1 + ALPHA * (measured / model - 1))
//
//  The comparison is only valid when what the pump pushes is what the cup receives:
//    * the pressure is steady (otherwise the line and the puck are storing or releasing water),
//    * the pressure is up (a saturated, flowing puck - not pre-infusion or a headspace fill),
//    * the scale reports a real pour, and the lockout at the start of the shot is over.
//  The ticks that pass all of these are summed over the whole shot, and only a shot with at least
//  MIN_TICKS of them (3 s at 100 Hz) is used.
//
//  One shot is not trusted on its own: the scale moves a fraction of the way towards each
//  measurement and is clamped to a believable range, so an odd shot cannot throw it off.
//
//  Header-only and free of Arduino dependencies: the host regression tests
//  (simulator-esp32s3/test/host/test_pump_flow.cpp) drive the exact code the firmware runs.
// =================================================================================================
#pragma once

#include <stdint.h>

// Espresso is slightly denser than water; the scale reports grams, the model millilitres.
constexpr float SB_ESPRESSO_DENSITY_G_PER_ML = 1.03f;

constexpr float SB_FLOW_SCALE_MIN     = 0.5f;
constexpr float SB_FLOW_SCALE_MAX     = 1.5f;
constexpr float SB_FLOW_SCALE_ALPHA   = 0.25f;   // fraction of the way per shot

class FlowCalibration {
public:
    static constexpr float    MIN_PRESSURE_BAR = 4.0f;
    static constexpr float    MAX_DPDT_BAR_S   = 0.5f;
    static constexpr float    MIN_CUP_FLOW     = 0.5f;   // ml/s
    static constexpr float    MIN_MODEL_FLOW   = 0.3f;   // ml/s
    static constexpr uint32_t MIN_TICKS        = 300;    // 3 s at 100 Hz
    // A single shot that disagrees with the model by more than this is a bad reading (a cup
    // knocked, a scale filter hiccup), not a pump that is twice as strong as its preset.
    static constexpr float    MIN_RATIO        = 0.5f;
    static constexpr float    MAX_RATIO        = 2.0f;

    void reset() {
        _sumModel = 0.0;
        _sumCup   = 0.0;
        _ticks    = 0;
    }

    // Feeds one control tick. `inSteadyShot` is the caller's own gate: the shot is brewing, the
    // scale is live and the lockout is over. `modelFlowMlS` must be the pump-side model flow with
    // the CURRENT flow scale applied, so the ratio is a correction of what is in use now.
    void addTick(bool inSteadyShot, float pressureBar, float dPdtBarS,
                 float modelFlowMlS, float cupFlowMlS) {
        if (!inSteadyShot) return;
        if (pressureBar < MIN_PRESSURE_BAR) return;
        if (dPdtBarS > MAX_DPDT_BAR_S || dPdtBarS < -MAX_DPDT_BAR_S) return;
        if (!(cupFlowMlS >= MIN_CUP_FLOW) || !(modelFlowMlS >= MIN_MODEL_FLOW)) return;
        _sumModel += modelFlowMlS;
        _sumCup   += cupFlowMlS;
        _ticks++;
    }

    uint32_t ticks() const { return _ticks; }

    // measured / model over every qualifying tick of the shot. False when the shot does not have
    // enough steady data, or the ratio is not believable.
    bool ratio(float& out) const {
        if (_ticks < MIN_TICKS || _sumModel <= 0.0) return false;
        const float r = (float)(_sumCup / _sumModel);
        if (!(r >= MIN_RATIO && r <= MAX_RATIO)) return false;
        out = r;
        return true;
    }

private:
    double   _sumModel = 0.0;
    double   _sumCup   = 0.0;
    uint32_t _ticks    = 0;
};

inline float flowScaleClamp(float s) {
    if (s != s) return 1.0f;                         // NaN: no correction
    if (s < SB_FLOW_SCALE_MIN) return SB_FLOW_SCALE_MIN;
    if (s > SB_FLOW_SCALE_MAX) return SB_FLOW_SCALE_MAX;
    return s;
}

inline float flowScaleLearn(float currentScale, float ratio) {
    return flowScaleClamp(currentScale * (1.0f + SB_FLOW_SCALE_ALPHA * (ratio - 1.0f)));
}
