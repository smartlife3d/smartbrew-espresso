// =================================================================================================
//  sim_physics.h - Real-Time Espresso Hydraulic & Extraction Physics Model
//
//  PLANT MODEL NOTE (important):
//  The simulated pump is modelled around the SAME constants the controller uses in
//  include/pump_presets.h. The controller's PID feed-forward line is drawn through
//  (0 bar, minActivationPct) and (9 bar, holdNineBarPct). If the simulated pump disagreed with
//  that curve the controller would have to fight a large steady-state error with its integral
//  term, the feed-forward would contribute nothing, and every HIL run would be worthless.
//
//  The plant is a flow-balance model:
//
//      Q_pump(duty, P) = (duty/100) * maxFlow * max(0, 1 - (P/stall)^1.35)   [ml/s]
//      Q_puck(P)       = sqrt(P / R)                                         [ml/s]
//      dP/dt           = (Q_pump - Q_puck) / COMPLIANCE                        [bar/s]
//
//  The puck resistance R is calibrated so that the MEDIUM grind sits exactly on the controller's
//  feed-forward line: at holdNineBarPct duty the equilibrium pressure is 9 bar.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>

enum class PuckGrind : uint8_t {
    COARSE = 0,
    MEDIUM,
    FINE,
    CHOKED
};

// Mirrors the controller's PumpPreset (include/pump_presets.h). Keep the two in sync.
struct SimPumpPreset {
    const char* key;
    const char* name;
    float minActivationPct;   // % duty below which the solenoid does not break free
    float holdNineBarPct;     // % duty that holds 9 bar
    float maxFlowMlS;         // free-flow delivery at 0 bar
    float stallBar;           // bar at which the delivery decays to zero
};

extern const SimPumpPreset SIM_PUMPS[];
extern const uint8_t        SIM_PUMP_COUNT;

class SimPhysics {
public:
    void begin();

    // 100 Hz simulation step
    void update();

    // Reset the shot physics (called when a new extraction starts)
    void resetShot();

    // Zero the cup weight (scale tare). Safe to call from the BLE task: it only raises a flag
    // that the next simulation step consumes.
    void requestTare();

    // ---- the post-shot weight tail ------------------------------------------------------------
    // The cutoff reading is not the yield: when the pump stops, the last of the coffee is still
    // draining out of the grouphead, so the cup keeps getting heavier for a moment afterwards.
    // The controller watches the scale through that tail (src/control/weight_settle.h) and the
    // clients show the yield climbing, so the simulator has to model it - otherwise the settling
    // behaviour can never be exercised on the bench.
    //
    // The tail ends with the cup coming off the scale, which is what keeps the scale reading 0 g
    // whenever the machine is idle. That matters: the controller captures its software tare offset
    // from the raw value it has at that moment, so a stale offset would make it read a large
    // NEGATIVE weight for the whole next shot.
    uint32_t tailMs() const { return _tailMs; }
    void     setTailMs(uint32_t ms) { _tailMs = ms; }
    bool     tailActive() const { return _tailActive; }

    // Getters
    float pressureBar() const { return _pBar; }
    float flowMlPerSec() const { return _flowMlS; }
    float weightGrams() const { return _weightG; }
    float volumePumpedMl() const { return _volumePumpedMl; }
    float puckResistance() const { return _puckR; }
    PuckGrind grind() const { return _grind; }
    const SimPumpPreset& pump() const { return SIM_PUMPS[_pumpIdx]; }
    uint8_t pumpIndex() const { return _pumpIdx; }

    // Configuration / Barista controls
    void setGrind(PuckGrind g);
    void setResistance(float r);
    void setWeight(float g);
    void setPumpIndex(uint8_t idx);

    // Resistance of the MEDIUM grind for the currently selected pump, i.e. the value that puts
    // the 9 bar operating point exactly on the controller's feed-forward line.
    static float mediumResistanceFor(const SimPumpPreset& p);

private:
    void step(float dt);
    void stepPostShotTail(float dt);
    float baseResistance() const;

    float _pBar = 0.0f;
    float _flowMlS = 0.0f;
    float _weightG = 0.0f;
    float _volumePumpedMl = 0.0f;
    float _puckR = 2.398f;    // Darcy puck resistance, recalibrated per pump preset
    PuckGrind _grind = PuckGrind::MEDIUM;

    // A hand-tuned resistance (CLI "puck <ohms>") survives a shot restart; a grind selection
    // overwrites it.
    bool  _customResistance = false;
    float _customResistanceValue = 2.398f;

    uint8_t _pumpIdx = 0;     // index into SIM_PUMPS, matches the controller's default (ULKA EX5)

    uint32_t _lastTickUs = 0;
    bool     _wasBrewing = false;

    // ---- the post-shot weight tail ------------------------------------------------------------
    uint32_t _tailMs = 1000UL;      // how long the tail lasts; 0 disables it
    uint32_t _tailStartMs = 0;
    uint32_t _tailLogMs = 0;        // when the last tail progress line was printed
    bool     _tailActive = false;

    std::atomic<bool> _tareRequested{false};

    // ---- plant constants --------------------------------------------------------------------
    static constexpr float HEADSPACE_ML      = 12.0f;   // chamber volume filled before pressurising
    static constexpr float PREWET_BAR        = 0.5f;    // pressure while the chamber is filling
    static constexpr float PREWET_TAU_S      = 0.08f;   // pre-wet pressure time constant
    // Line compliance in ml/bar: how much water it takes to raise the pressure by 1 bar once the
    // headspace is full. Measured on the bench machine with a blind basket: 7.1 ml for 3 -> 8 bar
    // (1.5 ml/bar, Classic 9-Bar) and 1.2 ml/bar (Gentle Lever), matching the controller's own
    // complianceMlPerBar default of 1.5. It used to be 0.25 ml/bar, six times stiffer than the
    // machine, which made the controller's pressure loop oscillate on the simulator only.
    static constexpr float COMPLIANCE        = 1.5f;
    static constexpr float EXTRACT_DENSITY   = 1.02f;   // g/ml of espresso in the cup
    static constexpr float IDLE_BLEED_RATE   = 5.0f;    // 1/s, grouphead venting while idle
};

extern SimPhysics g_simPhysics;
