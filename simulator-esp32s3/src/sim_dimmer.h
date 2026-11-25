// =================================================================================================
//  sim_dimmer.h - Zero-Cross Pulse Generation & PSM Gate Measurement
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>
#include "sim_pins.h"

class SimDimmer {
public:
    void begin(uint32_t mainsHz = 50);

    // Turn mains zero-crossing pulse generation on or off (simulates machine pump AC relay)
    void setMainsActive(bool active);
    bool isMainsActive() const { return _mainsActive.load(std::memory_order_relaxed); }

    // Switch between 50 Hz and 60 Hz mains
    void setMainsHz(uint32_t hz);
    uint32_t mainsHz() const { return _mainsHz.load(std::memory_order_relaxed); }

    // Measured duty cycle (0.0 to 100.0%) from the controller's PSM gate pin, averaged over the
    // last 400 ms. For the console: it is steady enough to read.
    float measuredDutyPct() const { return _measuredDutyPct.load(std::memory_order_relaxed); }

    // What the pump actually receives (0.0 to 100.0%): every conducting half-cycle is one piston
    // stroke, smoothed only by the pump's own inertia (PUMP_TAU_MS). The physics model uses this.
    // The 400 ms console average must not drive the pump: it delays the plant by ~200 ms, which a
    // real vibratory pump does not do, and makes the controller's pressure loop oscillate.
    float pumpDrivePct() const { return _pumpDrivePct.load(std::memory_order_relaxed); }

    // Counters for diagnostics
    uint32_t zcPulseCount() const { return _zcCount.load(std::memory_order_relaxed); }
    uint32_t gatePulseCount() const { return _gateCount.load(std::memory_order_relaxed); }

    // Service loop (called in main loop for telemetry / moving average calculations)
    void update();

private:
    // The GPIO interrupt handler must live in IRAM; the esp_timer callback runs in a task.
    static void timerCallback(void* arg);
    static void IRAM_ATTR gateIsrTrampoline();
    void IRAM_ATTR onGateEdge();

    std::atomic<bool>     _mainsActive{false};
    std::atomic<uint32_t> _mainsHz{50};
    std::atomic<uint32_t> _zcCount{0};
    std::atomic<uint32_t> _gateCount{0};
    std::atomic<float>    _measuredDutyPct{0.0f};
    std::atomic<float>    _pumpDrivePct{0.0f};
    float                 _pumpDriveState = 0.0f;   // timer task only
    std::atomic<bool>     _windowResetPending{false};

    esp_timer_handle_t    _timer = nullptr;

    // Moving window of zero-crossing cycles used to calculate the duty percentage.
    // Only the ZC timer task touches this array, so there is no cross-task race.
    static constexpr size_t WINDOW_SIZE = 40;   // 40 half-cycles = 400 ms at 50 Hz mains
    // Response time of the pump to a change in the stroke pattern.
    static constexpr float  PUMP_TAU_MS = 40.0f;
    uint8_t  _window[WINDOW_SIZE] = {0};
    size_t   _windowIdx = 0;
    volatile bool _currentCycleFired = false;
};

extern SimDimmer g_simDimmer;

