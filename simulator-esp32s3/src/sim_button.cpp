// =================================================================================================
//  sim_button.cpp - Machine State Manager & Brew Button Operation
// =================================================================================================
#include "sim_button.h"
#include "sim_dimmer.h"
#include <esp_timer.h>
#include <soc/gpio_reg.h>

SimButtonManager g_simButton;

// -------------------------------------------------------------------------------------------------
// IRAM-safe pin read. Arduino's digitalRead() is a flash function and must not be called from an
// ISR: the flash cache can be disabled by the Wi-Fi/BLE stack, which would make the ISR fault.
// The GPIO input register is always accessible.
// -------------------------------------------------------------------------------------------------
static inline int simPinLevelIr(uint8_t pin) {
    return (int)((*((volatile uint32_t *)GPIO_IN_REG) >> pin) & 1U);
}

void SimButtonManager::begin() {
    pinMode(PIN_SIM_BTN_IN, INPUT_PULLDOWN);
    pinMode(PIN_SIM_BARISTA_BTN, INPUT_PULLUP);

    _lastBaristaState = digitalRead(PIN_SIM_BARISTA_BTN);
    _btnWasHigh      = (simPinLevelIr(PIN_SIM_BTN_IN) == 1);
    _btnRisePend     = false;
    _btnFallPend     = false;
    _btnActed        = false;

    // Capture both edges of the controller's relay pulse. Polling in the main loop could miss a
    // pulse entirely if the loop stalls (BLE activity, a blocking print, ...).
    attachInterrupt(digitalPinToInterrupt(PIN_SIM_BTN_IN), btnIsrTrampoline, CHANGE);
}

void IRAM_ATTR SimButtonManager::btnIsrTrampoline() {
    g_simButton.onBtnEdge();
}

void IRAM_ATTR SimButtonManager::onBtnEdge() {
    const uint32_t nowUs = (uint32_t)esp_timer_get_time();
    const bool high = (simPinLevelIr(PIN_SIM_BTN_IN) == 1);

    if (high) {
        if (!_btnWasHigh) {
            _btnRiseUs   = nowUs;
            _btnRisePend = true;
        }
        _btnWasHigh = true;
    } else {
        if (_btnWasHigh) {
            const uint32_t durUs = nowUs - _btnRiseUs;
            _btnWidthMs  = (uint16_t)(durUs / 1000ULL);
            _btnFallPend = true;
        }
        _btnWasHigh = false;
    }
}

void SimButtonManager::toggleMachine(const char* reason) {
    const bool current = isBrewing();
    setBrewing(!current, reason);
}

void SimButtonManager::setBrewing(bool brew, const char* reason) {
    if (brew) {
        _state.store(SimMachineState::BREWING, std::memory_order_relaxed);
        g_simDimmer.setMainsActive(true);
        Serial.printf("[MACHINE] State -> BREWING (Reason: %s, Mains ZC started)\n", reason);
    } else {
        _state.store(SimMachineState::IDLE, std::memory_order_relaxed);
        g_simDimmer.setMainsActive(false);
        Serial.printf("[MACHINE] State -> IDLE (Reason: %s, Mains ZC stopped)\n", reason);
    }
}

void SimButtonManager::actOnControllerPulse() {
    _controllerPulseCount++;
    _lastPulseTimestampMs = millis();
    Serial.println(F("[BTN] Controller pulsed brew relay -> toggling machine"));
    toggleMachine("Controller BTN_TRIG Relay Pulse");
}

void SimButtonManager::update() {
    const uint32_t nowUs = (uint32_t)esp_timer_get_time();
    const uint32_t nowMs = millis();

    // ---- 1. A pulse that has already finished --------------------------------------------------
    // The main loop may have stalled long enough for the whole pulse to come and go. The recorded
    // width is then the only evidence of the press, so judge it on that and act now.
    if (_btnFallPend) {
        _btnFallPend = false;
        _lastPulseDurationMs = _btnWidthMs;
        const bool valid = (_btnWidthMs >= BTN_MIN_PULSE_MS && _btnWidthMs <= BTN_MAX_PULSE_MS);
        if (_btnActed) {
            // We already acted on this pulse when its rising edge was debounced.
            if (!valid) {
                // The pulse turned out not to be a legal button press - undo the toggle.
                Serial.printf("[BTN] Controller pulse width %u ms is outside %u-%u ms - rolled back\n",
                              (unsigned)_btnWidthMs, (unsigned)BTN_MIN_PULSE_MS, (unsigned)BTN_MAX_PULSE_MS);
                toggleMachine("Controller BTN_TRIG invalid pulse (rolled back)");
            }
        } else if (valid) {
            // The main loop never got to debounce the rising edge (it stalled for the whole
            // pulse), but the pulse itself was measured - act on it now.
            actOnControllerPulse();
        }
        // The pulse is over: arm the next one. (Without this the machine would only ever
        // respond to the FIRST controller pulse - the start - and could never be stopped.)
        _btnActed = false;
    }

    // ---- 2. A pulse that is still high ---------------------------------------------------------
    // The level has been stable for the debounce window: this is a real button press. React now,
    // the way a momentary switch would, instead of waiting for the whole pulse to end.
    if (_btnRisePend && !_btnActed) {
        if (simPinLevelIr(PIN_SIM_BTN_IN) == 1 &&
            (nowUs - _btnRiseUs) >= (uint32_t)BTN_DEBOUNCE_MS * 1000ULL) {
            _btnActed = true;
            actOnControllerPulse();
        }
    }

    // ---- 3. Physical Barista Button (onboard BOOT button on GPIO 0) -------------------------
    const bool baristaState = digitalRead(PIN_SIM_BARISTA_BTN);
    if (baristaState != _lastBaristaState) {
        if (nowMs - _baristaDebounceMs > 50) {   // 50 ms debounce
            _baristaDebounceMs = nowMs;
            _lastBaristaState = baristaState;
            if (baristaState == LOW) { // Button pressed (active-LOW)
                Serial.println(F("[BTN] Barista pressed physical front-panel button!"));
                toggleMachine("Physical Barista Button Press");
            }
        }
    }
}
