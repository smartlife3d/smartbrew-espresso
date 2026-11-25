// =================================================================================================
//  config_schema.h - Persisted system configuration (config.json in LittleFS + NVS mirror)
//  docs/API.md `GET|POST /api/v1/config`
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "pins.h"

#define SB_CFG_PATH     "/config.json"
#define SB_NVS_NS       "sbcfg"
#define SB_NVS_KEY      "blob"

inline const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "PANIC_EXCEPTION";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// ------------------------------------------------------------------------------------------------
// Pump model
// ------------------------------------------------------------------------------------------------
enum PumpModel : uint8_t {
    PUMP_ULKA_EX5 = 0,
    PUMP_ULKA_EX4,
    PUMP_OLAB_SILENT,
    PUMP_CUSTOM,
    PUMP_MODEL_COUNT
};

// ------------------------------------------------------------------------------------------------
// Pressure transducer selection (determines full-scale range and the safety cut-off)
// ------------------------------------------------------------------------------------------------
enum SensorRange : uint8_t {
    SENSOR_1_2MPA = 0,   // 12.0 bar full scale
    SENSOR_1_6MPA,       // 16.0 bar full scale
    SENSOR_RANGE_COUNT
};

// ------------------------------------------------------------------------------------------------
// Mains frequency: AUTO (detect from ZC) or user-locked
// ------------------------------------------------------------------------------------------------
enum MainsMode : uint8_t {
    MAINS_AUTO = 0,
    MAINS_50HZ,
    MAINS_60HZ,
    MAINS_MODE_COUNT
};

// ------------------------------------------------------------------------------------------------
// Fault / warning bitmask reported to the UI
// ------------------------------------------------------------------------------------------------
enum FaultBits : uint32_t {
    FAULT_NONE              = 0,
    FAULT_SENSOR_OPEN       = 1UL << 0,  // < 200 mV from transducer while brewing (open wire)
    FAULT_OVER_PRESSURE     = 1UL << 1,  // sustained above maxSafetyPressureBar
    FAULT_OVER_TEMP         = 1UL << 2,
    FAULT_NO_SCALE          = 1UL << 3,  // weight cutoff requested but no scale data
    FAULT_PROFILE_MISSING   = 1UL << 4,
    FAULT_BTN_NO_ACK        = 1UL << 5,  // soft button pulsed but no ZC burst followed
    FAULT_ZC_LOST           = 1UL << 6,  // ZC pulses vanished mid-shot
    FAULT_SCALE_STALE       = 1UL << 7,
    FAULT_SENSOR_OVERRANGE  = 1UL << 8,  // transducer voltage > 3050 mV (short to 5V or ADC saturation)
    FAULT_MAX_TIME          = 1UL << 9,  // absolute maximum brew time exceeded (120 s)
    FAULT_TOPOLOGY          = 1UL << 10  // permanent AC detected in IDLE / wiring check
};

// ------------------------------------------------------------------------------------------------
// One single struct holding everything that survives a reboot. Deliberately POD so it can be
// memcpy'd into NVS as a binary blob - keeps forward/backward compatibility simple: we always
// write a magic+length header and fall back to defaults on mismatch.
// ------------------------------------------------------------------------------------------------
struct SystemConfig {
    // --- identity / versioning -----------------------------------------------------------------
    uint32_t magic;                    // SB_CFG_MAGIC
    uint16_t version;                  // SB_CFG_VERSION
    uint16_t size;                     // sizeof(SystemConfig), guards struct growth

    // --- hardware selection --------------------------------------------------------------------
    uint8_t pumpModel;                 // PumpModel
    uint8_t sensorRange;               // SensorRange
    uint8_t mainsMode;                 // MainsMode
    uint8_t useAds1115;                // 0 = auto-detect (probe ADS1115, fall back to ADC1), 1 = force ADS1115, 2 = force internal ADC

