// =================================================================================================
//  config_store.cpp - SystemConfig persistence
//
//  Two copies on purpose:
//    NVS blob        - survives a LittleFS erase (a botched OTA filesystem update), tiny and atomic.
//    /config.json    - human readable, editable over the REST API and by hand over USB.
//  Load order is NVS first, then JSON overlays it field by field, so a hand-edited JSON wins.
// =================================================================================================

#include "config_schema.h"
#include "pump_presets.h"
#include "logger.h"
#include "control/scale_flow.h"
#include "control/flow_calibration.h"

#include <stddef.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <FS.h>

namespace {
Preferences nvs;
bool       g_nvsTried = false;

// True when `old` is a blob of an older version that only appended fields (see
// SB_CFG_APPEND_ONLY_SIZES), so its bytes are an exact prefix of the current struct.
bool appendOnlyPrefix(const SystemConfig& old, size_t blobLen) {
    struct VersionSize { uint16_t version; size_t size; };
    static const VersionSize kSizes[] = { SB_CFG_APPEND_ONLY_SIZES };
    for (const VersionSize& v : kSizes) {
        if (old.version == v.version) {
            return old.size == v.size && blobLen == v.size && v.size < sizeof(SystemConfig);
        }
    }
    return false;
}
}  // namespace

void ConfigStore::applyDefaults() {
    _cfg = sbConfigDefaults();
}

void ConfigStore::begin() {
    if (!_saveMutex) _saveMutex = xSemaphoreCreateMutex();
    nvs.begin(SB_NVS_NS, false);
    g_nvsTried = true;
    load();
}

