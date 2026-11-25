// =================================================================================================
//  hal_psm.cpp - Zero-Cross capture, mains-frequency auto-detection, brew-state synchronisation
//              and polarity-agnostic Bresenham Pulse Skipping Modulation.
//
//  SAFETY INVARIANTS:
//    1. Gate level is driven HIGH exclusively by the validated zero-cross ISR during commutation.
//       Task context (watchdog and forceGateOff) may force the gate LOW.
//    2. Integer-only inside the ISR: no FPU instructions, no float math, no locks/mutexes/logging.
//    3. At each zero crossing, the gate level for the coming half-cycle is written directly (HIGH
//       for conduct, LOW for skip). No esp_timer servo is needed.
//    4. The missing-pulse watchdog in the 100 Hz control task forces the gate LOW if AC stops.
// =================================================================================================

#include "hal_psm.h"
#include "logger.h"
#include "config_schema.h"

#include <esp_timer.h>
#include <esp_ipc.h>
#include <driver/gpio.h>
#include <soc/gpio_struct.h>

PsmDimmer* PsmDimmer::s_instance = nullptr;

namespace {
// The zero-cross ISR must run on the control core, so Wi-Fi and BLE interrupts on Core 0 cannot
// delay a TRIAC commutation.
constexpr BaseType_t kZcIsrCore = 1;

// ESP-IDF allocates the GPIO interrupt service on the core that installs it - Arduino does that on
// the first attachInterrupt() of the whole program - and every later handler shares that core.
// The binding therefore depends on who attaches first and from where. arm() is also called from
// the OTA recovery path, which can run on Core 0, so the attach is always executed on kZcIsrCore
// (directly, or through an IPC call to that core) instead of relying on the call order.
void attachZcIsr(void*) {
    attachInterrupt(digitalPinToInterrupt(PIN_ZC_IN), &PsmDimmer::zcIsrTrampoline, RISING);
}
}  // namespace

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------
void PsmDimmer::begin() {
    s_instance = this;

    // Hard-off first.
    pinMode(PIN_PSM_GATE, OUTPUT);
    digitalWrite(PIN_PSM_GATE, LOW);

    pinMode(PIN_ZC_IN, INPUT_PULLUP);

    _mode.store((uint8_t)PsmMode::PASSIVE);
    _dutyPct.store(0);
    _mainsHz.store(50);

    arm();
}

// -------------------------------------------------------------------------------------------------
// arm / disarm
// -------------------------------------------------------------------------------------------------
void PsmDimmer::arm() {
    if (_armed.load(std::memory_order_relaxed)) return;

    _mode.store((uint8_t)PsmMode::PASSIVE, std::memory_order_relaxed);
    _dutyPct.store(0, std::memory_order_relaxed);
    _bresenham = 0;
    _halfPhase = 0;
    _prevPulseUs = 0;
    _burstSeq = 0;
    _fireCurrentCycle = false;
    _acPowerPresent.store(0, std::memory_order_relaxed);
    _brewConfirmed.store(0, std::memory_order_relaxed);
    _missedCount.store(0, std::memory_order_relaxed);

    _forceOff.store(false, std::memory_order_relaxed);
    _lastPeriodUs.store(0, std::memory_order_relaxed);
    digitalWrite(PIN_PSM_GATE, LOW);

#if CONFIG_FREERTOS_UNICORE
    attachZcIsr(nullptr);
#else
    if (xPortGetCoreID() == kZcIsrCore) {
        attachZcIsr(nullptr);
    } else if (esp_ipc_call_blocking(kZcIsrCore, attachZcIsr, nullptr) != ESP_OK) {
        // Never leave the power stage without its zero-cross input. If the IPC call cannot run,
        // attach here and say so; the ISR may then sit on the calling core.
        attachZcIsr(nullptr);
        SB_LOGE("Zero-cross ISR could not be attached on Core %d - attached on Core %d instead",
                (int)kZcIsrCore, (int)xPortGetCoreID());
    }
#endif
    _armed.store(true, std::memory_order_relaxed);

    SB_LOGI("Power stage armed (direct half-cycle drive, passive passthrough when AC detected)");
}

