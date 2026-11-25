// =================================================================================================
//  scale_manager.h - NimBLE central scale state machine + predictive drip-compensated weight
//                    cutoff.   docs/Hardware.md and docs/PumpFlow.md
//
//  Weight cutoff (docs/PumpFlow.md)
//    W_stop  when  W_current >= W_target - clamp(dw/dt, 0.5, 6.0) * t_lag
//  where dw/dt is the OLS slope over the last config.scaleSlopeWindowMs of readings and t_lag is
//  config.scaleLagSec (learned from the measured drip when config.scaleLagAutoLearn is on).
//  The cutoff is not evaluated for the first config.scaleLockoutSec seconds of a shot, but the
//  regression window keeps filling through the lockout so the slope is primed when it lifts.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include "scale_interface.h"
#include "scale_bookoo.h"
#include "scale_timemore.h"
#include "scale_acaia.h"
#include "scale_generic.h"
#include "config_schema.h"
#include "control/scale_flow.h"

// Forward declaration of the NimBLE scan-callback shim (defined at the bottom of this header because
// it needs ScaleManager to be complete).
struct SbScaleScanCallbacks;

struct DiscoveredScale {
    char mac[18];
    char name[28];
    char link[12];
    int8_t rssi;
};

enum class ScaleState : uint8_t {
    IDLE = 0,       // nothing configured / BLE stack idle
    SCANNING,
    CONNECTING,
    CONNECTED,
    STALE,          // connected but no data for > 500 ms
};

class ScaleManager {
public:
    void begin();
    void end();                 // called before Wi-Fi re-init so NimBLE can be re-created

    // ---- scan / pair ---------------------------------------------------------------------------
    void startScan(uint8_t seconds = 8, bool discoveryOnly = false);
    void stopScan();
    bool scanning() const { return _state == ScaleState::SCANNING; }
    bool isDiscovering() const { return _discovering; }
    uint8_t discoveredCount() const { return _discoveredCount; }
    const DiscoveredScale* discoveredScales() const { return _discoveredScales; }
    // "AUTO" picks the strongest advertisement; otherwise the exact MAC.
    void pairTo(const char* macOrAuto);
    void disconnect();

    // ---- per-100 Hz tick from the control loop (Core 1, strictly non-blocking) -----------------
    // Maintains the OLS window, updates flow, and evaluates the predictive cutoff.
    // Feed the scale service. `brewing` is true while the controller owns the pump, `tailActive`
    // while the post-shot weight is still settling - the last of the coffee is still arriving, so
    // the weight flow has to stay live through it.
    void tick(uint32_t nowMs, float targetG, bool brewing, bool lockoutActive,
              bool tailActive = false);

    // ---- BLE state machine & GATT servicing (Core 0 "ble" task, may block) ---------------------
    void serviceBle(uint32_t nowMs);
    bool hasPendingBle() const {
        return _pendingConnect || _pendingReleaseBle || _pendingTeardownClient ||
               _pendingTare || _pendingDisconnect || _pendingScanReq || _pendingPairReq;
    }

    // ---- commands ------------------------------------------------------------------------------
    bool tare();
    // Clears a software tare offset after the scale has zeroed itself (late hardware tare ack).
    void dropSoftwareTare();
    void resetRegression();

    // ---- observation ---------------------------------------------------------------------------
    float  grams() const       { return _grams; }
    float  flowGs() const      { return _flowGs; }
    float  slopeGs() const     { return _slopeWin.slope(); }
    // The weight and the slope that tripped the last predictive cutoff.
    float  cutoffWeightG() const { return _cutoffWeightG; }
    float  cutoffSlopeGs() const { return _cutoffSlopeGs; }
    bool   connected() const   { return _state == ScaleState::CONNECTED; }
    ScaleState state() const   { return _state; }
    const char* stateName() const;
    const char* deviceName() const { return _name; }
    const char* mac() const     { return _mac; }
    ScaleLink link() const      { return _link; }
    int8_t  battery() const     { return _battery; }
    bool    tareActive() const  { return _tareActive; }
    // True when the last tare request was actually pushed to the scale over BLE, as opposed to a
    // local software-only offset. The pre-brew check uses this to tell a real hardware tare from
    // a best-effort local one.
    bool    tareCommandSent() const { return _tareCommandSent; }
    // True when the scale itself has reported a tared state in its telemetry since the last tare
    // request. This is the strongest evidence that the tare really landed on the hardware.
    bool    tareAcknowledged() const { return _tareAcked; }
    // The gross reading straight off the scale, before the software tare offset is applied.
    float   rawGrams() const   { return _rawGrams; }
    // The software tare offset currently in effect.
    float   tareOffsetG() const { return _tareOffsetG; }
    uint32_t tareCount() const { return _tareCount; }
    uint32_t lastUpdateMs() const { return _lastUpdateMs; }
    bool    stale() const       { return (millis() - _lastUpdateMs) > 500; }
    uint32_t updates() const    { return _updateCount; }
    uint32_t cutoffEvents() const { return _cutoffEvents; }
    bool    cutoffLatched() const  { return _cutoff; }
    bool    bleInitialized() const { return _bleInit; }

    // ---- predictive cutoff signal to the brew state machine -------------------------------------
    bool takeCutoffSignal();      // true exactly once, then self-clearing

    // ---- driver plumbing (called from the NimBLE notify callbacks) -------------------------------
    void publishReading(const ScaleReading& r);