    // --- pressure conditioning -----------------------------------------------------------------
    float   sensorZeroMv;              // ADC-node mV that represents 0 bar (default 333.33 = 500 mV / 1.5)
    float   sensorFsMv;                // ADC-node mV at full scale (default 3000 = 4500 mV / 1.5)
    float   emaAlpha;                  // EMA smoothing factor applied at the 100 Hz decimated rate
    float   maxSafetyPressureBar;      // hard cut-off; 0 = derive from sensorRange
    float   minSensorMv;               // below this the transducer is considered open (docs/Architecture.md)

    // --- control --------------------------------------------------------------------------------
    float   kp, ki, kd;                // PID gains (0 = use pump preset defaults)
    float   loopDt;                    // nominal control period in seconds (0.01 = 100 Hz)
    uint8_t headspaceAntiWindupBar;    // freeze integral below this pressure (x10 for fractional bar)

    // --- virtual button ------------------------------------------------------------------------
    uint16_t buttonPulseMs;            // momentary press duration (default 180)
    uint16_t buttonAckTimeoutMs;       // how long to wait for a ZC burst after a soft start (600)
    uint16_t buttonMinRetriggerMs;     // dead time between two pulses (250)

    // --- scale ----------------------------------------------------------------------------------
    float   scaleLagSec;               // predictive cutoff latency budget (default 0.6 s)
    char    scaleMac[18];              // "AA:BB:CC:DD:EE:FF" or "" (unpaired)
    uint8_t scaleLockoutSec;           // ignore weight for the first N seconds (default 3 s, docs/PumpFlow.md)

    // --- web ------------------------------------------------------------------------------------
    char    otaGitHubUrl[128];         // "" = disabled
    char    mcuHostname[32];           // mDNS / network hostname (default "smartbrew")

    // --- behavioural ----------------------------------------------------------------------------
    uint16_t preInfusionProtectSec;    // ignore weight-cutoff during the first N seconds of a shot
    uint8_t  verboseLogging;           // 0 = quiet, 1 = info, 2 = debug
    uint8_t  ledBrightness;            // status LED brightness (0..100 %, default 10)
    uint16_t topologyTimeoutSec;       // continuous IDLE AC duration before FAULT_TOPOLOGY (default 120 s)

    // --- pump flow model (version 6, docs/PumpFlow.md) --------------------------
    // Appended at the end on purpose: a version 5 NVS blob is an exact prefix of this struct, so
    // ConfigStore::load() can carry every older field across and only default these.
    float    complianceMlPerBar;       // hydraulic compliance for the puck-flow estimate (0 = off, default 1.5)
    float    pumpPqExponent;           // P-Q curve shape override (0 = use the pump preset's)
    uint16_t scaleSlopeWindowMs;       // time span of the weight-flow regression (default 1500 ms)
    uint8_t  scaleLagAutoLearn;        // 1 = learn scaleLagSec from the measured drip after each shot

    // --- pump flow calibration (version 7) -------------------------------------------------------
    float    pumpFlowScale;            // multiplies the preset's free flow; 1.0 = factory model (0.5..1.5)
    uint8_t  pumpFlowAutoCal;          // 1 = calibrate pumpFlowScale against the scale after each shot
    // --- pressure loop (version 8) --------------------------------------------------------------
    uint8_t  pressureCtrlMode;         // 0 = classic FF+PID, 1 = adaptive (default, pid_controller.h)
};

#define SB_CFG_MAGIC   0x53424331UL   // 'S','B','C','1'
#define SB_CFG_VERSION 8

// The size of the struct at each older version that only appended fields to the one before it.
// ConfigStore::load() carries such a blob over in full and defaults only the newer fields.
// Add a line here whenever a version appends fields; a version that reorders or removes fields
// must NOT be listed, so it falls back to the field-by-field migration.
#define SB_CFG_APPEND_ONLY_SIZES                                   \
    { 5, offsetof(SystemConfig, complianceMlPerBar) },             \
    { 6, offsetof(SystemConfig, pumpFlowScale) },                  \
    { 7, offsetof(SystemConfig, pressureCtrlMode) }

