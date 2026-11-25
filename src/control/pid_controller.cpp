// =================================================================================================
//  pid_controller.cpp - see pid_controller.h
// =================================================================================================

#include "pid_controller.h"
#include "logger.h"
#include "pump_physics.h"
// Setpoint shaping cut-off. 2 Hz lags a ramp by ~0.2 s; 1 Hz lagged it by ~0.4 s.
#ifndef SB_SP_FILTER_HZ
#define SB_SP_FILTER_HZ 2.0f
#endif
// Compliance used by the ramp feed-forward (ml/bar). Negative = the configured complianceMlPerBar,
// 0 = ramp feed-forward off.
#ifndef SB_RAMP_COMPLIANCE
#define SB_RAMP_COMPLIANCE -1.0f
#endif

void PidController::begin() {
    applyConfig(g_config.cfg());
    reset(0.0f);
}

void PidController::applyConfig(const SystemConfig& c) {
    _pump = &pumpPreset(c.pumpModel);
    _kp = (c.kp > 0.0f) ? c.kp : _pump->kp;
    _ki = (c.ki > 0.0f) ? c.ki : _pump->ki;
    _kd = (c.kd > 0.0f) ? c.kd : _pump->kd;
    _headspaceBar = (float)c.headspaceAntiWindupBar * 0.1f;
    _mode = c.pressureCtrlMode ? 1 : 0;
    _complianceMlPerBar = c.complianceMlPerBar > 0.0f ? c.complianceMlPerBar : 1.5f;
    _rampComplianceMlPerBar = SB_RAMP_COMPLIANCE >= 0.0f ? SB_RAMP_COMPLIANCE : _complianceMlPerBar;
    // The P-Q shape: an explicit config value wins, otherwise the pump preset's.
    _pqExponent = (c.pumpPqExponent > 0.0f) ? c.pumpPqExponent : _pump->pqExponent;
    if (_pqExponent <= 0.0f) _pqExponent = SB_PUMP_PQ_EXPONENT_DEFAULT;
    setFlowScale(c.pumpFlowScale);

    SB_LOGI("PID gains %s: Kp=%.2f Ki=%.2f Kd=%.2f, headspace freeze < %.2f bar, %s loop",
            _pump->name, _kp, _ki, _kd, _headspaceBar,
            _mode ? "adaptive (shaped setpoint, banded integral, FF cut above target)" : "classic");
    SB_LOGI("Pump model %s: %.1f ml/s free flow (x%.3f calibration = %.2f), stall %.1f bar, "
            "P-Q exponent %.2f%s, compliance %.2f ml/bar",
            _pump->name, _pump->maxFlowMlS, _flowScale, maxFlowMlS(), _pump->stallBar,
            _pqExponent, (c.pumpPqExponent > 0.0f) ? " (config)" : "", c.complianceMlPerBar);

    // The pump model is only drawn up to its stall pressure. A safety cut-off above it means the
    // flow model reads zero over a range the controller will still let the line reach.
    if (_pump->stallBar < g_config.safetyPressureBar()) {
        SB_LOGW("Pump %s stalls at %.1f bar, below the %.1f bar safety cut-off - the virtual "
                "flow reads 0 above the stall pressure", _pump->name, _pump->stallBar,
                g_config.safetyPressureBar());
    }
}

void PidController::reset(float initialMeas) {
    _integral   = 0.0f;
    _prevMeas   = initialMeas;
    _dMeasFiltered = 0.0f;
    _u          = 0.0f;
    _kiTerm     = 0.0f;
    _kpTerm     = 0.0f;
    _kdTerm     = 0.0f;
    _lastError  = 0.0f;
    _lastTarget = 0.0f;
    _satCount   = 0;
    _integralFrozen = false;
    _spInit     = false;
    _spFilt     = 0.0f;
    _spVel      = 0.0f;
    _lastUpdateMs = 0;
    _puckFlowEst  = 0.0f;
    _puckFfWeight = 0.0f;
    _manual     = false;
    _manualPct  = 0.0f;
}

