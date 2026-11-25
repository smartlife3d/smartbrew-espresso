// =================================================================================================
//  hal_pressure.cpp - see hal_pressure.h
// =================================================================================================

#include "hal_pressure.h"
#include "logger.h"
#include "pins.h"

#include <Wire.h>
#include <esp_task_wdt.h>

// -------------------------------------------------------------------------------------------------
// ADS1115 register map (only what we need)
// -------------------------------------------------------------------------------------------------
namespace {
constexpr uint8_t ADS_REG_CONV   = 0x00;   // conversion result (16-bit, MSB first)
constexpr uint8_t ADS_REG_CONFIG = 0x01;
constexpr uint8_t ADS_REG_LOHI   = 0x02;   // Lo/Hi threshold
constexpr uint8_t ADS_REG_STATUS = 0x03;   // bit0 OS = single conversion active
}  // namespace

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------
void PressureSensor::begin() {
    if (!_i2cMutex) {
        _i2cMutex = xSemaphoreCreateMutex();
    }
    const SystemConfig& c = g_config.cfg();

    // Pressure source selection:
    //   0 = auto-detect  - probe the ADS1115, fall back to the internal ADC1 if it is not present
    //   1 = force ADS1115 - do not fall back
    //   2 = force ADC1   - never touch I2C; the real-time loop stays free of I2C transactions
    // The default (0) used to behave exactly like 2, which made the "Auto-Detect" label a lie and
    // left the 16-bit ADC unused on installations that had one fitted.
    if (c.useAds1115 != 2) {
        probeAds1115();
    }

    if (_adsOk) {
        configureAdsForFastReads();
        _source = PressureSource::ADS1115;
        SB_LOGI("Pressure source: ADS1115 @0x%02X (continuous mode, GAIN_ONE +/-4.096V)",
                _adsAddr);
        // Firmware before 1.4.1 scaled ADS1115 codes at 0.25 mV instead of 0.125 mV, so a zero
        // calibration taken with it stored roughly twice the real node voltage (~667 mV instead of
        // ~333 mV). With the corrected scale such a zero makes the pressure read low: up to about
        // 1.7 bar below 2 bar, 0.4 bar at 9 bar. A real 0.5 V transducer never sits that high at
        // rest, so flag it.
        if (c.sensorZeroMv > 500.0f) {
            SB_LOGW("Stored pressure zero is %.0f mV - above any 0.5 V transducer at rest. If it was "
                    "calibrated with an older firmware, run the zero calibration again (Settings -> "
                    "Tools) with the line at atmospheric pressure.", c.sensorZeroMv);
        }
    } else {
        analogSetPinAttenuation(PIN_ADC_FALLBACK, ADC_11db);   // full 0..3100 mV range on the S3
        _source = PressureSource::ADC1_INTERNAL;
        SB_LOGI("Pressure source: internal ADC, oversampled");
    }

    // Seed the filter with the real resting pressure so the UI does not ramp from 0 on boot.
    for (int i = 0; i < 16; ++i) {
        serviceFast();
        delay(2);
    }
    _filteredBar  = mvToBar(_lastMv);
    _filterPrimed = true;
    SB_LOGI("Pressure baseline: %.3f V -> %.2f bar (zero=%.1f mV, fs=%.1f mV)",
            _lastMv / 1000.0f, _filteredBar, c.sensorZeroMv, c.sensorFsMv);
}

const char* PressureSensor::sourceName() const {
    switch (_source) {
        case PressureSource::ADS1115:      return "ADS1115";
        case PressureSource::ADC1_INTERNAL:return "INTERNAL";
        case PressureSource::SIMULATED:    return "SIM";
        default:                           return "NONE";
    }
}

// -------------------------------------------------------------------------------------------------
// ADS1115 discovery
// -------------------------------------------------------------------------------------------------
namespace {
// Reads one 16-bit ADS1115 register. Returns false on any bus error.
bool adsReadReg(uint8_t addr, uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)2) != 2) return false;
    out = ((uint16_t)Wire.read() << 8) | Wire.read();
    return true;
}
}  // namespace