void PsmDimmer::disarm() {
    if (!_armed.exchange(false)) {
        digitalWrite(PIN_PSM_GATE, LOW);
        return;
    }
    detachInterrupt(digitalPinToInterrupt(PIN_ZC_IN));

    _forceOff.store(true, std::memory_order_relaxed);
    _dutyPct.store(0, std::memory_order_relaxed);
    _bresenham.store(0, std::memory_order_relaxed);
    _fireCurrentCycle.store(false, std::memory_order_relaxed);
    digitalWrite(PIN_PSM_GATE, LOW);

    SB_LOGW("PSM DISARMED - gate forced LOW, ZC ISR detached.");
}

void PsmDimmer::forceGateOff() {
    // Immediate authoritative water cut. Sets _forceOff so ISR holds gate LOW in ALL modes (including PASSIVE).
    _forceOff.store(true, std::memory_order_relaxed);
    _dutyPct.store(0, std::memory_order_relaxed);
    _bresenham.store(0, std::memory_order_relaxed);
    _fireCurrentCycle.store(false, std::memory_order_relaxed);
    digitalWrite(PIN_PSM_GATE, LOW);
}

void PsmDimmer::releaseGate() {
    // Clears force-off state so normal gate control (PASSIVE or PROFILE) can resume
    _forceOff.store(false, std::memory_order_relaxed);
}

// -------------------------------------------------------------------------------------------------
// Command surface
// -------------------------------------------------------------------------------------------------
void PsmDimmer::setDutyPct(float pct) {
    if (isnan(pct) || pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f)             pct = 100.0f;
    _dutyPct.store((uint8_t)(pct + 0.5f), std::memory_order_relaxed);
}

void PsmDimmer::setMode(PsmMode m) {
    _mode.store((uint8_t)m, std::memory_order_relaxed);
}

uint32_t PsmDimmer::gateHoldUs() const {
    const float p = _lastPeriodUs.load(std::memory_order_relaxed);
    if (p > 5000.0f && p < 15000.0f) {
        return (uint32_t)(p + 0.5f);
    }
    return (_mainsHz.load(std::memory_order_relaxed) == 60) ? 8333UL : 10000UL;
}

// -------------------------------------------------------------------------------------------------
// Zero-cross ISR (IRAM)
// -------------------------------------------------------------------------------------------------
void IRAM_ATTR PsmDimmer::zcIsrTrampoline() {
    PsmDimmer* self = s_instance;
    if (!self) return;
    self->onZeroCross((uint32_t)esp_timer_get_time());
}

static inline void IRAM_ATTR gateWrite(uint8_t pin, bool high) {
    if (high) gpio_set_level((gpio_num_t)pin, 1);
    else      gpio_set_level((gpio_num_t)pin, 0);
}

