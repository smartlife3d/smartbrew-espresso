// =================================================================================================
//  brew_controller.cpp - Single-owner brew controller implementation
// =================================================================================================

#include "brew_controller.h"

#include "hal/hal_psm.h"
#include "hal/hal_pressure.h"
#include "hal/hal_button.h"
#include "control/pid_controller.h"
#include "control/profile_engine.h"
#include "control/shot_history.h"
#include "control/shot_tail.h"
#include "scale/scale_manager.h"
#include "logger.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_timer.h>

// Mains gap that ends a running shot or flush (see the tick). 300 ms = 15 half-cycles at 50 Hz,
// short enough that a button release still feels immediate.
#ifndef SB_AC_LOSS_STOP_MS
#define SB_AC_LOSS_STOP_MS 300UL
#endif
namespace {
// How often the passthrough open-wire notice may be written to the log. The condition is usually
// permanent (no transducer fitted), so repeating it every control tick would drown the log.
constexpr uint32_t OPEN_WIRE_WARN_INTERVAL_MS = 60000UL;
// How long the signal must be good before the notice is considered over. An unconnected input
// floats across the open-wire threshold, so a single good sample proves nothing.
constexpr uint32_t OPEN_WIRE_CLEAR_MS = 5000UL;

// ---- pre-brew gate ------------------------------------------------------------------------------
// The settle delay between "every hardware check passed" and "the brew actually starts". It gives
// the scale a moment to settle at zero and lets the operator see the pending start on the display
// before water moves.
constexpr uint32_t PREPARE_SETTLE_MS = 500UL;
// How long the controller waits for the scale to produce a fresh reading that confirms the tare.
constexpr uint32_t PREPARE_TARE_TIMEOUT_MS = 2000UL;
// How close to zero the net weight has to be for the tare to count as confirmed. Real scales sit
// within a few tenths of a gram after a tare, so this is generous enough not to trip on jitter.
constexpr float PREPARE_TARE_TOLERANCE_G = 1.0f;
// How many times the tare is attempted before the start is refused outright.
constexpr uint8_t PREPARE_TARE_MAX_ATTEMPTS = 2;
// How long a faulty pressure transducer must stay faulty before the start is refused. A single
// noisy sample must not block a brew, so the condition has to persist before it is believed.
constexpr uint32_t PREPARE_SENSOR_CONFIRM_MS = 200;
}  // namespace

const char* brewStateName(BrewState s) {
    switch (s) {
        case BrewState::IDLE:      return "IDLE";
        case BrewState::PREPARING: return "PREPARING";
        case BrewState::STARTING:  return "STARTING";
        case BrewState::BREWING:   return "BREWING";
        case BrewState::FLUSHING:  return "FLUSHING";
        case BrewState::STOPPING:  return "STOPPING";
        case BrewState::FAULT:     return "FAULT";
    }
    return "IDLE";
}

const char* pumpOwnerName(PumpOwner o) {
    switch (o) {
        case PumpOwner::PASSIVE:  return "PASSIVE";
        case PumpOwner::PROFILE:  return "PROFILE";
        case PumpOwner::STOPPING: return "STOPPING";
        case PumpOwner::FAULT:    return "FAULT";
    }
    return "PASSIVE";
}

PumpOwner BrewController::pumpOwner() const {
    switch (_state) {
        case BrewState::IDLE:
            return g_pid.isManual() ? PumpOwner::PROFILE : PumpOwner::PASSIVE;
        case BrewState::PREPARING:
            // The machine has not been asked to pump yet - the controller still only observes.
            return PumpOwner::PASSIVE;
        case BrewState::STARTING:
        case BrewState::BREWING:
        case BrewState::FLUSHING:
            return PumpOwner::PROFILE;
        case BrewState::STOPPING:
            return PumpOwner::STOPPING;
        case BrewState::FAULT:
            return PumpOwner::FAULT;
    }
    return PumpOwner::PASSIVE;
}

void BrewController::begin() {
    if (!_cmdQueue)   _cmdQueue   = xQueueCreate(16, sizeof(BrewCommand));
    if (!_eventQueue) _eventQueue = xQueueCreate(16, sizeof(BrewEvent));

    _state = BrewState::IDLE;
    _faults = FAULT_NONE;
    _bootMs = millis();
    _lastTickMs = millis();
    _idleAcActive = false;
    _idleAcSinceMs = 0;
    _idleStartMs = 0;
    g_psm.setMode(PsmMode::PASSIVE);
    SB_LOGI("Brew controller initialized in %s (single-owner model, 100%% passthrough)", brewStateName(_state));
}

void BrewController::reset() {
    BrewCommand cmd{CmdType::RESET};
    xQueueSend(_cmdQueue, &cmd, 0);
}

void BrewController::clearFaults() {
    BrewCommand cmd{CmdType::CLEAR_FAULTS};
    xQueueSend(_cmdQueue, &cmd, 0);
}

void BrewController::setSimulationMode(bool enable) {
    if (_cmdQueue) {
        BrewCommand cmd{CmdType::SET_SIM, enable ? 1.0f : 0.0f};
        if (xQueueSend(_cmdQueue, &cmd, 0) != pdTRUE) {
            SB_LOGW("Command queue full - SET_SIM dropped");
            return;
        }
    } else {
        _simMode.store(enable, std::memory_order_release);
        g_pressure.setSimulator(enable);
        if (enable) {
            g_psm.forceGateOff();
        } else if (_state == BrewState::IDLE) {
            g_psm.releaseGate();
            if (!g_pid.isManual()) {
                g_psm.setMode(PsmMode::PASSIVE);
            }
        }
    }
    SB_LOGI("Controller simulation mode -> %s", enable ? "ENABLED" : "DISABLED");
}

