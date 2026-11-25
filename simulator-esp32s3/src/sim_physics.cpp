// =================================================================================================
//  sim_physics.cpp - Real-Time Espresso Hydraulic & Extraction Physics Model
// =================================================================================================
#include "sim_physics.h"
#include "sim_dimmer.h"
#include "sim_button.h"
#include "sim_pressure.h"
#include "sim_ble_scale.h"
#include "pump_physics.h"
#include <algorithm>
#include <cmath>

SimPhysics g_simPhysics;

// -------------------------------------------------------------------------------------------------
// Pump presets - MUST match the controller's include/pump_presets.h, otherwise the controller's
// feed-forward curve describes a different pump than the one being simulated.
// -------------------------------------------------------------------------------------------------
const SimPumpPreset SIM_PUMPS[] = {
    { "ULKA_EX5",    "Ulka EX5 / EP5 (48W)", 14.0f, 36.0f, 10.8f, 15.0f },
    { "ULKA_EX4",    "Ulka EX4 / EP4 (48W)", 16.0f, 32.0f,  7.5f, 18.5f },
    { "OLAB_SILENT", "Olab Silent Green",    18.0f, 40.0f,  9.5f, 14.5f },
    { "CUSTOM",      "Custom / user tuned",  16.0f, 36.0f,  9.0f, 15.5f },
};
const uint8_t SIM_PUMP_COUNT = sizeof(SIM_PUMPS) / sizeof(SIM_PUMPS[0]);

namespace {
// Puck resistance as a multiple of the MEDIUM value for the selected pump.
constexpr float GRIND_R_FINE_COARSE = 0.65f;
constexpr float GRIND_R_FINE       = 1.83f;
constexpr float GRIND_R_CHOKED     = 3.65f;

// Puck dynamics - the bed swells, compacts under pressure and erodes as channels open up.
constexpr float R_SWELL       = 0.010f;
constexpr float R_COMPACTION  = 0.0006f;
constexpr float R_EROSION     = 0.0008f;
constexpr float R_VOLUME_REF  = 18.0f;   // ml pumped at which the swelling starts to saturate
constexpr float R_MIN         = 0.5f;
constexpr float R_MAX         = 15.0f;

// ---- the post-shot weight tail --------------------------------------------------------------------
// When the pump stops, the grouphead is not empty: the last of the coffee is still draining out
// of it, so the cup keeps getting heavier for a moment after the shot has ended. That tail is
// what the controller's settling window (src/control/weight_settle.h) exists to follow, and what
// the clients show as the yield climbing past the cutoff reading.
//
// The residual flow out of the grouphead decays exponentially. Over the default 1 s tail this
// adds roughly 1.3 g to the cup - the amount that actually ends up under the basket.
constexpr float    POST_SHOT_TAIL_Q0_MLS = 3.0f;    // the first drip out of the grouphead
constexpr float    POST_SHOT_TAIL_TAU_S = 0.5f;    // how quickly that drip decays
}  // namespace

// -------------------------------------------------------------------------------------------------
// The MEDIUM resistance that puts the 9 bar operating point exactly on the controller's
// feed-forward line for the given pump:
//
//   at duty = holdNineBarPct the pump delivers
//       Q = (holdNineBarPct/100) * maxFlow * (1 - (9/stall)^1.35)   [ml/s]
//   and the puck must pass that flow at 9 bar:
//       9 = Q^2 * R   ->   R = 9 / Q^2
// -------------------------------------------------------------------------------------------------
float SimPhysics::mediumResistanceFor(const SimPumpPreset& p) {
    const float qAt9 = calculateVirtualFlow(9.0f, p.holdNineBarPct, p.stallBar, p.maxFlowMlS);
    if (qAt9 <= 0.01f) return 2.398f;
    return 9.0f / (qAt9 * qAt9);
}

void SimPhysics::begin() {
    _pumpIdx = 0;
    _grind = PuckGrind::MEDIUM;
    _customResistance = false;
    _customResistanceValue = mediumResistanceFor(SIM_PUMPS[_pumpIdx]);
    resetShot();
    Serial.printf("[PHYSICS] Plant model ready: pump=%s, medium R=%.3f (FF line exact at 9 bar)\n",
                  SIM_PUMPS[_pumpIdx].name, _customResistanceValue);
}

void SimPhysics::setGrind(PuckGrind g) {
    _grind = g;
    _customResistance = false;
    _puckR = baseResistance();
}

void SimPhysics::setResistance(float r) {
    if (r < 0.1f) r = 0.1f;
    if (r > 20.0f) r = 20.0f;
    _customResistance = true;
    _customResistanceValue = r;
    _puckR = r;
}

