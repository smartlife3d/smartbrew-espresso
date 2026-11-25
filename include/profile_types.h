// =================================================================================================
//  profile_types.h - Profile / Phase / Trigger / ShotSample data structures
//  docs/Profiles.md
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <stdio.h>
#include <stddef.h>

#define SB_MAX_PHASES        10
#define SB_MAX_TRIGGERS      6
#define SB_PROFILE_NAME_LEN  40
#define SB_PHASE_NAME_LEN    28
#define SB_HISTORY_SAMPLES   1800   // 1800 * 10 Hz = 180 s of shot at full resolution
#define SB_HISTORY_ON_SHOT   900    // 90 s

// ------------------------------------------------------------------------------------------------
// Phase control mode
// ------------------------------------------------------------------------------------------------
enum class PhaseMode : uint8_t {
    PRESSURE = 0,   // closed-loop: track startVal -> endVal bar
    POWER,          // open-loop: drive startVal -> endVal duty % directly
    PAUSE,          // zero power, let pressure decay
    FLOW            // closed-loop: track startVal -> endVal ml/s flow
};

enum class Curve : uint8_t {
    LINEAR = 0,
    EASE_IN,
    EASE_OUT,
    EASE_IN_OUT,
    INSTANT,
    CONSTANT
};

enum class TriggerType : uint8_t {
    PRESSURE_GE = 0,
    PRESSURE_LE,
    WEIGHT_GE,
    WEIGHT_LE,
    TIME_SEC,
    FLOW_GE,
    FLOW_LE
};

enum class TriggerLogic : uint8_t {
    OR = 0,
    AND
};

struct Trigger {
    TriggerType type = TriggerType::TIME_SEC;
    float       value = 0.0f;
    bool        armed = true;    // false at phase entry for falling-edge triggers until signal > value
    bool        fired = false;   // latched once the armed trigger condition is met
};

struct Transition {
    float          durationSec = 5.0f;
    TriggerLogic   logic = TriggerLogic::OR;
    Trigger        triggers[SB_MAX_TRIGGERS];
    uint8_t        triggerCount = 0;
};

struct Phase {
    char      name[SB_PHASE_NAME_LEN] = {0};
    PhaseMode mode   = PhaseMode::PRESSURE;
    Curve     curve  = Curve::LINEAR;
    float     startVal = 0.0f;
    float     endVal   = 0.0f;
    uint8_t   maxPsmPct = 100;      // per-phase power ceiling
    float     flowLimit = 0.0f;     // pressure phase: pump flow ceiling, ml/s (0 = none)
    float     pressureLimit = 0.0f; // flow phase: pressure ceiling, bar (0 = none)
    Transition transition{};
};

struct Profile {
    char     id[40]   = {0};
    char     name[SB_PROFILE_NAME_LEN] = {0};
    char     author[32] = {0};
    float    targetWeightG     = 36.0f;
    float    maxTotalTimeSec   = 60.0f;
    uint8_t  phaseCount        = 0;
    Phase    phases[SB_MAX_PHASES]{};
};

// ------------------------------------------------------------------------------------------------
// One decoded telemetry row captured at 10 Hz during a shot
// ------------------------------------------------------------------------------------------------
struct ShotSample {
    uint32_t tMs;
    float    pBar;
    float    pTarget;
    float    flowVirt;
    uint8_t  psmPct;
    float    weightG;
    float    flowWeight;
    uint8_t  phaseIdx;
};

// ------------------------------------------------------------------------------------------------
// Transition evaluator - AND / OR exit conditions (docs/Profiles.md)
// ------------------------------------------------------------------------------------------------
// isSimulation: phases run their programmed duration and ignore physical triggers.
bool evaluateTransition(const Transition& tr, float pressure, float weight,
                        float flowVirt, float elapsedSec, bool isSimulation = false);
float applyCurve(Curve c, float t);           // t clamped to [0,1]; returns the shaped 0..1 value
const char* curveName(Curve c);
const char* triggerName(TriggerType t);
const char* modeName(PhaseMode m);
bool parseTriggerType(const char* s, TriggerType& out);
bool parseCurve(const char* s, Curve& out);

