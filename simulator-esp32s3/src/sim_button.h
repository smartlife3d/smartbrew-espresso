// =================================================================================================
//  sim_button.h - Machine State Manager & Brew Button Operation
//
//  The real machine has a MOMENTARY brew switch: pressing it toggles the pump. The controller
//  emulates that press with a PhotoMOS relay pulse on PIN_BTN_TRIG (180 ms by default).
//
//  The simulator therefore reacts on the RISING edge of that pulse, exactly like a real switch
//  would - waiting for the falling edge would add the full pulse width (180 ms) of latency to
//  every shot start and every stop.
//
//  Both edges are captured in the GPIO ISR so a stalled main loop can never lose a pulse. The ISR
//  reads the pin through the GPIO input register: Arduino's digitalRead() lives in flash and must
//  not be called from an ISR (the flash cache can be disabled by the Wi-Fi/BLE stack).
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>
#include "sim_pins.h"

enum class SimMachineState : uint8_t {
    IDLE = 0,
    BREWING
};

class SimButtonManager {
public:
    void begin();

    // Call from main loop to process debounce, pulses, and button inputs
    void update();

    // Machine extraction state
    SimMachineState state() const { return _state.load(std::memory_order_relaxed); }
    bool isBrewing() const { return _state.load(std::memory_order_relaxed) == SimMachineState::BREWING; }

    // Manually toggle machine state (e.g. barista front-panel button press or CLI)
    void toggleMachine(const char* reason = "Manual Barista Button");
    void setBrewing(bool brew, const char* reason = "Command");

    // Telemetry / Diagnostics
    uint32_t controllerPulseCount() const { return _controllerPulseCount; }
    uint32_t lastPulseDurationMs() const { return _lastPulseDurationMs; }
    uint32_t lastPulseTimestampMs() const { return _lastPulseTimestampMs; }

    // The controller constrains buttonPulseMs to 20..2000 ms (config_store.cpp), so a legal
    // controller pulse is never shorter than this.
    static constexpr uint16_t BTN_MIN_PULSE_MS  = 20;
    static constexpr uint16_t BTN_MAX_PULSE_MS  = 2500;
    // The level has to be stable this long before the machine reacts. It must stay below
    // BTN_MIN_PULSE_MS so that every legal controller pulse is still caught.
    static constexpr uint16_t BTN_DEBOUNCE_MS   = 10;

private:
    static void IRAM_ATTR btnIsrTrampoline();
    void IRAM_ATTR onBtnEdge();
    void actOnControllerPulse();

    std::atomic<SimMachineState> _state{SimMachineState::IDLE};

    // Controller virtual button input (PIN_SIM_BTN_IN / GPIO 6). The edges are captured in the
    // ISR so a stalled main loop can never miss a pulse.
    volatile uint32_t _btnRiseUs    = 0;
    volatile uint16_t _btnWidthMs   = 0;
    volatile bool     _btnRisePend  = false;
    volatile bool     _btnFallPend  = false;
    volatile bool     _btnWasHigh   = false;
    bool              _btnActed     = false;   // the pulse in flight has already toggled

    uint32_t _controllerPulseCount = 0;
    uint32_t _lastPulseDurationMs = 0;
    uint32_t _lastPulseTimestampMs = 0;

    // Physical barista button input (GPIO 0 BOOT button)
    bool     _lastBaristaState = HIGH;
    uint32_t _baristaDebounceMs = 0;
};

extern SimButtonManager g_simButton;
