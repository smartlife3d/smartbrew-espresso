// =================================================================================================
//  sim_pressure.cpp - Simulated Pressure Transducer Output (PWM/DAC & Voltage Output)
//
//  The real signal chain is:
//
//      transducer 0.5 - 4.5 V  ->[ 10k / 20k divider ]  ->  0.333 - 3.000 V  -> controller ADC
//
//  The simulator drives the node AFTER the divider, i.e. it emits 333.33 mV for 0 bar and
//  3000 mV for full scale, using the same full scale the controller is configured for.
//
//  A healthy 0.5-4.5 V transducer physically cannot leave the 0.333-3.000 V window, so the
//  controller's open-wire (<133 mV) and over-range (>3050 mV) protections can never trip on
//  healthy hardware. The manual override and the raw node-voltage injection deliberately break
//  that limit so those protection paths can be tested.
// =================================================================================================
#include "sim_pressure.h"

SimPressure g_simPressure;

void SimPressure::begin() {
#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
    ledcAttach(PIN_SIM_PRESSURE_PWM, PWM_FREQ_HZ, PWM_RESOLUTION);
    Serial.printf("[PRESSURE] LEDC PWM on GPIO %d at %u Hz (10-bit) -> RC filter -> controller ADC\n",
                  PIN_SIM_PRESSURE_PWM, (unsigned)PWM_FREQ_HZ);
#else
    uint32_t actFreq = ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION);
    ledcAttachPin(PIN_SIM_PRESSURE_PWM, PWM_CHANNEL);
    Serial.printf("[PRESSURE] LEDC PWM on GPIO %d at %u Hz (10-bit) -> RC filter -> controller ADC\n",
                  PIN_SIM_PRESSURE_PWM, (unsigned)actFreq);
#endif

    setBar(0.0f);
}

void SimPressure::setFullScaleBar(float bar) {
    if (bar < 1.0f)  bar = 1.0f;
    if (bar > 32.0f) bar = 32.0f;
    _fullScaleBar = bar;
    // Re-emit immediately so the change is visible without waiting for the next physics tick.
    if (_manualOverride) {
        setBar(_overrideBar);
    }
    Serial.printf("[PRESSURE] Transducer full scale -> %.1f bar (controller MUST use the same range: "
                  "1.2 MPa = 12 bar, 1.6 MPa = 16 bar)\n", _fullScaleBar);
}

void SimPressure::emitMv(float mv) {
    if (mv < 0.0f) mv = 0.0f;
    if (mv > PWM_RAIL_MV) mv = PWM_RAIL_MV;
    _currentMv.store(mv, std::memory_order_relaxed);

    // Convert millivolts to 10-bit PWM duty (assuming nominal 3300 mV rail)
    int duty = (int)((mv / PWM_RAIL_MV) * 1023.0f + 0.5f);
    if (duty < 0) duty = 0;
    if (duty > 1023) duty = 1023;

#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
    ledcWrite(PIN_SIM_PRESSURE_PWM, duty);
#else
    ledcWrite(PWM_CHANNEL, duty);
#endif
}

void SimPressure::setBar(float bar) {
    if (_rawMvOverride) {
        // Fault injection wins: the node voltage is forced.
        emitMv(_rawMv);
        return;
    }

    if (_manualOverride) {
        // The override may leave the transducer's nominal range on purpose, so that the
        // controller's over-range protection can be exercised.
        bar = _overrideBar;
        if (bar < 0.0f) bar = 0.0f;
        if (bar > 20.0f) bar = 20.0f;
    } else {
        // The hydraulic model: a real 0.5-4.5 V transducer simply cannot read beyond its range.
        if (bar < 0.0f) bar = 0.0f;
        if (bar > _fullScaleBar) bar = _fullScaleBar;
    }

    _currentBar.store(bar, std::memory_order_relaxed);

    // Node voltage: the controller divides by (sensorFsMv - sensorZeroMv) and scales by its own
    // full scale, so the simulator has to use the identical mapping.
    const float mv = NODE_ZERO_MV + (bar / _fullScaleBar) * (NODE_FS_MV - NODE_ZERO_MV);
    emitMv(mv);
}

void SimPressure::setManualOverride(bool enable, float bar) {
    _manualOverride = enable;
    _overrideBar = bar;
    if (enable) {
        setBar(bar);
        Serial.printf("[PRESSURE] Manual override locked at %.2f bar (%.1f mV)%s\n",
                      _currentBar.load(std::memory_order_relaxed),
                      _currentMv.load(std::memory_order_relaxed),
                      (bar > _fullScaleBar) ? "  [beyond the transducer range - fault injection]" : "");
    } else {
        Serial.println(F("[PRESSURE] Manual override cleared -> auto hydraulic model restored"));
    }
}

void SimPressure::setRawMvOverride(bool enable, float mv) {
    _rawMvOverride = enable;
    _rawMv = mv;
    if (enable) {
        emitMv(mv);
        Serial.printf("[PRESSURE] Raw node voltage forced to %.1f mV (fault injection)\n",
                      _currentMv.load(std::memory_order_relaxed));
    } else {
        Serial.println(F("[PRESSURE] Raw node voltage injection cleared"));
    }
}

void SimPressure::update() {
    // Re-apply in case the manual override or the full scale changed.
    if (_manualOverride || _rawMvOverride) {
        setBar(_overrideBar);
    }
}
