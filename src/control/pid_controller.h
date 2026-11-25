// =================================================================================================
//  pid_controller.h - Feed-forward + clamped PID with virtual flow estimation
//                     docs/PumpFlow.md
//
//      u_ff     = minActivationPct + (holdNineBarPct - minActivationPct)/9 * P_target
//      Q_virt   = (PSM%/100) * Q_maxFlow * max(0, 1 - (P_filtered/P_stallBar)^1.35)
//      u_total  = clamp(u_ff + Kp*e + Ki*int(e) - Kd*dP/dt, 0, phaseMaxPowerPct)
//
//  Two deliberate choices:
//    * Derivative on MEASUREMENT, not on error - a profile phase change steps the setpoint and
//      de/dt would spike (docs/Architecture.md).
//    * Headspace anti-windup - the integral is frozen below `headspaceBar` because an empty puck
//      presents ~0 bar resistance and the error would otherwise accumulate for the whole of
//      pre-infusion (docs/Architecture.md).
//
//  ADAPTIVE MODE (pressureCtrlMode = 1, the default) adds three things, borrowed from GaggiMate's
//  pressure controller and Gaggiuino's pump logic. Mode 0 is the loop above, unchanged.
//    1. Setpoint shaping: the target goes through a 2nd-order filter (2 Hz, damping 1.2) whose
//       slope is capped at 9 bar/s, so a fast ramp cannot run ahead of what the pump can follow.
//    2. Integral only near the target: it accumulates while |error| < max(0.5 bar, 30 % of the
//       target), or below the target once the shaped setpoint has settled. A ramp or a headspace
//       fill can no longer bank authority that then pushes the pressure past the target.
//    3. Feed-forward cut at the target: the target-based feed-forward is the duty a puck needs to
//       HOLD the pressure. Past the target it only adds water the pressure has to get rid of, so it
//       is scaled to 0 over 0.25 bar - judged on the pressure 0.25 s ahead while it is rising,
//       because the strokes already under way keep pushing it up after the duty drops (measured:
//       +0.4 bar after the cut on a sealed basket at 2.5 bar/s). While the pressure is already
//       falling a 20 % trickle is kept, so it does not undershoot.
//    4. Puck-aware feed-forward: the water leaving through the puck is estimated from the pump
//       model and the pressure slope (compliance), scaled to the target pressure and turned into
//       the duty that delivers it - the hold duty of THIS puck, not of the preset's reference one.
//    5. Hold-off above the target: more than 0.15 bar over and not falling -> pump off, with the
//       integral tracked to the "pump off" point so the loop resumes from there.
//    6. For a low target (a soak) the headspace freeze drops to half the target.
//    7. Ramp feed-forward: while the shaped target rises, add the duty for compliance x slope -
//       the water it takes just to raise the line pressure - so a ramp is not left ~1-2 s behind.
//    8. Fill: while the pressure, projected 0.5 s ahead, is still below the band around the
//       target, run at the phase's power ceiling instead of tapering proportionally.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include "config_schema.h"
#include "pump_presets.h"
#include "pump_physics.h"
#include "control/flow_calibration.h"

class PidController {
public:
    void begin();
    void reset(float initialMeas);

    // Advances the loop by one tick. `dt` comes from the measured interval between calls so the
    // integral stays correct even if a control iteration is late.
    float update(float measuredBar, float targetBar, uint8_t phaseMaxPct, float dt);

    // ---- virtual flow (ml/s), docs/PumpFlow.md ------------------------------------------------------
    float virtualFlow(float measuredBar, float dutyPct) const;

    // ---- feed-forward alone, exposed for the UI so users can see what the curve predicts -------
    float feedForward(float targetBar) const;

    // ---- FLOW phase feed-forward: the duty that makes the pump model deliver `targetFlowMlS` at
    //      the measured back-pressure (the inverse of virtualFlow), floored at the pump's
    //      activation duty so a small target still strokes the solenoid ------------------------
    float flowFeedForward(float targetFlowMlS, float measuredBar) const;
    float pqExponent() const { return _pqExponent; }

    // ---- calibration against the scale (src/control/flow_calibration.h) ---------------------
    // Multiplies the preset's free flow, so the virtual flow and the FLOW feed-forward both follow
    // the calibrated pump. A single aligned float: Core 0 may set it, Core 1 reads it.
    void  setFlowScale(float s)  { _flowScale = flowScaleClamp(s); }
    float flowScale() const      { return _flowScale; }
    float maxFlowMlS() const     { return _pump ? _pump->maxFlowMlS * _flowScale : 0.0f; }

    // ---- introspection -----------------------------------------------------------------------
    float lastOutput() const { return _u; }
    float integralTerm() const { return _kiTerm; }
    float proportionalTerm() const { return _kpTerm; }
    float derivativeTerm() const { return _kdTerm; }
    float lastError() const { return _lastError; }
    float lastTarget() const { return _lastTarget; }
    uint8_t saturationCount() const { return _satCount; }
    bool    integralFrozen() const { return _integralFrozen; }

    // Manual power override from the web UI (`OVERRIDE_PRESSURE` / manual slider).
    void setManualPct(float pct)   { _manual = true;  _manualPct = pct < 0 ? 0 : (pct > 100 ? 100 : pct); }
    void clearManual()             { _manual = false; _manualPct = 0.0f; }
    bool    isManual() const       { return _manual; }
    float   manualPct() const      { return _manualPct; }

    // Populated from config on begin(): explicit gains win, otherwise the pump preset's defaults.
    void applyConfig(const SystemConfig& c);

private:
    const PumpPreset* _pump = nullptr;
    float _kp = 0, _ki = 0, _kd = 0;
    float _headspaceBar = 1.5f;
    float _pqExponent = SB_PUMP_PQ_EXPONENT_DEFAULT;
    volatile float _flowScale = 1.0f;

    float _integral   = 0.0f;
    float _prevMeas   = 0.0f;
    float _dMeasFiltered = 0.0f;
    float _u          = 0.0f;
    float _kiTerm     = 0.0f;
    float _kpTerm     = 0.0f;
    float _kdTerm     = 0.0f;
    float _lastError  = 0.0f;
    float _lastTarget = 0.0f;
    uint8_t _satCount = 0;
    bool    _integralFrozen = false;
    // Adaptive mode (pressureCtrlMode = 1)
    uint8_t _mode     = 1;
    bool    _spInit   = false;
    float   _spFilt   = 0.0f;   // shaped setpoint (bar)
    float   _spVel    = 0.0f;   // its slope (bar/s)
    uint32_t _lastUpdateMs = 0; // a gap means a non-pressure phase ran in between
    float   _complianceMlPerBar = 1.5f;
    float   _rampComplianceMlPerBar = 1.5f;
    float   _puckFlowEst  = 0.0f;   // water leaving through the puck (ml/s), low-passed
    float   _puckFfWeight = 0.0f;   // 0..1 blend of the puck-aware feed-forward

    bool  _manual = false;
    float _manualPct = 0.0f;
};

extern PidController g_pid;