void SimPhysics::setPumpIndex(uint8_t idx) {
    if (idx >= SIM_PUMP_COUNT) return;
    _pumpIdx = idx;
    // Re-derive the puck resistances for the new pump so the feed-forward line stays exact.
    // An operator-tuned resistance is left alone.
    if (!_customResistance) {
        _puckR = baseResistance();
    }
}

void SimPhysics::setWeight(float g) {
    _weightG = std::max(0.0f, g);
    _tailActive = false;  // a manual injection overrides whatever the tail was doing
    g_simBleScale.setWeight(_weightG);
}

void SimPhysics::requestTare() {
    _tareRequested.store(true, std::memory_order_relaxed);
}

void SimPhysics::resetShot() {
    _pBar = 0.0f;
    _flowMlS = 0.0f;
    _volumePumpedMl = 0.0f;
    _weightG = 0.0f;   // a new extraction means a new cup on the scale
    _puckR = baseResistance();
    _tailActive = false;  // a new shot cancels any post-shot tail that was still running
}

// Resistance of the current grind for the current pump, unless the operator pinned a value.
float SimPhysics::baseResistance() const {
    if (_customResistance) return _customResistanceValue;
    const float medium = mediumResistanceFor(SIM_PUMPS[_pumpIdx]);
    switch (_grind) {
        case PuckGrind::COARSE: return medium * GRIND_R_FINE_COARSE;
        case PuckGrind::FINE:   return medium * GRIND_R_FINE;
        case PuckGrind::CHOKED: return medium * GRIND_R_CHOKED;
        case PuckGrind::MEDIUM:
        default:                return medium;
    }
}

// -------------------------------------------------------------------------------------------------
// Fixed-step integration. A stalled main loop must never be able to destabilise the model, so the
// elapsed time is consumed in whole 10 ms steps and the catch-up is capped.
// -------------------------------------------------------------------------------------------------
void SimPhysics::update() {
    const uint32_t nowUs = micros();
    if (_lastTickUs == 0) {
        _lastTickUs = nowUs;
        return;
    }
    const uint32_t dtUs = nowUs - _lastTickUs;
    // NOTE: _lastTickUs must only advance when a step is actually taken. The main loop calls
    // update() thousands of times per second, so the elapsed time has to accumulate across
    // calls - otherwise dtUs is the gap since the previous *call* (a few hundred us) and the
    // 10 ms threshold is never reached, so the plant would never advance.
    if (dtUs < 10000) return;   // 100 Hz simulation step
    _lastTickUs = nowUs;

    // A tare requested by the controller over BLE must take effect before the next notification.
    if (_tareRequested.exchange(false, std::memory_order_relaxed)) {
        _weightG = 0.0f;
        g_simBleScale.setWeight(0.0f);
        _tailActive = false;  // the coffee that was in the cup is tared away
    }

    static constexpr uint32_t STEP_US   = 10000;
    static constexpr uint32_t MAX_STEPS = 20;      // cap the catch-up at 200 ms of simulation
    uint32_t steps = dtUs / STEP_US;
    if (steps > MAX_STEPS) steps = MAX_STEPS;

    for (uint32_t i = 0; i < steps; ++i) {
        step(0.01f);
    }

    // Publish to the simulated hardware.
    g_simPressure.setBar(_pBar);
    g_simBleScale.setWeight(_weightG);
}

// -------------------------------------------------------------------------------------------------
// The post-shot weight tail.
//
// After the pump stops, the grouphead keeps dripping for a moment. The residual flow decays
// exponentially, so the cup keeps getting heavier until the tail runs out - and then the cup comes
// off the scale, which puts the scale back to 0 g for the next shot.
// -------------------------------------------------------------------------------------------------
void SimPhysics::stepPostShotTail(float dt) {
    if (!_tailActive) return;

    const uint32_t elapsedMs = millis() - _tailStartMs;

    if (elapsedMs >= _tailMs) {
        // The tail is over and the cup comes off the scale.
        _tailActive = false;
        Serial.printf("[PHYSICS] Post-shot weight tail over at %.2f g - cup removed from the scale\n",
                      _weightG);
        _weightG = 0.0f;
        g_simBleScale.setWeight(0.0f);
        g_simBleScale.notifyNow();
        return;
    }

    // The residual flow out of the grouphead, decaying.
    const float t = elapsedMs / 1000.0f;
    const float q = POST_SHOT_TAIL_Q0_MLS * expf(-t / POST_SHOT_TAIL_TAU_S);
    _flowMlS = q;
    _weightG += q * EXTRACT_DENSITY * dt;

    // Print the tail as it happens. The idle status line only refreshes every 1.5 s, which is
    // longer than the whole tail - without this the tail would be invisible on the console and
    // it would look as if the scale simply froze at the cutoff weight.
    if (elapsedMs - _tailLogMs >= 200) {
        _tailLogMs = elapsedMs;
        Serial.printf("[PHYSICS] Tail %4lu ms: scale sends %.2f g (drip %.2f ml/s)\n",
                      (unsigned long)elapsedMs, _weightG, q);
    }
}