// -------------------------------------------------------------------------------------------------
void ConfigStore::load() {
    applyDefaults();

    // ---- 1. NVS binary blob -------------------------------------------------------------------
    bool fromNvs = false;
    if (g_nvsTried) {
        const size_t len = nvs.getBytesLength(SB_NVS_KEY);
        const size_t headerSize = sizeof(uint32_t) + 2 * sizeof(uint16_t);
        if (len >= headerSize) {
            SystemConfig oldCfg{};
            const size_t copyLen = (len < sizeof(SystemConfig)) ? len : sizeof(SystemConfig);
            nvs.getBytes(SB_NVS_KEY, &oldCfg, copyLen);
            if (oldCfg.magic != SB_CFG_MAGIC) {
                SB_LOGW("NVS config magic mismatch - using defaults");
                applyDefaults();
            } else if (oldCfg.version != SB_CFG_VERSION && appendOnlyPrefix(oldCfg, len)) {
                // That version only appended fields, so the old blob is an exact prefix of the
                // current struct: keep every field it carries and default only the newer ones.
                applyDefaults();
                memcpy(&_cfg, &oldCfg, oldCfg.size);
                SB_LOGI("NVS config v%u -> v%u: kept all settings, newer fields defaulted",
                        (unsigned)oldCfg.version, (unsigned)SB_CFG_VERSION);
                _cfg.version = SB_CFG_VERSION;
                _cfg.size    = sizeof(SystemConfig);
                fromNvs = true;
            } else if (oldCfg.size != sizeof(SystemConfig) || oldCfg.version != SB_CFG_VERSION) {
                // Struct grew or shrank between firmware versions: keep the known-good defaults for
                // anything the old blob did not carry.
                SB_LOGW("NVS config size %u / version %u != current %u / %u - migrating",
                        oldCfg.size, oldCfg.version, (unsigned)sizeof(SystemConfig), SB_CFG_VERSION);
                const uint8_t  oldPump   = oldCfg.pumpModel;
                const uint8_t  oldSensor = oldCfg.sensorRange;
                const float    oldZero   = oldCfg.sensorZeroMv;
                char           macCopy[sizeof(oldCfg.scaleMac)];
                strlcpy(macCopy, oldCfg.scaleMac, sizeof(macCopy));
                const float    oldLag    = oldCfg.scaleLagSec;
                const float    oldDt     = oldCfg.loopDt;

                applyDefaults();
                _cfg.version = SB_CFG_VERSION;
                _cfg.pumpModel = oldPump < PUMP_MODEL_COUNT ? oldPump : _cfg.pumpModel;
                _cfg.sensorRange = oldSensor < SENSOR_RANGE_COUNT ? oldSensor : _cfg.sensorRange;
                _cfg.sensorZeroMv = oldZero;
                _cfg.scaleLagSec = oldLag;
                if (oldDt >= 0.001f && oldDt <= 0.1f) _cfg.loopDt = oldDt;
                strlcpy(_cfg.scaleMac, macCopy, sizeof(_cfg.scaleMac));
                _cfg.size = sizeof(SystemConfig);
                _cfg.magic = SB_CFG_MAGIC;
                fromNvs = true;
            } else {
                memcpy(&_cfg, &oldCfg, sizeof(SystemConfig));
                fromNvs = true;
            }
        }
    }

    // ---- 2. /config.json overlay ---------------------------------------------------------------
    bool fromJson = false;
    if (LittleFS.exists(SB_CFG_PATH)) {
        File f = LittleFS.open(SB_CFG_PATH, "r");
        if (f) {
            DynamicJsonDocument doc(2048);
            const DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (err) {
                SB_LOGE("config.json is malformed (%s) - keeping the previous values", err.c_str());
            } else {
                fromJson = true;
                if (doc["pump"]) {
                    _cfg.pumpModel = pumpPresetFromKey(doc["pump"] | "").id;
                }
                if (doc["sensor_mpa"] == "1.2")  _cfg.sensorRange = SENSOR_1_2MPA;
                if (doc["sensor_mpa"] == "1.6")  _cfg.sensorRange = SENSOR_1_6MPA;
                if (doc["mains_mode"].is<int>()) _cfg.mainsMode = (uint8_t)constrain(doc["mains_mode"] | 0, 0, 2);
                if (doc["force_ads"].is<int>())  _cfg.useAds1115 = (uint8_t)constrain(doc["force_ads"] | 0, 0, 2);
                if (doc.containsKey("sensorZeroMv")) _cfg.sensorZeroMv = doc["sensorZeroMv"].as<float>();
                if (doc.containsKey("sensorFsMv"))   _cfg.sensorFsMv   = doc["sensorFsMv"].as<float>();
                if (doc.containsKey("emaAlpha"))     _cfg.emaAlpha     = doc["emaAlpha"].as<float>();
                if (doc.containsKey("minSensorMv"))  _cfg.minSensorMv  = doc["minSensorMv"].as<float>();
                if (doc.containsKey("maxSafetyPressureBar"))
                    _cfg.maxSafetyPressureBar = doc["maxSafetyPressureBar"].as<float>();
                if (doc.containsKey("kp")) _cfg.kp = doc["kp"].as<float>();
                if (doc.containsKey("ki")) _cfg.ki = doc["ki"].as<float>();
                if (doc.containsKey("kd")) _cfg.kd = doc["kd"].as<float>();
                if (doc.containsKey("headspaceBar")) {
                    float hb = doc["headspaceBar"] | 1.5f;
                    _cfg.headspaceAntiWindupBar = (uint8_t)constrain((int)(hb * 10.0f + 0.5f), 0, 99);
                }
                if (doc.containsKey("buttonPulseMs"))
                    _cfg.buttonPulseMs = doc["buttonPulseMs"].as<uint16_t>();
                if (doc.containsKey("buttonAckTimeoutMs"))
                    _cfg.buttonAckTimeoutMs = doc["buttonAckTimeoutMs"].as<uint16_t>();
                if (doc.containsKey("buttonMinRetriggerMs"))
                    _cfg.buttonMinRetriggerMs = doc["buttonMinRetriggerMs"].as<uint16_t>();
                if (doc.containsKey("scaleLagSec"))
                    _cfg.scaleLagSec = doc["scaleLagSec"].as<float>();
                if (doc.containsKey("scaleMac"))
                    strlcpy(_cfg.scaleMac, doc["scaleMac"] | "AUTO", sizeof(_cfg.scaleMac));
                if (doc.containsKey("scaleLockoutSec"))
                    _cfg.scaleLockoutSec = doc["scaleLockoutSec"].as<uint8_t>();
                if (doc.containsKey("preInfusionProtectSec"))
                    _cfg.preInfusionProtectSec = doc["preInfusionProtectSec"].as<uint16_t>();
                if (doc.containsKey("otaGitHubUrl"))
                    strlcpy(_cfg.otaGitHubUrl, doc["otaGitHubUrl"] | "", sizeof(_cfg.otaGitHubUrl));
                if (doc.containsKey("mcuHostname")) {
                    const char* hn = doc["mcuHostname"] | "smartbrew";
                    if (hn && hn[0]) strlcpy(_cfg.mcuHostname, hn, sizeof(_cfg.mcuHostname));
                }
                if (doc.containsKey("verboseLogging"))
                    _cfg.verboseLogging = doc["verboseLogging"].as<uint8_t>();
                if (doc.containsKey("ledBrightness"))
                    _cfg.ledBrightness = (uint8_t)constrain(doc["ledBrightness"].as<int>(), 0, 100);
                if (doc.containsKey("topologyTimeoutSec"))
                    _cfg.topologyTimeoutSec = doc["topologyTimeoutSec"].as<uint16_t>();
                if (doc.containsKey("complianceMlPerBar"))
                    _cfg.complianceMlPerBar = doc["complianceMlPerBar"].as<float>();
                if (doc.containsKey("pumpPqExponent"))
                    _cfg.pumpPqExponent = doc["pumpPqExponent"].as<float>();
                if (doc.containsKey("scaleSlopeWindowMs"))
                    _cfg.scaleSlopeWindowMs = doc["scaleSlopeWindowMs"].as<uint16_t>();
                if (doc.containsKey("scaleLagAutoLearn"))
                    _cfg.scaleLagAutoLearn = doc["scaleLagAutoLearn"].as<uint8_t>() ? 1 : 0;
                if (doc.containsKey("pumpFlowScale"))
                    _cfg.pumpFlowScale = doc["pumpFlowScale"].as<float>();
                if (doc.containsKey("pumpFlowAutoCal"))
                    _cfg.pumpFlowAutoCal = doc["pumpFlowAutoCal"].as<uint8_t>() ? 1 : 0;
                if (doc.containsKey("pressureCtrlMode"))
                    _cfg.pressureCtrlMode = doc["pressureCtrlMode"].as<uint8_t>() ? 1 : 0;
            }
        }
    }

    // Range validation
    _cfg.emaAlpha = constrain(_cfg.emaAlpha, 0.01f, 1.0f);
    _cfg.buttonPulseMs = constrain(_cfg.buttonPulseMs, (uint16_t)20, (uint16_t)2000);
    _cfg.buttonAckTimeoutMs = constrain(_cfg.buttonAckTimeoutMs, (uint16_t)200, (uint16_t)3000);
    _cfg.buttonMinRetriggerMs = constrain(_cfg.buttonMinRetriggerMs, (uint16_t)100, (uint16_t)2000);
    _cfg.scaleLagSec = constrain(_cfg.scaleLagSec, 0.0f, 3.0f);
    _cfg.scaleLockoutSec = constrain(_cfg.scaleLockoutSec, (uint8_t)0, (uint8_t)30);
    _cfg.preInfusionProtectSec = constrain(_cfg.preInfusionProtectSec, (uint16_t)0, (uint16_t)30);
    if (_cfg.ledBrightness > 100) _cfg.ledBrightness = 10;
    if (_cfg.topologyTimeoutSec < 10 || _cfg.topologyTimeoutSec > 3600) _cfg.topologyTimeoutSec = 120;
    _cfg.minSensorMv = constrain(_cfg.minSensorMv, 50.0f, 1000.0f);
    _cfg.kp = constrain(_cfg.kp, 0.0f, 50.0f);
    _cfg.ki = constrain(_cfg.ki, 0.0f, 50.0f);
    _cfg.kd = constrain(_cfg.kd, 0.0f, 50.0f);
    if (!(_cfg.complianceMlPerBar >= 0.0f && _cfg.complianceMlPerBar <= 10.0f))
        _cfg.complianceMlPerBar = 1.5f;
    // 0 means "use the preset"; anything else must be a believable curve shape.
    if (!(_cfg.pumpPqExponent == 0.0f ||
          (_cfg.pumpPqExponent >= 0.5f && _cfg.pumpPqExponent <= 4.0f)))
        _cfg.pumpPqExponent = 0.0f;
    _cfg.scaleSlopeWindowMs = constrain(_cfg.scaleSlopeWindowMs, SB_SLOPE_WINDOW_MIN_MS,
                                        SB_SLOPE_WINDOW_MAX_MS);
    _cfg.scaleLagAutoLearn = _cfg.scaleLagAutoLearn ? 1 : 0;
    _cfg.pumpFlowScale = flowScaleClamp(_cfg.pumpFlowScale);
    _cfg.pumpFlowAutoCal = _cfg.pumpFlowAutoCal ? 1 : 0;
    _cfg.pressureCtrlMode = (_cfg.pressureCtrlMode > 1) ? 1 : _cfg.pressureCtrlMode;

    // Self-heal: make sure the derived limits always agree with the sensor range.
    if (_cfg.maxSafetyPressureBar <= 0.05f) {
        _cfg.maxSafetyPressureBar = SB_PRESSURE_SAFETY[_cfg.sensorRange];
    } else {
        _cfg.maxSafetyPressureBar = constrain(_cfg.maxSafetyPressureBar, 1.0f, 20.0f);
    }
    if (_cfg.loopDt <= 0.0f) _cfg.loopDt = 1.0f / (float)CONTROL_HZ;

    bool migratedHostname = false;
    if (_cfg.mcuHostname[0] == '\0' || strcmp(_cfg.mcuHostname, "brew-mcu") == 0) {
        strlcpy(_cfg.mcuHostname, "smartbrew", sizeof(_cfg.mcuHostname));
        migratedHostname = true;
    }

    SB_LOGI("Config loaded (%s%s) pump=%s sensor=%.0fMPa mains=%s zero=%.1fmV",
            fromNvs ? "nvs" : "defaults", fromJson ? "+json" : "",
            pumpPreset(_cfg.pumpModel).name, _cfg.sensorRange == SENSOR_1_2MPA ? 1.2 : 1.6,
            _cfg.mainsMode == MAINS_AUTO ? "auto" : (_cfg.mainsMode == MAINS_50HZ ? "50" : "60"),
            _cfg.sensorZeroMv);

    if ((!fromNvs && !fromJson) || migratedHostname) {
        save();
    }
}

