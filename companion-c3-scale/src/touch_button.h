// touch_button.h - TTP223 single-channel capacitive touch key (HW-763 style).
//
// The module output is latched by default ("self-locking" solder bridges), so a
// single touch raises OUT until the next touch clears it. This driver therefore
// derives the press from the RISING edge and emits the event on the FALLING
// edge, which yields identical behaviour for:
//
//   * toggle mode   - rising = touch 1, falling = touch 2
//   * momentary mode - rising = press, falling = release
//
// Poll at >= 20 Hz.

#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class TouchEvent : uint8_t {
    None = 0,
    Tap,      // short press  -> tare
    Hold,     // long press   -> display power toggle
};

class TouchButton {
public:
    TouchButton();

    void begin(uint8_t pin, bool activeHigh = true);

    // Returns one event per press; call at >= 20 Hz.
    TouchEvent poll(uint32_t nowMs);

    bool rawPressed() const { return _level; }
    uint8_t pin() const { return _pin; }

private:
    bool     readLevel() const;

    uint8_t  _pin;
    bool     _activeHigh;
    bool     _level;        // debounced logical level
    bool     _lastRaw;
    bool     _inPress;
    uint32_t _pressStart;
    uint32_t _lastChange;
};