void SimPhysics::step(float dt) {
    const bool isBrewing = g_simButton.isBrewing();

    // Detect state transitions.
    if (isBrewing && !_wasBrewing) {
        resetShot();            // a new extraction starts from a clean cup
        _wasBrewing = true;
    } else if (!isBrewing && _wasBrewing) {
        _wasBrewing = false;
        // The cup is NOT taken off the scale the instant the machine stops. The last of the coffee
        // is still draining out of the grouphead, so the scale keeps climbing for a moment - that
        // tail is what the controller's settling window follows.
        //
        // The cup still comes off the scale at the end of the tail, and that matters: the
        // controller captures its software tare offset from the last raw value it received, so the
        // scale must read 0 g whenever the machine is idle. If the raw weight were still the old
        // shot's value at the start of the next shot, the controller would hold the previous shot's
        // offset and would read a large NEGATIVE weight for the whole shot - which would also stop
        // the gravimetric cut-off from ever firing.
        _tailActive = true;
        _tailStartMs = millis();
        if (_tailMs == 0) {
            // The operator asked for no tail: the cup comes off straight away.
            _tailActive = false;
            _weightG = 0.0f;
            g_simBleScale.setWeight(0.0f);
            g_simBleScale.notifyNow();
        } else {
            Serial.printf("[PHYSICS] Post-shot weight tail started at %.2f g (%lu ms)\n",
                          _weightG, (unsigned long)_tailMs);
        }
    }

    if (!isBrewing) {
        // Machine idle: the grouphead vents and the pressure bleeds away.
        _pBar = std::max(0.0f, _pBar - _pBar * IDLE_BLEED_RATE * dt);
        _flowMlS = 0.0f;
        stepPostShotTail(dt);
        return;
    }

    const float dutyPct = g_simDimmer.pumpDrivePct();
    const SimPumpPreset& pump = SIM_PUMPS[_pumpIdx];

    // ---- 1. Pump delivery at the current pressure --------------------------------------------
    float qPump = 0.0f;
    if (dutyPct > pump.minActivationPct) {
        qPump = calculateVirtualFlow(_pBar, dutyPct, pump.stallBar, pump.maxFlowMlS);
    }

    // ---- 2. Headspace filling ------------------------------------------------------------------
    // While the chamber above the puck is still filling, the pump runs into an open volume: the
    // pressure only reflects the dry puck's pre-wet backpressure and nothing reaches the cup yet.
    if (_volumePumpedMl < HEADSPACE_ML) {
        _volumePumpedMl += qPump * dt;
        _flowMlS = qPump;
        const float prewet = (qPump > 0.0f) ? PREWET_BAR : 0.0f;
        _pBar += (prewet - _pBar) * (dt / PREWET_TAU_S);
        if (_pBar < 0.0f) _pBar = 0.0f;
        return;
    }

    // ---- 3. Puck dynamics ---------------------------------------------------------------------
    {
        const float vNorm = _volumePumpedMl / R_VOLUME_REF;
        const float rNorm = _puckR / mediumResistanceFor(pump);
        const float dR_swell      = (R_SWELL * _flowMlS * dt) / (1.0f + vNorm * vNorm);
        const float dR_compaction = (R_COMPACTION * _pBar * _pBar * dt) / (1.0f + rNorm * rNorm);
        const float dR_erosion    = (R_EROSION * _flowMlS * _pBar * dt);
        float nextR = _puckR + dR_swell + dR_compaction - dR_erosion;
        if (nextR < R_MIN) nextR = R_MIN;
        if (nextR > R_MAX) nextR = R_MAX;
        _puckR = nextR;
    }

    // ---- 4. Flow balance ----------------------------------------------------------------------
    // The pump pushes qPump into the pressurised volume; the puck lets sqrt(P/R) through. The
    // difference is what raises or lowers the pressure.
    const float qPuck = (_pBar > 0.05f) ? sqrtf(_pBar / std::max(0.05f, _puckR)) : 0.0f;
    _pBar += ((qPump - qPuck) / COMPLIANCE) * dt;
    if (_pBar < 0.0f) _pBar = 0.0f;

    // ---- 5. Cup yield -------------------------------------------------------------------------
    _flowMlS = qPuck;
    _volumePumpedMl += qPuck * dt;
    _weightG += qPuck * EXTRACT_DENSITY * dt;
}
