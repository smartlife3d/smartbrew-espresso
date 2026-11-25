// =================================================================================================
//  weight_settle.h - the post-shot weight tail
//
//  The cutoff reading is not the yield. When the pump stops, the last of the coffee is still
//  draining out of the grouphead, so the cup keeps getting heavier for a second or two. A client
//  that latches the value which tripped the cutoff therefore shows a yield that is too low.
//
//  This tiny state machine watches the scale through that tail and decides when the yield is
//  final. The settled yield is the HIGHEST weight seen in the window, so lifting the cup
//  afterwards cannot pull the number back down.
//
//  It is header-only and free of Arduino dependencies on purpose: the host regression tests drive
//  it exactly the way the firmware does, so the timing rules are covered by real assertions
//  rather than by inspection.
// =================================================================================================
#pragma once

#include <stdint.h>

// A move of at least this much counts as "the cup is still filling".
constexpr float    WEIGHT_SETTLE_DELTA_G   = 0.2f;
// A move of at least this much means the cup was taken off the scale, so the yield is final.
constexpr float    WEIGHT_SETTLE_LIFT_G    = 1.0f;
// How long the weight has to stay still before the yield is considered final.
constexpr uint32_t WEIGHT_SETTLE_STABLE_MS = 2000UL;
// The window never stays open longer than this, whatever the scale does.
constexpr uint32_t WEIGHT_SETTLE_MAX_MS    = 10000UL;

enum class WeightSettleResult : uint8_t {
    OPEN = 0,      // still watching the cup fill
    SETTLED,       // the weight stopped changing
    LIFTED,        // the cup was taken off the scale
    DISCONNECTED,  // the scale link went away
    TIMED_OUT      // the window expired
};

class WeightSettle {
public:
    // Opens the window at the moment the pump stops.
    void open(uint32_t nowMs, float weightG) {
        _active = true;
        _startMs = nowMs;
        _stableSinceMs = nowMs;
        _lastWeightG = weightG;
        _finalWeightG = weightG;
    }

    void reset() {
        _active = false;
        _startMs = 0;
        _stableSinceMs = 0;
        _lastWeightG = 0.0f;
        _finalWeightG = 0.0f;
    }

    bool  active() const { return _active; }
    float finalWeightG() const { return _finalWeightG; }

    // Feeds one weight reading. `connected` is false when the scale link is gone.
    WeightSettleResult update(uint32_t nowMs, float weightG, bool connected) {
        if (!_active) return WeightSettleResult::SETTLED;

        // The elapsed times are computed as SIGNED deltas on purpose. The window is opened with
        // the millisecond clock, while the updates arrive with the control tick's clock, which was
        // captured a moment earlier - so the first update can carry a timestamp that is a few
        // milliseconds in the PAST. An unsigned subtraction would wrap that round to ~4.29e9 and
        // read as "the window has already expired", which would kill the tail on its first tick.
        const int32_t sinceStart  = (int32_t)(nowMs - _startMs);
        const int32_t sinceStable = (int32_t)(nowMs - _stableSinceMs);

        // The highest weight seen is the yield in the cup, so it is tracked on every reading -
        // not only on the ones that register as a change.
        if (weightG > _finalWeightG) _finalWeightG = weightG;

        // The reading is processed FIRST: if the link dropped on the very last update, that
        // reading is still the best information about the cup that we have.
        if (weightG < _lastWeightG - WEIGHT_SETTLE_LIFT_G) {
            // The cup came off the scale. The yield is final - do not follow it back down.
            _active = false;
            return WeightSettleResult::LIFTED;
        }
        if (weightG > _lastWeightG + WEIGHT_SETTLE_DELTA_G) {
            // The cup is still filling. This is the tail the operator wants to see.
            _lastWeightG = weightG;
            _stableSinceMs = nowMs;
        }

        if (!connected) {
            _active = false;
            return WeightSettleResult::DISCONNECTED;
        }

        // The cap comes before the stable check, because a scale that creeps upward for ever
        // would otherwise keep taking the "the cup is still filling" branch and never reach it.
        if (sinceStart >= (int32_t)WEIGHT_SETTLE_MAX_MS) {
            _active = false;
            return WeightSettleResult::TIMED_OUT;
        }

        // Within the noise band: the weight is not moving enough to call it a change.
        if (sinceStable >= (int32_t)WEIGHT_SETTLE_STABLE_MS) {
            _active = false;
            return WeightSettleResult::SETTLED;
        }
        return WeightSettleResult::OPEN;
    }

private:
    bool     _active = false;
    uint32_t _startMs = 0;
    uint32_t _stableSinceMs = 0;
    float    _lastWeightG = 0.0f;
    float    _finalWeightG = 0.0f;
};
