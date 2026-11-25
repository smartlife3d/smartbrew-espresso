// =================================================================================================
//  profile_engine.cpp - see profile_engine.h
// =================================================================================================

#include "profile_engine.h"
#include "logger.h"
#include "config_schema.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>

#define PROFILE_DIR   "/profiles"
#define PROFILE_INDEX "/profiles/index.json"

// -------------------------------------------------------------------------------------------------
// Shared helpers declared in profile_types.h
// -------------------------------------------------------------------------------------------------
const char* curveName(Curve c) {
    switch (c) {
        case Curve::LINEAR:      return "linear";
        case Curve::EASE_IN:     return "ease_in";
        case Curve::EASE_OUT:    return "ease_out";
        case Curve::EASE_IN_OUT: return "ease_in_out";
        case Curve::INSTANT:     return "instant";
        case Curve::CONSTANT:    return "constant";
    }
    return "linear";
}

const char* triggerName(TriggerType t) {
    switch (t) {
        case TriggerType::PRESSURE_GE: return "pressure_ge";
        case TriggerType::PRESSURE_LE: return "pressure_le";
        case TriggerType::WEIGHT_GE:   return "weight_ge";
        case TriggerType::WEIGHT_LE:   return "weight_le";
        case TriggerType::TIME_SEC:    return "time_sec";
        case TriggerType::FLOW_GE:     return "flow_ge";
        case TriggerType::FLOW_LE:     return "flow_le";
    }
    return "time_sec";
}

const char* modeName(PhaseMode m) {
    switch (m) {
        case PhaseMode::PRESSURE: return "pressure";
        case PhaseMode::POWER:    return "power";
        case PhaseMode::PAUSE:    return "pause";
        case PhaseMode::FLOW:     return "flow";
    }
    return "pressure";
}

bool parseTriggerType(const char* s, TriggerType& out) {
    if (!s) return false;
    static const struct { const char* n; TriggerType t; } tbl[] = {
        {"pressure_ge", TriggerType::PRESSURE_GE},
        {"pressure_le", TriggerType::PRESSURE_LE},
        {"weight_ge",   TriggerType::WEIGHT_GE},
        {"weight_le",   TriggerType::WEIGHT_LE},
        {"time_sec",    TriggerType::TIME_SEC},
        {"flow_ge",     TriggerType::FLOW_GE},
        {"flow_le",     TriggerType::FLOW_LE},
    };
    for (auto& e : tbl) {
        if (strcasecmp(e.n, s) == 0) { out = e.t; return true; }
    }
    return false;
}

bool parseCurve(const char* s, Curve& out) {
    if (!s) return false;
    static const struct { const char* n; Curve c; } tbl[] = {
        {"linear", Curve::LINEAR}, {"ease_in", Curve::EASE_IN}, {"ease_out", Curve::EASE_OUT},
        {"ease_in_out", Curve::EASE_IN_OUT}, {"instant", Curve::INSTANT},
        {"constant", Curve::CONSTANT}, {"flat", Curve::CONSTANT},
    };
    for (auto& e : tbl) {
        if (strcasecmp(e.n, s) == 0) { out = e.c; return true; }
    }
    return false;
}

// Smoothstep (3t^2 - 2t^3) for ease-in-out; the other easings are simple blends of linear and
// smoothstep - cheap, monotone, and visually indistinguishable from the real easing curves for a
// pressure setpoint.
float applyCurve(Curve c, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return (c == Curve::CONSTANT) ? 0.0f : 1.0f;
    switch (c) {
        case Curve::LINEAR:
            return t;
        case Curve::INSTANT:
            return (t >= 1.0f) ? 1.0f : 0.0f;
        case Curve::CONSTANT:
            return 0.0f;
        case Curve::EASE_IN: {
            const float progress = t;
            return progress * progress;
        }
        case Curve::EASE_OUT: {
            const float remaining = 1.0f - t;
            return 1.0f - (remaining * remaining);
        }
        case Curve::EASE_IN_OUT: {
            // Cubic Hermite smoothstep S-curve
            return t * t * (3.0f - 2.0f * t);
        }
    }
    return t;
}

bool evaluateTransition(const Transition& tr, float pressure, float weight,
                        float flowVirt, float elapsedSec, bool isSimulation) {
    if (tr.triggerCount == 0) {
        const float dur = tr.durationSec > 0.0f ? tr.durationSec : 4.0f;
        return elapsedSec >= dur;
    }

    // If duration is specified (> 0) and exceeded, the phase has timed out -> advance
    if (tr.durationSec > 0.0f && elapsedSec >= tr.durationSec) {
        return true;
    }

    if (isSimulation) {
        // In simulation mode, phases run for their full programmed durationSec so that
        // the client graphs and HUD timeline match the dashed profile preview 1:1.
        // Global targetWeightG cutoff is handled in BrewController::tick().
        if (tr.durationSec <= 0.0f && elapsedSec >= 5.0f) {
            return true;
        }
        return false;
    }

    bool all = true;
    bool any = false;
    for (uint8_t i = 0; i < tr.triggerCount; ++i) {
        const Trigger& t = tr.triggers[i];
        bool rawHit = false;
        switch (t.type) {
            case TriggerType::PRESSURE_GE: rawHit = pressure >= t.value; break;
            // C-08: Require a 0.25 s phase entry grace window before <= triggers can fire so a phase
            // does not skip at t=0.00 s while pressure/flow is still ramping or settling.
            case TriggerType::PRESSURE_LE: rawHit = (elapsedSec >= 0.25f) && (pressure <= t.value); break;
            case TriggerType::WEIGHT_GE:   rawHit = weight   >= t.value; break;
            case TriggerType::WEIGHT_LE:   rawHit = (elapsedSec >= 0.25f) && (weight   <= t.value); break;
            case TriggerType::TIME_SEC:    rawHit = elapsedSec >= t.value; break;
            case TriggerType::FLOW_GE:     rawHit = flowVirt >= t.value; break;
            case TriggerType::FLOW_LE:     rawHit = (elapsedSec >= 0.25f) && (flowVirt <= t.value); break;
        }
        const bool hit = t.fired || (t.armed && rawHit);
        any = any || hit;
        all = all && hit;
    }

    return (tr.logic == TriggerLogic::AND) ? all : any;
}

