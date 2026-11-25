// =================================================================================================
//  scale_flow.h - gravimetric flow from the scale, and the drip lag that the cutoff predicts
//
//  Two small rules live here:
//
//    WeightSlope      the cup flow in g/s: an ordinary-least-squares slope over the weight readings
//                     of the last `windowMs` milliseconds. The window is sized in TIME, not in
//                     samples, because the supported scales report at very different rates (an Acaia
//                     at ~20 Hz, a Timemore or Felicita at 5-8 Hz). A fixed sample count covered 1 s
//                     on one scale and 4 s on another, so the slope - and the cutoff prediction built
//                     on it - behaved differently per scale.
//
//    scaleLagMeasure  after a shot that was ended by the weight cutoff, how long the drip after the
//    scaleLagLearn    cutoff decision actually lasted, and how the configured lag moves towards it.
//
//  Header-only and free of Arduino dependencies on purpose: the host regression tests
//  (simulator-esp32s3/test/host/test_pump_flow.cpp) drive exactly the code the firmware runs.
// =================================================================================================
#pragma once

#include <stdint.h>
#include <string.h>

// ---- predictive cutoff -------------------------------------------------------------------------
// The cutoff fires when  W >= W_target - clamp(slope, MIN, MAX) * lag.  The clamp keeps a noisy
// slope from moving the cutoff point by more than a believable amount.
constexpr float SB_CUTOFF_SLOPE_MIN_GS = 0.5f;
constexpr float SB_CUTOFF_SLOPE_MAX_GS = 6.0f;

inline float sbCutoffSlopeClamp(float slopeGs) {
    if (slopeGs < SB_CUTOFF_SLOPE_MIN_GS) return SB_CUTOFF_SLOPE_MIN_GS;
    if (slopeGs > SB_CUTOFF_SLOPE_MAX_GS) return SB_CUTOFF_SLOPE_MAX_GS;
    return slopeGs;
}

// ---- regression window -------------------------------------------------------------------------
constexpr uint16_t SB_SLOPE_WINDOW_DEFAULT_MS = 1500;
constexpr uint16_t SB_SLOPE_WINDOW_MIN_MS     = 500;
constexpr uint16_t SB_SLOPE_WINDOW_MAX_MS     = 4000;

class WeightSlope {
public:
    // 4 s at 16 Hz. A faster scale with a long window simply drops its oldest readings early.
    static constexpr uint8_t  CAPACITY    = 64;
    // A slope needs a few readings spread over a meaningful time, or it is noise.
    static constexpr uint8_t  MIN_SAMPLES = 3;
    static constexpr uint32_t MIN_SPAN_MS = 300;
    // The slope is clamped to this range: the cup never gets lighter while it fills, and no
    // espresso shot pours faster than this.
    static constexpr float    MAX_SLOPE_GS = 20.0f;

    void reset() {
        _n = 0;
        _slope = 0.0f;
    }

    // Adds one reading taken at `tMs` and drops everything older than `windowMs`.
    void push(uint32_t tMs, float grams, uint32_t windowMs) {
        if (windowMs < SB_SLOPE_WINDOW_MIN_MS) windowMs = SB_SLOPE_WINDOW_MIN_MS;
        if (windowMs > SB_SLOPE_WINDOW_MAX_MS) windowMs = SB_SLOPE_WINDOW_MAX_MS;

        if (_n == CAPACITY) dropOldest(1);
        _t[_n] = tMs;
        _w[_n] = grams;
        _n++;

        // Signed age, so a reading that carries a slightly earlier timestamp cannot wrap round
        // to "four billion milliseconds old".
        uint8_t stale = 0;
        while (stale < _n - 1 && (int32_t)(tMs - _t[stale]) > (int32_t)windowMs) stale++;
        if (stale) dropOldest(stale);

        compute();
    }

    float   slope() const { return _slope; }
    uint8_t count() const { return _n; }
    uint32_t spanMs() const { return _n ? (_t[_n - 1] - _t[0]) : 0; }

private:
    void dropOldest(uint8_t k) {
        if (k >= _n) { _n = 0; return; }
        memmove(_t, _t + k, (size_t)(_n - k) * sizeof(_t[0]));
        memmove(_w, _w + k, (size_t)(_n - k) * sizeof(_w[0]));
        _n = (uint8_t)(_n - k);
    }

    void compute() {
        if (_n < MIN_SAMPLES || spanMs() < MIN_SPAN_MS) { _slope = 0.0f; return; }
        double sx = 0, sy = 0, sxy = 0, sxx = 0;
        for (uint8_t i = 0; i < _n; ++i) {
            const double x = (double)(_t[i] - _t[0]) * 0.001;
            const double y = _w[i];
            sx += x; sy += y; sxy += x * y; sxx += x * x;
        }
        const double dn  = _n;
        const double den = dn * sxx - sx * sx;
        if (den < 1e-9 && den > -1e-9) { _slope = 0.0f; return; }
        double m = (dn * sxy - sx * sy) / den;
        if (m < 0.0) m = 0.0;
        if (m > MAX_SLOPE_GS) m = MAX_SLOPE_GS;
        _slope = (float)m;
    }

    uint32_t _t[CAPACITY] = {};
    float    _w[CAPACITY] = {};
    uint8_t  _n = 0;
    float    _slope = 0.0f;
};

// ---- learned drip lag --------------------------------------------------------------------------
//
// The cutoff fires at  W_cut = W_target - s * lag  (s = the clamped slope at that moment). After
// it fires, the drip still adds  drip = W_settled - W_cut  to the cup. The yield lands on target
// when  s * lag == drip, so the lag this installation actually needs is
//
//     lag_measured = drip / s
//
// Measuring from the cutoff reading rather than from the target makes the estimate immune to how
// far one scale reading happened to overshoot the threshold.
//
// One shot is not trusted on its own: the configured lag moves a fraction of the way towards each
// measurement, so it converges over roughly ten shots and a single odd shot cannot throw it off.
constexpr float SB_LAG_LEARN_ALPHA     = 0.1f;   // fraction of the way per shot
constexpr float SB_LAG_MIN_SLOPE_GS    = 0.3f;   // slower than this the drip/slope ratio is noise
constexpr float SB_LAG_MAX_DRIP_G      = 6.0f;   // more than this is a bumped scale, not a drip
constexpr float SB_LAG_MAX_SEC         = 3.0f;   // the configurable range of scaleLagSec

// Returns false when the shot cannot tell us anything about the lag.
inline bool scaleLagMeasure(float cutoffG, float settledG, float slopeAtCutoffGs, float& outLagSec) {
    if (!(slopeAtCutoffGs >= SB_LAG_MIN_SLOPE_GS)) return false;   // also rejects NaN
    const float drip = settledG - cutoffG;
    if (!(drip >= 0.0f) || drip > SB_LAG_MAX_DRIP_G) return false;
    float lag = drip / sbCutoffSlopeClamp(slopeAtCutoffGs);
    if (lag > SB_LAG_MAX_SEC) lag = SB_LAG_MAX_SEC;
    outLagSec = lag;
    return true;
}

inline float scaleLagLearn(float currentLagSec, float measuredLagSec) {
    float next = currentLagSec + SB_LAG_LEARN_ALPHA * (measuredLagSec - currentLagSec);
    if (next < 0.0f) next = 0.0f;
    if (next > SB_LAG_MAX_SEC) next = SB_LAG_MAX_SEC;
    return next;
}
