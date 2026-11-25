// =================================================================================================
//  brew_controller.h - Single-owner brew controller with FreeRTOS command/event queues,
//                      lock-free telemetry snapshots, and deterministic safety interlocks.
//                      docs/Architecture.md
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include "config_schema.h"
#include "profile_types.h"
#include "telemetry_wire.h"
#include "hal/hal_psm.h"
#include "control/weight_settle.h"
#include "pump_physics.h"
#include "control/flow_calibration.h"

enum class BrewState : uint8_t {
    IDLE = 0,
    PREPARING,     // pre-brew gate: hardware check, scale tare verification, settle delay
    STARTING,      // soft button pulsed, waiting for the machine to route pump power
    BREWING,
    FLUSHING,      // grouphead flush: fixed TRIAC duty, no profile, no PID, no weight cutoff
    STOPPING,      // duty=0, stop pulse queued, waiting for AC power to drop
    FAULT
};

const char* brewStateName(BrewState s);

enum class PumpOwner : uint8_t {
    PASSIVE = 0,   // Machine owns pump (IDLE / 100% passthrough)
    PROFILE,       // Controller owns pump (STARTING / BREWING)
    STOPPING,      // Controller cutting water, waiting for AC release
    FAULT          // Gate held off due to fault condition
};

const char* pumpOwnerName(PumpOwner o);

struct Telemetry {
    uint32_t tMs        = 0;
    BrewState state     = BrewState::IDLE;
    PumpOwner pumpOwner = PumpOwner::PASSIVE;
    PsmMode   psmMode   = PsmMode::PASSIVE;
    uint8_t  phaseIdx   = 0;
    char     phaseName[SB_PHASE_NAME_LEN] = "-";
    char     profileId[40] = "default";
    char     profileName[SB_PROFILE_NAME_LEN] = "Default";
    float    pBar       = 0.0f;
    float    pTarget    = 0.0f;
    float    flowVirt   = 0.0f;
    uint8_t  psmPct     = 0;
    float    weightG    = 0.0f;
    float    flowWeight = 0.0f;
    bool     scaleConn  = false;
    int8_t   scaleBat   = -1;
    uint8_t  acHz       = 50;
    uint8_t  phasePct   = 0;
    uint16_t doseG      = 0;     // dose x10
    uint32_t flushRemMs = 0;     // remaining flush duration (ms) while state == FLUSHING
    // True while the machine is driving the pump and no pressure transducer is detected. This is a
    // notice, not a fault: no transducer fitted is a valid configuration and the ESP32 must not cut
    // the machine's own boiler fill. It is debounced on the firmware side so it can be shown once.
    bool     sensorOpenWarn = false;
};

// -------------------------------------------------------------------------------------------------
// Cross-core Command & Event structures
// -------------------------------------------------------------------------------------------------
enum class CmdType : uint8_t {
    START,
    STOP,
    FLUSH,
    TARE,
    NEXT_PHASE,
    OVERRIDE_PRESSURE,
    CLEAR_OVERRIDE,
    MANUAL_POWER,
    CLEAR_MANUAL,
    LOAD_PROFILE,
    SET_SIM,
    RESET,
    CLEAR_FAULTS
};

struct BrewCommand {
    CmdType type = CmdType::START;
    float   value = 0.0f;
    char    strArg[40] = {0};

    BrewCommand() = default;
    explicit BrewCommand(CmdType t, float v = 0.0f) : type(t), value(v) {}
};

enum class EventType : uint8_t {
    STATE_CHANGED,
    SHOT_COMPLETED,
    FAULT_SET
};

struct BrewEvent {
    EventType type;
    BrewState state;
    uint32_t  faults;
    uint32_t  shotDurationMs;
    float     peakBar;
    float     doseG;
    char      reason[40];
    // SHOT_COMPLETED only: the drip lag learned from this shot, or < 0 when none was learned. The
    // control core only calculates it; Core 0 applies it and saves the config (flash I/O).
    float     learnedLagSec = -1.0f;
    // SHOT_COMPLETED only: the pump flow calibration learned from this shot, or < 0 when none.
    float     learnedFlowScale = -1.0f;
};

// -------------------------------------------------------------------------------------------------
class BrewController {
public:
    void begin();
    void reset();