// ------------------------------------------------------------------------------------------------
// Ledger dose
//
// The reading that trips the cutoff is not the yield: the last of the coffee is still arriving in
// the cup for a second or two after the pump stops. The brew ledger therefore records the settled
// yield once it is known, and falls back to the cutoff reading until then.
//
// This lives in the shared header (and is header-inline) so the host regression tests can exercise
// it without an ESP32 toolchain.
// ------------------------------------------------------------------------------------------------
inline float ledgerDoseG(float cutoffG, float settledG) {
    return (settledG > cutoffG) ? settledG : cutoffG;
}

// ------------------------------------------------------------------------------------------------
// Transition reason
//
// A phase change is never reported as an unexplained event: the trigger that fired is named, with
// the measured value and the threshold that was crossed. This lives in the shared header (and is
// header-inline) so the host regression tests can exercise it without an ESP32 toolchain.
//
// The reason is written into `out` (always NUL-terminated). `outLen` should be at least 96.
// ------------------------------------------------------------------------------------------------
inline void sbCopyText(char* dst, size_t dstLen, const char* src) {
    if (!dst || dstLen == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstLen && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

inline void describeTransitionReason(const Transition& tr, float pressure, float weight,
                                     float flowVirt, float elapsedSec,
                                     char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';

    // A phase with no triggers runs for its fixed duration and then ends.
    if (tr.triggerCount == 0) {
        const float dur = tr.durationSec > 0.0f ? tr.durationSec : 4.0f;
        if (elapsedSec >= dur) {
            snprintf(out, outLen, "duration %.1f s elapsed", dur);
        }
        return;
    }

    // The phase timeout always wins: it is the backstop that ended the phase.
    if (tr.durationSec > 0.0f && elapsedSec >= tr.durationSec) {
        snprintf(out, outLen, "phase timeout after %.1f s", tr.durationSec);
        return;
    }

    // Name every trigger that is satisfied, joined with the phase's AND / OR logic.
    char buf[96];
    size_t used = 0;
    buf[0] = '\0';

    for (uint8_t i = 0; i < tr.triggerCount; ++i) {
        const Trigger& t = tr.triggers[i];
        bool hit = false;
        char one[48];
        one[0] = '\0';

        switch (t.type) {
            case TriggerType::PRESSURE_GE:
                hit = pressure >= t.value;
                snprintf(one, sizeof(one), "pressure %.2f bar >= %.2f bar", pressure, t.value);
                break;
            case TriggerType::PRESSURE_LE:
                hit = pressure <= t.value;
                snprintf(one, sizeof(one), "pressure %.2f bar <= %.2f bar", pressure, t.value);
                break;
            case TriggerType::WEIGHT_GE:
                hit = weight >= t.value;
                snprintf(one, sizeof(one), "weight %.1f g >= %.1f g", weight, t.value);
                break;
            case TriggerType::WEIGHT_LE:
                hit = weight <= t.value;
                snprintf(one, sizeof(one), "weight %.1f g <= %.1f g", weight, t.value);
                break;
            case TriggerType::TIME_SEC:
                hit = elapsedSec >= t.value;
                snprintf(one, sizeof(one), "phase time %.1f s >= %.1f s", elapsedSec, t.value);
                break;
            case TriggerType::FLOW_GE:
                hit = flowVirt >= t.value;
                snprintf(one, sizeof(one), "flow %.2f ml/s >= %.2f ml/s", flowVirt, t.value);
                break;
            case TriggerType::FLOW_LE:
                hit = flowVirt <= t.value;
                snprintf(one, sizeof(one), "flow %.2f ml/s <= %.2f ml/s", flowVirt, t.value);
                break;
        }
        if (!hit) continue;

        const char* sep = (used > 0)
            ? ((tr.logic == TriggerLogic::AND) ? " AND " : " OR ")
            : "";
        const int n = snprintf(buf + used, sizeof(buf) - used, "%s%s", sep, one);
        if (n < 0 || (size_t)n >= sizeof(buf) - used) break;   // do not let the list run past the end
        used += (size_t)n;
    }

    if (used > 0) {
        sbCopyText(out, outLen, buf);
    } else {
        sbCopyText(out, outLen, "the transition condition was met");
    }
}