// -------------------------------------------------------------------------------------------------
// ProfileEngine
// -------------------------------------------------------------------------------------------------
void ProfileEngine::begin() {
    if (!LittleFS.exists(PROFILE_DIR)) LittleFS.mkdir(PROFILE_DIR);
}

const char* ProfileEngine::phaseName() const {
    if (_phaseIdx < _p.phaseCount) return _p.phases[_phaseIdx].name;
    return "-";
}

float ProfileEngine::phaseProgress() const {
    if (_phaseIdx >= _p.phaseCount) return 1.0f;
    const float d = _p.phases[_phaseIdx].transition.durationSec;
    if (d <= 0.0f) return 1.0f;
    float t = _phaseElapsedSec / d;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

void ProfileEngine::start(uint32_t nowMs) {
    if (!_loaded) {
        SB_LOGE("start() with no profile loaded");
        return;
    }
    _override = false;
    _finished = false;
    _running  = true;
    _startMs  = (nowMs > 0) ? nowMs : millis();
    enterPhase(0, _startMs);

    SB_LOGI("[BREW] Profile '%s' started - %u phase(s), target %.1f g, max %.1f s",
            _p.name, _p.phaseCount, _p.targetWeightG, _p.maxTotalTimeSec);
}

void ProfileEngine::stop() {
    _running  = false;
    _finished = false;
    _override = false;
}

void ProfileEngine::nextPhase(uint32_t nowMs) {
    if (!_loaded) return;
    if (_phaseIdx + 1 >= _p.phaseCount) {
        SB_LOGI("NEXT_PHASE requested but already on the last phase.");
        _finished = true;
        return;
    }
    SB_LOGI("[PHASE] '%s' -> '%s' (requested by the operator)", phaseName(), _p.phases[_phaseIdx + 1].name);
    enterPhase(_phaseIdx + 1, nowMs ? nowMs : millis());
}

void ProfileEngine::overrideTarget(float bar) {
    _override    = true;
    _overrideBar = bar;
}

void ProfileEngine::enterPhase(uint8_t idx, uint32_t nowMs) {
    if (idx >= _p.phaseCount) { _finished = true; _running = false; return; }

    _phaseIdx      = idx;
    _phaseStartMs  = (nowMs > 0) ? nowMs : millis();
    _phaseElapsedSec = 0.0f;
    _override      = false;

    // Edge-triggered semantics: clear the trigger latches so a condition that is already true when
    // the phase begins has to go false and true again to fire.
    for (uint8_t i = 0; i < _p.phases[idx].transition.triggerCount; ++i) {
        Trigger& t = _p.phases[idx].transition.triggers[i];
        t.fired = false;
        t.armed = (t.type != TriggerType::PRESSURE_LE &&
                   t.type != TriggerType::WEIGHT_LE &&
                   t.type != TriggerType::FLOW_LE);
    }

    const Phase& ph = _p.phases[idx];
    _currentTarget = ph.startVal;

    // The phase target is the one number an operator wants to see for every phase: what the
    // controller is aiming at, how it gets there and what the power ceiling is.
    const char* unit = (ph.mode == PhaseMode::FLOW)  ? " ml/s"
                     : (ph.mode == PhaseMode::POWER) ? "%"
                                                     : " bar";
    if (ph.mode == PhaseMode::PAUSE) {
        SB_LOGI("[PHASE] %u/%u '%s' - pause (no flow), psm ceiling %u%%",
                idx + 1, _p.phaseCount, ph.name, ph.maxPsmPct);
    } else if (ph.startVal == ph.endVal) {
        SB_LOGI("[PHASE] %u/%u '%s' - %s mode, target %.2f%s, psm ceiling %u%%",
                idx + 1, _p.phaseCount, ph.name, modeName(ph.mode),
                ph.startVal, unit, ph.maxPsmPct);
    } else {
        SB_LOGI("[PHASE] %u/%u '%s' - %s mode, target %.2f -> %.2f%s over %.1f s (%s), psm ceiling %u%%",
                idx + 1, _p.phaseCount, ph.name, modeName(ph.mode),
                ph.startVal, ph.endVal, unit, ph.transition.durationSec,
                curveName(ph.curve), ph.maxPsmPct);
    }
    if (ph.mode == PhaseMode::PRESSURE && ph.flowLimit > 0.0f)
        SB_LOGI("[PHASE]   flow limit %.1f ml/s", ph.flowLimit);
    if (ph.mode == PhaseMode::FLOW && ph.pressureLimit > 0.0f)
        SB_LOGI("[PHASE]   pressure limit %.1f bar", ph.pressureLimit);
}

// -------------------------------------------------------------------------------------------------
// Work out why a transition fired, so the log can say "why" instead of just "the phase changed".
// -------------------------------------------------------------------------------------------------
void ProfileEngine::buildTransitionReason(const Transition& tr, float pressure, float weight,
                                          float flowVirt, float elapsedSec, bool isSimulation) {
    if (isSimulation) {
        // A simulated phase always ends on its duration, never on a physical trigger.
        snprintf(_transReason, sizeof(_transReason), "simulation phase ran its %.1f s", elapsedSec);
        return;
    }
    describeTransitionReason(tr, pressure, weight, flowVirt, elapsedSec,
                              _transReason, sizeof(_transReason));
}

float ProfileEngine::targetForPhase(const Phase& ph, float tNorm) const {
    const float shaped = applyCurve(ph.curve, tNorm);
    return ph.startVal + (ph.endVal - ph.startVal) * shaped;
}

// -------------------------------------------------------------------------------------------------
float ProfileEngine::tick(float pressureBar, float weightG, float flowVirt, uint32_t nowMs, bool isSimulation) {
    if (!_running || !_loaded || _phaseIdx >= _p.phaseCount) {
        _currentTarget = 0.0f;
        return 0.0f;
    }

    _phaseElapsedSec = (nowMs >= _phaseStartMs) ? ((float)(nowMs - _phaseStartMs) * 0.001f) : 0.0f;

    // Global shot timeout
    const float totalSec = (nowMs >= _startMs) ? ((float)(nowMs - _startMs) * 0.001f) : 0.0f;
    if (_p.maxTotalTimeSec > 0.5f && totalSec >= _p.maxTotalTimeSec) {
        SB_LOGW("Shot hit maxTotalTimeSec (%.1f s) - ending run.", _p.maxTotalTimeSec);
        _finished = true;
        _running  = false;
        _currentTarget = 0.0f;
        return 0.0f;
    }

    const Phase& ph = _p.phases[_phaseIdx];
    const float dur = ph.transition.durationSec;
    const float tNorm = dur > 0.0f ? _phaseElapsedSec / dur : 1.0f;

    // ---- setpoint ------------------------------------------------------------------------------
    if (_override) {
        _currentTarget = _overrideBar;
    } else if (ph.mode == PhaseMode::PRESSURE || ph.mode == PhaseMode::FLOW || ph.mode == PhaseMode::POWER) {
        _currentTarget = targetForPhase(ph, tNorm);
    } else {   // PAUSE
        _currentTarget = 0.0f;
    }

    // ---- transition ----------------------------------------------------------------------------
    const bool done = transitionSatisfied(ph.transition, pressureBar, weightG, flowVirt,
                                          _phaseElapsedSec, isSimulation);
    if (done) {
        buildTransitionReason(ph.transition, pressureBar, weightG, flowVirt,
                              _phaseElapsedSec, isSimulation);
        if (_phaseIdx + 1 >= _p.phaseCount) {
            SB_LOGI("[PHASE] '%s' -> end of profile after %.1f s (%s)",
                    ph.name, totalSec, _transReason);
            _finished = true;
            _running  = false;
        } else {
            SB_LOGI("[PHASE] '%s' -> '%s' after %.1f s (%s)",
                    ph.name, _p.phases[_phaseIdx + 1].name, _phaseElapsedSec, _transReason);
            enterPhase(_phaseIdx + 1, nowMs);
        }
    }
    return _currentTarget;
}

bool ProfileEngine::transitionSatisfied(const Transition& tr, float pressure, float weight,
                                        float flowVirt, float elapsedSec, bool isSimulation) {
    if (!isSimulation && _phaseIdx < _p.phaseCount) {
        Transition& mutTr = _p.phases[_phaseIdx].transition;
        for (uint8_t i = 0; i < mutTr.triggerCount; ++i) {
            Trigger& t = mutTr.triggers[i];
            if (!t.armed) {
                switch (t.type) {
                    case TriggerType::PRESSURE_LE: t.armed = (pressure > t.value); break;
                    case TriggerType::WEIGHT_LE:   t.armed = (weight   > t.value); break;
                    case TriggerType::FLOW_LE:     t.armed = (flowVirt > t.value); break;
                    default:                       t.armed = true;                 break;
                }
            }
            if (t.armed && !t.fired) {
                switch (t.type) {
                    case TriggerType::PRESSURE_GE: t.fired = (pressure >= t.value); break;
                    case TriggerType::PRESSURE_LE: t.fired = (elapsedSec >= 0.25f) && (pressure <= t.value); break;
                    case TriggerType::WEIGHT_GE:   t.fired = (weight   >= t.value); break;
                    case TriggerType::WEIGHT_LE:   t.fired = (elapsedSec >= 0.25f) && (weight   <= t.value); break;
                    case TriggerType::TIME_SEC:    t.fired = (elapsedSec >= t.value); break;
                    case TriggerType::FLOW_GE:     t.fired = (flowVirt >= t.value); break;
                    case TriggerType::FLOW_LE:     t.fired = (elapsedSec >= 0.25f) && (flowVirt <= t.value); break;
                }
            }
        }
        return evaluateTransition(mutTr, pressure, weight, flowVirt, elapsedSec, isSimulation);
    }
    return evaluateTransition(tr, pressure, weight, flowVirt, elapsedSec, isSimulation);
}

// -------------------------------------------------------------------------------------------------
// JSON parsing (docs/Profiles.md schema)
// -------------------------------------------------------------------------------------------------
bool ProfileEngine::parseJson(const char* json, Profile& out, char* err, size_t errLen) {
    if (err && errLen) err[0] = '\0';
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, json)) {
        if (err && errLen) snprintf(err, errLen, "malformed JSON");
        return false;
    }

    Profile p{};
    const char* profileName = doc["name"] | (doc["label"] | "Unnamed");
    strlcpy(p.name, profileName, sizeof(p.name));

    // ID fallback
    if (doc.containsKey("id") && doc["id"].as<const char*>() && strlen(doc["id"].as<const char*>()) > 0) {
        strlcpy(p.id, doc["id"], sizeof(p.id));
    } else {
        // Derive ID from name
        size_t j = 0;
        for (size_t i = 0; profileName[i] && j < sizeof(p.id) - 1; ++i) {
            char c = profileName[i];
            if (isalnum(c)) p.id[j++] = tolower(c);
            else if (c == ' ' || c == '-' || c == '_') {
                if (j > 0 && p.id[j-1] != '_') p.id[j++] = '_';
            }
        }
        p.id[j] = '\0';
        if (p.id[0] == '\0') strlcpy(p.id, "custom", sizeof(p.id));
    }

    strlcpy(p.author, doc["author"] | (doc.containsKey("type") && strcmp(doc["type"] | "", "pro") == 0 ? "Imported" : ""), sizeof(p.author));

    // Target weight: check native, an imported `targets` array, or globalStopConditions
    if (doc.containsKey("targetWeightG")) {
        p.targetWeightG = doc["targetWeightG"].as<float>();
    } else if (doc.containsKey("target_weight")) {
        p.targetWeightG = doc["target_weight"].as<float>();
    } else if (doc.containsKey("globalStopConditions") && doc["globalStopConditions"].containsKey("weight")) {
        p.targetWeightG = doc["globalStopConditions"]["weight"].as<float>();
    } else {
        p.targetWeightG = 36.0f; // Default baseline
    }

    p.maxTotalTimeSec = doc.containsKey("maxTotalTimeSec") ? doc["maxTotalTimeSec"].as<float>() : (doc["max_duration"] | 60.0f);

    JsonArray phases = doc.containsKey("phases") ? doc["phases"].as<JsonArray>() : doc["steps"].as<JsonArray>();
    if (!phases.isNull() && phases.size() > 0) {
        const int n = (int)min<size_t>(phases.size(), SB_MAX_PHASES);
        float lastSetVal = 0.0f;

        for (int i = 0; i < n; ++i) {
            JsonObject po = phases[i];
            Phase& ph = p.phases[i];
            strlcpy(ph.name, po["name"] | "Phase", sizeof(ph.name));

            // Determine Mode (support native mode, pump object, pump string)
            if (po.containsKey("pump")) {
                if (po["pump"].is<JsonObject>()) {
                    const char* pt = po["pump"]["target"] | "pressure";
                    ph.mode = (strcasecmp(pt, "flow") == 0) ? PhaseMode::FLOW : PhaseMode::PRESSURE;
                } else if (po["pump"].is<const char*>()) {
                    const char* pt = po["pump"].as<const char*>();
                    ph.mode = (strcasecmp(pt, "flow") == 0) ? PhaseMode::FLOW : PhaseMode::PRESSURE;
                } else {
                    ph.mode = PhaseMode::PRESSURE;
                }
            } else {
                const char* m = po["mode"] | (po["type"] | "pressure");
                if (strcasecmp(m, "flow") == 0) {
                    ph.mode = PhaseMode::FLOW;
                } else if (strcasecmp(m, "power") == 0) {
                    ph.mode = PhaseMode::POWER;
                } else if (strcasecmp(m, "pause") == 0) {
                    ph.mode = PhaseMode::PAUSE;
                } else {
                    ph.mode = PhaseMode::PRESSURE;
                }
            }

            // Curve parsing
            parseCurve(po["curve"] | "linear", ph.curve);

            // Determine Start and End values (support native startVal/endVal, pump object, or target object)
            if (po.containsKey("target") && po["target"].is<JsonObject>()) {
                // Compound target { start, end, curve, time }
                JsonObject bpt = po["target"].as<JsonObject>();
                ph.startVal = bpt.containsKey("start") ? bpt["start"].as<float>() : 0.0f;
                ph.endVal   = bpt.containsKey("end") ? bpt["end"].as<float>() : ph.startVal;
                parseCurve(bpt["curve"] | "linear", ph.curve);
                if (bpt.containsKey("time")) {
                    ph.transition.durationSec = bpt["time"].as<float>() / 1000.0f;
                }
            } else if (po.containsKey("pump") && po["pump"].is<JsonObject>()) {
                // Imported pump object { target, pressure, flow }
                JsonObject gp = po["pump"].as<JsonObject>();
                float v = (ph.mode == PhaseMode::FLOW || ph.mode == PhaseMode::POWER) ? (gp["flow"] | 0.0f) : (gp["pressure"] | 0.0f);
                ph.startVal = v;
                ph.endVal   = v;
                // An imported linear transition ramps from the previous setpoint
                if (po.containsKey("transition") && po["transition"].is<JsonObject>()) {
                    const char* tt = po["transition"]["type"] | "instant";
                    if (strcasecmp(tt, "linear") == 0 && i > 0) {
                        ph.startVal = lastSetVal;
                        ph.endVal   = v;
                        ph.curve    = Curve::LINEAR;
                    }
                }
            } else if (po.containsKey("target")) {
                float v = po["target"].as<float>();
                ph.startVal = v;
                ph.endVal   = v;
            } else {
                ph.startVal = po.containsKey("startVal") ? po["startVal"].as<float>() : (po["value"] | 0.0f);
                ph.endVal   = po.containsKey("endVal") ? po["endVal"].as<float>() : (po["value"] | ph.startVal);
            }

            // Smooth ramp mapping
            if (po.containsKey("ramp")) {
                const char* rm = po["ramp"] | "instant";
                if (strcasecmp(rm, "smooth") == 0) ph.curve = Curve::EASE_IN_OUT;
                else if (strcasecmp(rm, "instant") == 0) ph.curve = Curve::INSTANT;
            }

            lastSetVal = ph.endVal;
            ph.maxPsmPct = (uint8_t)constrain(po["maxPsmPct"] | 100, 0, 100);
            // Advanced limits (docs/Profiles.md): 0 or absent = no limit.
            ph.flowLimit     = constrain(po["flowLimit"] | 0.0f, 0.0f, 12.0f);
            ph.pressureLimit = constrain(po["pressureLimit"] | 0.0f, 0.0f, 15.0f);

            // Duration
            if (po.containsKey("duration")) {
                ph.transition.durationSec = po["duration"].as<float>();
            } else if (po.containsKey("durationSec")) {
                ph.transition.durationSec = po["durationSec"].as<float>();
            } else if (po.containsKey("transition") && po["transition"].is<JsonObject>() && po["transition"].containsKey("durationSec")) {
                ph.transition.durationSec = po["transition"]["durationSec"].as<float>();
            } else if (po.containsKey("stopConditions") && po["stopConditions"].is<JsonObject>() && po["stopConditions"].containsKey("time")) {
                ph.transition.durationSec = po["stopConditions"]["time"].as<float>() / 1000.0f;
            } else if (ph.transition.durationSec <= 0.0f) {
                ph.transition.durationSec = 5.0f;
            }

            ph.transition.logic = TriggerLogic::OR;
            if (po.containsKey("transition") && po["transition"].is<JsonObject>()) {
                const char* log = po["transition"]["logic"] | "OR";
                if (strcasecmp(log, "AND") == 0) ph.transition.logic = TriggerLogic::AND;
            }

            // Triggers collection
            uint8_t tc = 0;

            // 1. Native triggers
            if (po.containsKey("transition") && po["transition"].is<JsonObject>() && po["transition"].containsKey("triggers")) {
                JsonArray trigs = po["transition"]["triggers"].as<JsonArray>();
                for (JsonObject t : trigs) {
                    if (tc >= SB_MAX_TRIGGERS) break;
                    TriggerType tt;
                    if (parseTriggerType(t["type"] | "", tt)) {
                        p.phases[i].transition.triggers[tc].type  = tt;
                        p.phases[i].transition.triggers[tc].value = t["value"] | 0.0f;
                        tc++;
                    }
                }
            }

            // 2. Imported `targets` array
            if (po.containsKey("targets") && po["targets"].is<JsonArray>()) {
                for (JsonObject gt : po["targets"].as<JsonArray>()) {
                    if (tc >= SB_MAX_TRIGGERS) break;
                    const char* gtt = gt["type"] | "";
                    const char* op  = gt["operator"] | "gte";
                    float val = gt["value"] | 0.0f;

                    if (strcasecmp(gtt, "pressure") == 0) {
                        p.phases[i].transition.triggers[tc].type = (strcasecmp(op, "lte") == 0) ? TriggerType::PRESSURE_LE : TriggerType::PRESSURE_GE;
                        p.phases[i].transition.triggers[tc].value = val;
                        tc++;
                    } else if (strcasecmp(gtt, "flow") == 0) {
                        p.phases[i].transition.triggers[tc].type = (strcasecmp(op, "lte") == 0) ? TriggerType::FLOW_LE : TriggerType::FLOW_GE;
                        p.phases[i].transition.triggers[tc].value = val;
                        tc++;
                    } else if (strcasecmp(gtt, "volumetric") == 0 || strcasecmp(gtt, "weight") == 0) {
                        p.phases[i].transition.triggers[tc].type = TriggerType::WEIGHT_GE;
                        p.phases[i].transition.triggers[tc].value = val;
                        if (val > 0.0f && p.targetWeightG <= 0.1f) p.targetWeightG = val;
                        tc++;
                    }
                }
            }

            // 3. Structured stopConditions
            if (po.containsKey("stopConditions") && po["stopConditions"].is<JsonObject>()) {
                JsonObject sc = po["stopConditions"].as<JsonObject>();
                if (sc.containsKey("weight") && tc < SB_MAX_TRIGGERS) {
                    p.phases[i].transition.triggers[tc].type  = TriggerType::WEIGHT_GE;
                    p.phases[i].transition.triggers[tc].value = sc["weight"].as<float>();
                    tc++;
                }
                if (sc.containsKey("pressure") && tc < SB_MAX_TRIGGERS) {
                    p.phases[i].transition.triggers[tc].type  = TriggerType::PRESSURE_GE;
                    p.phases[i].transition.triggers[tc].value = sc["pressure"].as<float>();
                    tc++;
                }
            }

            // 4. Standalone weight property
            if (po.containsKey("weight") && po["weight"].as<float>() > 0.0f && tc < SB_MAX_TRIGGERS) {
                p.phases[i].transition.triggers[tc].type  = TriggerType::WEIGHT_GE;
                p.phases[i].transition.triggers[tc].value = po["weight"].as<float>();
                if (p.targetWeightG <= 0.1f) p.targetWeightG = po["weight"].as<float>();
                tc++;
            }

            ph.transition.triggerCount = tc;
        }
        p.phaseCount = (uint8_t)n;
    }

    if (p.phaseCount == 0) {
        if (err && errLen) snprintf(err, errLen, "profile has no phases");
        return false;
    }
    // A profile that never terminates would hold the pump open indefinitely.
    if (p.maxTotalTimeSec < 1.0f) p.maxTotalTimeSec = 60.0f;

    out = p;
    return true;
}