    // ---- non-blocking command surface (thread-safe, callable from any core/task) ---------------
    bool requestStart(char* errBuf = nullptr, size_t errBufLen = 0);
    bool requestStop(const char* reason = "user stop");
    // Grouphead flush: wake the machine, then hold a fixed low duty for `seconds`. No profile,
    // no PID and no weight cutoff - it is a rinse, not a shot.
    bool requestFlush(float seconds, char* errBuf = nullptr, size_t errBufLen = 0);
    bool requestTare(char* errBuf = nullptr, size_t errBufLen = 0);
    bool requestNextPhase();
    bool setPressureOverride(float bar);
    bool clearPressureOverride();
    bool setManualPower(float pct);
    bool clearManualPower();
    bool loadProfile(const char* id);
    void clearFaults();

    // ---- JSON command dispatcher (thread-safe, queues commands) --------------------------------
    bool handleJsonCommand(const char* json, String& response);

    // ---- 100 Hz real-time control tick (Core 1) ------------------------------------------------
    void tick(uint32_t nowMs);

    // ---- telemetry (lock-free double-buffer / seqlock, callable from any core) -----------------
    void snapshot(Telemetry& out) const;
    // Binary telemetry frame (see include/telemetry_wire.h). Returns false if the buffer is too
    // small. Replaces the JSON object that used to go out at 20 Hz.
    // Builds one binary telemetry frame. Returns false on failure; outLen receives the frame
    // length, which varies because the three strings are stored length-prefixed.
    bool telemetryBin(uint8_t* out, size_t cap, size_t* outLen);

    // ---- event queue (Core 0 task polls and handles history save / broadcast) ------------------
    bool pollEvent(BrewEvent& evt);

    // ---- observation ----------------------------------------------------------------------------
    BrewState state() const      { return _state.load(std::memory_order_relaxed); }
    PumpOwner pumpOwner() const;
    uint32_t  faults() const     { return _faults; }
    void      setFault(uint32_t bit, const char* why);
    uint32_t  shotCount() const  { return _shotCount; }
    uint32_t  startMs() const    { return _shotStartMs; }
    uint32_t  stopMs() const     { return _shotStopMs; }
    float     peakBar() const    { return _peakBar; }
    bool      overrideActive() const { return _overrideActive; }
    float     overrideBar() const    { return _overrideBar; }
    bool      simWeightOverride() const { return _simWeightOverride; }
    void      setSimWeight(float g)     { _simWeightOverride = true; _simWeightG = g; }
    void      clearSimWeight()          { _simWeightOverride = false; _simWeightG = 0.0f; }
    bool      simulationMode() const    { return _simMode.load(std::memory_order_acquire); }
    void      setSimulationMode(bool enable);

    // Interlock queries
    bool sensorOpen() const    { return (_faults & FAULT_SENSOR_OPEN) != 0; }
    bool overPressure() const  { return (_faults & FAULT_OVER_PRESSURE) != 0; }

    // True while the machine is driving the pump with no pressure transducer detected. A notice,
    // not a fault - the ESP32 does not cut the machine's own pump in this state.
    bool sensorOpenWarnActive() const { return _sensorOpenWarnActive; }

    // True while the post-shot weight is still settling - the last of the coffee is still draining
    // out of the grouphead, so the cup ends up heavier than the reading that triggered the cutoff.
    // A client must keep showing the live weight (and the highest value it has seen) until this
    // goes false, otherwise it displays a yield that is too low.
    bool weightSettling() const { return _settle.active(); }
    // The highest weight seen during the settling window - the real yield in the cup.
    float settledWeightG() const { return _settle.finalWeightG(); }

    uint32_t loopUsMin() const { return _loopUsMin == 0xFFFFFFFF ? 0 : _loopUsMin; }
    uint32_t loopUsMax() const { return _loopUsMax; }
    // Timestamps (micros) of the stages of the last tick: start, commands, psm/button, pressure,
    // scale, interlocks, state machine. The control task logs them when a tick runs long.
    uint32_t stageUs(int i) const { return _stageUs[i]; }
    float    loopUsAvg() const { return _loopUsAvg; }

private:
    void enterState(BrewState s, const char* why);
    void processCommands(uint32_t now);   // `now` = the calling tick's clock, never re-read

    // ---- post-shot weight settling ---------------------------------------------------------------
    // After the pump stops, coffee keeps dripping out of the grouphead for a moment, so the cup
    // ends up heavier than the reading that triggered the cutoff. These open and service the
    // window in which the controller keeps watching the scale, and report it on the wire so every
    // client shows the real yield in the cup rather than the cutoff value.
    void openWeightSettle(uint32_t now);
    void serviceWeightSettle(uint32_t now, float weightG);
    void closeWeightSettle(const char* why);

    // Reports a finished shot. This is held back until the post-shot weight tail has run out, so
    // the brew ledger and the clients are told the yield that actually ended up in the cup.
    void finalizeShotSave();

