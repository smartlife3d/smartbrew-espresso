// =================================================================================================
//  sim_dimmer.cpp - Zero-Cross Pulse Generation & PSM Gate Measurement
// =================================================================================================
#include "sim_dimmer.h"
#include <esp_timer.h>
#include <rom/ets_sys.h>
#include <math.h>

SimDimmer g_simDimmer;

void IRAM_ATTR SimDimmer::gateIsrTrampoline() {
    g_simDimmer.onGateEdge();
}

void IRAM_ATTR SimDimmer::onGateEdge() {
    _gateCount.fetch_add(1, std::memory_order_relaxed);
    _currentCycleFired = true;
}

// The ZC timer runs in the esp_timer task, not in an ISR, so this callback is a normal task
// function and may call flash code. (Only the GPIO interrupt handler below needs IRAM_ATTR.)
void SimDimmer::timerCallback(void* arg) {
    SimDimmer* self = reinterpret_cast<SimDimmer*>(arg);
    if (!self->_mainsActive.load(std::memory_order_relaxed)) {
        digitalWrite(PIN_SIM_ZC_OUT, LOW);
        return;
    }

    // 1. Record the previous cycle outcome into the duty window.
    //    The gate level is read as well as the edge flag because the controller drives the gate
    //    for a whole half-cycle: a continuous HIGH means "still conducting", not "new edge".
    if (self->_windowResetPending.exchange(false, std::memory_order_relaxed)) {
        memset(self->_window, 0, sizeof(self->_window));
        self->_windowIdx = 0;
    }
    const uint8_t fired = (self->_currentCycleFired || digitalRead(PIN_SIM_GATE_IN) == HIGH) ? 1 : 0;
    self->_window[self->_windowIdx] = fired;
    self->_windowIdx = (self->_windowIdx + 1) % WINDOW_SIZE;
    self->_currentCycleFired = false;

    // 2. Publish the duty measured over the window. Doing it here keeps the window array private
    //    to the timer task - no cross-task access, no torn reads.
    size_t firedCount = 0;
    for (size_t i = 0; i < WINDOW_SIZE; ++i) {
        if (self->_window[i]) firedCount++;
    }
    self->_measuredDutyPct.store(((float)firedCount / (float)WINDOW_SIZE) * 100.0f, std::memory_order_relaxed);

    // 3. The pump drive: this half-cycle's stroke through the pump's first-order response.
    const float periodMs = (self->_mainsHz.load(std::memory_order_relaxed) == 60) ? 8.333f : 10.0f;
    const float alpha = 1.0f - expf(-periodMs / PUMP_TAU_MS);
    self->_pumpDriveState += alpha * ((fired ? 100.0f : 0.0f) - self->_pumpDriveState);
    self->_pumpDrivePct.store(self->_pumpDriveState, std::memory_order_relaxed);

    // 4. Fire the Zero-Cross pulse (active-HIGH, ~150 us wide)
    digitalWrite(PIN_SIM_ZC_OUT, HIGH);
    self->_zcCount.fetch_add(1, std::memory_order_relaxed);

    // Short hardware delay for the pulse width (mimics RobotDyn optocoupler output)
    ets_delay_us(150);
    digitalWrite(PIN_SIM_ZC_OUT, LOW);
}

void SimDimmer::begin(uint32_t mainsHz) {
    pinMode(PIN_SIM_ZC_OUT, OUTPUT);
    digitalWrite(PIN_SIM_ZC_OUT, LOW);

    pinMode(PIN_SIM_GATE_IN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_SIM_GATE_IN), gateIsrTrampoline, RISING);

    setMainsHz(mainsHz);
}

void SimDimmer::setMainsHz(uint32_t hz) {
    if (hz != 50 && hz != 60) hz = 50;
    _mainsHz.store(hz, std::memory_order_relaxed);

    if (_timer != nullptr) {
        esp_timer_stop(_timer);
        esp_timer_delete(_timer);
        _timer = nullptr;
    }

    // 50 Hz -> 100 zero crossings/sec -> 10,000 us period
    // 60 Hz -> 120 zero crossings/sec -> 8,333 us period
    const uint64_t periodUs = (hz == 60) ? 8333ULL : 10000ULL;

    const esp_timer_create_args_t timerArgs = {
        .callback = &SimDimmer::timerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sim_zc_timer",
        .skip_unhandled_events = true
    };

    esp_timer_create(&timerArgs, &_timer);
    esp_timer_start_periodic(_timer, periodUs);
}

void SimDimmer::setMainsActive(bool active) {
    _mainsActive.store(active, std::memory_order_relaxed);
    if (!active) {
        digitalWrite(PIN_SIM_ZC_OUT, LOW);
        _currentCycleFired = false;
        // The window belongs to the timer task, so the reset is handed over to it.
        _windowResetPending.store(true, std::memory_order_relaxed);
        _measuredDutyPct.store(0.0f, std::memory_order_relaxed);
        _pumpDrivePct.store(0.0f, std::memory_order_relaxed);
    }
}

void SimDimmer::update() {
    // The duty is measured and published by the ZC timer callback (one sample per zero-crossing
    // period), so there is nothing left to do here. The method is kept because the main loop
    // treats the dimmer like every other peripheral.
}