bool ProfileEngine::loadById(const char* rawId) {
    if (!rawId || !*rawId) return false;
    const char* id = rawId;
    const char* slash = strrchr(id, '/');
    if (slash) id = slash + 1;
    char clean[48];
    strlcpy(clean, id, sizeof(clean));
    size_t len = strlen(clean);
    if (len > 5 && strcmp(clean + len - 5, ".json") == 0) {
        clean[len - 5] = '\0';
    }
    id = clean;

    if (!*id || strlen(id) > 32) return false;
    for (size_t i = 0; id[i] != '\0'; ++i) {
        char c = id[i];
        if (!isalnum(c) && c != '_' && c != '-') {
            SB_LOGE("Invalid profile id '%s'", id);
            return false;
        }
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s.json", PROFILE_DIR, id);

    File f = LittleFS.open(path, "r");
    if (!f) {
        const String preset = seedJson(id);   // tagged, so a later seed update can refresh it
        if (preset.length()) {
            if (!LittleFS.exists(PROFILE_DIR)) LittleFS.mkdir(PROFILE_DIR);
            File wf = LittleFS.open(path, "w");
            if (wf) { wf.print(preset); wf.close(); }
            f = LittleFS.open(path, "r");
        }
    }
    if (!f) {
        SB_LOGE("Profile '%s' not found (%s)", id, path);
        return false;
    }
    std::string body;
    body.reserve(f.size() + 1);
    while (f.available()) body.push_back(f.read());
    f.close();

    Profile p{};
    char err[64];
    if (!parseJson(body.c_str(), p, err, sizeof(err))) {
        SB_LOGE("Profile '%s' parse failed: %s", id, err);
        return false;
    }
    setActive(p);
    _dirty = false;
    SB_LOGI("Loaded profile '%s' (%s) - %u phases", p.id, p.name, p.phaseCount);
    return true;
}

bool ProfileEngine::loadActive(const char* id) {
    char want[64];
    if (id && *id) {
        strlcpy(want, id, sizeof(want));
    } else {
        // Read /profiles/index.json -> {"active":"blooming_allonge"}
        want[0] = '\0';
        if (LittleFS.exists(PROFILE_INDEX)) {
            File f = LittleFS.open(PROFILE_INDEX, "r");
            if (f) {
                DynamicJsonDocument doc(512);
                deserializeJson(doc, f);
                f.close();
                strlcpy(want, doc["active"] | "", sizeof(want));
            }
        }
    }
    if (!*want) strlcpy(want, "classic_9bar", sizeof(want));
    if (loadById(want)) return true;
    return loadById("blooming_allonge");
}

// -------------------------------------------------------------------------------------------------
// Built-in seed profile (docs/Profiles.md example, shipped as /profiles/blooming_allonge.json)
// -------------------------------------------------------------------------------------------------
const char* ProfileEngine::defaultProfileJson() {
    return R"JSON({
  "id": "blooming_allonge",
  "name": "Blooming Espresso (Light Roast)",
  "author": "SmartLife 3D",
  "targetWeightG": 40.0,
  "maxTotalTimeSec": 55.0,
  "phases": [
    { "name": "Fast Fill", "mode": "pressure", "startVal": 3.0, "endVal": 3.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 5.0, "logic": "OR", "triggers": [
        { "type": "pressure_ge", "value": 2.5 },
        { "type": "weight_ge", "value": 1.5 } ] } },
    { "name": "Zero-Power Bloom", "mode": "power", "startVal": 0.0, "endVal": 0.0,
      "curve": "instant", "maxPsmPct": 0,
      "transition": { "durationSec": 12.0, "logic": "OR", "triggers": [] } },
    { "name": "Ramp to Peak", "mode": "pressure", "startVal": 2.0, "endVal": 8.5,
      "curve": "ease_in_out", "maxPsmPct": 100,
      "transition": { "durationSec": 4.0, "logic": "OR", "triggers": [] } },
    { "name": "Declining Extraction", "mode": "pressure", "startVal": 8.5, "endVal": 5.5,
      "curve": "ease_out", "maxPsmPct": 100,
      "transition": { "durationSec": 30.0, "logic": "OR", "triggers": [
        { "type": "weight_ge", "value": 40.0 } ] } }
  ]
})JSON";
}

