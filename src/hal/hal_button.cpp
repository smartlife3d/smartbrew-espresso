// =================================================================================================
//  hal_button.cpp - see hal_button.h
// =================================================================================================

#include "hal_button.h"
#include "logger.h"

#include <esp_timer.h>
#include <esp_cpu.h>

// -------------------------------------------------------------------------------------------------
// ButtonRelay
// -------------------------------------------------------------------------------------------------
void ButtonRelay::begin() {
    // De-energised before anything else can fail.
    pinMode(PIN_BTN_TRIG, OUTPUT);
    digitalWrite(PIN_BTN_TRIG, LOW);
    _widthMs  = g_config.cfg().buttonPulseMs ? g_config.cfg().buttonPulseMs : BTN_PULSE_MS;
    _busy     = false;
    _armed    = true;

    SB_LOGI("Virtual button armed, pulse=%ums, retrigger dead time=%ums",
            _widthMs, g_config.cfg().buttonMinRetriggerMs);
}

void ButtonRelay::setPulseWidthMs(uint16_t ms) {
    if (ms < 20)  ms = 20;
    if (ms > 2000) ms = 2000;
    _widthMs = ms;
    g_config.cfg().buttonPulseMs = ms;
}

ButtonResult ButtonRelay::pulseFor(uint32_t ms) {
    if (ms < 20) ms = 20;
    if (ms > 30000) ms = 30000;
    const uint16_t saved = _widthMs;
    _widthMs = (uint16_t)std::min<uint32_t>(ms, 65535U);
    const ButtonResult r = pulse();
    _widthMs = saved;              // the configured press length is untouched
    return r;
}

ButtonResult ButtonRelay::pulse() {
    if (!_armed) return ButtonResult::BLOCKED_DISARMED;
    if (_busy)    return ButtonResult::BLOCKED_RETRIGGER;

    // Debounce the machine's own button: two pulses closer than `buttonMinRetriggerMs` would read
    // as a double-click to some machine logic boards and abort the brew instead of stopping it.
    const uint32_t now = millis();
    if (_lastPulseMs != 0 &&
        (now - _lastPulseMs) < (uint32_t)g_config.cfg().buttonMinRetriggerMs) {
        _blocked++;
        return ButtonResult::BLOCKED_RETRIGGER;
    }

    digitalWrite(PIN_BTN_TRIG, HIGH);
    _busy = true;
    _activeWidthMs = _widthMs;
    _lastPulseMs = now;
    _pulses++;

    // One-shot from task context: fully legal timer usage, and far more precise than polling the
    // control loop. Releases the pin after `pulseWidthMs`.
    if (!_timer) {
        esp_timer_create_args_t args = {};
        args.callback = &ButtonRelay::releaseCb;
        args.arg      = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name     = "sb_btn_release";
        args.skip_unhandled_events = true;
        esp_timer_create(&args, &_timer);
    }
    if (_timer) {
        esp_timer_start_once(_timer, (uint64_t)_activeWidthMs * 1000ULL);
    } else {
        // No timer available (should not happen) - fall back to the coarse service() path.
        SB_LOGE("button release timer unavailable; falling back to 100 Hz polling");
    }
    return ButtonResult::PULSED;
}

void ButtonRelay::requestPulse(ButtonPurpose purpose) {
    if (!_armed) return;
    const uint32_t now = millis();
    const uint32_t deadTime = (uint32_t)g_config.cfg().buttonMinRetriggerMs;
    if (!_busy && (_lastPulseMs == 0 || (now - _lastPulseMs) >= deadTime)) {
        pulse();
        _pending = false;
        _pendingPurpose = ButtonPurpose::NONE;
        _pendingRequestedMs = 0;
    } else {
        _pending = true;
        _pendingPurpose = purpose;
        _pendingRequestedMs = now;
    }
}

void ButtonRelay::cancelPending() {
    _pending = false;
    _pendingPurpose = ButtonPurpose::NONE;
    _pendingRequestedMs = 0;
}