// -------------------------------------------------------------------------------------------------
// Serialised wrapper: every task that saves takes the same mutex, so two saves can never write
// /config.json.tmp at the same time or rename a half-written file into place.
bool ConfigStore::save() {
    if (_saveMutex) xSemaphoreTake(_saveMutex, portMAX_DELAY);
    const bool ok = saveUnlocked();
    if (_saveMutex) xSemaphoreGive(_saveMutex);
    return ok;
}

bool ConfigStore::saveUnlocked() {
    _cfg.magic   = SB_CFG_MAGIC;
    _cfg.version = SB_CFG_VERSION;
    _cfg.size    = sizeof(SystemConfig);

    bool ok = true;

    // ---- NVS ----------------------------------------------------------------------------------
    if (g_nvsTried) {
        const size_t w = nvs.putBytes(SB_NVS_KEY, &_cfg, sizeof(SystemConfig));
        if (w != sizeof(SystemConfig)) {
            SB_LOGE("NVS config write short (%u/%u)", (unsigned)w, (unsigned)sizeof(SystemConfig));
            ok = false;
        }
    }

    // ---- JSON (atomic write-tmp-then-rename) ---------------------------------------------------
    const char* tmpPath = "/config.json.tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        SB_LOGE("Cannot write %s", tmpPath);
        return false;
    }
    char buf[1600];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"pump\": \"%s\",\n"
        "  \"sensor_mpa\": \"%s\",\n"
        "  \"mains_mode\": %d,\n"
        "  \"force_ads\": %d,\n"
        "  \"sensorZeroMv\": %.2f,\n"
        "  \"sensorFsMv\": %.2f,\n"
        "  \"emaAlpha\": %.3f,\n"
        "  \"minSensorMv\": %.2f,\n"
        "  \"maxSafetyPressureBar\": %.2f,\n"
        "  \"kp\": %.3f,\n  \"ki\": %.3f,\n  \"kd\": %.3f,\n"
        "  \"headspaceBar\": %.2f,\n"
        "  \"buttonPulseMs\": %u,\n"
        "  \"buttonAckTimeoutMs\": %u,\n"
        "  \"buttonMinRetriggerMs\": %u,\n"
        "  \"scaleLagSec\": %.3f,\n"
        "  \"scaleMac\": \"%s\",\n"
        "  \"scaleLockoutSec\": %d,\n"
        "  \"preInfusionProtectSec\": %d,\n"
        "  \"otaGitHubUrl\": \"%s\",\n"
        "  \"mcuHostname\": \"%s\",\n"
        "  \"verboseLogging\": %d,\n"
        "  \"ledBrightness\": %u,\n"
        "  \"topologyTimeoutSec\": %u,\n"
        "  \"complianceMlPerBar\": %.2f,\n"
        "  \"pumpPqExponent\": %.3f,\n"
        "  \"scaleSlopeWindowMs\": %u,\n"
        "  \"scaleLagAutoLearn\": %u,\n"
        "  \"pumpFlowScale\": %.3f,\n"
        "  \"pumpFlowAutoCal\": %u,\n"
        "  \"pressureCtrlMode\": %u\n"
        "}\n",
        pumpPreset(_cfg.pumpModel).key,
        _cfg.sensorRange == SENSOR_1_2MPA ? "1.2" : "1.6",
        _cfg.mainsMode, _cfg.useAds1115,
        _cfg.sensorZeroMv, _cfg.sensorFsMv, _cfg.emaAlpha, _cfg.minSensorMv,
        _cfg.maxSafetyPressureBar,
        _cfg.kp, _cfg.ki, _cfg.kd, _cfg.headspaceAntiWindupBar * 0.1f,
        _cfg.buttonPulseMs, _cfg.buttonAckTimeoutMs, _cfg.buttonMinRetriggerMs,
        _cfg.scaleLagSec, _cfg.scaleMac, _cfg.scaleLockoutSec,
        _cfg.preInfusionProtectSec, _cfg.otaGitHubUrl,
        _cfg.mcuHostname[0] ? _cfg.mcuHostname : "smartbrew",
        _cfg.verboseLogging,
        _cfg.ledBrightness, _cfg.topologyTimeoutSec,
        _cfg.complianceMlPerBar, _cfg.pumpPqExponent,
        (unsigned)_cfg.scaleSlopeWindowMs, (unsigned)_cfg.scaleLagAutoLearn,
        _cfg.pumpFlowScale, (unsigned)_cfg.pumpFlowAutoCal, (unsigned)_cfg.pressureCtrlMode);
    f.print(buf);
    f.close();

    if (!LittleFS.rename(tmpPath, SB_CFG_PATH)) {
        LittleFS.remove(SB_CFG_PATH);
        if (!LittleFS.rename(tmpPath, SB_CFG_PATH)) {
            SB_LOGE("Failed to rename %s to %s", tmpPath, SB_CFG_PATH);
            ok = false;
        }
    }

    SB_LOGD("Config saved to NVS + %s", SB_CFG_PATH);
    return ok;
}
// Convenience accessor singleton (declared in config_schema.h)
ConfigStore g_config;
