# Pump Flow, Cup Flow and the Weight Cutoff

How the controller estimates the flow of water through the puck, measures the flow into the cup,
and decides when to stop a shot on weight. The code lives in:

| File | What it holds |
|---|---|
| `include/pump_physics.h` | Pump P-Q model, its inverse (FLOW feed-forward), hydraulic compliance |
| `src/control/scale_flow.h` | Time-windowed weight slope, cutoff slope clamp, learned drip lag |
| `src/control/flow_calibration.h` | Calibrating the pump model against the scale |
| `src/control/pid_controller.cpp` | Applies the pump preset, exponent and calibration |
| `src/brew_controller.cpp` | Puck-flow estimate, FLOW phases, lag and calibration learning |
| `src/scale/scale_manager.cpp` | Weight regression and the predictive cutoff |

All three headers are header-only and free of Arduino dependencies. The host regression tests in
`simulator-esp32s3/test/host/test_pump_flow.cpp` run them directly (`./test/host/run.sh`).

---

## 1. The pump model (virtual flow)

A vibratory pump driven by pulse skipping delivers one stroke per fired half-cycle, so delivered
volume is linear in duty. Back-pressure reduces each stroke along the pump's P-Q curve:

```
Q_pump = (duty / 100) × Q_max × max(0, 1 − (P / P_stall) ^ n)
```

| Symbol | Meaning | Source |
|---|---|---|
| `Q_max` | free-discharge flow at 0 bar and 100 % duty | pump preset `maxFlowMlS` × `pumpFlowScale` |
| `P_stall` | pressure at which the pump delivers nothing | pump preset `stallBar` |
| `n` | shape of the curve | preset `pqExponent` (1.35), overridden by config `pumpPqExponent` when non-zero |

The presets are in `include/pump_presets.h`. Every preset currently uses `n = 1.35`, because no
pump has been measured on this rig yet. Enter a measured value per pump, or set `pumpPqExponent`.

At start-up the controller logs a warning when a pump's stall pressure is below the safety cut-off:
the virtual flow would read zero over a pressure range the line can still reach.

## 2. Puck flow: the hydraulic compliance correction

While the pressure rises, part of what the pump delivers is stored (in the line, the group and the
headspace) instead of passing the puck. While it falls, that water is released through the puck:

```
Q_puck = Q_pump − C × dP/dt          (C = complianceMlPerBar, default 1.5 ml/bar; 0 = off)
```

`dP/dt` comes from `PuckFlowRate`. The pressure ripple of a steady shot (a few tenths of a bar)
would otherwise differentiate into ±1–2 bar/s and swing the estimate by several ml/s, so:

- the rate is low-pass filtered with α = 0.02 per 10 ms tick (τ ≈ 0.5 s), and
- anything inside ±1 bar/s is ignored (dead band).

±0.3 bar of ripple then moves the estimate by less than 0.1 ml/s, and a real 3 bar/s ramp keeps
about two thirds of its correction. The result, smoothed once more, is the `flow_virt` value in
telemetry and the orange **Flow** curve on every chart. FLOW phases and the `flow_ge` / `flow_le`
triggers use the scale's cup flow when a scale reports one, and fall back to this estimate.

To measure `C` for your machine, run a shot at fixed duty and compare the integrated virtual flow
with the scale's yield over the ramp.

## 3. FLOW phases

A FLOW phase targets a puck flow in ml/s. The duty is:

```
duty = clamp( FF(target, P) + 12 × (target − measured), 0, phase ceiling )
FF(target, P) = max( minActivationPct, target / (Q_max × (1 − (P/P_stall)^n)) × 100 )
```

`FF` is the inverse of the pump model at the pressure the line is at right now. It never goes below
the preset's activation duty, below which the solenoid does not stroke at all. At or past the stall
pressure it asks for 100 %, and the phase ceiling decides what is allowed. `measured` is the scale's
cup flow when a scale reports one, and the puck-flow estimate otherwise.

