#include "touch_button.h"

#include "scale_config.h"

TouchButton::TouchButton()
    : _pin(PIN_TOUCH_OUT), _activeHigh(true), _level(false), _lastRaw(false),
      _inPress(false), _pressStart(0), _lastChange(0) {}

void TouchButton::begin(uint8_t pin, bool activeHigh) {
    _pin = pin;
    _activeHigh = activeHigh;
    pinMode(_pin, INPUT);          // TTP223 drives the pad, module pull-up
    _lastRaw = readLevel();
    _level = _lastRaw;
    _inPress = _level;
    _pressStart = millis();
    _lastChange = millis();
}

bool TouchButton::readLevel() const {
    const bool raw = digitalRead(_pin) != LOW;
    return _activeHigh ? raw : !raw;
}

TouchEvent TouchButton::poll(uint32_t nowMs) {
    const bool raw = readLevel();
    if (raw != _lastRaw) {
        _lastRaw = raw;
        _lastChange = nowMs;       // restart the debounce window
    }

    if (raw == _level) {
        return TouchEvent::None;   // still debouncing, or no change
    }

    if ((uint32_t)(nowMs - _lastChange) < TOUCH_DEBOUNCE_MS) {
        return TouchEvent::None;
    }

    _level = raw;
    if (_level) {
        _inPress = true;
        _pressStart = nowMs;
        return TouchEvent::None;
    }

    if (!_inPress) {
        return TouchEvent::None;   // spurious falling edge
    }
    _inPress = false;

    const uint32_t held = nowMs - _pressStart;
    return (held >= TOUCH_HOLD_MS) ? TouchEvent::Hold : TouchEvent::Tap;
}