// =================================================================================================
//  hal_psm.h - Zero-Cross capture, mains-frequency auto-detection, brew-state synchronisation
//              and polarity-agnostic Bresenham Pulse Skipping Modulation.
//
//  docs/Architecture.md.
//
//  SAFETY MODEL
//    The TRIAC gate pin is driven HIGH exclusively from the validated zero-cross ISR during
//    commutation. Emergency cut-off functions (forceGateOff and the missing-pulse watchdog) may
//    force PIN_PSM_GATE LOW from task context. That guarantees the pump is only energised within
//    microseconds of a genuine voltage zero crossing, even if the rest of the firmware wedges.
//
//  CORE 1 ONLY. ESP-IDF runs a GPIO interrupt on the core where the interrupt service was installed.
//  arm() always performs the attach on Core 1 (through an IPC call when it is invoked from another
//  core, e.g. the OTA recovery path), so Wi-Fi/BLE activity on Core 0 cannot add jitter to the
//  commutation window.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>
#include "pins.h"

enum class AcState : uint8_t {
    UNKNOWN = 0,   // no ZC burst seen yet
    IDLE,          // powered but not brewing (machine idle with logic board live)
    BREWING        // sustained pump power - the machine is extracting
};

enum class PsmMode : uint8_t {
    PASSIVE = 0,   // 100% passthrough: gate HIGH on first valid ZC edge, no Bresenham skip
    PROFILE        // Profile / PID modulation: 4-pulse ZC handshake, Bresenham duty cycle
};

class PsmDimmer {
public:
    // ---- lifecycle -----------------------------------------------------------------------------
    void begin();
    void arm();                     // (re)attach the ZC ISR - call after every OTA partition switch
    void disarm();                  // OTA safety: detach ISR, force gate LOW, stop servo timer
    bool isArmed() const { return _armed; }

    // ---- command surface (task context) -------------------------------------------------------
    void setDutyPct(float pct);     // 0..100, thread safe (atomic store)
    float dutyPct() const          { return (float)_dutyPct.load(std::memory_order_relaxed); }
    void setMode(PsmMode m);
    PsmMode mode() const           { return (PsmMode)_mode.load(std::memory_order_relaxed); }
    // Snapshot of the "auto-detect mains frequency" setting. The ISR must not read the live
    // config struct (a non-atomic POD that Core 0 may be writing), so the control task publishes
    // the value here and the ISR only ever reads this atomic.
    void setMainsAutoDetect(bool on) { _mainsAuto.store(on ? 1 : 0, std::memory_order_relaxed); }
    void forceGateOff();            // immediate latched water cut (holds gate LOW until releaseGate), does NOT touch the machine
    void releaseGate();             // clears force-off state so normal gate control can resume
    bool isGateForcedOff() const   { return _forceOff.load(std::memory_order_relaxed); }

    // ---- observation --------------------------------------------------------------------------
    uint32_t mainsHz() const        { return _mainsHz.load(std::memory_order_relaxed); }       // 50 or 60
    uint32_t detectedHz() const     { return _detectedHz.load(std::memory_order_relaxed); }    // 0 until first detection
    bool     acPowerPresent() const { return _acPowerPresent.load(std::memory_order_relaxed) != 0; }
    bool     brewConfirmed() const  { return _brewConfirmed.load(std::memory_order_relaxed) != 0; }
    uint32_t lastPulseUs() const    { return _lastPulseUs.load(std::memory_order_relaxed); }
    uint32_t pulsesAccepted() const { return _pulseCount.load(std::memory_order_relaxed); }
    uint32_t pulsesRejected() const { return _glitchCount.load(std::memory_order_relaxed); }
    uint32_t fullCycles() const     { return _cycleCount.load(std::memory_order_relaxed); }
    float    lastZcPeriodUs() const { return (float)_lastPeriodUs.load(std::memory_order_relaxed); }
    float    lastGateHoldUs() const { return (float)gateHoldUs(); }
    uint16_t detectionProgress() const { return (uint16_t)_detectSamples.load(std::memory_order_relaxed); }
    // Zero-cross losses while the controller was driving the pump (see setAcLossExpected). The
    // pulses also stop after every stop, flush end or machine-driven shot; those are not counted.
    uint32_t missedTimeoutCount() const { return _missedCount.load(std::memory_order_relaxed); }
    // Set by the brew controller every tick, before tickWatchdog(): true while losing the mains is
    // the normal end of pump power (idle, stopping, machine in charge), false while the controller
    // itself is running the pump. Only an unexpected loss is logged as a warning and counted.
    void setAcLossExpected(bool expected) { _acLossExpected.store(expected, std::memory_order_relaxed); }
    // Accepted / rejected edges over the last ZC_NOISE_WINDOW_MS (rolling, refreshed by
    // tickWatchdog). The lifetime counters above say nothing about the signal now: a single burst
    // at power-on would otherwise count against every later shot until a reboot.
    uint32_t recentAccepted() const { return _recentAccepted.load(std::memory_order_relaxed); }
    uint32_t recentRejected() const { return _recentRejected.load(std::memory_order_relaxed); }
    static constexpr uint32_t ZC_NOISE_WINDOW_MS = 2000;