// -------------------------------------------------------------------------------------------------
float PidController::feedForward(float targetBar) const {
    // docs/PumpFlow.md - the feed-forward line is drawn through (0 bar, minActivationPct) and
    // (9 bar, holdNineBarPct). Clamped to 0..100 so a 12 bar target cannot demand 180 % duty.
    float ff = _pump->minActivationPct +
               ((_pump->holdNineBarPct - _pump->minActivationPct) / 9.0f) * targetBar;
    if (ff < 0.0f)   ff = 0.0f;
    if (ff > 100.0f) ff = 100.0f;
    return ff;
}

// -------------------------------------------------------------------------------------------------
float PidController::virtualFlow(float measuredBar, float dutyPct) const {
    return calculateVirtualFlow(measuredBar, dutyPct, _pump->stallBar, maxFlowMlS(),
                                _pqExponent);
}

// -------------------------------------------------------------------------------------------------
float PidController::flowFeedForward(float targetFlowMlS, float measuredBar) const {
    if (targetFlowMlS <= 0.0f) return 0.0f;
    float duty = calculateDutyForFlow(targetFlowMlS, measuredBar, _pump->stallBar,
                                      maxFlowMlS(), _pqExponent);
    // Below its activation duty the solenoid does not stroke at all, so a small flow target would
    // otherwise be fed a duty that delivers nothing.
    if (duty < _pump->minActivationPct) duty = _pump->minActivationPct;
    return duty > 100.0f ? 100.0f : duty;
}

