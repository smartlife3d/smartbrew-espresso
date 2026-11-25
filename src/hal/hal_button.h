// =================================================================================================
//  hal_button.h - Non-blocking momentary pulse generator for the galvanically isolated virtual
//                 brew button (TLP222A / AQY212EH / PC817).   docs/Architecture.md
//
//  The relay is wired in parallel with the espresso machine's own momentary brew switch, so a
//  180 ms pulse is indistinguishable from a human finger: the machine's stock logic board still
//  owns the boiler PID, the 3-way solenoid and the pump feed.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include "config_schema.h"
#include "pins.h"

enum class ButtonResult : uint8_t {
    PULSED,
    BLOCKED_RETRIGGER,     // inside the minimum retrigger dead time
    BLOCKED_DISARMED
};

enum class ButtonPurpose : uint8_t {
    NONE = 0,
    START,
    STOP,
    MANUAL
};

class ButtonRelay {
public:
    void begin();

    // Starts a pulse and returns immediately. Called from task context only.
    ButtonResult pulse();
    // Bench wiring test only: one pulse of `ms` (up to 30 s), ignoring the 2 s cap on the
    // configured press length. The configured length is left unchanged.
    ButtonResult pulseFor(uint32_t ms);

    // Requests a pulse; if blocked by dead-time or busy, queues it and fires as soon as ready.
    void requestPulse(ButtonPurpose purpose = ButtonPurpose::NONE);

    // Cancels any pending/deferred pulse without altering currently executing pulse
    void cancelPending();

    // Forces the output low right now (used by the OTA disarm path).
    void abort();

    bool          busy() const           { return _busy; }
    bool          pending() const        { return _pending; }
    bool          isArmed() const        { return _armed; }
    ButtonPurpose pendingPurpose() const { return _pendingPurpose; }
    uint32_t      pendingAgeMs() const   { return _pending ? (millis() - _pendingRequestedMs) : 0; }
    uint32_t      pulses() const         { return _pulses; }
    uint32_t      blocked() const        { return _blocked; }
    uint32_t      lastPulseMs() const    { return _lastPulseMs; }
    uint16_t      pulseWidthMs() const   { return _widthMs; }
    void setPulseWidthMs(uint16_t ms);

    // Services the trailing edge and any pending pulse. Call from the 100 Hz control loop.
    void service();

private:
    static void releaseCb(void* arg);

    volatile bool          _busy = false;
    volatile bool          _pending = false;
    volatile ButtonPurpose _pendingPurpose = ButtonPurpose::NONE;
    volatile uint32_t      _pendingRequestedMs = 0;
    volatile bool          _armed = false;
    uint32_t               _pulses = 0;
    uint32_t               _blocked = 0;
    uint32_t               _lastPulseMs = 0;
    uint16_t               _widthMs = BTN_PULSE_MS;
    uint32_t               _activeWidthMs = BTN_PULSE_MS;   // length of the pulse in flight
    esp_timer_handle_t     _timer = nullptr;
};

extern ButtonRelay g_button;

// ------------------------------------------------------------------------------------------------
// Onboard status LED: blue=boot, green=ready, cyan=brewing, red=fault (docs/Hardware.md)
// ------------------------------------------------------------------------------------------------
class StatusLed {
public:
    void begin();
    void update(uint32_t nowMs, bool brewing, bool ready, bool fault);
    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void setBrightness(uint8_t pct);
    uint8_t brightness() const { return _brightness; }
    void clearManual() { _manual = false; }
    void setManual(bool m = true) { _manual = m; }
private:
    uint8_t _r = 0, _g = 0, _b = 0;
    uint8_t _brightness = 10;
    bool _manual = false;
    uint32_t _nextUpdate = 0;
};

extern StatusLed g_statusLed;