    // ---- NimBLE callbacks -----------------------------------------------------------------------
    static void onNotifyTrampoline(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len,
                                   bool isNotify);
    static void scanCompleteCb(const NimBLEScanResults& results);
    // Public because the NimBLE scan-callback shim below has to reach it.
    static void scanResult(const NimBLEAdvertisedDevice* dev);
    void        onAdvertised(const NimBLEAdvertisedDevice* dev);
    void        onClientDisconnected();

    /// NimBLE delivers scan results and the scan end through one callback object, kept static.
    static SbScaleScanCallbacks s_scaleCallbacks;

private:
    void         ensureBleInitialized();
    void         doStartScan(uint8_t seconds, bool discoveryOnly);
    ScaleDriver* driverFor(ScaleLink l);
    bool         matchScale(const char* name, const char* uuidList, ScaleLink& outLink) const;
    void         setState(ScaleState s);
    bool         connectTo(NimBLEAddress addr, ScaleLink link, const char* name);
    void         teardownClient();

    // --- BLE radio lifecycle ---------------------------------------------------------------------
    //
    // The BLE stack and the Wi-Fi stack share one radio, and ESP-IDF refuses to run with Wi-Fi
    // modem sleep disabled while BLE is enabled (the coexistence core aborts the chip). Modem
    // sleep costs the telemetry link hundreds of milliseconds of latency, so the BLE stack is
    // only brought up when there is a scale to talk to.
    //
    //   scaleConfigured()  true  -> a scale was paired in Settings; keep the radio up and keep
    //                               looking for it until it connects.
    //                       false -> nothing is paired; the radio stays down except for a
    //                               discovery scan the operator asks for explicitly.
    bool         scaleConfigured() const;
    void         releaseBleIfIdle();
    void         rememberScale(const char* mac, const char* name, ScaleLink link, int8_t rssi);

    // --- connection state ------------------------------------------------------------------------
    ScaleState _state = ScaleState::IDLE;
    bool       _bleInit = false;
    char       _name[28] = {0};
    char       _mac[18]  = {0};
    ScaleLink  _link = ScaleLink::NONE;

    NimBLEClient* _client = nullptr;
    ScaleDriver*  _driver = nullptr;

    // Driver instances, one per family. Which one is live is given by _link.
    BookooDriver    _bookoo;
    TimemoreDriver  _timemore;
    AcaiaDriver     _acaia;
    GenericDriver   _generic;

    // --- readings --------------------------------------------------------------------------------
    volatile float _rawGrams = 0.0f;
    volatile float _tareOffsetG = 0.0f;
    volatile float _grams = 0.0f;
    volatile float _flowGs = 0.0f;
    volatile int8_t _battery = -1;
    volatile bool  _tareActive = false;
    volatile bool  _tareAcked = false;      // the scale reported a tared state itself
    volatile bool  _tareCommandSent = false; // the last tare command was written to the scale
    uint32_t _tareCount = 0;
    volatile bool  _valid = false;
    volatile uint32_t _lastUpdateMs = 0;
    uint32_t _updateCount = 0;
    uint32_t _cutoffEvents = 0;

    // --- OLS regression over a TIME window (docs/PumpFlow.md, src/control/scale_flow.h) ---------------
    // The window is config.scaleSlopeWindowMs long whatever the scale's report rate, so a 5 Hz
    // Timemore and a 20 Hz Acaia produce a slope over the same stretch of the shot.
    WeightSlope _slopeWin;
    bool     _cutoff = false;      // latched until the control loop consumes it
    // The reading and the slope at the moment the cutoff fired. The drip that follows is measured
    // against these to learn the lag (see BrewController::finalizeShotSave).
    float    _cutoffWeightG = 0.0f;
    float    _cutoffSlopeGs = 0.0f;

    bool     _brewing = false;
    bool     _lockoutActive = false;
    bool     _tailActive = false;   // the post-shot weight is still settling
    void     pushReading(float grams, uint32_t nowMs);

    // --- Reconnect & deferred BLE operations (serviced on Core 0) -------------------------------
    mutable portMUX_TYPE _readingsMux = portMUX_INITIALIZER_UNLOCKED;
    bool          _pendingConnect = false;
    bool          _pendingReleaseBle = false;
    bool          _pendingTeardownClient = false;
    volatile bool _pendingTare = false;
    volatile bool _pendingDisconnect = false;
    volatile bool _pendingScanReq = false;
    uint8_t       _pendingScanSeconds = 8;
    bool          _pendingScanDiscoveryOnly = false;
    volatile bool _pendingPairReq = false;
    char          _pendingPairMac[18] = {0};
    NimBLEAddress _pendingAddr;
    ScaleLink     _pendingLink = ScaleLink::NONE;
    uint32_t      _reconnectAfterMs = 0;

    // --- Discovery scan deadline -----------------------------------------------------------------
    uint32_t _scanDeadlineMs = 0;

    // --- Discovery buffer -----------------------------------------------------------------------
    bool            _discovering = false;
    uint8_t         _discoveredCount = 0;
    DiscoveredScale _discoveredScales[8]{};
};

extern ScaleManager g_scale;

// -------------------------------------------------------------------------------------------------
// NimBLE routes advertising reports and the end of a scan through a callback object. This shim is
// stateless and forwards straight to the singleton, which is bound in ScaleManager::begin().
// NimBLEScan::stop() does not call onScanEnd(): only a scan that runs out its duration reports here.
// -------------------------------------------------------------------------------------------------
struct SbScaleScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override { ScaleManager::scanResult(dev); }
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        (void)reason;
        ScaleManager::scanCompleteCb(results);
    }
};