bool PressureSensor::probeAds1115() {
    // Probe at 100 kHz. The breakout's own pull-ups (often 10 kOhm) and the wire length decide how
    // fast the bus can go; 400 kHz failed on a real build where 100 kHz worked.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
    Wire.setTimeOut(50);
    // Try every legal ADS1115 address so the user can straddle ADDR high or low.
    // ADDR pin -> GND = 0x48, VDD = 0x49, SDA = 0x4A, SCL = 0x4B
    const uint8_t candidates[4] = {0x48, 0x49, 0x4A, 0x4B};
    for (uint8_t addr : candidates) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() != 0) continue;

        // Something answers. Make sure it is an ADS1x15 and not another 0x48 part (TMP102,
        // PCF8591, LM75). Its threshold registers reset to Lo = 0x8000, Hi = 0x7FFF and this
        // firmware never writes them. The config register's top bit is no proof: it reads 0 while
        // the chip converts, i.e. in continuous mode, which a breakout that kept power across an
        // ESP32 restart is still in.
        uint16_t cfg = 0, lo = 0, hi = 0;
        if (!adsReadReg(addr, ADS_REG_CONFIG, cfg) || !adsReadReg(addr, 0x02, lo) ||
            !adsReadReg(addr, 0x03, hi)) {
            SB_LOGW("I2C device at 0x%02X answers but its registers cannot be read", addr);
            continue;
        }
        const bool thresholdsMatch = (lo == 0x8000 && hi == 0x7FFF);
        const bool idleConfig      = (cfg & 0x8000) != 0;
        if (!thresholdsMatch && !idleConfig) {
            SB_LOGW("I2C device at 0x%02X is not an ADS1115 (CONFIG=0x%04X LO=0x%04X HI=0x%04X)",
                    addr, cfg, lo, hi);
            continue;
        }
        _adsOk   = true;
        _adsAddr = addr;

        // Step up to 400 kHz only if a read works there; otherwise stay at 100 kHz. One 2-byte
        // conversion read takes ~0.4 ms at 100 kHz, which the 100 Hz control loop absorbs easily.
        Wire.setClock(400000);
        uint16_t check = 0;
        const bool fast = adsReadReg(addr, ADS_REG_CONFIG, check) && check == cfg;
        if (!fast) Wire.setClock(100000);
        SB_LOGI("ADS1115 probe OK at 0x%02X (CONFIG=0x%04X), bus %s", addr, cfg,
                fast ? "400 kHz" : "100 kHz (400 kHz read failed - check pull-ups / wire length)");
        return true;
    }
    SB_LOGW("ADS1115 not found on I2C (SDA GPIO %d, SCL GPIO %d) - falling back to the internal "
            "ADC. Run `i2c` on the console to check the bus.", PIN_I2C_SDA, PIN_I2C_SCL);
    return false;
}
void PressureSensor::configureAdsForFastReads() {
    // Continuous mode, AIN0 vs GND, GAIN_ONE (+/-4.096 V), DR=111, COMP_QUE = 00.
    const uint16_t cfg = 0x42E0;
    _adsConfig = cfg;

    Wire.beginTransmission(_adsAddr);
    Wire.write(ADS_REG_CONFIG);
    Wire.write((uint8_t)(cfg >> 8));
    Wire.write((uint8_t)(cfg & 0xFF));
    if (Wire.endTransmission() != 0) {
        SB_LOGE("ADS1115 config write failed");
    }
}

// -------------------------------------------------------------------------------------------------
// Fast read path (called at 100 Hz from control task or on-demand)
// -------------------------------------------------------------------------------------------------
void PressureSensor::serviceFast() {
    if (_simEnabled) {
        const float jitter = (rand() % 2001 - 1000) / 1000.0f * _simNoise;   // +/- _simNoise mV
        const float bar    = _simBar;
        _lastMv     = barToMv(bar) + jitter;
        _lastRawBar = mvToBar(_lastMv);
        return;
    }

    switch (_source) {
        case PressureSource::ADS1115: {
            if (_i2cMutex && xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                Wire.beginTransmission(_adsAddr);
                Wire.write(ADS_REG_CONV);
                if (Wire.endTransmission(false) == 0 && Wire.requestFrom(_adsAddr, (uint8_t)2) == 2) {
                    const int16_t code = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
                    _lastMv = (float)code * _adsMvPerCode;   // GAIN_ONE: 4096 mV / 32768 = 0.125 mV/LSB
                }
                xSemaphoreGive(_i2cMutex);
            }
            break;
        }
        case PressureSource::ADC1_INTERNAL: {
            // Oversample 32x (~300 us total) across ~6 PWM/ripple periods to integrate out carrier ripple
            uint32_t sum = 0;
            for (int i = 0; i < 32; ++i) {
                sum += analogReadMilliVolts(PIN_ADC_FALLBACK);
            }
            const float rawMv = (float)sum * (1.0f / 32.0f);
            if (!_filterPrimed) {
                _lastMv = rawMv;
            } else {
                // Alpha = 0.15 gives ~65 ms time constant at 100 Hz, removing remaining ripple
                _lastMv = 0.15f * rawMv + 0.85f * _lastMv;
            }
            break;
        }
        default:
            return;
    }
    _lastRawBar = mvToBar(_lastMv);
}

float PressureSensor::readAdsRawMv()   { serviceFast(); return _lastMv; }
float PressureSensor::readAdc1Mv()     { serviceFast(); return _lastMv; }