    // ---- pre-brew gate ------------------------------------------------------------------------
    // A brew does not begin the instant the operator asks for it. Before the machine's brew button
    // is pulsed, the controller verifies that the hardware the shot depends on is actually working:
    // the power stage is armed and its zero-cross path has a clean history, the pressure transducer
    // returns a usable reading (the PID flies on it), the virtual button relay is armed, and the
    // scale - if one is connected - has been tared AND the tare confirmed by a fresh reading.
    //
    // Only when every check passes does the shot begin, and it begins PREPARE_SETTLE_MS later so
    // the scale has settled at zero and the display shows the pending start before water moves.
    bool runPreBrewChecks(uint32_t now, char* fail, size_t failLen);
    bool beginPreBrew(uint32_t now);
    void servicePreBrew(uint32_t now);
    void abortPreBrew(const char* why, bool alreadyLogged = false);
    void launchBrew(uint32_t now);
    // Shared "the shot starts here" bookkeeping for the simulated and the real path.
    void beginShotRun(uint32_t now, bool simulated);

    // Cut the gate LOW while the machine drives the pump itself (passthrough) because a genuine
    // safety condition was measured. Held until the machine releases the AC or the operator
    // clears the fault. Never used for "no transducer fitted" - see engagePassthroughHold().
    void engagePassthroughHold(uint32_t faultBit, const char* why);

    // Written by Core 1 control task, observed atomically by other tasks
    std::atomic<BrewState> _state{BrewState::IDLE};
    uint32_t               _faults = FAULT_NONE;

    uint32_t _bootMs      = 0;
    uint32_t _shotStartMs = 0;
    uint32_t _shotStopMs  = 0;
    uint32_t _lastTickMs  = 0;
    uint32_t _lastTickUs  = 0;
    uint32_t _loopUsMin   = 0xFFFFFFFF;
    uint32_t _loopUsMax   = 0;
    volatile uint32_t _stageUs[7] = {0};
    float    _loopUsAvg   = 10000.0f;
    uint32_t _shotCount   = 0;
    float    _peakBar     = 0.0f;
    bool     _peakLogged  = false;
    uint32_t _historyDecimate = 0;

    // soft-start & stop tracking
    uint32_t _startRequestMs = 0;
    uint32_t _stopRequestMs  = 0;
    bool     _stopPulseRetried = false;
    uint32_t _acLostSinceMs = 0;     // start of the current mains gap while the pump runs (0 = none)
    // Why the shot was stopped. Captured when the controller enters STOPPING so the final summary
    // can report the reason together with the dose and the duration.
    char     _stopReason[40] = {0};

    // ---- pre-brew gate --------------------------------------------------------------------------
    bool     _prepActive        = false;  // the gate is running
    bool     _prepTareIssued    = false;  // a tare was requested and is being verified
    bool     _prepTareVerified  = false;  // the tare was confirmed by a fresh reading
    uint8_t  _prepTareAttempts  = 0;      // how many tares have been issued (1 or 2)
    uint32_t _prepStartMs       = 0;      // when the gate opened
    uint32_t _prepTareMs        = 0;      // when the current tare attempt was issued
    uint32_t _prepTareDeadlineMs= 0;      // when the current tare attempt gives up
    uint32_t _prepUpdatesAtTare = 0;      // scale update counter captured at tare time
    uint32_t _prepReadyMs       = 0;      // when every check finally passed
    uint32_t _prepLaunchAtMs    = 0;      // _prepReadyMs + the settle delay
    // A faulty pressure transducer is confirmed over a short window rather than on a single
    // sample, so a noisy ADC reading cannot block a brew on its own.
    uint32_t _prepSensorFaultSinceMs = 0;
    char     _prepSensorFaultReason[80] = {0};

    // ---- post-shot weight settling ---------------------------------------------------------------
    // The settling rules themselves live in WeightSettle (src/control/weight_settle.h) so they can
    // be covered by the host regression tests.
    WeightSettle _settle;
    // The control tick's own clock. Every time comparison in the settling window uses this one
    // source, so the window can never be handed a timestamp from a different clock.
    uint32_t _nowMs = 0;
    // The yield that ended up in the cup: the highest weight the scale showed while the post-shot
    // tail was running. It is what the brew ledger records.
    float _settledYieldG = 0.0f;
    // The gauges the post-shot tail records.
    //
    // Once the pump stops, the line pressure falls away and the flow reads zero. Recording that
    // collapse would trail a tail of dying gauges behind the shot, so the tail records the gauges
    // as they were when the pump stopped. The weight is untouched: the last of the coffee is still
    // arriving, and the curve has to end at the settled yield.
    //
    // The rule that drives them lives in ShotTail (src/control/shot_tail.h) so it can be covered by
    // the host regression tests.
    float _tailHoldPBar     = 0.0f;
    float _tailHoldFlowVirt = 0.0f;