// Sensor full-scale pressure per SensorRange
static constexpr float SB_PRESSURE_FS[SENSOR_RANGE_COUNT] = {12.0f, 16.0f};
// Default safety cut-off per SensorRange (docs/Architecture.md)
static constexpr float SB_PRESSURE_SAFETY[SENSOR_RANGE_COUNT] = {12.5f, 15.0f};

// ------------------------------------------------------------------------------------------------
// Compile-time defaults
// ------------------------------------------------------------------------------------------------
inline SystemConfig sbConfigDefaults() {
    SystemConfig c{};
    c.magic      = SB_CFG_MAGIC;
    c.version    = SB_CFG_VERSION;
    c.size       = sizeof(SystemConfig);
    c.pumpModel  = PUMP_ULKA_EX5;
    c.sensorRange= SENSOR_1_2MPA;
    c.mainsMode  = MAINS_AUTO;
    c.useAds1115 = 0;              // auto

    // The 10k/20k divider scales 0.5-4.5 V down to 0.333-3.000 V (docs/Hardware.md)
    c.sensorZeroMv  = 500.0f / 1.5f;   // 333.33 mV  == 0 bar
    c.sensorFsMv    = 4500.0f / 1.5f;  // 3000.00 mV  == full scale
    c.emaAlpha      = 0.25f;
    c.minSensorMv   = 200.0f / 1.5f;   // open-wire trip (ADC node equivalent of 200 mV sensor)

    c.kp = 0.0f; c.ki = 0.0f; c.kd = 0.0f;      // 0 => inherit the pump preset
    c.loopDt = 1.0f / (float)CONTROL_HZ;
    c.headspaceAntiWindupBar = 15;             // 1.5 bar, stored x10

    c.buttonPulseMs       = BTN_PULSE_MS;
    c.buttonAckTimeoutMs  = 600;
    c.buttonMinRetriggerMs= 250;

    c.scaleLagSec      = 0.6f;
    c.scaleLockoutSec  = 3;
    strlcpy(c.scaleMac, "", sizeof(c.scaleMac));

    c.otaGitHubUrl[0] = '\0';
    strlcpy(c.mcuHostname, "smartbrew", sizeof(c.mcuHostname));

    c.preInfusionProtectSec = 3;
    c.verboseLogging        = 1;
    c.ledBrightness         = 10;
    c.topologyTimeoutSec    = 120;

    c.complianceMlPerBar    = 1.5f;
    c.pumpPqExponent        = 0.0f;    // the pump preset's own exponent
    c.scaleSlopeWindowMs    = 1500;
    c.scaleLagAutoLearn     = 1;

    c.pumpFlowScale         = 1.0f;
    c.pumpFlowAutoCal       = 1;
    c.pressureCtrlMode      = 1;
    return c;
}

// ------------------------------------------------------------------------------------------------
// Tiny JSON <-> struct binding helpers (ArduinoJson 6). Kept here so config.h does not need to
// include ArduinoJson.
// ------------------------------------------------------------------------------------------------
class ConfigStore {
public:
    // Load from NVS first (survives a LittleFS wipe), then overlay /config.json if present.
    void begin();
    void load();
    bool save();                      // writes both NVS blob and /config.json

    SystemConfig&       cfg()       { return _cfg; }
    const SystemConfig& cfg() const { return _cfg; }

    float pressureFsBar()    const { return SB_PRESSURE_FS[_cfg.sensorRange]; }
    float safetyPressureBar() const {
        return _cfg.maxSafetyPressureBar > 0.05f ? _cfg.maxSafetyPressureBar
                                                 : SB_PRESSURE_SAFETY[_cfg.sensorRange];
    }
    uint8_t headspaceBar()   const { return _cfg.headspaceAntiWindupBar; }

    void applyDefaults();

private:
    bool saveUnlocked();
    SystemConfig _cfg{};
    // save() is called from several tasks (telemetry, web handlers, console, zero calibration).
    // They all write the same /config.json.tmp and rename it, so saves are serialised. Never call
    // save() from the control task: it blocks on this mutex and on flash.
    SemaphoreHandle_t _saveMutex = nullptr;
};

// Convenience accessor
extern ConfigStore g_config;