bool BrewController::requestStart(char* errBuf, size_t errBufLen) {
    if (_state != BrewState::IDLE) {
        if (errBuf && errBufLen > 0) snprintf(errBuf, errBufLen, "machine not idle (%s)", brewStateName(_state));
        return false;
    }
    if (!_simMode && g_psm.acPowerPresent()) {
        if (errBuf && errBufLen > 0) snprintf(errBuf, errBufLen, "machine pump active (autofill or manual)");
        return false;
    }
    if (!g_profile.loaded()) {
        if (errBuf && errBufLen > 0) snprintf(errBuf, errBufLen, "no profile loaded");
        return false;
    }
    if (!_cmdQueue) return false;
    BrewCommand cmd{CmdType::START};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::requestStop(const char* reason) {
    // A4: Allow STOP in BREWING, STARTING, and in IDLE if machine pump is active (or in sim mode)
    if (_state == BrewState::STOPPING) {
        return false;
    }
    if (!_simMode && _state == BrewState::IDLE && !g_psm.acPowerPresent()) {
        return false;
    }
    if (!_cmdQueue) return false;
    BrewCommand cmd{CmdType::STOP};
    if (reason) strlcpy(cmd.strArg, reason, sizeof(cmd.strArg));
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::requestFlush(float seconds, char* errBuf, size_t errBufLen) {
    if (_state != BrewState::IDLE && _state != BrewState::PREPARING) {
        if (errBuf && errBufLen > 0) snprintf(errBuf, errBufLen, "machine busy (%s)", brewStateName(_state));
        return false;
    }
    if (!_simMode && g_psm.acPowerPresent()) {
        if (errBuf && errBufLen > 0) snprintf(errBuf, errBufLen, "machine pump active (autofill or manual)");
        return false;
    }
    if (seconds < 1.0f)   seconds = 1.0f;
    if (seconds > 60.0f)  seconds = 60.0f;
    if (!_cmdQueue) return false;
    BrewCommand cmd{CmdType::FLUSH, seconds};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::requestTare(char* errBuf, size_t errBufLen) {
    const BrewState st = _state.load(std::memory_order_acquire);
    if (st != BrewState::IDLE && st != BrewState::PREPARING) {
        if (errBuf && errBufLen > 0) snprintf(errBuf, errBufLen, "machine busy (%s)", brewStateName(st));
        return false;
    }
    if (!_cmdQueue) return false;
    BrewCommand cmd{CmdType::TARE};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::requestNextPhase() {
    BrewCommand cmd{CmdType::NEXT_PHASE};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::setPressureOverride(float bar) {
    BrewCommand cmd{CmdType::OVERRIDE_PRESSURE, bar};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::clearPressureOverride() {
    BrewCommand cmd{CmdType::CLEAR_OVERRIDE};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::setManualPower(float pct) {
    BrewCommand cmd{CmdType::MANUAL_POWER, pct};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::clearManualPower() {
    BrewCommand cmd{CmdType::CLEAR_MANUAL};
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

bool BrewController::loadProfile(const char* id) {
    if (!id || !*id) return false;
    BrewCommand cmd{CmdType::LOAD_PROFILE};
    strlcpy(cmd.strArg, id, sizeof(cmd.strArg));
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

void BrewController::setFault(uint32_t bit, const char* why) {
    if (_faults & bit) return;
    _faults |= bit;
    SB_LOGE("FAULT 0x%04X set: %s", (unsigned)bit, why);
    if (_eventQueue) {
        BrewEvent evt{EventType::FAULT_SET, _state, _faults, 0, _peakBar, 0.0f, ""};
        strlcpy(evt.reason, why ? why : "fault", sizeof(evt.reason));
        xQueueSend(_eventQueue, &evt, 0);
    }
}

// -------------------------------------------------------------------------------------------------
// Passthrough safety hold
//
// The ESP32 is not driving the pump here - the machine is (boiler autofill or the physical brew
// switch). Most interlocks therefore only report, because the machine's own logic board owns the
// pump and the ESP32 must not fight it. A measured over-pressure or a saturated sensor node is
// different: those are genuine faults (dead-head, stuck valve, shorted transducer) and the pump
// must stop.
//
// The hold is LATCHED. Releasing it as soon as the reading dips would make the gate chatter on
// and off at the interlock threshold, re-running a dead-headed pump every cycle. It is released
// when the machine drops the AC (the FAULT -> IDLE path) or when the operator clears the fault
// and the condition is genuinely gone.
// -------------------------------------------------------------------------------------------------
void BrewController::engagePassthroughHold(uint32_t faultBit, const char* why) {
    if (_passthroughHold) return;
    _passthroughHold = true;
    _passthroughHoldFault = faultBit;

    setFault(faultBit, why);

    // Immediate water cut and latch it.
    g_psm.forceGateOff();

    // The passive-observation window is over: the machine is no longer pumping normally.
    _idleAcActive = false;
    _idleAcSinceMs = 0;
    _shotStopMs = _nowMs ? _nowMs : millis();   // the tick's clock (N1)

    enterState(BrewState::FAULT, why);

    SB_LOGE("PASSTHROUGH SAFETY HOLD: pump gate forced LOW (%s). "
            "It stays closed until the machine releases the AC.", why ? why : "safety condition");
}

void BrewController::enterState(BrewState s, const char* why) {
    if (_state == s) {
        if (s == BrewState::IDLE) {
            g_button.cancelPending();
            g_psm.releaseGate();
            if (!g_pid.isManual()) {
                g_psm.setMode(PsmMode::PASSIVE);
            }
        }
        return;
    }
    SB_LOGI("State %s -> %s (%s)", brewStateName(_state), brewStateName(s), why);
    if (s == BrewState::STOPPING) {
        _stopRequestMs = _nowMs ? _nowMs : millis();
        // The pump is about to stop, and the last of the coffee is still draining out of the
        // grouphead. Open the settling window now - while the state still says BREWING, so the
        // window knows this was a real shot and not a rinse.
        //
        // The tick's clock is used, not a fresh millis(): the settling window compares its own
        // timestamps against the ones the updates carry, and two different clocks would let a
        // few milliseconds of skew read as "the window has already expired".
        openWeightSettle(_nowMs);
        // Remember why the shot is ending so the final summary can report it.
        strlcpy(_stopReason, (why && *why) ? why : "stopped", sizeof(_stopReason));
    }
    _state = s;
    if (_state == BrewState::IDLE) {
        _flushingSession = false;
        _flushPending = false;
        g_button.cancelPending(); // A3: cancel any queued pulse when leaving STOPPING
        g_psm.releaseGate();      // R2-01: clear force-off latch when returning to IDLE
        _passthroughHold = false; // the hold is over once normal gate control resumes
        _passthroughHoldFault = FAULT_NONE;
        _prepActive = false;      // a pending pre-brew start does not survive an idle transition
        _prepTareIssued = false;
        _prepTareVerified = false;
        _prepSensorFaultSinceMs = 0;
        _prepReadyMs = 0;
        _prepLaunchAtMs = 0;
        if (!g_pid.isManual()) {
            g_psm.setMode(PsmMode::PASSIVE); // A1: passive 100% passthrough
        }
        // Apply a profile activation that was requested while the machine was busy. This is the
        // only place the phase table is ever swapped, and it happens with no shot running.
        if (_pendingProfileId[0]) {
            const char* want = _pendingProfileId;
            _pendingProfileId[0] = '\0';   // clear first: loadById() must not be able to re-enter
            if (g_profile.loadById(want)) {
                SB_LOGI("Deferred profile '%s' loaded on return to IDLE", want);
            } else {
                SB_LOGW("Deferred profile '%s' failed to load", want);
            }
        }
    } else if (_state == BrewState::STARTING || _state == BrewState::BREWING ||
               _state == BrewState::FLUSHING) {
        g_psm.releaseGate();
        g_psm.setMode(PsmMode::PROFILE);
        if (_state == BrewState::BREWING) {
            _historyDecimate = 0; // R3-08: clean 100 ms alignment from shot start
        }
    } else if (_state == BrewState::PREPARING) {
        // The pre-brew gate only observes: the power stage stays in passive passthrough until the
        // shot is actually launched, so nothing is driven while the checks run.
        g_psm.setMode(PsmMode::PASSIVE);
    } else if (_state == BrewState::STOPPING || _state == BrewState::FAULT) {
        g_psm.setMode(PsmMode::PROFILE); // R2-01: switch out of PASSIVE
        g_psm.forceGateOff();
    }
    if (_eventQueue) {
        BrewEvent evt{EventType::STATE_CHANGED, s, _faults, 0, _peakBar, 0.0f, ""};
        strlcpy(evt.reason, why ? why : "", sizeof(evt.reason));
        xQueueSend(_eventQueue, &evt, 0);
    }
}

bool BrewController::pollEvent(BrewEvent& evt) {
    if (!_eventQueue) return false;
    return xQueueReceive(_eventQueue, &evt, 0) == pdTRUE;
}

void BrewController::snapshot(Telemetry& out) const {
    out = _telemetryBuf[0];
    uint32_t s1 = 0, s2 = 0;
    int retries = 0;
    do {
        s1 = _telemetrySeq.load(std::memory_order_acquire);
        if ((s1 & 1U) != 0U) {
            if (++retries > 100) break;
            continue;
        }
        out = _telemetryBuf[(s1 >> 1) & 1U];
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = _telemetrySeq.load(std::memory_order_acquire);
    } while (s1 != s2 && ++retries < 100);
}

// -------------------------------------------------------------------------------------------------
// Binary telemetry frame.
//
// Replaces the ~350 byte JSON object that used to go out at 20 Hz. The layout lives in
// include/telemetry_wire.h and is shared with every WebSocket client. The numeric fields are
// scaled integers that reproduce the decimal places the JSON printed, so a client sees exactly
// the same numbers it saw before.
// -------------------------------------------------------------------------------------------------
bool BrewController::telemetryBin(uint8_t* out, size_t cap, size_t* outLen) {
    if (!out || !outLen || cap < SB_TELEMETRY_MIN_LEN) return false;

    Telemetry t;
    snapshot(t);

    SbTelemetryHead h;
    memset(&h, 0, sizeof(h));
    h.magic   = SB_TELEMETRY_MAGIC;
    h.version = SB_TELEMETRY_VERSION;
    h.seq     = _frameSeq++;
    h.t_ms    = t.tMs;

    switch (t.state) {
        case BrewState::IDLE:     h.state = SB_STATE_IDLE;     break;
        // The pre-brew gate rides on the existing STARTING state on the wire: a client that does
        // not know about the gate still sees "the shot is starting", and no client has to learn a
        // new state code to stay compatible.
        case BrewState::PREPARING:
        case BrewState::STARTING: h.state = SB_STATE_STARTING; break;
        case BrewState::BREWING:  h.state = SB_STATE_BREWING;  break;
        case BrewState::FLUSHING: h.state = SB_STATE_FLUSHING; break;
        case BrewState::STOPPING: h.state = SB_STATE_STOPPING; break;
        case BrewState::FAULT:    h.state = SB_STATE_FAULT;    break;
        default:                  h.state = SB_STATE_IDLE;     break;
    }
    h.mode       = (t.psmMode == PsmMode::PROFILE) ? SB_MODE_PROFILE : SB_MODE_PASSIVE;
    switch (t.pumpOwner) {
        case PumpOwner::PASSIVE:  h.pump_owner = SB_OWNER_PASSIVE;  break;
        case PumpOwner::PROFILE:  h.pump_owner = SB_OWNER_PROFILE;  break;
        case PumpOwner::STOPPING: h.pump_owner = SB_OWNER_STOPPING; break;
        case PumpOwner::FAULT:    h.pump_owner = SB_OWNER_FAULT;    break;
        default:                  h.pump_owner = SB_OWNER_PASSIVE;  break;
    }

    h.flags = 0;
    if (t.scaleConn)      h.flags |= SB_FLAG_SCALE_CONNECTED;
    if (t.sensorOpenWarn) h.flags |= SB_FLAG_SENSOR_OPEN;
    if (_simMode)         h.flags |= SB_FLAG_SIMULATION;
    if (_settle.active() && !_flushingSession) h.flags |= SB_FLAG_WEIGHT_SETTLING;

    h.phase_idx = t.phaseIdx;
    h.phase_pct = t.phasePct;
    h.psm_pct   = t.psmPct;
    h.scale_bat = t.scaleBat;
    h.ac_hz     = t.acHz;
    h.faults    = _faults;

    // Scaled integers - same precision the JSON used to print.
    auto clampU16 = [](float v, float maxVal) -> uint16_t {
        if (v < 0.0f) v = 0.0f;
        if (v > maxVal) v = maxVal;
        return (uint16_t)(v + 0.5f);
    };
    auto clampI16 = [](float v) -> int16_t {
        if (v < -32767.0f) v = -32767.0f;
        if (v > 32767.0f)  v = 32767.0f;
        return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    };
    h.p_bar       = clampU16(t.pBar * 100.0f, 65535.0f);          // 0.01 bar
    h.p_target    = clampU16(t.pTarget * 100.0f, 65535.0f);       // 0.01 of the phase unit
    h.flow_virt   = clampU16(t.flowVirt * 100.0f, 65535.0f);      // centi-ml/s
    h.flow_weight = clampI16(t.flowWeight * 100.0f);              // centi-g/s
    h.weight_g    = clampI16(t.weightG * 10.0f);                  // deci-gram, signed
    h.duty_ff     = clampI16(g_pid.feedForward(t.pTarget) * 10.0f);  // deci-%
    h.flow_slope  = clampI16(g_scale.slopeGs() * 100.0f);         // centi-g/s

    memcpy(out, &h, sizeof(h));
    size_t off = sizeof(h);

    // The three strings are stored length-prefixed, so nothing is ever truncated. The profile id
    // matters most: the dashboard uses it to ask the controller for the active profile, and a
    // truncated id used to make the two ends chase each other across two different names.
    const char* phaseName = t.phaseName[0] ? t.phaseName : "-";
    const char* scaleName = g_scale.deviceName();
    const char* profileId = t.profileId[0] ? t.profileId : "default";

    off += sbTelemetryPutString(out + off, cap - off, phaseName, SB_TELEMETRY_MAX_PHASE_NAME);
    off += sbTelemetryPutString(out + off, cap - off, scaleName, SB_TELEMETRY_MAX_SCALE_NAME);
    off += sbTelemetryPutString(out + off, cap - off, profileId,  SB_TELEMETRY_MAX_PROFILE_ID);
    if (off + 1 > cap) return false;

    out[off] = sbCrc8Atm(out, off);
    *outLen = off + 1;
    return true;
}

// -------------------------------------------------------------------------------------------------
// Pre-brew gate
//
// A brew is not started the instant the operator asks for it. Before the machine's brew button is
// pulsed, the controller checks that the hardware the shot depends on is actually working:
//
//   * the power stage is armed, and its zero-cross path has a clean history,
//   * the pressure transducer returns a usable reading - this is what the PID flies on,
//   * the virtual button relay is armed, and
//   * the scale, if one is connected, has been tared AND the tare has been confirmed by a fresh
//     reading of the scale.
//
// Only when every check passes does the shot begin, and it begins PREPARE_SETTLE_MS later, so the
// scale has time to settle at zero and the operator can see the pending start before water moves.
// -------------------------------------------------------------------------------------------------
bool BrewController::runPreBrewChecks(uint32_t now, char* fail, size_t failLen) {
    if (fail && failLen) fail[0] = '\0';
    auto setFail = [&](const char* why) {
        if (fail && failLen) strlcpy(fail, why, failLen);
        return false;
    };

    const Profile& prof = g_profile.profile();

    SB_LOGI("[PRE-BREW] Hardware check starting (profile '%s', %u phase(s), target %.1f g, max %.1f s)",
            prof.name[0] ? prof.name : "unnamed", g_profile.phaseCount(),
            prof.targetWeightG, prof.maxTotalTimeSec);

    // --- profile -------------------------------------------------------------------------------
    if (!g_profile.loaded()) return setFail("no profile loaded");
    if (g_profile.phaseCount() == 0) return setFail("the loaded profile has no phases");

    // --- power stage / zero cross --------------------------------------------------------------
    if (!g_psm.isArmed()) {
        return setFail("the power stage is disarmed - the pump cannot be controlled");
    }
    // The noise verdict is taken over the last couple of seconds only. Counting since boot made a
    // single burst (machine power-on, a relay) block every later shot until a reboot, and on a
    // machine whose zero-cross edge bounces it slowly drifted towards the limit anyway.
    const uint32_t zcAccepted = g_psm.recentAccepted();
    const uint32_t zcRejected = g_psm.recentRejected();
    if (g_psm.pulsesAccepted() > 0) {
        SB_LOGI("[PRE-BREW]   power stage  ARMED - zero cross proven (last %lus: %lu pulses, %lu rejected; "
                "since boot %lu / %lu), mains %lu Hz",
                (unsigned long)(PsmDimmer::ZC_NOISE_WINDOW_MS / 1000), (unsigned long)zcAccepted,
                (unsigned long)zcRejected, (unsigned long)g_psm.pulsesAccepted(),
                (unsigned long)g_psm.pulsesRejected(), (unsigned long)g_psm.mainsHz());
    } else {
        // The machine is normally off at this point, so no zero-cross pulse has been seen yet.
        // That is not a fault - the pulses only arrive once the machine's logic board is live.
        SB_LOGI("[PRE-BREW]   power stage  ARMED - zero cross not yet seen (machine idle), mains %lu Hz",
                (unsigned long)g_psm.mainsHz());
    }
    // Only dropouts while the controller drove the pump are counted (the pulses ending after a stop
    // is normal). Short ones are ridden through by the brew loop; this is a hint, not a failure.
    if (g_psm.missedTimeoutCount() > 0) {
        SB_LOGI("[PRE-BREW]   power stage  %lu zero-cross dropout(s) during controller-run shots since boot",
                (unsigned long)g_psm.missedTimeoutCount());
    }
    if (zcAccepted > 0 && zcRejected > (zcAccepted * 4)) {
        return setFail("the zero-cross signal is too noisy - check the zero-cross wiring");
    }

    // --- pressure transducer -------------------------------------------------------------------
    const char* pSrc = g_pressure.sourceName();
    if (g_pressure.source() == PressureSource::NONE) {
        return setFail("no pressure source is available");
    }
    if (g_pressure.isOverRange()) {
        // The gate confirms a faulty transducer over a short window instead of trusting a single
        // sample - see PREPARE_SENSOR_CONFIRM_MS. The reason is recorded here and the abort
        // happens in servicePreBrew() if the condition persists.
        _prepSensorFaultSinceMs = now;
        strlcpy(_prepSensorFaultReason,
                "the pressure transducer is saturated - it is shorted or miswired",
                sizeof(_prepSensorFaultReason));
        SB_LOGW("[PRE-BREW]   transducer   FAULT - saturated; confirming for %lu ms",
                (unsigned long)PREPARE_SENSOR_CONFIRM_MS);
    } else if (g_pressure.isOpenWire()) {
        // No transducer fitted is a valid configuration for a machine-driven shot, but a profile
        // brew flies on the pressure reading, so there is nothing sensible to run without it.
        _prepSensorFaultSinceMs = now;
        strlcpy(_prepSensorFaultReason,
                "no pressure transducer detected (open circuit) - a profile brew needs "
                "pressure feedback",
                sizeof(_prepSensorFaultReason));
        SB_LOGW("[PRE-BREW]   transducer   FAULT - open circuit; confirming for %lu ms",
                (unsigned long)PREPARE_SENSOR_CONFIRM_MS);
    } else {
        SB_LOGI("[PRE-BREW]   transducer   OK - source %s, node %.0f mV, %.2f bar",
                pSrc, g_pressure.lastMv(), g_pressure.lastBar());
    }

    // --- virtual button ------------------------------------------------------------------------
    if (!g_button.isArmed()) {
        return setFail("the virtual brew button is disarmed - the machine cannot be started");
    }
    SB_LOGI("[PRE-BREW]   button       ARMED - pulse %u ms",
            (unsigned)g_button.pulseWidthMs());

    // --- scale ---------------------------------------------------------------------------------
    if (g_scale.connected()) {
        SB_LOGI("[PRE-BREW]   scale        CONNECTED - %s [%s], battery %d %%, raw %.1f g",
                g_scale.deviceName(), scaleLinkName(g_scale.link()),
                (int)g_scale.battery(), g_scale.rawGrams());
    } else {
        SB_LOGW("[PRE-BREW]   scale        NOT CONNECTED - this shot runs on time/volume limits only");
    }

    SB_LOGI("[PRE-BREW] Hardware check passed%s",
            (_prepSensorFaultSinceMs != 0) ? " (pending transducer confirmation)" : "");
    return true;
}

bool BrewController::beginPreBrew(uint32_t now) {
    char fail[96] = {0};
    _prepSensorFaultSinceMs = 0;
    _prepSensorFaultReason[0] = '\0';
    if (!runPreBrewChecks(now, fail, sizeof(fail))) {
        SB_LOGE("[PRE-BREW] Hardware check FAILED - %s. The brew was not started.", fail);
        return false;
    }

    _prepActive        = true;
    _prepTareIssued    = false;
    _prepTareVerified  = false;
    _prepTareAttempts  = 0;
    _prepStartMs       = now;
    _prepReadyMs       = 0;
    _prepLaunchAtMs    = 0;

    if (g_scale.connected()) {
        _prepTareAttempts = 1;
        _prepTareIssued  = true;
        _prepTareMs      = now;
        _prepTareDeadlineMs = now + PREPARE_TARE_TIMEOUT_MS;
        _prepUpdatesAtTare = g_scale.updates();
        g_scale.tare();
        SB_LOGI("[PRE-BREW] Scale tare requested - waiting for the scale to confirm it");
    }

    enterState(BrewState::PREPARING, "pre-brew checks passed");
    return true;
}

void BrewController::abortPreBrew(const char* why, bool alreadyLogged) {
    _prepActive       = false;
    _prepTareIssued   = false;
    _prepTareVerified = false;
    _prepSensorFaultSinceMs = 0;
    if (!alreadyLogged) {
        SB_LOGE("[PRE-BREW] Start aborted - %s. The brew was not started.",
                why ? why : "unknown reason");
    }
    enterState(BrewState::IDLE, "pre-brew gate aborted");
}

void BrewController::servicePreBrew(uint32_t now) {
    // The machine must not be pumping while the gate is open - if somebody pressed the machine's
    // own brew button in the meantime, the controller must not add a second one.
    if (g_psm.acPowerPresent()) {
        abortPreBrew("the machine pump became active during preparation");
        return;
    }

    // --- pressure transducer confirmation -----------------------------------------------------
    // A transducer that read faulty when the gate opened gets a short window to recover. Only a
    // condition that persists blocks the shot.
    if (_prepSensorFaultSinceMs != 0) {
        const bool stillFaulty = g_pressure.isOverRange() || g_pressure.isOpenWire();
        if (!stillFaulty) {
            SB_LOGI("[PRE-BREW]   transducer   RECOVERED - node %.0f mV, %.2f bar",
                    g_pressure.lastMv(), g_pressure.lastBar());
            _prepSensorFaultSinceMs = 0;
            _prepSensorFaultReason[0] = '\0';
        } else if (now - _prepSensorFaultSinceMs >= PREPARE_SENSOR_CONFIRM_MS) {
            SB_LOGE("[PRE-BREW] Hardware check FAILED - %s. The brew was not started.",
                    _prepSensorFaultReason);
            abortPreBrew(_prepSensorFaultReason, true);
            return;
        } else {
            // Still inside the confirmation window - keep waiting.
            return;
        }
    }

    // --- scale tare verification ---------------------------------------------------------------
    if (_prepTareIssued) {
        if (!g_scale.connected()) {
            abortPreBrew("the scale disconnected during preparation");
            return;
        }
        // C-02: Allow a 150 ms grace window after queueing a hardware BLE tare before inspecting
        // incoming BLE packets (unless the scale explicitly asserted tareActive), so an in-flight
        // pre-tare BLE notification cannot prematurely trigger a retry or false verification.
        const bool graceElapsed = (now - _prepTareMs >= 150UL) || g_scale.tareAcknowledged();
        const bool fresh = graceElapsed && (g_scale.updates() > _prepUpdatesAtTare);
        const float net  = g_scale.grams();

        if (fresh) {
            if (net > -PREPARE_TARE_TOLERANCE_G && net < PREPARE_TARE_TOLERANCE_G) {
                _prepTareVerified = true;
                _prepTareIssued   = false;
                SB_LOGI("[PRE-BREW] Scale tare VERIFIED - net %.1f g, %s",
                        net, g_scale.tareAcknowledged()
                            ? "acknowledged by the scale" : "confirmed by a fresh reading");
            } else if ((now - _prepTareMs >= 450UL) && _prepTareAttempts < PREPARE_TARE_MAX_ATTEMPTS) {
                // The scale moved while it was being tared (a cup being placed, for instance).
                // Tare once more and give it another window.
                _prepTareAttempts++;
                _prepTareMs         = now;
                _prepTareDeadlineMs = now + PREPARE_TARE_TIMEOUT_MS;
                _prepUpdatesAtTare  = g_scale.updates();
                g_scale.tare();
                SB_LOGW("[PRE-BREW] Scale tare did not settle (net %.1f g) - taring again (attempt %u/%u)",
                        net, (unsigned)_prepTareAttempts, (unsigned)PREPARE_TARE_MAX_ATTEMPTS);
                return;
            } else if (now - _prepTareMs >= 450UL) {
                char why[80];
                snprintf(why, sizeof(why),
                         "the scale would not tare - it still reads %.1f g after %u attempts",
                         net, (unsigned)_prepTareAttempts);
                abortPreBrew(why);
                return;
            }
        }

        if (!_prepTareVerified && now >= _prepTareDeadlineMs) {
            char why[80];
            snprintf(why, sizeof(why),
                     "the scale did not confirm the tare within %lu ms",
                     (unsigned long)PREPARE_TARE_TIMEOUT_MS);
            abortPreBrew(why);
            return;
        }
    }

    // --- everything is verified: arm the settle timer -------------------------------------------
    // A tare that is still outstanding blocks the launch: the brew must never start on a scale
    // whose tare has not been proven.
    if (_prepTareIssued) return;

    if (_prepReadyMs == 0) {
        _prepReadyMs    = now;
        _prepLaunchAtMs = now + PREPARE_SETTLE_MS;
        if (_prepTareVerified) {
            SB_LOGI("[PRE-BREW] All hardware checks passed (scale tared) - the brew starts in %lu ms",
                    (unsigned long)PREPARE_SETTLE_MS);
        } else {
            SB_LOGI("[PRE-BREW] All hardware checks passed - the brew starts in %lu ms",
                    (unsigned long)PREPARE_SETTLE_MS);
        }
        return;
    }

    if (now >= _prepLaunchAtMs) {
        _prepActive = false;
        launchBrew(now);
    }
}

void BrewController::beginShotRun(uint32_t now, bool simulated) {
    if (!_passthroughHold) _faults = FAULT_NONE;
    _peakBar = 0.0f;
    _peakLogged = false;
    _stopPulseRetried = false;
    _flushingSession = false;
    _historyDecimate = 0;
    _settle.reset();   // a new shot starts with no post-shot tail to follow
    _stoppedByWeightCutoff = false;
    _settleOutcome = WeightSettleResult::OPEN;
    _flowRate.reset(simulated ? 0.0f : g_pressure.lastBar());
    _flowCal.reset();
    g_pid.reset(simulated ? 0.0f : g_pressure.lastBar());
    g_pid.clearManual();
    if (simulated) {
        char simProfName[SB_PROFILE_NAME_LEN];
        snprintf(simProfName, sizeof(simProfName), "%s (SIM)",
                 g_profile.profile().name[0] ? g_profile.profile().name : "Default");
        g_history.resetCapture(g_profile.profile().id[0] ? g_profile.profile().id : "default", simProfName);
    } else {
        g_history.resetCapture();
    }
    g_scale.resetRegression();
}

void BrewController::launchBrew(uint32_t now) {
    beginShotRun(now, false);
    g_psm.setMode(PsmMode::PROFILE);
    g_profile.start(now);
    _shotStartMs = now;
    _shotStopMs  = 0;
    // The held gauges belong to the shot that just ended - a new shot starts with fresh ones.
    _tailHoldPBar     = 0.0f;
    _tailHoldFlowVirt = 0.0f;
    _startRequestMs = now;
    g_button.requestPulse(ButtonPurpose::START);
    SB_LOGI("[BREW] START - profile '%s', %u phase(s), target %.1f g",
            g_profile.profile().name[0] ? g_profile.profile().name : "unnamed",
            (unsigned)g_profile.phaseCount(), g_profile.profile().targetWeightG);
    enterState(BrewState::STARTING, "soft start button pulsed");
}

// -------------------------------------------------------------------------------------------------
// Post-shot weight settling
//
// The cutoff reading is not the yield. When the pump stops, the last of the coffee is still
// draining out of the grouphead, so the cup keeps getting heavier for a second or two. This
// window watches the scale through that tail and reports it, so every client can show the real
// yield in the cup instead of the value that merely tripped the cutoff.
// -------------------------------------------------------------------------------------------------
void BrewController::openWeightSettle(uint32_t now) {
    if (_simMode || _flushingSession) return;      // a simulation or a rinse has no cup to fill
    if (!g_scale.connected()) return;              // nothing to watch
    if (_state != BrewState::BREWING && _state != BrewState::STARTING) return;

    _settle.open(now, g_scale.grams());
    SB_LOGI("[SETTLE] Post-shot weight settling started at %.1f g - watching the cup fill",
            _settle.finalWeightG());
}

void BrewController::serviceWeightSettle(uint32_t now, float weightG) {
    if (!_settle.active()) return;

    const WeightSettleResult r = _settle.update(now, weightG, g_scale.connected());
    // The running maximum is the yield in the cup, whether the window is still open or has just
    // closed. It is kept here so the ledger can record it even if the next shot starts first.
    _settledYieldG = _settle.finalWeightG();
    if (r != WeightSettleResult::OPEN) _settleOutcome = r;
    switch (r) {
        case WeightSettleResult::OPEN:
            return;
        case WeightSettleResult::SETTLED:
            closeWeightSettle("the weight stopped changing");
            break;
        case WeightSettleResult::LIFTED:
            closeWeightSettle("the cup was lifted");
            break;
        case WeightSettleResult::DISCONNECTED:
            closeWeightSettle("the scale disconnected");
            break;
        case WeightSettleResult::TIMED_OUT:
            closeWeightSettle("the settling window expired");
            break;
    }
}

void BrewController::finalizeShotSave() {
    if (!_shotPendingSave) return;
    _shotPendingSave = false;

    // The ledger records the yield that ended up in the cup, not the reading that merely tripped
    // the cutoff.
    g_history.setSettledDose(_settledYieldG);

    // Learn the drip lag from this shot (docs/PumpFlow.md).
    //
    // Only a shot that the predictive cutoff ended, and whose drip was watched until the weight
    // stopped moving (or the cup came off), says anything about the lag. A disconnect or a window
    // that timed out on a creeping scale does not. The new value is only CALCULATED here: saving
    // it writes flash, which never happens on the control core, so it travels to Core 0 on the
    // completion event and is applied and saved there.
    float learnedLag = -1.0f;
    if (_stoppedByWeightCutoff && g_config.cfg().scaleLagAutoLearn &&
        (_settleOutcome == WeightSettleResult::SETTLED ||
         _settleOutcome == WeightSettleResult::LIFTED)) {
        const float cutoffG = g_scale.cutoffWeightG();
        const float slopeGs = g_scale.cutoffSlopeGs();
        float measured = 0.0f;
        if (scaleLagMeasure(cutoffG, _settledYieldG, slopeGs, measured)) {
            const float current = g_config.cfg().scaleLagSec;
            learnedLag = scaleLagLearn(current, measured);
            SB_LOGI("[SETTLE] Drip after cutoff %.1f g at %.2f g/s -> measured lag %.2f s; "
                    "lag %.2f s -> %.2f s", _settledYieldG - cutoffG, slopeGs, measured,
                    current, learnedLag);
        } else {
            SB_LOGI("[SETTLE] Shot not used to learn the lag (drip %.1f g, slope %.2f g/s)",
                    _settledYieldG - cutoffG, slopeGs);
        }
    }
    _stoppedByWeightCutoff = false;

    // Calibrate the pump flow model against the cup. Like the lag, it is only calculated here and
    // applied and saved on Core 0.
    float learnedFlowScale = -1.0f;
    if (g_config.cfg().pumpFlowAutoCal) {
        float ratio = 0.0f;
        if (_flowCal.ratio(ratio)) {
            const float current = g_pid.flowScale();
            learnedFlowScale = flowScaleLearn(current, ratio);
            SB_LOGI("[FLOW] Cup flow / model flow %.3f over %.1f s of steady shot -> flow scale "
                    "%.3f -> %.3f", ratio, _flowCal.ticks() / (float)CONTROL_HZ, current,
                    learnedFlowScale);
        } else if (_flowCal.ticks() > 0) {
            SB_LOGI("[FLOW] Shot not used to calibrate the pump model (%.1f s of steady data)",
                    _flowCal.ticks() / (float)CONTROL_HZ);
        }
    }
    _flowCal.reset();

    if (_eventQueue) {
        BrewEvent evt{EventType::SHOT_COMPLETED, BrewState::IDLE, _faults, _pendingShotDurMs,
                      _pendingShotPeakBar, _settledYieldG, "shot complete", learnedLag,
                      learnedFlowScale};
        xQueueSend(_eventQueue, &evt, 0);
    }
}

void BrewController::closeWeightSettle(const char* why) {
    // The helper has already left the OPEN state, so this must not test for it: the final weight
    // is still on the object until it is reset.
    SB_LOGI("[SETTLE] Final yield %.1f g (%s)", _settle.finalWeightG(),
            why ? why : "settled");
    _settle.reset();
}

// -------------------------------------------------------------------------------------------------
// Process incoming command queue on Core 1 inside tick()
// -------------------------------------------------------------------------------------------------
// `now` is the calling tick's clock. It must not be re-read here: the state the commands switch to
// runs later in the SAME tick and compares against that tick's clock, so a fresher millis() stamp
// would look like it lies in the future (FAULT_BTN_NO_ACK on a flush start, an instant "max brew
// time" stop of a simulated shot, the pre-brew grace windows skipped).
void BrewController::processCommands(uint32_t now) {
    if (!_cmdQueue) return;
    BrewCommand cmd;

    while (xQueueReceive(_cmdQueue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
            case CmdType::START:
                if (_state != BrewState::IDLE) {
                    SB_LOGW("START ignored: machine not idle (%s)", brewStateName(_state));
                    break;
                }
                if (!_simMode && g_psm.acPowerPresent()) {
                    SB_LOGW("START refused: machine pump already active (autofill or manual)");
                    break;
                }
                if (!g_profile.loaded()) {
                    setFault(FAULT_PROFILE_MISSING, "no profile loaded");
                    SB_LOGE("START refused: no profile loaded.");
                    break;
                }
                g_button.cancelPending();

                if (_simMode) {
                    beginShotRun(now, true);
                    _simP  = 0.0f;
                    _simF  = 0.0f;
                    _simW  = 0.0f;
                    _simWf = 0.0f;
                    _simR  = 0.11f;
                    _simV  = 0.0f;
                    g_psm.setMode(PsmMode::PROFILE);
                    g_psm.forceGateOff();
                    g_profile.start(now);
                    _shotStartMs = now;
                    _shotStopMs = 0;
                    _startRequestMs = now;
                    SB_LOGI("[BREW] START - simulation, profile '%s'",
                            g_profile.profile().name[0] ? g_profile.profile().name : "unnamed");
                    enterState(BrewState::BREWING, "simulation shot started");
                } else {
                    // Real shot: the pre-brew gate takes over. It checks the hardware, tares and
                    // verifies the scale, and only then starts the brew (500 ms later).
                    beginPreBrew(now);
                }
                break;

            case CmdType::STOP:
                if (_state == BrewState::STOPPING) {
                    break;
                }
                if (_state == BrewState::PREPARING) {
                    // The brew has not started yet: there is nothing to stop on the machine, so
                    // cancel the pending start instead of pulsing the machine's stop button.
                    abortPreBrew(cmd.strArg[0] ? cmd.strArg : "cancelled by the operator");
                    break;
                }
                if (!_simMode && _state == BrewState::IDLE && !g_psm.acPowerPresent()) {
                    break;
                }
                // 1. Cut water immediately
                g_psm.forceGateOff();
                g_pid.clearManual();
                SB_LOGI("[STOP] Stop requested - %s",
                        cmd.strArg[0] ? cmd.strArg : "stop command");

                if (_simMode) {
                    if (!_flushingSession) _shotStopMs = now;
                    _stopPulseRetried = false;
                    enterState(BrewState::STOPPING, cmd.strArg[0] ? cmd.strArg : "simulation stop");
                } else {
                    // 2. Pulse stop button if machine was active or in IDLE with AC present (A4)
                    g_button.requestPulse(ButtonPurpose::STOP);
                    if (!_flushingSession) _shotStopMs = now;
                    _stopPulseRetried = false;
                    enterState(BrewState::STOPPING, cmd.strArg[0] ? cmd.strArg : "stop command");
                }
                break;

            case CmdType::FLUSH: {
                // Grouphead flush. Wake the machine with a start pulse, then hold a fixed low duty
                // for the requested time: no profile, no PID, no weight cutoff. It is a rinse, not
                // a shot, so nothing in the brew pipeline is touched.
                if (_state != BrewState::IDLE && _state != BrewState::PREPARING) {
                    SB_LOGW("FLUSH ignored: machine busy (%s)", brewStateName(_state));
                    break;
                }
                if (_state == BrewState::PREPARING) {
                    // A pending start is cancelled by the flush - the machine was never asked to
                    // brew, so there is nothing to stop out there.
                    abortPreBrew("replaced by a grouphead flush");
                }
                if (!_simMode && g_psm.acPowerPresent()) {
                    SB_LOGW("FLUSH refused: machine pump already active");
                    break;
                }
                float secs = cmd.value;
                if (secs < 1.0f)  secs = 1.0f;
                if (secs > 60.0f) secs = 60.0f;
                if (_shotPendingSave) {
                    finalizeShotSave();
                }
                _settle.reset();
                g_scale.resetRegression();
                if (!_passthroughHold) _faults = FAULT_NONE;
                _flushPending = true;
                _flushingSession = true;
                _flushDurationMs = (uint32_t)(secs * 1000.0f);
                g_pid.clearManual();
                g_psm.setMode(PsmMode::PROFILE);
                _startRequestMs = now;

                if (_simMode) {
                    _simP = 9.0f;
                    _simF = 5.5f;
                    _flushPending = false;
                    _flushUntilMs = now + _flushDurationMs;
                    _flushStartMs = now;
                    _flushStopRequested = false;
                    g_psm.forceGateOff();
                    enterState(BrewState::FLUSHING, "simulated flush started");
                } else {
                    g_button.requestPulse(ButtonPurpose::START);
                    enterState(BrewState::STARTING, "flush start button pulsed");
                }
                break;
            }

            case CmdType::TARE:
                if (_state != BrewState::IDLE && _state != BrewState::PREPARING) {
                    SB_LOGW("TARE ignored: machine is %s", brewStateName(_state));
                    break;
                }
                _simW = 0.0f;
                _simWf = 0.0f;
                // C-03: Only clear shot timer and history capture when IDLE; zeroing _shotStartMs
                // mid-shot causes (now - _shotStartMs) > 120000 to trip FAULT_MAX_TIME immediately.
                if (_state == BrewState::IDLE) {
                    _shotStartMs = 0;
                    _shotStopMs = 0;
                    g_history.resetCapture();
                }
                if (g_scale.connected()) {
                    g_scale.tare();
                } else {
                    SB_LOGW("[SCALE] Tare requested but no scale is connected");
                }
                break;

            case CmdType::NEXT_PHASE:
                if (_state == BrewState::BREWING) {
                    g_profile.nextPhase(now);
                    SB_LOGI("NEXT_PHASE requested");
                }
                break;

            case CmdType::OVERRIDE_PRESSURE:
                _overrideActive = true;
                _overrideBar    = constrain(cmd.value, 0.0f, 16.0f);
                g_profile.overrideTarget(_overrideBar);
                SB_LOGI("Pressure target override -> %.2f bar", _overrideBar);
                break;

            case CmdType::CLEAR_OVERRIDE:
                _overrideActive = false;
                _overrideBar    = 0.0f;
                g_profile.overrideTarget(0.0f);
                SB_LOGI("Pressure target override cleared");
                break;

            case CmdType::MANUAL_POWER:
                g_pid.setManualPct(cmd.value);
                SB_LOGI("Manual power -> %.0f %%", cmd.value);
                break;

            case CmdType::CLEAR_MANUAL:
                g_pid.clearManual();
                SB_LOGI("Manual power cleared");
                break;

            case CmdType::LOAD_PROFILE:
                if (_state == BrewState::IDLE) {
                    if (!g_profile.loadById(cmd.strArg)) {
                        SB_LOGW("Failed to load profile '%s'", cmd.strArg);
                    }
                } else {
                    // Defer instead of refusing: the phase table must not be swapped while the
                    // control loop is reading it, but the operator's choice should still take
                    // effect as soon as the machine goes idle.
                    strlcpy(_pendingProfileId, cmd.strArg, sizeof(_pendingProfileId));
                    SB_LOGW("LOAD_PROFILE deferred until idle (machine is %s): '%s'",
                            brewStateName(_state), cmd.strArg);
                }
                break;

            case CmdType::SET_SIM: {
                const bool enable = (cmd.value > 0.5f);
                _simMode.store(enable, std::memory_order_release);
                g_pressure.setSimulator(enable);
                if (enable) {
                    g_psm.forceGateOff();
                } else if (_state == BrewState::IDLE) {
                    g_psm.releaseGate();
                    if (!g_pid.isManual()) {
                        g_psm.setMode(PsmMode::PASSIVE);
                    }
                }
                break;
            }

            case CmdType::RESET:
                g_button.abort();
                g_profile.stop();
                _prepActive       = false;
                _prepTareIssued   = false;
                _prepTareVerified = false;
                _prepSensorFaultSinceMs = 0;
                _prepReadyMs      = 0;
                _prepLaunchAtMs   = 0;
                _faults = FAULT_NONE;
                _peakBar = 0.0f;
                _peakLogged = false;
                _overrideActive = false;
                _flushingSession = false;
                _overPressureSinceMs = 0;
                _sensorOpenSinceMs = 0;
                _overRangeSinceMs = 0;
                _idleAcActive = false;
                _idleAcSinceMs = 0;
                _idleStartMs = 0;
                _shotStartMs = _shotStopMs = 0;
                _simW = _simWf = 0.0f;
                _historyDecimate = 0;
                g_history.resetCapture();
                g_psm.releaseGate();
                g_psm.setMode(PsmMode::PASSIVE);
                g_pid.reset(g_pressure.lastBar());
                enterState(BrewState::IDLE, "reset");
                break;

            case CmdType::CLEAR_FAULTS: {
                const bool hadTopology   = (_faults & FAULT_TOPOLOGY) != 0;
                const bool passthroughHeld = _passthroughHold;
                _faults = FAULT_NONE;

                if (passthroughHeld) {
                    // A passthrough safety hold is still in force. The condition that caused it
                    // (over-pressure / saturated sensor while the machine drives the pump) has not
                    // been shown to be gone, so the gate stays closed.
                    g_psm.forceGateOff();
                    setFault(_passthroughHoldFault ? _passthroughHoldFault : FAULT_OVER_PRESSURE,
                             "passthrough safety hold still active; gate held LOW");
                    SB_LOGW("CLEAR_FAULTS refused: passthrough safety hold still active");
                } else if (!g_psm.acPowerPresent()) {
                    _idleAcActive = false;
                    _idleAcSinceMs = 0;
                    enterState(BrewState::IDLE, "faults cleared");
                } else if (hadTopology) {
                    // The permanent-mains condition is still present. Hold the gate LOW.
                    g_psm.forceGateOff();
                    setFault(FAULT_TOPOLOGY, "mains AC still active; gate held LOW");
                    SB_LOGW("CLEAR_FAULTS: mains AC still active under topology fault; gate held LOW");
                } else if (_state == BrewState::FAULT) {
                    if (!g_pressure.isOpenWire() && !g_pressure.isOverRange() &&
                        g_pressure.lastBar() < g_config.safetyPressureBar()) {
                        SB_LOGW("Operator clearing FAULT while AC present; releasing gate hold");
                        _shotStopMs = 0;
                        _idleAcActive = false;
                        _idleAcSinceMs = 0;
                        enterState(BrewState::IDLE, "faults cleared by operator");
                    } else {
                        g_psm.forceGateOff();
                        SB_LOGW("CLEAR_FAULTS refused state transition: fault condition still active");
                    }
                }
                SB_LOGI("Faults cleared");
                break;
            }
        }
    }
}

// -------------------------------------------------------------------------------------------------
// 100 Hz Real-Time Control Tick (Core 1)
// -------------------------------------------------------------------------------------------------
void BrewController::tick(uint32_t nowMs) {
    const uint32_t nowUs = micros();
    if (_lastTickUs != 0) {
        const uint32_t periodUs = nowUs - _lastTickUs;
        if (periodUs < _loopUsMin) _loopUsMin = periodUs;
        if (periodUs > _loopUsMax) _loopUsMax = periodUs;
        _loopUsAvg = _loopUsAvg * 0.99f + (float)periodUs * 0.01f;
    }
    _lastTickUs = nowUs;
    _stageUs[0] = nowUs;

    // ONE clock for the whole tick. Every timestamp written during this tick - including those
    // written while the command queue is drained - must come from this value. A second millis()
    // read can be one millisecond later; a stamp taken from it is then "in the future" for the
    // rest of the tick, and every `now - stamp` wraps to ~4.29e9 and reads as "timed out".
    const uint32_t now = nowMs;
    _nowMs = now;
    _historyDecimate++;
    float dt = (float)(now - _lastTickMs) * 0.001f;
    _lastTickMs = now;
    if (dt <= 0.0f) dt = g_config.cfg().loopDt;
    if (dt > 0.25f)  dt = 0.25f;

    // 1. Drain commands from other tasks (on this tick's clock - see above)
    processCommands(now);

#if defined(SB_BENCH) && (SB_BENCH != 0)
    // Bench guard for the one-clock rule: no stamp the state machine compares against may lie
    // ahead of this tick's clock. A signed difference catches it without tripping on wrap-around.
    {
        const uint32_t stamps[] = { _startRequestMs, _shotStartMs, _prepTareMs,
                                    _prepSensorFaultSinceMs, _flushStartMs, _stopRequestMs };
        for (uint32_t st : stamps) {
            if (st != 0 && (int32_t)(st - now) > 0) {
                SB_LOGE("CLOCK: a timestamp is %ld ms ahead of the tick clock - a second "
                        "millis() read has crept in", (long)(int32_t)(st - now));
                break;
            }
        }
    }
#endif

    // 2. Hardware services
    _stageUs[1] = micros();   // commands drained
    // Losing the zero-cross is only unexpected while the controller itself runs the pump. After a
    // stop, or while the machine is in charge, the pulses ending is how pump power is released.
    g_psm.setAcLossExpected(!(_state == BrewState::BREWING || _state == BrewState::STARTING ||
                              _state == BrewState::FLUSHING));
    g_psm.tickWatchdog();
    g_button.service();

    // 3. Pressure sampling.
    _stageUs[2] = micros();   // psm / button serviced
    //    sampleBar() takes its own reading (serviceFast) before filtering, so calling
    //    serviceFast() here as well would sample the sensor twice per 10 ms tick.
    const float p = _simMode ? _simP : g_pressure.sampleBar();
    if (p > _peakBar) _peakBar = p;

    const float weightG = _simMode ? _simW : (_simWeightOverride ? _simWeightG : g_scale.grams());
    const bool  lockout = (now - _shotStartMs) < ((uint32_t)g_config.cfg().scaleLockoutSec * 1000UL) ||
                          (now - _shotStartMs) < ((uint32_t)g_config.cfg().preInfusionProtectSec * 1000UL);

    // 3b. Scale link upkeep + predictive-cutoff evaluation (skipped during grouphead flush).
    _stageUs[3] = micros();   // pressure sampled
    if (!_flushingSession) {
        const bool scaleBrewing = (_state == BrewState::BREWING);
        // While the post-shot weight is settling the last of the coffee is still arriving, so the
        // scale service keeps the weight flow alive through the tail.
        const bool scaleTail = _settle.active();
        g_scale.tick(now, g_profile.profile().targetWeightG, scaleBrewing, scaleBrewing && lockout,
                     scaleTail);

        // 3c. Post-shot weight settling. The window outlives the stop, so this runs in every state:
        // it is what lets the client show the real yield in the cup instead of the cutoff value.
        serviceWeightSettle(now, weightG);

        // 3d. Post-shot tail capture.
        if (_settle.active() && (_historyDecimate % 10 == 0) && _shotStartMs > 0) {
            ShotSample s;
            s.tMs        = (now > _shotStartMs) ? (now - _shotStartMs) : 0;
            s.pBar       = _tailHoldPBar;
            s.pTarget    = 0.0f;
            s.flowVirt   = _tailHoldFlowVirt;
            s.psmPct     = 0;
            s.weightG    = weightG;
            s.flowWeight = g_scale.flowGs();
            s.phaseIdx   = g_profile.phaseIndex();
            g_history.addTailSample(s);
        }
    }

    // A shot that ended while the tail was still running is only reported now that the yield in
    // the cup is known. Without a tail there is nothing to wait for and this never fires.
    if (_shotPendingSave && !_settle.active()) {
        finalizeShotSave();
    }

    const bool acPresent = g_psm.acPowerPresent();
    // How long the mains has been missing without a break. A single 35 ms zero-cross gap is not
    // the machine releasing its button: a switching pump on the same feed, or the machine's own
    // pump control, can drop a few half-cycles mid-shot (seen on hardware: one gap ended a
    // shot 4 s in). The gate is already off while the pulses are missing (hal_psm), so waiting
    // costs nothing in safety; only a gap that lasts SB_AC_LOSS_STOP_MS ends the shot.
    if (acPresent) {
        _acLostSinceMs = 0;
    } else if (_acLostSinceMs == 0) {
        _acLostSinceMs = now ? now : 1;
    }
    const bool acLostForGood = !acPresent && (now - _acLostSinceMs) >= SB_AC_LOSS_STOP_MS;
    const bool owned = (_state == BrewState::BREWING || _state == BrewState::STARTING ||
                        _state == BrewState::FLUSHING);
    const bool brewOwned = (_state == BrewState::BREWING ||
                            (_state == BrewState::STARTING && !_flushingSession));

    // 4. Interlock safety evaluations
    _stageUs[4] = micros();   // scale serviced
    if (!_simMode) {
    const float safety = g_config.safetyPressureBar();
    if (p >= safety) {
        if (_overPressureSinceMs == 0) _overPressureSinceMs = now;
        if (now - _overPressureSinceMs > 150) {
            if (!(_faults & FAULT_OVER_PRESSURE)) {
                setFault(FAULT_OVER_PRESSURE, "sustained over-pressure");
                if (owned) {
                    g_psm.forceGateOff();
                    requestStop("over-pressure cut-off");
                } else if (acPresent) {
                    engagePassthroughHold(FAULT_OVER_PRESSURE,
                                          "sustained over-pressure during machine-driven pump");
                }
            }
        }
    } else {
        _overPressureSinceMs = 0;
        if (_faults & FAULT_OVER_PRESSURE) {
            _faults &= ~FAULT_OVER_PRESSURE;
            SB_LOGI("Pressure normal: FAULT_OVER_PRESSURE cleared");
        }
    }

    const bool openWire = g_pressure.isOpenWire();

    if (brewOwned && openWire) {
        // The ESP32 owns the pump: an open-wire sensor means we would be flying blind, so this cuts
        // the water and stops the shot.
        if (_sensorOpenSinceMs == 0) _sensorOpenSinceMs = now;
        if (now - _sensorOpenSinceMs > 200) {
            if (!(_faults & FAULT_SENSOR_OPEN)) {
                setFault(FAULT_SENSOR_OPEN, "transducer open-wire detected");
                g_psm.forceGateOff();
                requestStop("sensor open-wire cut-off");
            }
        }
        _sensorOpenWarnActive = false;
        _openWireClearMs = 0;
    } else if (!openWire) {
        _sensorOpenSinceMs = 0;
        if (_faults & FAULT_SENSOR_OPEN) {
            _faults &= ~FAULT_SENSOR_OPEN;
            SB_LOGI("Transducer signal detected: FAULT_SENSOR_OPEN cleared");
        }
        // Only clear the notice once the signal has been good for a moment. One good sample is not
        // proof: an unconnected input floats and crosses the threshold constantly, so clearing on
        // the first good reading would make the notice flicker on and off.
        if (_sensorOpenWarnActive || _openWireWarnFired) {
            if (_openWireClearMs == 0) {
                _openWireClearMs = now;
            } else if (now - _openWireClearMs >= OPEN_WIRE_CLEAR_MS) {
                _sensorOpenWarnActive = false;
                _openWireWarnFired = false;
                _openWireClearMs = 0;
                _lastOpenWireWarnMs = 0;   // the next episode warns immediately
            }
        }
    } else {
        // Passthrough: no transducer fitted is a valid configuration, not a fault. Warn once and then
        // at most once a minute, but never cut the machine's own pump.
        _sensorOpenSinceMs = 0;
        _openWireClearMs = 0;
        _sensorOpenWarnActive = true;
        if (!_openWireWarnFired || (now - _lastOpenWireWarnMs >= OPEN_WIRE_WARN_INTERVAL_MS)) {
            _openWireWarnFired = true;
            _lastOpenWireWarnMs = now;
            SB_LOGW("No transducer signal (%.0f mV at the divider: sensor unpowered, unwired or "
                    "open) - warning only outside a controller-run shot; no transducer fitted is a "
                    "valid configuration", g_pressure.lastMv());
        }
    }

    // Over-range is only interesting while the pump can actually be running: either the ESP32 owns
    // it, or the machine is driving it (passthrough). In neither case should an idle, unpowered
    // sensor raise a fault.
    if (g_pressure.isOverRange() && (owned || acPresent)) {
        if (_overRangeSinceMs == 0) _overRangeSinceMs = now;
        if (now - _overRangeSinceMs > 200) {
            if (!(_faults & FAULT_SENSOR_OVERRANGE)) {
                setFault(FAULT_SENSOR_OVERRANGE, "transducer voltage saturated / shorted (>3.05V)");
                if (owned) {
                    g_psm.forceGateOff();
                    requestStop("sensor over-range cut-off");
                } else if (acPresent) {
                    engagePassthroughHold(FAULT_SENSOR_OVERRANGE,
                                          "sensor saturated / shorted during machine-driven pump");
                }
            }
        }
    } else if (!g_pressure.isOverRange()) {
        _overRangeSinceMs = 0;
        if (_faults & FAULT_SENSOR_OVERRANGE) {
            _faults &= ~FAULT_SENSOR_OVERRANGE;
            SB_LOGI("Sensor voltage normal: FAULT_SENSOR_OVERRANGE cleared");
        }
    } else {
        _overRangeSinceMs = 0;
    }
    } // end if (!_simMode)

    // Pressure rate of change for the compliance correction below: slow-filtered and dead-banded
    // (PuckFlowRate, include/pump_physics.h) so the ripple of a steady pressure cannot swing the
    // flow estimate - only a real ramp moves it.
    const float flowDpdt = _flowRate.update(p, dt);

    // Update continuous filtered virtual flow (tau ~ 160 ms at 100 Hz).
    //
    // The pump model gives the PUMP-side flow. While the pressure rises, part of that is stored in
    // the line, the group and the headspace instead of passing the puck, and while it falls the
    // stored water is released through the puck. The compliance correction turns the estimate
    // into the puck-side flow - the quantity a FLOW phase and the flow triggers are about:
    //     Q_puck = Q_pump - C * dP/dt          (C = config.complianceMlPerBar, 0 = off)
    if (_state == BrewState::IDLE) {
        _filteredFlowVirt = 0.0f;
    } else {
        const float pumpFlow = g_pid.virtualFlow(p, (float)g_psm.dutyPct());
        const float rawVirtFlow = complianceCorrectedFlow(pumpFlow, flowDpdt,
                                                          g_config.cfg().complianceMlPerBar);
        constexpr float kFlowAlpha = 0.06f;
        _filteredFlowVirt = kFlowAlpha * rawVirtFlow + (1.0f - kFlowAlpha) * _filteredFlowVirt;
    }

    // The gauges the post-shot tail will hold.
    //
    // They follow the live readings for as long as the pump is running, and they are frozen from
    // the moment the pump stops: the line pressure falls away and the flow reads zero, and the
    // tail shows the shot as it ended rather than that collapse. The weight is not part of this -
    // the last of the coffee is still arriving, so the tail keeps carrying it to the yield.
    shotTailHoldGauges(p, _filteredFlowVirt,
                       (_state == BrewState::BREWING || _state == BrewState::STARTING),
                       _tailHoldPBar, _tailHoldFlowVirt);

    // 5. State Machine
    _stageUs[5] = micros();   // interlocks evaluated
    switch (_state) {
        case BrewState::IDLE:
            if (_simMode) {
                _simP  = 0.0f;
                _simF  = 0.0f;
                _simW  = 0.0f;
                _simWf = 0.0f;
                _simR  = 0.11f;
                _simV  = 0.0f;
                g_psm.forceGateOff();
                break;
            }

            if (g_pid.isManual()) {
                g_psm.setMode(PsmMode::PROFILE);
                g_psm.setDutyPct(g_pid.manualPct());
            } else {
                g_psm.setMode(PsmMode::PASSIVE);
            }

            if (acPresent) {
                if (!_idleAcActive) {
                    _idleAcActive = true;
                    _idleAcSinceMs = now;
                    _idleStartMs = now;
                    _historyDecimate = 0; // R3-08: clean 100 ms alignment for passive capture
                    _peakBar = 0.0f; // R2-12: reset peakBar on passive shot start
                    g_history.resetCapture("passive", "Passive Manual/Autofill"); // R2-12: dedicated passive profile
                    SB_LOGI("Mains AC active in IDLE: machine autofill / manual bypass (100%% passthrough)");
                }

                // A5: Topology check (CR-02, NX-06, R2-04, R3-12)
                const uint32_t topTimeoutMs = (uint32_t)(g_config.cfg().topologyTimeoutSec ? g_config.cfg().topologyTimeoutSec : 120) * 1000UL;
                if (_idleAcSinceMs != 0 && (now - _idleAcSinceMs > topTimeoutMs)) {
                    setFault(FAULT_TOPOLOGY, "suspected permanent mains AC in IDLE");
                    g_psm.forceGateOff();
                    enterState(BrewState::FAULT, "suspected permanent mains AC fault");
                    break;
                }

                // B3: Passive shot observation (R2-05: decimated to 10 Hz so 900 samples = 90 s)
                if (g_history.captureActive() && (_historyDecimate % 10 == 0)) {
                    ShotSample s;
                    s.tMs        = (now > _idleStartMs) ? (now - _idleStartMs) : 0;
                    s.pBar       = p;
                    s.pTarget    = 0.0f;
                    s.flowVirt   = 0.0f;
                    s.psmPct     = 100;
                    s.weightG    = weightG;
                    s.flowWeight = g_scale.flowGs();
                    s.phaseIdx   = 0;
                    g_history.addSample(s);
                }
            } else {
                if (_idleAcActive) {
                    _idleAcActive = false;
                    const uint32_t durMs = (now > _idleStartMs) ? (now - _idleStartMs) : 0;
                    SB_LOGI("Mains AC dropped in IDLE: boiler fill / manual pump complete (dur=%lums)", (unsigned long)durMs);
                    if (durMs >= 5000 && _eventQueue) {
                        _shotCount++;
                        // The curve is frozen so that the next shot can start capturing straight
                        // away while this one is still waiting to be written to flash.
                        g_history.freezeShot(durMs);
                        _shotPendingSave    = true;
                        _pendingShotDurMs   = durMs;
                        _pendingShotPeakBar = _peakBar;
                        _settledYieldG      = g_history.doseG();
                        finalizeShotSave();
                    }
                    _idleAcSinceMs = 0;
                }
            }
            break;

        case BrewState::PREPARING:
            // The pre-brew gate. Nothing is driven here - the controller only verifies the
            // hardware and waits for the scale tare to be confirmed.
            servicePreBrew(now);
            break;

        case BrewState::STARTING:
            if (_simMode) {
                enterState(BrewState::BREWING, "simulation mode active");
                break;
            }
            g_psm.setDutyPct(0.0f);
            if (acPresent && g_psm.brewConfirmed()) {
                if (_flushPending) {
                    _flushPending = false;
                    _flushUntilMs = now + _flushDurationMs;
                    _flushStartMs = now;
                    _flushStopRequested = false;
                    enterState(BrewState::FLUSHING, "ZC burst acknowledged flush start");
                } else {
                    enterState(BrewState::BREWING, "ZC burst acknowledged soft start");
                }
            } else if (now - _startRequestMs > (uint32_t)g_config.cfg().buttonAckTimeoutMs) {
                _flushPending = false;
                setFault(FAULT_BTN_NO_ACK, "no ZC burst within button ack window");
                g_button.abort();
                enterState(BrewState::FAULT, "soft start not acknowledged");
            }
            break;

        case BrewState::FLUSHING: {
            if (_simMode) {
                g_psm.forceGateOff();
                _simP = 9.0f;
                _simF = 5.5f;
                if (now >= _flushUntilMs && !_flushStopRequested) {
                    _flushStopRequested = true;
                    requestStop("flush complete");
                }
                break;
            }

            // A rinse, not a shot: a fixed low duty, no PID, no profile, no weight cutoff.
            g_psm.setDutyPct(_flushDutyPct);

            // The machine released the pump on its own (its own timeout or the brew switch).
            if (acLostForGood && (now - _flushStartMs) > 500) {
                g_psm.forceGateOff();
                _shotStopMs = now;
                _stopPulseRetried = false;
                enterState(BrewState::STOPPING, "flush ended (AC lost)");
                break;
            }

            if (now >= _flushUntilMs && !_flushStopRequested) {
                _flushStopRequested = true;   // ask once, not every tick
                SB_LOGI("Flush complete (%u ms) - stopping.", (unsigned)_flushDurationMs);
                requestStop("flush complete");
            }
            break;
        }

        case BrewState::BREWING: {
            if (_simMode) {
                g_psm.forceGateOff();

                // Absolute maximum brew time limit (120 s)
                if (now - _shotStartMs > 120000UL) {
                    requestStop("max brew time limit");
                    break;
                }

                // Target setpoint from profile engine (in simulation mode, pass isSimulation = true)
                const float target = g_profile.tick(_simP, _simW, _simF, now, true);
                const Phase& curPhase = g_profile.currentPhase();

                float targetP = 0.0f;
                if (curPhase.mode == PhaseMode::PRESSURE) {
                    targetP = target;
                } else if (curPhase.mode == PhaseMode::FLOW) {
                    if (target <= 0.01f || curPhase.maxPsmPct == 0) {
                        targetP = 0.0f;
                    } else {
                        // Flow control mode (ml/s target): invert Q = sqrt(P / R) -> P = Q^2 * R
                        targetP = constrain(target * target * _simR, 0.0f, 11.5f);
                    }
                } else if (curPhase.mode == PhaseMode::POWER) {
                    if (target <= 0.05f || curPhase.maxPsmPct == 0) {
                        // Zero-Power Bloom: pump off, pressure bleeds smoothly
                        targetP = 0.0f;
                    } else {
                        // Duty percentage % (0..100%)
                        const float cappedDuty = constrain(target, 0.0f, (float)curPhase.maxPsmPct);
                        targetP = (cappedDuty / 100.0f) * 9.5f;
                    }
                } else { // PhaseMode::PAUSE
                    targetP = 0.0f;
                }

                // First-order pressure lag (tau ~ 0.12s at 100 Hz)
                _simP += (targetP - _simP) * 0.08f;
                if (_simP < 0.01f) _simP = 0.0f;
                if (_simP > _peakBar) _peakBar = _simP;

                // 3. Torricelli / Darcy's Law Approximation: Q = sqrt(P / R)
                const float rawF = (_simP > 0.01f) ? sqrtf(_simP / std::max(0.05f, _simR)) : 0.0f;

                // 1. Dynamic Puck Resistance (R): increases monotonically with cumulative water
                //    volume (_simV) as grounds swell, and applied pressure (_simP) as the bed compacts
                const float vNorm = _simV / 13.5f;
                const float rNorm = _simR / 1.85f;
                const float dR_swell = (0.052f * rawF) / (1.0f + vNorm * vNorm);
                const float dR_press = (0.0075f * _simP * _simP) / (1.0f + rNorm * rNorm * rNorm);
                _simR += (dR_swell + dR_press) * dt;

                // 2. Low-Pass EMA Filter on Flow:
                //    At 100 Hz control tick, alpha = 0.04 yields an effective per-packet smoothing
                //    of ~0.185 at the 20 Hz WebSocket telemetry rate, rounding off sharp corners.
                constexpr float kFlowEmaAlpha = 0.04f;
                _simF = kFlowEmaAlpha * rawF + (1.0f - kFlowEmaAlpha) * _simF;
                if (_simF < 0.01f) _simF = 0.0f;

                // Integrate water volume delivered into puck and compute cup yield after headspace saturation
                _simV += _simF * dt;
                const float satNorm = constrain((_simV - 11.5f) / 9.5f, 0.0f, 1.0f);
                const float sat = satNorm * satNorm * (3.0f - 2.0f * satNorm);
                const float targetWf = (_simP > 0.1f) ? (_simF * sat * 0.95f) : 0.0f;
                _simWf = 0.05f * targetWf + 0.95f * _simWf;
                if (_simWf < 0.01f) _simWf = 0.0f;
                _simW += _simWf * dt;

                // Gravimetric cutoff
                float targetWeight = g_profile.profile().targetWeightG;
                if (targetWeight > 0.0f && _simW >= targetWeight) {
                    requestStop("weight target reached");
                    break;
                }

                // Profile complete
                if (!g_profile.running() && g_profile.finished()) {
                    requestStop("profile complete");
                    break;
                }

                // Capture shot sample (decimated to 10 Hz)
                if (g_history.captureActive() && (_historyDecimate % 10 == 0)) {
                    ShotSample s;
                    s.tMs        = (now > _shotStartMs) ? (now - _shotStartMs) : 0;
                    s.pBar       = _simP;
                    s.pTarget    = target;
                    s.flowVirt   = _simF;
                    s.psmPct     = (_simP > 0.05f) ? 100 : 0;
                    s.weightG    = _simW;
                    s.flowWeight = _simWf;
                    s.phaseIdx   = g_profile.phaseIndex();
                    g_history.addSample(s);
                }
                break;
            }

            // Absolute maximum brew time limit (120 s)
            if (now - _shotStartMs > 120000UL) {
                setFault(FAULT_MAX_TIME, "max brew time 120s exceeded");
                requestStop("max brew time limit");
                break;
            }

            // AC loss mid-shot: physical button switch released or machine auto-stopped
            if (acLostForGood && (now - _shotStartMs) > 500) {
                SB_LOGI("Mains gone for %lu ms - the machine released the pump",
                        (unsigned long)(now - _acLostSinceMs));
                g_psm.forceGateOff();
                _shotStopMs = now;
                _stopPulseRetried = false;
                enterState(BrewState::STOPPING, "physical button released (AC lost)");
                break;
            }

            // Calculate target and flow (normalized to ml/s; espresso density ~ 1.03 g/ml)
            const float scaleFlowGs = g_scale.flowGs();
            const float cupFlowMlS  = scaleFlowGs / SB_ESPRESSO_DENSITY_G_PER_ML;
            const float flowMlPerSec = (g_scale.connected() && scaleFlowGs > 0.001f)
                                     ? cupFlowMlS
                                     : _filteredFlowVirt;

            const float target = g_profile.tick(p, weightG, flowMlPerSec, now);
            const Phase& curPhase = g_profile.currentPhase();

            float duty = 0.0f;
            if (g_pid.isManual()) {
                duty = g_pid.manualPct();
            } else if (curPhase.mode == PhaseMode::FLOW) {
                if (target <= 0.01f || curPhase.maxPsmPct == 0) {
                    duty = 0.0f;
                } else {
                    // Feed-forward from the pump model: the duty that delivers the target flow at
                    // the pressure the line is at right now. It used to be a fixed line
                    // (18 %/(ml/s) * target + 15 %) that ignored both the pump preset and the
                    // back-pressure, and asked 51 % for a 2 ml/s target at 6 bar where the Ulka EX5
                    // model needs 26 %.
                    const float ffDuty = constrain(g_pid.flowFeedForward(target, p), 0.0f,
                                                   (float)curPhase.maxPsmPct);
                    // The proportional term trims what the model gets wrong, against the measured
                    // cup flow when a scale reports one and the puck-flow estimate otherwise.
                    const float flowErr = target - flowMlPerSec;
                    duty = constrain(ffDuty + flowErr * 12.0f, 0.0f, (float)curPhase.maxPsmPct);
                }
                // Pressure limit: run the pressure loop on the limit alongside and take the lower
                // duty, so a choking puck cannot drive the pressure past it.
                if (curPhase.pressureLimit > 0.0f) {
                    const float pDuty = g_pid.update(p, curPhase.pressureLimit, curPhase.maxPsmPct, dt);
                    if (pDuty < duty) duty = pDuty;
                }
            } else if (curPhase.mode == PhaseMode::POWER) {
                duty = constrain(target, 0.0f, (float)curPhase.maxPsmPct);
            } else if (curPhase.mode == PhaseMode::PAUSE) {
                duty = 0.0f;
            } else { // PhaseMode::PRESSURE
                // Flow limit: the duty the pump model needs to deliver the limit at the current
                // pressure becomes the phase's power ceiling (a gentle fill / pre-infusion that
                // behaves the same on any pump, unlike a raw power percentage).
                uint8_t ceiling = curPhase.maxPsmPct;
                if (curPhase.flowLimit > 0.0f) {
                    const float limDuty = g_pid.flowFeedForward(curPhase.flowLimit, p);
                    if (limDuty < (float)ceiling) ceiling = (uint8_t)(limDuty + 0.5f);
                }
                duty = g_pid.update(p, target, ceiling, dt);
            }
            g_psm.setDutyPct(duty);

            // Pump model calibration: compare the model with the cup while the shot is steady
            // (src/control/flow_calibration.h). The model flow is the pump-side flow without the
            // compliance correction - in a steady state the two are the same, and the gate below
            // only lets steady ticks through.
            _flowCal.addTick(g_scale.connected() && !g_scale.stale() && !lockout && !g_pid.isManual(),
                             p, _flowRate.filteredRate(),
                             g_pid.virtualFlow(p, (float)g_psm.dutyPct()), cupFlowMlS);

            // Weight cutoff (the scale was already serviced in step 3b of this cycle)
            if (g_scale.takeCutoffSignal()) {
                // Remembered so the drip that follows can be measured against the cutoff point
                // and the lag learned from it (finalizeShotSave).
                _stoppedByWeightCutoff = true;
                requestStop("weight target reached");
                break;
            }

            // Profile complete
            if (!g_profile.running() && g_profile.finished()) {
                requestStop("profile complete");
                break;
            }

            // Capture shot sample (R2-05: decimated to 10 Hz so 900 samples = 90 s)
            if (g_history.captureActive() && (_historyDecimate % 10 == 0)) {
                ShotSample s;
                s.tMs        = (now > _shotStartMs) ? (now - _shotStartMs) : 0;
                s.pBar       = p;
                s.pTarget    = target;
                s.flowVirt   = _filteredFlowVirt;
                s.psmPct     = (uint8_t)(g_psm.dutyPct() + 0.5f);
                s.weightG    = weightG;
                s.flowWeight = g_scale.flowGs();
                s.phaseIdx   = g_profile.phaseIndex();
                g_history.addSample(s);
            }
            break;
        }

        case BrewState::STOPPING:
            g_psm.forceGateOff();
            if (_simMode) {
                if (_flushingSession) {
                    _flushingSession = false;
                    _shotStopMs = _shotStartMs;
                    enterState(BrewState::IDLE, "simulated flush complete");
                    break;
                }
                _shotCount++;
                const uint32_t durMs = (_shotStopMs > _shotStartMs) ? (_shotStopMs - _shotStartMs) : 0;
                g_profile.stop();
                g_pid.reset(0.0f);
                SB_LOGI("[STOP] %s - %.1f g in %.1f s (simulation, peak %.2f bar)",
                        _stopReason, _simW, durMs / 1000.0f, _peakBar);
                // A simulated shot has no scale, so there is no post-shot tail to wait for.
                g_history.freezeShot(durMs);
                _shotPendingSave    = true;
                _pendingShotDurMs   = durMs;
                _pendingShotPeakBar = _peakBar;
                _settledYieldG      = _simW;
                finalizeShotSave();
                enterState(BrewState::IDLE, "simulation shot complete");
                break;
            }

            // Machine vents 3-way valve and drops AC: safe to finalize shot and return to IDLE!
            if (!acPresent) {
                if (_flushingSession) {
                    _flushingSession = false;
                    _shotStopMs = _shotStartMs;
                    g_pid.reset(g_pressure.lastBar());
                    enterState(BrewState::IDLE, "flush complete (machine released pump power)");
                    break;
                }
                _shotCount++;
                const uint32_t durMs = (_shotStopMs > _shotStartMs) ? (_shotStopMs - _shotStartMs) : 0;
                g_profile.stop();
                g_pid.reset(g_pressure.lastBar());

                // The curve is frozen here, while it still belongs to this shot: the ledger entry
                // is written a moment later, once the post-shot tail has run out, and by then the
                // next shot may already be capturing. The shot's own brewing time is passed in, so
                // the tail samples that follow cannot stretch it.
                g_history.freezeShot(durMs);

                const float doseG = g_history.doseG();
                SB_LOGI("[STOP] %s - %.1f g in %.1f s (peak %.2f bar, ended in phase %u '%s')",
                        _stopReason, doseG, durMs / 1000.0f, _peakBar,
                        (unsigned)(g_profile.phaseIndex() + 1), g_profile.phaseName());

                // The completion is held back until the tail has run out, so the ledger and the
                // clients are told the yield that ended up in the cup. With no scale there is no
                // tail to wait for, and the shot is final right away.
                _shotPendingSave    = true;
                _pendingShotDurMs   = durMs;
                _pendingShotPeakBar = _peakBar;
                _settledYieldG      = doseG;
                if (!_settle.active()) {
                    finalizeShotSave();
                }
                enterState(BrewState::IDLE, "machine released pump power");
            } else if (_stopRequestMs > 0 && (now - _stopRequestMs > 3000)) {
                // AC still active after 3s: retry virtual button pulse once
                if (!_stopPulseRetried) {
                    _stopPulseRetried = true;
                    g_button.requestPulse(ButtonPurpose::STOP);
                    SB_LOGW("AC still active after 3s in STOPPING - retrying button pulse");
                } else if (now - _stopRequestMs > 5000) {
                    setFault(FAULT_BTN_NO_ACK, "AC remained active after stop pulse");
                    enterState(BrewState::FAULT, "stop pulse not acknowledged");
                }
            }
            break;

        case BrewState::FAULT:
            g_psm.forceGateOff();
            if (!acPresent && (now - _stopRequestMs) > 1000) {
                _shotStopMs = 0;
                _stopRequestMs = 0;
                if (!_passthroughHold) _faults = FAULT_NONE;
                enterState(BrewState::IDLE, "fault cleared, machine idle");
            }
            break;
    }

    // 6. Refresh Telemetry snapshot double-buffer (lock-free seqlock)
    _stageUs[6] = micros();   // state machine done
    Telemetry t;
    const bool flushActive = _flushingSession &&
                             (_state == BrewState::STARTING || _state == BrewState::FLUSHING || _state == BrewState::STOPPING);
    const bool shotRunning = !_flushingSession && (_state == BrewState::BREWING || _state == BrewState::STOPPING);
    const bool shotPending = !_flushingSession && (_state == BrewState::PREPARING || _state == BrewState::STARTING);
    if (_state == BrewState::FLUSHING && _flushUntilMs > now) {
        t.flushRemMs = _flushUntilMs - now;
    } else if (_flushingSession && _state == BrewState::STARTING) {
        t.flushRemMs = _flushDurationMs;
    } else {
        t.flushRemMs = 0;
    }
    if (flushActive) {
        t.tMs = t.flushRemMs;
    } else if (shotRunning && _shotStartMs > 0 && now >= _shotStartMs) {
        t.tMs = now - _shotStartMs;
    } else if (!shotPending && _shotStartMs > 0 && _shotStopMs > _shotStartMs) {
        t.tMs = _shotStopMs - _shotStartMs;   // the frozen final duration of the last shot
    } else {
        t.tMs = 0;
    }
    t.state      = flushActive ? BrewState::FLUSHING : _state.load();
    t.pumpOwner  = pumpOwner();
    t.psmMode    = g_psm.mode();
    t.phaseIdx   = g_profile.phaseIndex();
    strlcpy(t.phaseName, g_profile.phaseName(), sizeof(t.phaseName));
    strlcpy(t.profileId, g_profile.profile().id[0] ? g_profile.profile().id : "default", sizeof(t.profileId));
    strlcpy(t.profileName, g_profile.profile().name[0] ? g_profile.profile().name : "Default", sizeof(t.profileName));
    t.pBar       = _simMode ? _simP : p;
    t.pTarget    = g_profile.currentTarget();
    t.flowVirt   = _simMode ? _simF : _filteredFlowVirt;
    t.psmPct     = _simMode ? (_state == BrewState::BREWING && _simP > 0.05f ? 100 : 0) : (uint8_t)(g_psm.dutyPct() + 0.5f);
    t.weightG    = _simMode ? _simW : weightG;
    t.flowWeight = _simMode ? _simWf : g_scale.flowGs();
    t.scaleConn  = _simMode ? true : g_scale.connected();
    t.scaleBat   = _simMode ? 100 : g_scale.battery();
    t.acHz       = (uint8_t)g_psm.mainsHz();
    t.phasePct   = (uint8_t)(g_profile.phaseProgress() * 100.0f);
    t.doseG      = (uint16_t)(_simMode ? (_simW * 10.0f) : (g_scale.grams() * 10.0f));
    t.sensorOpenWarn = _sensorOpenWarnActive;

    // C-04: Odd/even seqlock with release barriers so readers never see a torn Telemetry struct
    const uint32_t seq = _telemetrySeq.load(std::memory_order_relaxed);
    const uint32_t nextSlot = ((seq >> 1) + 1U) & 1U;
    _telemetrySeq.store(seq + 1U, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_acq_rel);
    _telemetryBuf[nextSlot] = t;
    std::atomic_thread_fence(std::memory_order_acq_rel);
    _telemetrySeq.store(seq + 2U, std::memory_order_release);
}

// -------------------------------------------------------------------------------------------------
// JSON command dispatcher
// -------------------------------------------------------------------------------------------------
bool BrewController::handleJsonCommand(const char* json, String& response) {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, json)) {
        response = "{\"ok\":false,\"err\":\"malformed json\"}";
        return false;
    }

    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "START") == 0) {
        char err[64] = {0};
        if (requestStart(err, sizeof(err))) {
            response = "{\"ok\":true,\"cmd\":\"START\"}";
            return true;
        } else {
            response = String("{\"ok\":false,\"cmd\":\"START\",\"err\":\"") + (err[0] ? err : "busy") + "\"}";
            return false;
        }
    }
    if (strcmp(cmd, "STOP") == 0) {
        const char* reason = doc["reason"] | "remote stop";
        if (requestStop(reason)) {
            response = "{\"ok\":true,\"cmd\":\"STOP\"}";
            return true;
        } else {
            response = "{\"ok\":false,\"cmd\":\"STOP\",\"err\":\"not running\"}";
            return false;
        }
    }
    if (strcmp(cmd, "FLUSH") == 0) {
        char err[64] = {0};
        const float secs = doc.containsKey("sec") ? doc["sec"].as<float>() : 5.0f;
        if (requestFlush(secs, err, sizeof(err))) {
            response = "{\"ok\":true,\"cmd\":\"FLUSH\"}";
            return true;
        }
        response = String("{\"ok\":false,\"cmd\":\"FLUSH\",\"err\":\"") + (err[0] ? err : "busy") + "\"}";
        return false;
    }
    if (strcmp(cmd, "TARE") == 0) {
        char err[64] = {0};
        if (requestTare(err, sizeof(err))) {
            response = "{\"ok\":true,\"cmd\":\"TARE\"}";
            return true;
        }
        response = String("{\"ok\":false,\"cmd\":\"TARE\",\"err\":\"") + (err[0] ? err : "busy") + "\"}";
        return false;
    }
    if (strcmp(cmd, "NEXT_PHASE") == 0) {
        requestNextPhase();
        response = "{\"ok\":true,\"cmd\":\"NEXT_PHASE\"}";
        return true;
    }
    if (strcmp(cmd, "OVERRIDE_PRESSURE") == 0) {
#if defined(SB_BENCH) && (SB_BENCH != 0)
        if (!doc.containsKey("bar")) {
            clearPressureOverride();
            response = "{\"ok\":true,\"override\":false}";
        } else {
            const float bar = doc["bar"].as<float>();
            setPressureOverride(bar);
            response = "{\"ok\":true,\"override\":true,\"bar\":" + String(bar, 2) + "}";
        }
        return true;
#else
        response = "{\"ok\":false,\"cmd\":\"OVERRIDE_PRESSURE\",\"err\":\"bench feature disabled\"}";
        return false;
#endif
    }
    if (strcmp(cmd, "MANUAL_POWER") == 0) {
#if defined(SB_BENCH) && (SB_BENCH != 0)
        if (!doc.containsKey("pct")) {
            clearManualPower();
            response = "{\"ok\":true,\"manual\":false}";
        } else {
            const float pct = doc["pct"].as<float>();
            setManualPower(pct);
            response = "{\"ok\":true,\"manual\":true,\"pct\":" + String(pct, 0) + "}";
        }
        return true;
#else
        response = "{\"ok\":false,\"cmd\":\"MANUAL_POWER\",\"err\":\"bench feature disabled\"}";
        return false;
#endif
    }
    if (strcmp(cmd, "CLEAR_FAULTS") == 0) {
        clearFaults();
        response = "{\"ok\":true}";
        return true;
    }
    if (strcmp(cmd, "USE") == 0 || strcmp(cmd, "LOAD_PROFILE") == 0) {
        const char* id = doc["id"] | "";
        if (id[0] && loadProfile(id)) {
            File idx = LittleFS.open("/profiles/index.tmp", "w");
            if (idx) {
                idx.printf("{\"active\":\"%s\"}", id);
                idx.close();
                if (!LittleFS.rename("/profiles/index.tmp", "/profiles/index.json")) {
                    LittleFS.remove("/profiles/index.json");
                    LittleFS.rename("/profiles/index.tmp", "/profiles/index.json");
                }
            }
            response = String("{\"ok\":true,\"cmd\":\"USE\",\"id\":\"") + id + "\"}";
            return true;
        } else {
            response = "{\"ok\":false,\"cmd\":\"USE\",\"err\":\"failed to load profile\"}";
            return false;
        }
    }
    if (strcmp(cmd, "SIM") == 0) {
        bool sim = doc["on"].as<bool>();
        setSimulationMode(sim);
        response = String("{\"ok\":true,\"cmd\":\"SIM\",\"sim\":") + (sim ? "true" : "false") + "}";
        return true;
    }

    response = "{\"ok\":false,\"err\":\"unknown command\"}";
    return false;
}

BrewController g_brew;