    // Called from the 100 Hz control task to service the missing-pulse watchdog
    void tickWatchdog();

    // Gate hold window in microseconds for the currently selected mains frequency
    uint32_t gateHoldUs() const;

public:
    // Attached as the GPIO interrupt handler (IRAM).
    static void IRAM_ATTR zcIsrTrampoline();

private:
    void IRAM_ATTR onZeroCross(uint32_t nowUs);

    // Bresenham accumulator, shared between ISR and control task (atomic)
    std::atomic<int32_t>  _bresenham{0};
    std::atomic<bool>     _fireCurrentCycle{false};

    // --- IRAM-safe shared state (all single-word integer atomics so the ISR is integer-only) ---
    std::atomic<uint8_t>  _mode{(uint8_t)PsmMode::PASSIVE};
    std::atomic<uint8_t>  _dutyPct{0};
    std::atomic<uint32_t> _lastPeriodUs{0};
    std::atomic<uint32_t> _mainsHz{50};
    std::atomic<uint32_t> _detectedHz{0};
    std::atomic<uint32_t> _newMainsDetected{0};
    std::atomic<uint32_t> _acPowerPresent{0};
    std::atomic<uint32_t> _brewConfirmed{0};
    std::atomic<uint32_t> _lastPulseUs{0};
    std::atomic<uint32_t> _pulseCount{0};
    std::atomic<uint32_t> _glitchCount{0};
    std::atomic<uint32_t> _cycleCount{0};
    std::atomic<uint32_t> _missedCount{0};
    std::atomic<uint32_t> _detectSamples{0};
    // Rolling noise window: snapshots of the lifetime counters every ZC_NOISE_SLOT_MS, kept by the
    // control task only. The published differences are atomics so any core can read them.
    static constexpr uint32_t ZC_NOISE_SLOT_MS = 250;
    static constexpr uint8_t  ZC_NOISE_SLOTS   = ZC_NOISE_WINDOW_MS / ZC_NOISE_SLOT_MS;
    uint32_t _snapAcc[ZC_NOISE_SLOTS] = {0};
    uint32_t _snapRej[ZC_NOISE_SLOTS] = {0};
    uint8_t  _snapIdx    = 0;
    uint32_t _snapLastMs = 0;
    std::atomic<uint32_t> _recentAccepted{0};
    std::atomic<uint32_t> _recentRejected{0};
    std::atomic<bool>     _armed{false};
    std::atomic<bool>     _forceOff{false};
    std::atomic<bool>     _acLossExpected{true};
    std::atomic<uint8_t>  _mainsAuto{1};   // 1 = auto-detect 50/60 Hz (snapshot of the config)

    // --- Burst & ISR state ---
    uint32_t              _prevPulseUs   = 0;
    std::atomic<uint32_t> _burstFirstUs{0};
    std::atomic<uint32_t> _burstSeq{0};
    uint32_t _detectAccumUs = 0;
    uint8_t  _halfPhase     = 0;    // toggles every accepted ZC edge
    uint32_t _zcDetectCount = 0;

    static PsmDimmer* s_instance;
};

extern PsmDimmer g_psm;