// -------------------------------------------------------------------------------------------------
// 100 Hz decimated + EMA-filtered sample (docs/Architecture.md)
// -------------------------------------------------------------------------------------------------
float PressureSensor::sampleBar() {
    serviceFast();

    const SystemConfig& c = g_config.cfg();
    const float alpha = c.emaAlpha;
    if (!_filterPrimed) {
        _filteredBar  = _lastRawBar;
        _filterPrimed = true;
    } else {
        _filteredBar = alpha * _lastRawBar + (1.0f - alpha) * _filteredBar;
        if (_filteredBar < 0.0f) _filteredBar = 0.0f;
    }
    return _filteredBar;
}

// -------------------------------------------------------------------------------------------------
// Conversion helpers
// -------------------------------------------------------------------------------------------------
float PressureSensor::mvToBar(float nodeMv) const {
    const SystemConfig& c = g_config.cfg();
    const float span = c.sensorFsMv - c.sensorZeroMv;
    if (fabsf(span) < 1.0f) return 0.0f;
    const float bar = (nodeMv - c.sensorZeroMv) / span * SB_PRESSURE_FS[c.sensorRange];
    return bar > 0.0f ? bar : 0.0f;
}

float PressureSensor::barToMv(float bar) const {
    const SystemConfig& c = g_config.cfg();
    const float span = c.sensorFsMv - c.sensorZeroMv;
    const float b   = bar < 0.0f ? 0.0f : (bar > SB_PRESSURE_FS[c.sensorRange]
                                            ? SB_PRESSURE_FS[c.sensorRange] : bar);
    return c.sensorZeroMv + (b / SB_PRESSURE_FS[c.sensorRange]) * span;
}

// -------------------------------------------------------------------------------------------------
// Simulator
// -------------------------------------------------------------------------------------------------
void PressureSensor::setSimulator(bool enable) {
    _simEnabled = enable;
    if (enable) {
        _simNoise = 25.0f;    // +/- 25 mV of synthetic breadboard-style noise
        _simBar   = _filteredBar;
        SB_LOGI("Pressure SIMULATOR engaged (software-only testing). Real sensor ignored.");
    } else {
        SB_LOGI("Pressure SIMULATOR disengaged, hardware source = %s", sourceName());
        _filterPrimed = false;
    }
}

// -------------------------------------------------------------------------------------------------
// Zero-pressure calibration (docs/API.md `POST /api/v1/calibrate/zero`)
// -------------------------------------------------------------------------------------------------
bool PressureSensor::calibrateZero(uint8_t n) {
    if (_source == PressureSource::NONE) {
        SB_LOGE("Zero calibration impossible: no pressure source");
        _calState.store(CalState::FAILED, std::memory_order_relaxed);
        strlcpy(_calMsg, "no pressure source", sizeof(_calMsg));
        return false;
    }
    if (n < 8)   n = 8;
    if (n > 200) n = 200;

    _calState.store(CalState::RUNNING, std::memory_order_relaxed);
    strlcpy(_calMsg, "calibrating", sizeof(_calMsg));

    if (_simEnabled) {
        SB_LOGW("Zero calibration requested while the simulator is engaged - using live reading anyway.");
    }

    double sum = 0.0;
    uint32_t got = 0;
    for (uint8_t i = 0; i < n; ++i) {
        esp_task_wdt_reset();
        serviceFast();
        sum += _lastMv;
        got++;
        delay(5);
    }
    if (!got) {
        _calState.store(CalState::FAILED, std::memory_order_relaxed);
        strlcpy(_calMsg, "no samples obtained", sizeof(_calMsg));
        return false;
    }

    const float avgMv = (float)(sum / (double)got);
    g_config.cfg().sensorZeroMv = avgMv;
    _filterPrimed = false;
    g_config.save();

    snprintf(_calMsg, sizeof(_calMsg), "%.1f mV", avgMv);
    _calState.store(CalState::SUCCESS, std::memory_order_relaxed);

    SB_LOGI("Zero-pressure calibration: averaged %u samples -> %.2f mV (was %.2f mV) = 0.00 bar",
            got, avgMv, 0.0f);
    return true;
}

bool PressureSensor::startZeroCalibration(uint8_t n) {
    if (_calState.load(std::memory_order_relaxed) == CalState::RUNNING) {
        SB_LOGW("Zero calibration already running");
        return false;
    }
    _calState.store(CalState::RUNNING, std::memory_order_relaxed);
    strlcpy(_calMsg, "running", sizeof(_calMsg));

    BaseType_t ret = xTaskCreatePinnedToCore([](void* arg) {
        uint32_t count = (uint32_t)(uintptr_t)arg;
        esp_task_wdt_add(NULL);
        g_pressure.calibrateZero((uint8_t)count);
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
    }, "cal_zero", 4096, (void*)(uintptr_t)n, 1, nullptr, 0);

    if (ret != pdPASS) {
        _calState.store(CalState::FAILED, std::memory_order_relaxed);
        strlcpy(_calMsg, "task spawn failed", sizeof(_calMsg));
        return false;
    }
    return true;
}

PressureSensor g_pressure;