// -------------------------------------------------------------------------------------------------
float PidController::update(float measuredBar, float targetBar, uint8_t phaseMaxPct, float dt) {
    if (dt <= 0.0f || dt > 0.5f) dt = g_config.cfg().loopDt;
    if (dt <= 0.0f) dt = 0.01f;

    // Manual power mode from the UI bypasses the loop entirely but still reports the FF curve so the
    // user can see how their manual setting compares with the model.
    if (_manual) {
        _lastTarget = targetBar;
        _lastError  = targetBar - measuredBar;
        _u          = _manualPct;
        _kpTerm = _kiTerm = _kdTerm = 0.0f;
        _integral = 0.0f;
        return _u;
    }

    // ---- setpoint shaping (adaptive mode) ------------------------------------------------------
    float target = targetBar;
    const uint32_t nowMs = millis();
    // A power / flow / pause phase in between does not call this loop. Restart the shaping on the
    // new target instead of gliding from the last pressure phase's value.
    if (_lastUpdateMs != 0 && (nowMs - _lastUpdateMs) > 100) _spInit = false;
    _lastUpdateMs = nowMs;
    if (_mode == 1) {
        if (!_spInit) {
            // Start on the target itself: the first phase begins without a lag.
            _spFilt = targetBar;
            _spVel  = 0.0f;
            _spInit = true;
        }
        constexpr float kOmega   = 2.0f * 3.14159265f * SB_SP_FILTER_HZ;
        constexpr float kDamping = 1.2f;
        constexpr float kMaxRate = 9.0f;                       // bar/s
        const float acc = kOmega * kOmega * (targetBar - _spFilt) - 2.0f * kDamping * kOmega * _spVel;
        _spVel += acc * dt;
        if (_spVel >  kMaxRate) _spVel =  kMaxRate;
        if (_spVel < -kMaxRate) _spVel = -kMaxRate;
        _spFilt += _spVel * dt;
        target = _spFilt;
    }
    const float e   = target - measuredBar;
    _lastTarget     = target;
    _lastError      = e;

    // ---- derivative on measurement with low-pass filtering (anti-kick & anti-noise) ----------
    const float dP = (measuredBar - _prevMeas) / dt;
    _prevMeas = measuredBar;
    constexpr float kAlphaD = 0.20f; // tau ~ 40 ms at 100 Hz (fc ~ 4 Hz)
    _dMeasFiltered = kAlphaD * dP + (1.0f - kAlphaD) * _dMeasFiltered;
    _kdTerm = -_kd * _dMeasFiltered;

    // ---- proportional --------------------------------------------------------------------------
    _kpTerm = _kp * e;

    // ---- integral with headspace anti-windup --------------------------------------------------
    // Classic: frozen below the headspace pressure. Adaptive: also only inside a band around the
    // target, so a ramp or a headspace fill cannot wind it up.
    bool integrating = (measuredBar >= _headspaceBar);
    if (_mode == 1) {
        // A low target (a 1.5 bar soak) sits at or below the headspace pressure, so the classic
        // freeze would keep the integral off for the whole phase and the pressure would hang
        // short of it (measured: 1.1 bar against 1.5 bar for 8 s). Freeze only below half the
        // target there; the band below still keeps the headspace fill from winding it up.
        integrating = measuredBar >= fminf(_headspaceBar, 0.5f * target);
        // Near the target: always. Below it and outside the band: only once the shaped setpoint has
        // settled, so a ramp or the headspace fill cannot wind it up, but a fast-running puck that
        // the feed-forward cannot hold still gets pulled up to the target.
        const float band = fmaxf(0.5f, 0.3f * target);
        const bool  settled = fabsf(_spVel) < 0.3f && fabsf(targetBar - _spFilt) < 0.2f;
        integrating = integrating && (fabsf(e) < band || (e > 0.0f && settled));
    }
    _integralFrozen = !integrating;
    if (integrating) {
        // Clamp the raw accumulator so a long shot at a rising target can never bank an
        // unrecoverable amount of authority.
        _integral += e * dt;
        const float iLimit = 100.0f / (_ki > 0.0f ? _ki : 1.0f);
        if (_integral >  iLimit) _integral =  iLimit;
        if (_integral < -iLimit) _integral = -iLimit;
    }
    _kiTerm = _ki * _integral;

    // ---- feed-forward + sum, clamped by the per-phase ceiling ---------------------------------
    float ff = feedForward(target);
    if (_mode == 1) {
        // Puck-aware feed-forward (Gaggiuino's "duty to maintain the flow"). The target line above
        // assumes one fixed puck - the one that needs holdNineBarPct at 9 bar - and a real puck can
        // need half of that, which the integral then takes tens of seconds to learn. Estimate the
        // water actually leaving through the puck from the pump model and the pressure slope,
        // scale it to the target pressure (orifice flow ~ sqrt(P)) and feed the duty that delivers
        // it. Used once the puck carries a steady flow; blended in over 1 s.
        const float qPump = virtualFlow(measuredBar, _u);
        const float qPuck = qPump - _complianceMlPerBar * _dMeasFiltered;
        _puckFlowEst += (qPuck - _puckFlowEst) * (dt / 0.8f);   // ~0.8 s low-pass
        const bool steady = measuredBar > 2.0f && target > 2.0f && _puckFlowEst > 0.2f;
        _puckFfWeight += ((steady ? 1.0f : 0.0f) - _puckFfWeight) * (dt / 1.0f);
        if (_puckFfWeight > 0.01f) {
            const float qAtTarget = _puckFlowEst * sqrtf(target / measuredBar);
            const float ffPuck = flowFeedForward(qAtTarget, target);
            ff = ff + (ffPuck - ff) * _puckFfWeight;
        }
        // Ramp feed-forward: a rising target needs extra water just to raise the pressure in
        // the line, the group and the headspace - compliance x slope - on top of what the puck
        // takes. Without it the loop only reacts once an error has built up, and a 3 s ramp
        // ended ~1-2 s behind the profile. GaggiMate's controller inverts the same plant model.
        if (_spVel > 0.0f) {
            const float qFull = virtualFlow(measuredBar, 100.0f);
            if (qFull > 0.1f) ff += 100.0f * (_rampComplianceMlPerBar * _spVel) / qFull;
        }
    }
    if (_mode == 1) {
        // Brake on where the pressure is heading, not where it is: while it rises, look
        // kLookaheadS ahead. In a stiff line the last few strokes before the target land after it,
        // and a pressure the pump has pushed past the target can only come down through the puck.
        constexpr float kLookaheadS = 0.25f;
        constexpr float kCutBar     = 0.25f;   // feed-forward reaches 0 this far past the target
        const float rising    = _dMeasFiltered > 0.0f ? _dMeasFiltered : 0.0f;
        const float overshoot = measuredBar + rising * kLookaheadS - target;
        if (overshoot > 0.0f) {
            float scale = 1.0f - overshoot / kCutBar;
            if (scale < 0.0f) scale = 0.0f;
            float cut = ff * scale;
            if (_dMeasFiltered < -0.2f && cut < 0.2f * ff) cut = 0.2f * ff;   // already falling
            ff = cut;
        }
    }
    float       out = ff + _kpTerm + _kiTerm + _kdTerm;
    // Fill: far below the target - the headspace above the puck still filling, the start of a
    // pre-infusion - run at the phase's power ceiling until the pressure, projected kFillAheadS
    // ahead, enters the band around the target; the band, the brake above and the hold-off take
    // it from there. Proportional control alone tapers the duty as the gap closes, so a 3 bar
    // pre-infusion crept up at 40-55 % and was still 0.8 bar short after 4 s (Gaggiuino likewise
    // drives hard while more than 2 bar below the target).
    if (_mode == 1 && target > 0.5f) {
        constexpr float kFillAheadS = 0.5f;
        const float band      = fmaxf(0.3f, 0.25f * target);
        const float rising    = _dMeasFiltered > 0.0f ? _dMeasFiltered : 0.0f;
        const float projected = measuredBar + rising * kFillAheadS;
        if (projected < target - band) out = 1000.0f;   // clamped to the ceiling below
    }
    const float raw = out;   // before the hold-off below, for the anti-windup
    // Gaggiuino's rule: above the target, do not pump unless the pressure is already falling.
    // A small positive sum of leftover integral, a partly cut feed-forward and a derivative that
    // turns positive while the line leaks down slowly otherwise adds single strokes on top of a
    // pressure that is already too high (measured: 1-8 % duty at +0.23 bar on a sealed basket).
    // Through a puck an over-target pressure falls quickly, so the trickle above still applies.
    bool heldOff = false;
    if (_mode == 1 && measuredBar > target + 0.15f && _dMeasFiltered > -0.3f && out > 0.0f) {
        out = 0.0f;
        heldOff = true;
        // Output tracking (as in GaggiMate's pressure/flow hand-off): pull the integral down to
        // where the sum is 0, so the loop resumes from "pump off" instead of jumping back to the
        // full feed-forward. The target-based feed-forward over-estimates what a puck needs on a
        // falling target, and without this the integral needed ~30 s to learn it, so the pressure
        // rode the hold-off margin and the pump toggled 0 <-> 40 % for the whole decline.
        if (_ki > 0.0f) {
            const float iLimit = 100.0f / _ki;
            float iTrack = -(ff + _kpTerm + _kdTerm) / _ki;
            if (iTrack < -iLimit) iTrack = -iLimit;
            if (_integral > iTrack) _integral = iTrack;
            _kiTerm = _ki * _integral;
        }
    }
    const float ceiling = phaseMaxPct > 100.0f ? 100.0f : (float)phaseMaxPct;
    if (out > ceiling) { out = ceiling; _satCount++; }
    if (out < 0.0f)    { out = 0.0f;   _satCount++; }
    // Conditional integration: if we are saturated *and* pushing further into the rail, unwind.
    // Judged on the sum before the hold-off: the hold-off zeroing the output is not saturation,
    // and treating it as such cancelled the negative integral that a falling target needs, so
    // the pressure trailed the target by the 0.15 bar hold-off margin and the pump toggled on
    // and off (measured on a lever decline: +0.1-0.3 bar, duty 0 <-> 40 %).
    if (raw >= ceiling && raw < 999.0f && e > 0.0f && integrating) _integral -= e * dt * 2.0f;
    if (raw <= 0.0f    && e < 0.0f && integrating && !heldOff) _integral -= e * dt * 2.0f;

    _u = out;
    return _u;
}

PidController g_pid;