Example, Ulka EX5 at 6 bar: the pump can deliver 10.8 × (1 − 0.4^1.35) = 7.67 ml/s at full duty, so
a 2 ml/s target needs 26 % duty.

## 4. Cup flow from the scale

The scale reports weight. The cup flow is the ordinary-least-squares slope of the weight over the
last **`scaleSlopeWindowMs`** milliseconds (default 1500, range 500–4000):

```
slope = (n Σ t·w − Σ t Σ w) / (n Σ t² − (Σ t)²)      clamped to 0 … 20 g/s
```

- The window is sized in **time**, not in samples. The supported scales report at very different
  rates (about 20 Hz down to 5 Hz), and a fixed sample count would cover a different stretch of the
  shot on each one.
- A slope needs at least 3 readings spanning at least 300 ms.
- The window keeps filling during the start-of-shot lockout (`scaleLockoutSec`). The lockout only
  suppresses the cutoff decision, so the slope is already primed when the lockout lifts.

The value goes out as `flow_weight` (g/s). Every client draws it as the dashed pale-amber
**Cup Flow (scale)** curve, converted to ml/s with a density of 1.03 g/ml.

## 5. Predictive weight cutoff

```
stop when  W ≥ W_target − clamp(slope, 0.5, 6.0) × scaleLagSec
```

The coffee already between the puck and the cup still lands after the pump stops, so the shot is
stopped early by the amount that will still arrive.

### Learning the lag

With `scaleLagAutoLearn` on (default), every shot that the cutoff ended teaches the lag. The shot's
post-shot settling window must also have closed normally: the weight stopped moving, or the cup was
lifted. Then:

```
lag_measured = (W_settled − W_at_cutoff) / clamp(slope_at_cutoff, 0.5, 6.0)
scaleLagSec  ← scaleLagSec + 0.1 × (lag_measured − scaleLagSec)
```

A shot is ignored when the slope was below 0.3 g/s, the drip was negative, or the drip was over
6 g (a bumped scale, not a drip). The lag stays within 0–3 s. It converges in about ten shots.

## 6. Calibrating the pump model against the scale

With `pumpFlowAutoCal` on (default), every shot compares the model with the cup over its steady
part:

| Gate | Value |
|---|---|
| line pressure | ≥ 4 bar |
| \|dP/dt\| | < 0.5 bar/s |
| scale | connected, not stale, lockout over |
| control | not in manual |
| cup flow / model flow | ≥ 0.5 / ≥ 0.3 ml/s |

With at least 3 s of such data, and a ratio between 0.5 and 2.0:

```
ratio          = Σ cup flow / Σ model flow
pumpFlowScale ← clamp(pumpFlowScale × (1 + 0.25 × (ratio − 1)), 0.5, 1.5)
```

`pumpFlowScale` multiplies the preset's free flow, so the virtual flow and the FLOW feed-forward
both follow the calibrated pump. Pressure phases are not affected: their feed-forward uses the
preset's duty-to-hold-pressure figures. Set the scale back to 1.00 in Settings to return to the
factory model.

## 7. Where the learning happens

The learned lag and the calibration are **calculated** on the control task (Core 1) when the shot is
finalised. They travel to the telemetry task (Core 0) on the `SHOT_COMPLETED` event, and that task
applies and **saves** them. The control task never writes flash.

## 8. Settings

| Key (REST `/api/v1/config`, `config.json`) | Default | Range |
|---|---|---|
| `complianceMlPerBar` | 1.5 | 0 – 10 (0 = off) |
| `pumpPqExponent` | 0 (use preset) | 0, or 0.5 – 4 |
| `scaleSlopeWindowMs` | 1500 | 500 – 4000 |
| `scaleLagSec` | 0.6 | 0 – 3 |
| `scaleLagAutoLearn` | 1 | 0 / 1 |
| `pumpFlowScale` | 1.0 | 0.5 – 1.5 |
| `pumpFlowAutoCal` | 1 | 0 / 1 |

All of them are in the web dashboard under Settings → Advanced.