    // ---- drip-lag learning (docs/PumpFlow.md) ------------------------------
    // Set when the predictive weight cutoff ended the shot; the drip that follows is measured
    // against the cutoff point once the settling window has closed.
    bool               _stoppedByWeightCutoff = false;
    // How the settling window ended. Only SETTLED and LIFTED give a trustworthy final yield.
    WeightSettleResult _settleOutcome = WeightSettleResult::OPEN;

    // ---- puck-flow estimate (F-5) ----------------------------------------------------------------
    // The pressure rate that the hydraulic compliance correction subtracts from the pump flow.
    PuckFlowRate _flowRate;
    // Pump model calibration against the scale (src/control/flow_calibration.h).
    FlowCalibration _flowCal;
    // A shot that has ended but whose ledger entry is still waiting for the tail to run out. The
    // completion is only reported once the yield is known, so the ledger holds the real yield and
    // not the reading that merely tripped the cutoff.
    bool     _shotPendingSave  = false;
    uint32_t _pendingShotDurMs = 0;
    float    _pendingShotPeakBar = 0.0f;
    // Profile activation deferred until the machine is idle again.
    //
    // The phase table lives in ProfileEngine and is read by the control task every 10 ms, so it
    // must only ever be swapped by the control task, and only while no shot is running. A request
    // that arrives mid-shot is parked here and applied the next time the machine returns to IDLE.
    char _pendingProfileId[40] = {0};

    // grouphead flush
    bool     _flushPending    = false;
    bool     _flushingSession = false;
    uint32_t _flushDurationMs = 0;
    uint32_t _flushUntilMs    = 0;
    uint32_t _flushStartMs    = 0;
    bool     _flushStopRequested = false;
    float    _flushDutyPct    = 45.0f;   // low, steady duty - enough to rinse, not to extract

    // interlock timers
    uint32_t _overPressureSinceMs = 0;
    uint32_t _sensorOpenSinceMs   = 0;
    uint32_t _overRangeSinceMs    = 0;
    bool     _idleAcActive        = false;
    uint32_t _idleAcSinceMs       = 0;
    uint32_t _idleStartMs         = 0;

    // Passthrough safety hold: the ESP32 is not driving the pump (the machine is), but a genuine
    // over-pressure or a saturated sensor node forced the gate LOW. Latched so the pump cannot
    // chatter on/off while the condition flickers; released when the machine drops the AC.
    bool     _passthroughHold     = false;
    uint32_t _passthroughHoldFault = FAULT_NONE;

    // Passthrough open-wire notice.
    //
    // The notice used to be guarded by a single boolean that was cleared by any one good reading.
    // An unconnected transducer leaves the ADC node floating, and a floating input crosses the
    // open-wire threshold many times a second - so the guard was reset almost immediately and the
    // warning turned into a log flood. The notice is now rate-limited by time and only clears after
    // the signal has been good for a while.
    bool     _sensorOpenWarnActive = false;   // notice currently active (telemetry)
    bool     _openWireWarnFired    = false;   // the log line has been emitted for this episode
    uint32_t _lastOpenWireWarnMs   = 0;       // when the log line was last emitted
    uint32_t _openWireClearMs      = 0;       // when the good-signal debounce started

    // manual overrides
    bool  _overrideActive = false;
    float _overrideBar    = 0.0f;
    float _filteredFlowVirt = 0.0f;

    // simulator injection
    bool  _simWeightOverride = false;
    float _simWeightG        = 0.0f;
    std::atomic<bool> _simMode{false};
    float _simP              = 0.0f;
    float _simF              = 0.0f;
    float _simW              = 0.0f;
    float _simWf             = 0.0f;
    float _simR              = 0.11f;
    float _simV              = 0.0f;

    // FreeRTOS Queues
    QueueHandle_t _cmdQueue = nullptr;
    QueueHandle_t _eventQueue = nullptr;

    // Seqlock double-buffer for telemetry
    mutable std::atomic<uint32_t> _telemetrySeq{0};
    uint16_t _frameSeq{0};   // sequence counter of the binary telemetry frames
    Telemetry _telemetryBuf[2]{};
};

extern BrewController g_brew;