void ButtonRelay::releaseCb(void* arg) {
    ButtonRelay* self = (ButtonRelay*)arg;
    digitalWrite(PIN_BTN_TRIG, LOW);
    self->_busy = false;
}

void ButtonRelay::abort() {
    if (_timer) esp_timer_stop(_timer);
    digitalWrite(PIN_BTN_TRIG, LOW);
    _busy = false;
    _pending = false;
    _pendingPurpose = ButtonPurpose::NONE;
    _pendingRequestedMs = 0;
}

void ButtonRelay::service() {
    const uint32_t now = millis();
    if (_busy && (now - _lastPulseMs >= _activeWidthMs)) {
        digitalWrite(PIN_BTN_TRIG, LOW);
        _busy = false;
    }
    if (_pending) {
        // C-07: Give STOP pulses a generous but finite expiry (at least deadTime + _widthMs + 2500ms)
        // so a STOP right after START is never dropped, while a stuck pulse cannot fire on a later shot.
        const uint32_t deadTime = (uint32_t)g_config.cfg().buttonMinRetriggerMs;
        const uint32_t expiryMs = (_pendingPurpose == ButtonPurpose::STOP)
                                ? std::max<uint32_t>(3000U, deadTime + _widthMs + 2500U)
                                : std::max<uint32_t>(500U,  deadTime + _widthMs + 250U);
        if (_pendingRequestedMs != 0 && (now - _pendingRequestedMs > expiryMs)) {
            SB_LOGW("Pending button pulse (purpose=%d) expired (>%lums) - discarded",
                    (int)_pendingPurpose, (unsigned long)expiryMs);
            _pending = false;
            _pendingPurpose = ButtonPurpose::NONE;
            _pendingRequestedMs = 0;
        } else if (!_busy) {
            if (_lastPulseMs == 0 || (now - _lastPulseMs) >= deadTime) {
                _pending = false;
                _pendingPurpose = ButtonPurpose::NONE;
                _pendingRequestedMs = 0;
                pulse();
            }
        }
    }
}

// -------------------------------------------------------------------------------------------------
// StatusLed: Blue=Boot, Green=Ready/Idle, Cyan=Brewing, Red=Fault
// -------------------------------------------------------------------------------------------------
void StatusLed::begin() {
    _brightness = g_config.cfg().ledBrightness ? g_config.cfg().ledBrightness : 10;
    _manual = false;
    _nextUpdate = 0;
    // Blue on boot
    setColor(0, 0, 255);
}

void StatusLed::setBrightness(uint8_t pct) {
    _brightness = constrain(pct, 0, 100);
    setColor(_r, _g, _b);
}

void StatusLed::setColor(uint8_t r, uint8_t g, uint8_t b) {
    _r = r;
    _g = g;
    _b = b;
    const uint8_t sr = (uint8_t)(((uint16_t)r * _brightness) / 100);
    const uint8_t sg = (uint8_t)(((uint16_t)g * _brightness) / 100);
    const uint8_t sb = (uint8_t)(((uint16_t)b * _brightness) / 100);
#if defined(PIN_RGB_LED) && PIN_RGB_LED >= 0
    rgbLedWrite(PIN_RGB_LED, sr, sg, sb);
#endif
}

void StatusLed::update(uint32_t nowMs, bool brewing, bool ready, bool fault) {
    if (_manual) return;
    if (nowMs < _nextUpdate) return;
    _nextUpdate = nowMs + 100; // 10 Hz refresh

    if (fault) {
        // Red: Fault
        setColor(255, 0, 0);
    } else if (brewing) {
        // Cyan: Profile Brewing active
        setColor(0, 200, 255);
    } else if (ready) {
        // Green: Ready / Idle (100% passthrough armed)
        setColor(0, 255, 0);
    } else {
        // Blue: Booting / Connecting
        setColor(0, 0, 255);
    }
}

// -------------------------------------------------------------------------------------------------
ButtonRelay  g_button;
StatusLed    g_statusLed;