void IRAM_ATTR PsmDimmer::onZeroCross(uint32_t nowUs) {
    // ---- 1. Glitch rejection -----------------------------------------------------------------
    const uint32_t dt = nowUs - _prevPulseUs;
    if (_prevPulseUs != 0 && dt < (uint32_t)ZC_GLITCH_US) {
        _glitchCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // P0-1 FIX: Update _prevPulseUs on EVERY accepted edge immediately.
    _prevPulseUs = nowUs;
    _pulseCount.fetch_add(1, std::memory_order_relaxed);
    _lastPulseUs.store(nowUs, std::memory_order_relaxed);
    _acPowerPresent.store(1, std::memory_order_relaxed);
    _lastPeriodUs.store(dt, std::memory_order_relaxed);

    // ---- 2. Periodic-burst tracking / brew confirmation --------------------------------------
    uint32_t burstFirst = _burstFirstUs.load(std::memory_order_relaxed);
    uint32_t burstSeq   = _burstSeq.load(std::memory_order_relaxed);
    if (dt != 0 && (dt < 7500UL || dt > 11500UL)) {
        burstSeq       = 0;
        burstFirst     = nowUs;
        _detectAccumUs = 0;
        _zcDetectCount = 0;
    }

    if (burstFirst == 0) burstFirst = nowUs;

    // The burst must also fit inside ZC_BURST_WINDOW_US. The per-pulse period check above already
    // restarts the sequence when a pulse is late, so this is a belt-and-braces bound that keeps the
    // documented "4 pulses within 45 ms" rule true rather than relying on the period window alone.
    if ((nowUs - burstFirst) > (uint32_t)ZC_BURST_WINDOW_US) {
        burstSeq   = 0;
        burstFirst = nowUs;
    }

    if (burstSeq == 0)     burstSeq = 1;
    else                   burstSeq++;

    _burstFirstUs.store(burstFirst, std::memory_order_relaxed);
    _burstSeq.store(burstSeq, std::memory_order_relaxed);

    if (burstSeq >= (uint32_t)ZC_BURST_PULSES && !_brewConfirmed.load(std::memory_order_relaxed)) {
        _brewConfirmed.store(1, std::memory_order_relaxed);
    }

    // ---- 3. Mains frequency auto-detection (integer-only, no logging in ISR) ------------------
    // The setting is read from an atomic snapshot published by the control task, never from the
    // live config struct, so the ISR never races a Core 0 config write.
    if (_mainsAuto.load(std::memory_order_relaxed) && _zcDetectCount < 8 && dt >= 7500UL && dt <= 11500UL) {
        _detectAccumUs += dt;
        _zcDetectCount++;
        _detectSamples.store(_zcDetectCount, std::memory_order_relaxed);
        if (_zcDetectCount >= 8) {
            const uint32_t avgUs = _detectAccumUs / _zcDetectCount;   // ~10000 -> 50 Hz, ~8333 -> 60 Hz
            const uint32_t hz = (avgUs <= 9100UL) ? 60U : 50U;
            if (_detectedHz.load(std::memory_order_relaxed) != hz) {
                _detectedHz.store(hz, std::memory_order_relaxed);
                _newMainsDetected.store(hz, std::memory_order_relaxed);
            }
            _mainsHz.store(hz, std::memory_order_relaxed);
        }
    }

    // ---- 4. Direct gate driving: PASSIVE (100% passthrough) or Bresenham PSM (PROFILE) ---------
    if (!_armed.load(std::memory_order_relaxed) || _forceOff.load(std::memory_order_relaxed)) {
        _bresenham.store(0, std::memory_order_relaxed);
        _fireCurrentCycle.store(false, std::memory_order_relaxed);
        gateWrite(PIN_PSM_GATE, false);
        return;
    }

    if (_mode.load(std::memory_order_relaxed) == (uint8_t)PsmMode::PASSIVE) {
        // PASSIVE: 100% full conduction passthrough. Opens on first valid ZC edge without delay.
        _bresenham.store(0, std::memory_order_relaxed);
        _fireCurrentCycle.store(true, std::memory_order_relaxed);
        gateWrite(PIN_PSM_GATE, true);
        return;
    }

    // PROFILE mode: require 4-pulse ZC handshake before firing
    if (!_brewConfirmed.load(std::memory_order_relaxed)) {
        _bresenham.store(0, std::memory_order_relaxed);
        _fireCurrentCycle.store(false, std::memory_order_relaxed);
        gateWrite(PIN_PSM_GATE, false);
        return;
    }

    _halfPhase ^= 1U;
    if (_halfPhase == 0) {
        // Start of a full AC cycle: evaluate Bresenham accumulator
        const uint32_t duty = _dutyPct.load(std::memory_order_relaxed);
        if (duty == 0) {
            _bresenham.store(0, std::memory_order_relaxed);
            _fireCurrentCycle.store(false, std::memory_order_relaxed);
        } else {
            int32_t acc = _bresenham.load(std::memory_order_relaxed) + (int32_t)duty;
            if (acc >= 100) {
                acc -= 100;
                _fireCurrentCycle.store(true, std::memory_order_relaxed);
                _cycleCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                _fireCurrentCycle.store(false, std::memory_order_relaxed);
            }
            _bresenham.store(acc, std::memory_order_relaxed);
        }
    }

    // Drive gate directly for this half-cycle
    gateWrite(PIN_PSM_GATE, _fireCurrentCycle.load(std::memory_order_relaxed));
}

// -------------------------------------------------------------------------------------------------
// Missing-pulse watchdog & deferred event logging (serviced from the 100 Hz control task)
// -------------------------------------------------------------------------------------------------
void PsmDimmer::tickWatchdog() {
    // Task context: refresh the ISR-visible snapshot of the auto-detect setting. The ISR must not
    // dereference the live config struct while Core 0 may be writing it.
    setMainsAutoDetect(g_config.cfg().mainsMode == MAINS_AUTO);

    // Rolling noise window. Each slot holds the lifetime counters as they were when the slot was
    // last written; the oldest slot is ZC_NOISE_WINDOW_MS old, so "now minus oldest" is the count
    // over the window. Unsigned subtraction stays correct across counter wrap-around.
    const uint32_t nowMs = millis();
    if (_snapLastMs == 0 || (nowMs - _snapLastMs) >= ZC_NOISE_SLOT_MS) {
        _snapLastMs = nowMs;
        const uint32_t acc = _pulseCount.load(std::memory_order_relaxed);
        const uint32_t rej = _glitchCount.load(std::memory_order_relaxed);
        _snapIdx = (uint8_t)((_snapIdx + 1) % ZC_NOISE_SLOTS);   // now the oldest slot
        _recentAccepted.store(acc - _snapAcc[_snapIdx], std::memory_order_relaxed);
        _recentRejected.store(rej - _snapRej[_snapIdx], std::memory_order_relaxed);
        _snapAcc[_snapIdx] = acc;
        _snapRej[_snapIdx] = rej;
    }

    if (!_armed.load(std::memory_order_relaxed)) return;

    // Deferred logging of mains frequency detection from task context
    const uint32_t detected = _newMainsDetected.exchange(0, std::memory_order_relaxed);
    if (detected) {
        SB_LOGI("Mains detected: %uHz", (unsigned)detected);
    }

    const uint32_t now   = (uint32_t)esp_timer_get_time();
    const uint32_t last  = _lastPulseUs.load(std::memory_order_relaxed);
    const bool     have  = _acPowerPresent.load(std::memory_order_relaxed) != 0;

    if (have && last != 0 && (now - last) > (uint32_t)ZC_MISSING_TIMEOUT_US) {
        _acPowerPresent.store(0, std::memory_order_relaxed);
        _brewConfirmed.store(0, std::memory_order_relaxed);
        const bool expected = _acLossExpected.load(std::memory_order_relaxed);
        if (!expected) _missedCount.fetch_add(1, std::memory_order_relaxed);
        _burstSeq.store(0, std::memory_order_relaxed);
        _burstFirstUs.store(0, std::memory_order_relaxed);
        _bresenham.store(0, std::memory_order_relaxed);
        _fireCurrentCycle.store(false, std::memory_order_relaxed);
        gateWrite(PIN_PSM_GATE, false);
        if (expected) {
            // The pump power went away as it should (after a stop, a flush, or a shot the machine
            // ran itself). Nothing to warn about.
            SB_LOGD("Zero-cross ended (pump power released) - ac_power_present=false");
        } else {
            SB_LOGW("ZC missing-pulse timeout (>%lums) while the controller drives the pump - "
                    "ac_power_present=false", (unsigned long)(ZC_MISSING_TIMEOUT_US / 1000));
        }
    }
}

// -------------------------------------------------------------------------------------------------
PsmDimmer g_psm;