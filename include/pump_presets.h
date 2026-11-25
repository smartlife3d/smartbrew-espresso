// =================================================================================================
//  pump_presets.h - Vibratory pump physics + default PID gains
//  docs/PumpFlow.md
//
//  Each preset captures a pump's non-linear pressure/flow behaviour:
//    minActivationPct - duty needed just to break the solenoid free at 0 bar
//    holdNineBarPct  - duty needed to hold 9 bar (the feed-forward curve is drawn through 0 and 9)
//    maxFlowMlS      - free-flow delivery of the pump at 0 bar
//    stallBar        - pressure at which flow decays to zero
// =================================================================================================
#pragma once

#include <Arduino.h>
#include "config_schema.h"

struct PumpPreset {
    uint8_t     id;                // PumpModel
    const char* key;               // config.json token
    const char* name;              // human readable
    float       minActivationPct;  // % duty at 0 bar
    float       holdNineBarPct;    // % duty at 9 bar
    float       maxFlowMlS;        // ml/s at 0 bar
    float       stallBar;          // bar at 0 ml/s
    float       kp, ki, kd;        // default PID gains
    float       watts;
    // Shape of the P-Q curve: Q = duty * maxFlow * (1 - (P / stall)^pqExponent).
    // Every preset carries 1.35 because no pump has been measured on this rig yet - the field is
    // here so a measured curve can be entered per pump (or overridden in config as pumpPqExponent)
    // instead of the shape being hard-wired into the physics for every pump alike.
    float       pqExponent;
};

// ------------------------------------------------------------------------------------------------
// pump model presets, see docs/PumpFlow.md.
// ------------------------------------------------------------------------------------------------
static const PumpPreset SB_PUMP_PRESETS[PUMP_MODEL_COUNT] = {
    // id                      key              name                        min   9bar  maxFlow stall   kp    ki    kd   W      pq
    { PUMP_ULKA_EX5,       "ULKA_EX5",       "Ulka EX5 / EP5 (48W)",      14.0f, 36.0f, 10.8f, 15.0f, 14.0f, 3.5f, 1.2f, 48.0f, 1.35f },
    { PUMP_ULKA_EX4,       "ULKA_EX4",       "Ulka EX4 / EP4 (48W)",      16.0f, 32.0f,  7.5f, 18.5f, 12.0f, 3.0f, 1.0f, 48.0f, 1.35f },
    { PUMP_OLAB_SILENT,    "OLAB_SILENT",    "Olab Silent Green",         18.0f, 40.0f,  9.5f, 14.5f, 16.0f, 4.0f, 1.5f, 61.0f, 1.35f },
    // CUSTOM: user-supplied numbers arrive via config.json; these are conservative starting values.
    { PUMP_CUSTOM,         "CUSTOM",         "Custom / user tuned",       16.0f, 36.0f,  9.0f, 15.5f, 13.0f, 3.5f, 1.2f, 50.0f, 1.35f },
};

inline const PumpPreset& pumpPreset(uint8_t id) {
    for (uint8_t i = 0; i < PUMP_MODEL_COUNT; ++i) {
        if (SB_PUMP_PRESETS[i].id == id) return SB_PUMP_PRESETS[i];
    }
    return SB_PUMP_PRESETS[0];
}

inline const PumpPreset& pumpPresetFromKey(const char* key) {
    if (key) {
        for (uint8_t i = 0; i < PUMP_MODEL_COUNT; ++i) {
            if (strcasecmp(SB_PUMP_PRESETS[i].key, key) == 0) return SB_PUMP_PRESETS[i];
        }
    }
    return pumpPreset(g_config.cfg().pumpModel);
}