String ProfileEngine::seedJson(const char* id) {
    const char* preset = getPresetJson(id);
    if (!preset) return String();
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, preset)) return String(preset);   // never happens for a built-in
    doc["seed_version"] = SB_SEED_PROFILE_VERSION;
    String out;
    serializeJsonPretty(doc, out);
    return out;
}

int ProfileEngine::seedVersionOf(const String& fileJson) {
    StaticJsonDocument<32> filter;
    filter["seed_version"] = true;
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, fileJson, DeserializationOption::Filter(filter))) return -1;
    if (!doc["seed_version"].is<int>()) return -1;
    return doc["seed_version"].as<int>();
}

const char* ProfileEngine::builtinProfiles() {
    return defaultProfileJson();
}

const char* ProfileEngine::getPresetJson(const char* id) {
    if (!id || !*id) return nullptr;
    if (strcmp(id, "blooming_allonge") == 0) return defaultProfileJson();
    if (strcmp(id, "classic_9bar") == 0) {
        return R"JSON({
  "id": "classic_9bar",
  "name": "Classic 9-Bar",
  "author": "SmartLife 3D",
  "targetWeightG": 36.0,
  "maxTotalTimeSec": 45.0,
  "phases": [
    { "name": "Pre-Infusion", "mode": "pressure", "startVal": 3.0, "endVal": 3.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 4.0, "logic": "OR", "triggers": [ { "type": "pressure_ge", "value": 3.0 } ] } },
    { "name": "Ramp to 9 Bar", "mode": "pressure", "startVal": 3.0, "endVal": 9.0,
      "curve": "ease_in_out", "maxPsmPct": 100,
      "transition": { "durationSec": 3.0, "logic": "OR", "triggers": [] } },
    { "name": "Peak Extraction", "mode": "pressure", "startVal": 9.0, "endVal": 9.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 20.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 30.0 } ] } },
    { "name": "Gentle Decline", "mode": "pressure", "startVal": 9.0, "endVal": 6.0,
      "curve": "ease_out", "maxPsmPct": 100,
      "transition": { "durationSec": 10.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 36.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "turbo_shot") == 0) {
        return R"JSON({
  "id": "turbo_shot",
  "name": "Turbo Shot",
  "author": "SmartLife 3D",
  "targetWeightG": 42.0,
  "maxTotalTimeSec": 25.0,
  "phases": [
    { "name": "Fast Ramp", "mode": "pressure", "startVal": 2.0, "endVal": 6.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 2.5, "logic": "OR", "triggers": [] } },
    { "name": "6-Bar Flat", "mode": "pressure", "startVal": 6.0, "endVal": 6.0,
      "curve": "constant", "maxPsmPct": 100,
      "transition": { "durationSec": 18.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 42.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "gentle_lever") == 0) {
        return R"JSON({
  "id": "gentle_lever",
  "name": "Gentle Lever Profile",
  "author": "SmartLife 3D",
  "targetWeightG": 36.0,
  "maxTotalTimeSec": 45.0,
  "phases": [
    { "name": "Low Soak", "mode": "pressure", "startVal": 1.5, "endVal": 1.5,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 8.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 1.0 } ] } },
    { "name": "Spring Ramp", "mode": "pressure", "startVal": 2.0, "endVal": 8.5,
      "curve": "ease_in_out", "maxPsmPct": 100,
      "transition": { "durationSec": 3.0, "logic": "OR", "triggers": [] } },
    { "name": "Spring Decay", "mode": "pressure", "startVal": 8.5, "endVal": 5.0,
      "curve": "ease_out", "maxPsmPct": 100,
      "transition": { "durationSec": 25.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 36.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "italian_dark") == 0) {
        return R"JSON({
  "id": "italian_dark",
  "name": "Italian Dark Roast",
  "author": "SmartLife 3D",
  "targetWeightG": 30.0,
  "maxTotalTimeSec": 35.0,
  "phases": [
    { "name": "Soft Pre-Wet", "mode": "pressure", "startVal": 2.0, "endVal": 2.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 3.0, "logic": "OR", "triggers": [] } },
    { "name": "9-Bar Flat Pull", "mode": "pressure", "startVal": 9.0, "endVal": 9.0,
      "curve": "constant", "maxPsmPct": 100,
      "transition": { "durationSec": 25.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 30.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "londinium") == 0) {
        // Adapted from Gaggiuino's "Londinium" default (CC BY-NC 4.0, github.com/Zer0-bit/gaggiuino,
        // src/eeprom_data/default_profiles.h). A lever-machine shot: fast fill, a pump-off soak,
        // 9 bar, then a slow decline to 3 bar with a 3 ml/s flow limit.
        return R"JSON({
  "id": "londinium",
  "name": "Londinium (Lever)",
  "author": "Gaggiuino (CC BY-NC 4.0), adapted",
  "targetWeightG": 40.0,
  "maxTotalTimeSec": 60.0,
  "phases": [
    { "name": "Fill", "mode": "pressure", "startVal": 4.0, "endVal": 4.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 10.0, "logic": "OR", "triggers": [ { "type": "pressure_ge", "value": 4.0 } ] } },
    { "name": "Soak", "mode": "pause", "startVal": 0.0, "endVal": 0.0,
      "curve": "instant", "maxPsmPct": 100,
      "transition": { "durationSec": 10.0, "logic": "OR", "triggers": [ { "type": "pressure_le", "value": 0.7 } ] } },
    { "name": "Rise to 9 Bar", "mode": "pressure", "startVal": 4.0, "endVal": 9.0,
      "curve": "linear", "maxPsmPct": 100, "flowLimit": 3.0,
      "transition": { "durationSec": 1.5, "logic": "OR", "triggers": [] } },
    { "name": "Hold", "mode": "pressure", "startVal": 9.0, "endVal": 9.0,
      "curve": "constant", "maxPsmPct": 100, "flowLimit": 3.0,
      "transition": { "durationSec": 4.0, "logic": "OR", "triggers": [] } },
    { "name": "Lever Decline", "mode": "pressure", "startVal": 9.0, "endVal": 3.0,
      "curve": "linear", "maxPsmPct": 100, "flowLimit": 3.0,
      "transition": { "durationSec": 20.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 40.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "iuiuiu_classic") == 0) {
        // Adapted from Gaggiuino's "IUIUIU Classic" default (CC BY-NC 4.0). Flow pre-infusion with a
        // pressure limit, a soak, a 7.5 -> 6 bar transition, then a declining-flow extraction.
        return R"JSON({
  "id": "iuiuiu_classic",
  "name": "IUIUIU Classic",
  "author": "Gaggiuino (CC BY-NC 4.0), adapted",
  "targetWeightG": 36.0,
  "maxTotalTimeSec": 75.0,
  "phases": [
    { "name": "Flow Pre-Infusion", "mode": "flow", "startVal": 3.0, "endVal": 3.0,
      "curve": "linear", "maxPsmPct": 100, "pressureLimit": 4.0,
      "transition": { "durationSec": 20.0, "logic": "OR", "triggers": [
        { "type": "pressure_ge", "value": 4.0 }, { "type": "weight_ge", "value": 4.0 } ] } },
    { "name": "Soak", "mode": "pause", "startVal": 0.0, "endVal": 0.0,
      "curve": "instant", "maxPsmPct": 100,
      "transition": { "durationSec": 30.0, "logic": "OR", "triggers": [
        { "type": "pressure_le", "value": 2.0 }, { "type": "weight_ge", "value": 4.0 } ] } },
    { "name": "Ramp", "mode": "pressure", "startVal": 2.0, "endVal": 7.5,
      "curve": "ease_in_out", "maxPsmPct": 100, "flowLimit": 3.0,
      "transition": { "durationSec": 5.0, "logic": "OR", "triggers": [] } },
    { "name": "Transition", "mode": "pressure", "startVal": 7.5, "endVal": 6.0,
      "curve": "linear", "maxPsmPct": 100, "flowLimit": 3.0,
      "transition": { "durationSec": 4.0, "logic": "OR", "triggers": [] } },
    { "name": "Declining Flow", "mode": "flow", "startVal": 2.5, "endVal": 1.0,
      "curve": "linear", "maxPsmPct": 100, "pressureLimit": 6.0,
      "transition": { "durationSec": 15.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 36.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "adaptive") == 0) {
        // Adapted from Gaggiuino's "Adaptive" default (CC BY-NC 4.0), itself after the adaptive
        // profiles of the Decent community: fill to 3 bar, hold, rise to 9 bar, then hold a flow
        // and let the pressure follow the puck (limited to 9 bar).
        return R"JSON({
  "id": "adaptive",
  "name": "Adaptive (Flow)",
  "author": "Gaggiuino (CC BY-NC 4.0), adapted",
  "targetWeightG": 36.0,
  "maxTotalTimeSec": 75.0,
  "phases": [
    { "name": "Fill", "mode": "pressure", "startVal": 3.0, "endVal": 3.0,
      "curve": "linear", "maxPsmPct": 100, "flowLimit": 7.0,
      "transition": { "durationSec": 20.0, "logic": "OR", "triggers": [ { "type": "pressure_ge", "value": 3.0 } ] } },
    { "name": "Pre-Infusion Hold", "mode": "pressure", "startVal": 3.0, "endVal": 3.0,
      "curve": "constant", "maxPsmPct": 100,
      "transition": { "durationSec": 6.0, "logic": "OR", "triggers": [] } },
    { "name": "Rise to 9 Bar", "mode": "pressure", "startVal": 3.0, "endVal": 9.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 6.0, "logic": "OR", "triggers": [] } },
    { "name": "Adaptive Flow", "mode": "flow", "startVal": 2.0, "endVal": 2.5,
      "curve": "linear", "maxPsmPct": 100, "pressureLimit": 9.0,
      "transition": { "durationSec": 30.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 36.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "blooming_flow") == 0) {
        // Adapted from Gaggiuino's "Blooming espresso" default (CC BY-NC 4.0): a flow fill, a long
        // bloom, then a 2 ml/s extraction with a 6 bar limit.
        return R"JSON({
  "id": "blooming_flow",
  "name": "Blooming Espresso (Flow)",
  "author": "Gaggiuino (CC BY-NC 4.0), adapted",
  "targetWeightG": 36.0,
  "maxTotalTimeSec": 80.0,
  "phases": [
    { "name": "Flow Fill", "mode": "flow", "startVal": 4.0, "endVal": 4.0,
      "curve": "linear", "maxPsmPct": 100, "pressureLimit": 7.0,
      "transition": { "durationSec": 20.0, "logic": "OR", "triggers": [ { "type": "pressure_ge", "value": 7.0 } ] } },
    { "name": "Bloom", "mode": "pause", "startVal": 0.0, "endVal": 0.0,
      "curve": "instant", "maxPsmPct": 100,
      "transition": { "durationSec": 30.0, "logic": "OR", "triggers": [
        { "type": "pressure_le", "value": 0.6 }, { "type": "weight_ge", "value": 5.0 } ] } },
    { "name": "Ramp to Flow", "mode": "flow", "startVal": 0.5, "endVal": 2.0,
      "curve": "ease_in_out", "maxPsmPct": 100, "pressureLimit": 9.0,
      "transition": { "durationSec": 5.0, "logic": "OR", "triggers": [] } },
    { "name": "Steady Flow", "mode": "flow", "startVal": 2.0, "endVal": 2.0,
      "curve": "constant", "maxPsmPct": 100, "pressureLimit": 6.0,
      "transition": { "durationSec": 15.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 36.0 } ] } }
  ]
})JSON";
    }
    if (strcmp(id, "slow_pi_flat") == 0) {
        // SmartBrew's own: a long, low-flow pre-infusion until the first drops (the technique
        // popularised by flow-control machines), then a flat 9 bar.
        return R"JSON({
  "id": "slow_pi_flat",
  "name": "Slow Pre-Infusion + 9 Bar",
  "author": "SmartBrew",
  "targetWeightG": 36.0,
  "maxTotalTimeSec": 60.0,
  "phases": [
    { "name": "Low-Flow Pre-Infusion", "mode": "flow", "startVal": 1.5, "endVal": 1.5,
      "curve": "constant", "maxPsmPct": 100, "pressureLimit": 4.0,
      "transition": { "durationSec": 15.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 2.0 } ] } },
    { "name": "Ramp to 9 Bar", "mode": "pressure", "startVal": 4.0, "endVal": 9.0,
      "curve": "ease_in_out", "maxPsmPct": 100,
      "transition": { "durationSec": 4.0, "logic": "OR", "triggers": [] } },
    { "name": "9-Bar Extraction", "mode": "pressure", "startVal": 9.0, "endVal": 9.0,
      "curve": "constant", "maxPsmPct": 100,
      "transition": { "durationSec": 30.0, "logic": "OR", "triggers": [ { "type": "weight_ge", "value": 36.0 } ] } }
  ]
})JSON";
    }
    return nullptr;
}

ProfileEngine g_profile;