// =================================================================================================
//  hal_pressure.h - Pressure acquisition with internal-ADC1 and optional ADS1115 support,
//                    100 Hz decimation and EMA filtering.  docs/Hardware.md and docs/Architecture.md.
//
//  Signal chain (docs/Hardware.md):
//      transducer 0.5 - 4.5 V  ->[ R_TOP 10k / R_BOT 20k, 1/1.5 ]  ->  0.333 - 3.000 V
//      -> ADS1115 AIN0 (GAIN_ONE, +/-4.096 V FSR)  and/or  -> GPIO1 ADC1_CH0
//
//  Conversion (docs/Architecture.md):
//      P_bar = max(0, (V_sensor_mV - V_zero_mV) / (V_fs_mV - V_zero_mV) * P_fs)
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config_schema.h"

enum class PressureSource : uint8_t {
    NONE = 0,
    ADS1115,
    ADC1_INTERNAL,
    SIMULATED
};

enum class CalState : uint8_t {
    IDLE = 0,
    RUNNING,
    SUCCESS,
    FAILED
};

class PressureSensor {
public:
    void begin();

    // Called from the 100 Hz control task (Core 1). For ADS1115 this reads the conversion register;
    // for ADC1 it performs oversampled analogReadMilliVolts.
    void serviceFast();

    // Called from the 100 Hz control loop. Decimates everything accumulated since the previous
    // call, converts to bar and applies the EMA (docs/Architecture.md). Returns the filtered bar value.
    float sampleBar();

    // Latest filtered value without advancing the filter (for UI / telemetry).
    float lastBar() const   { return _filteredBar; }
    float rawBar() const     { return _lastRawBar; }
    float lastMv() const     { return _lastMv; }       // ADC-node millivolts
    uint32_t fresh() const   { return _rawSampleCount; }

    PressureSource source() const { return _source; }
    const char*    sourceName() const;
    bool           adsPresent() const { return _source == PressureSource::ADS1115; }
    bool           isOverRange() const { return !_simEnabled && _lastMv > 3050.0f; }
    bool           isOpenWire() const  { return !_simEnabled && _lastMv < g_config.cfg().minSensorMv; }

    // ---- software-only test injection (docs/Testing.md, Stage 3) --------------------------
    void setSimulator(bool enable);
    bool simulator() const  { return _simEnabled; }
    void setSimBar(float bar) { _simBar = bar; }   // drives the "measured" reading

    // ---- calibration --------------------------------------------------------------------------
    // Averages `n` samples taken at atmospheric pressure and stores the node voltage as zero bar.
    bool calibrateZero(uint8_t n = 60);
    bool startZeroCalibration(uint8_t n = 60);
    bool isCalibrating() const { return _calState.load(std::memory_order_relaxed) == CalState::RUNNING; }
    CalState calState() const { return _calState.load(std::memory_order_relaxed); }
    const char* calMessage() const { return _calMsg; }

    // Convert a raw ADC reading to bar using the live config.
    float mvToBar(float nodeMv) const;
    float barToMv(float bar) const;

private:
    bool probeAds1115();
    void configureAdsForFastReads();
    float readAdsRawMv();
    float readAdc1Mv();

    PressureSource _source = PressureSource::NONE;

    // --- ADS1115 --------------------------------------------------------------------------------
    bool     _adsOk        = false;
    uint8_t  _adsAddr      = 0x48;
    uint16_t _adsConfig    = 0x42E0;   // MUX=100(AIN0-GND), PGA=001(+/-4.096V), MODE=0(continuous), DR=111
    // GAIN_ONE (PGA=001): +/-4.096 V. The signed 16-bit result has 32768 codes per side, so one
    // code is 4096 mV / 32768 = 0.125 mV (ADS1115 datasheet, FSR table). Dividing the full
    // 8.192 V span by 32768 instead would double every reading.
    float    _adsMvPerCode = 4096.0f / 32768.0f;   // 0.125 mV/LSB

    // --- decimation accumulator ----------------------------------------------------------------
    double   _accumMv      = 0.0;
    uint32_t _rawSampleCount = 0;
    float    _lastRawBar   = 0.0f;
    float    _lastMv       = 0.0f;

    // --- filtering -----------------------------------------------------------------------------
    float    _filteredBar  = 0.0f;
    bool     _filterPrimed = false;

    // --- simulator -----------------------------------------------------------------------------
    volatile bool _simEnabled = false;
    volatile float _simBar    = 0.0f;
    volatile float _simNoise  = 0.0f;

    uint32_t _sampleWindowRaw = 0;   // diagnostics

    // --- calibration & concurrency -------------------------------------------------------------
    std::atomic<CalState> _calState{CalState::IDLE};
    char                  _calMsg[64] = {0};
    SemaphoreHandle_t     _i2cMutex = nullptr;
};

extern PressureSensor g_pressure;