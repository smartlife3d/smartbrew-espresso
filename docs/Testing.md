# Testing the controller

This page describes how to verify a build before it goes near a machine: first the automated
tests, then four bench stages on the bare controller board. Each stage assumes the one before it
passed. Building and flashing are in [Operation.md](Operation.md), the design behind the checks in
[Architecture.md](Architecture.md), the wiring in [Hardware.md](Hardware.md).

## Automated tests

### Host regression tests

These tests compile real source files of the controller and the simulator on your computer and
run them against small stubs. They need only a C++17 compiler (`g++` by default; set
`CXX=clang++` to change it), no ESP32 and no PlatformIO.

```bash
cd simulator-esp32s3
./test/host/run.sh
```

The script ends with `Host regression tests PASSED` (exit 0) or `Host regression tests FAILED`
(exit 1).

| Test file | Compiles | Checks |
|---|---|---|
| `test_physics.cpp` | `simulator-esp32s3/src/sim_physics.cpp` | The simulated hydraulics advance when called in a tight loop, reach the 9-bar pressure at the feed-forward duty, react to a duty change, model the post-shot drip |
| `test_button.cpp` | `simulator-esp32s3/src/sim_button.cpp` | The simulated machine reacts to every button pulse, not only the first, so it can be started and stopped |
| `test_transitions.cpp` | `include/profile_types.h` | The reason logged when a phase ends: weight, pressure, flow and time triggers, OR / AND logic, timeouts |
| `test_weight_settle.cpp` | `src/control/weight_settle.h` | When the post-shot yield is final: weight stable, cup lifted, scale lost, time cap; the ledger records the settled yield |
| `test_settle_chain.cpp` | simulator physics + `weight_settle.h` | The drip the simulator produces ends where the controller's settling rules say it does |
| `test_shot_tail.cpp` | `src/control/shot_tail.h` | Post-shot samples go to the finished shot, only raise its weight and peak, and do not lengthen its brew time |
| `test_pump_flow.cpp` | `include/pump_physics.h`, `src/control/scale_flow.h`, `src/control/flow_calibration.h` | Pump flow model and compliance, cup flow from the scale, learned cutoff lag, pump-flow calibration ([PumpFlow.md](PumpFlow.md)) |

Run them after changing any of those files. The console's own unit tests are
`tools/test_brewmon.py`.

### Pressure loop plant check
`tools/pid_plant_sim/run.sh` compiles the real `src/control/pid_controller.cpp` with the default
settings (Ulka EX5 preset gains, `pressureCtrlMode` 1) and runs Classic 9-Bar against a lumped
hydraulic model: the firmware's pump model delivered in whole half-cycle strokes, line compliance
0.3–3 ml/bar, and a puck passing 1–3.5 ml/s at 9 bar or a sealed blind basket with an OPV at
10 bar. It prints the overshoot in pre-infusion and at 9 bar, the dip after the ramp and the mean
tracking error for the classic and the adaptive loop. Run it after changing the pressure loop.
On the bench machine (blind basket, OPV about 10 bar) the default adaptive loop peaked at 9.2 bar,
in line with the model's +0.17–0.22 bar.

### HIL simulator

A second ESP32-S3 running the simulator firmware plays the machine: zero-cross pulses while its
pump runs, a modelled transducer voltage, the button, and a Bookoo scale (`Bookoo_SIM`). The
controller then pulls complete shots with weight cutoff and post-shot drip, and faults can be
injected from the simulator's console. See [`../simulator-esp32s3/README.md`](../simulator-esp32s3/README.md).

## Bench preparation

**Target:** the controller board alone (ESP32-S3-DevKitC-1, N8R8 or N16R8). No pump, no boiler,
no machine.

**You need:** a data USB-C cable, a multimeter, a breadboard, an LED with a 330 Ω resistor, a
10 kΩ potentiometer, and a function generator or second microcontroller that can output a
0–3.3 V square wave at 100 Hz, 120 Hz and 1 kHz. Optional: an ADS1115 module, an oscilloscope, a
BLE scale or the HIL simulator.

> **Mains safety.** Stages 1–4 use no mains. Do not connect the dimmer, the pump or any AC wiring
> until the pump preview at the end, and never with the boiler heated and unattended.

**Firmware.** Most steps use commands that exist only in the bench build:

```bash
pio run -e esp32s3-bench -t upload --upload-port /dev/cu.usbmodem101
./monitor.sh -p /dev/cu.usbmodem101 --reset --log
```

`./flash.sh` always flashes the normal build, where bench commands answer
`ERR: bench feature disabled in production build (-DSB_BENCH=1 required)`. Put the normal build
back with `./flash.sh` when you are done.

**Reading the log.** Every line starts with `I|`, `W|` or `E|`; the examples below leave that out.
The console timestamps each line, which the timing checks rely on.

**Boot banner.** After `--reset` the console shows (keep a copy as your baseline):

```
  UNIVERSAL ESPRESSO PRESSURE & FLOW PROFILING CONTROLLER
  firmware 1.5.0   chip ESP32-S3 rev 0   2 cores @ 240 MHz
  flash 8 MB   PSRAM 8 MB   littlefs 1920 kB   reset: ...
Tasks: ctrl(Core 1, 100Hz) tele(Core 0, 20Hz brew/2Hz idle) loop(services) - running.
Profile 'Classic 9-Bar' with 4 phase(s). Press start to begin a shot.
No zero-cross signal yet - awaiting mains signal. On the bench, drive a
50 Hz / 60 Hz pulse train into the zero-cross input, or run `sim on` for a software-only test.
```

`PSRAM 0 MB` means the wrong module or `memory_type`. To check what is on the chip see
[Operation.md](Operation.md#checking-what-is-on-the-chip).

**Dashboard.** Without stored Wi-Fi the controller opens the hotspot `SmartBrew-XXXX`. Join it and
open `http://192.168.4.1`; the dashboard loads before any Wi-Fi is set up. **Pass:** the
portafilter dot is green and Settings → Advanced → Live System Data shows `Free Heap / Min` in the
tens of kB or more and `Free PSRAM` close to 8 MB.

## Stage 1: Power stage: button and zero-cross

### 1.1 Button output

```
GPIO 6 ───[ 330 Ω ]─── LED anode,   LED cathode ─── GND
```

Type `btn`. The LED lights for 180 ms and the console prints
`Virtual button PULSED (pulse #1, 180 ms)`. Type `btn` twice quickly: the second reports
`Virtual button BLOCKED (retrigger dead time) ...` (250 ms dead time between pulses).

**Pass:** about 180 ms per pulse (180 ± 2 ms on a scope), the counter goes up, the quick second
pulse is blocked. Leave the LED in place for stage 4.

### 1.2 Zero-cross frequency detection

The zero-cross input sees one rising edge per mains half cycle, so 50 Hz mains is a **100 Hz**
square wave (10.000 ms) and 60 Hz mains is **120 Hz** (8.333 ms). Connect generator output to
GPIO 4 and ground to GND. Leave Settings → Hardware → Mains Frequency at **Auto-Detect**.

Start 100 Hz and type `zc`:

```
  armed         yes
  duty          0.00 %
  mains         50 Hz (locked auto, detected 50 Hz)
  gate hold     10000 us   (hard limit 16000 us)
  ac_present    YES
  brew_confirmed YES
  pulses        412 accepted / 0 rejected as glitch / 0 missing-timeouts
  last period   10.000 ms   full AC cycles fired 0
  ZC detection  8 / 8 samples
```

The log also shows `Mains detected: 50Hz` and
`Mains AC active in IDLE: machine autofill / manual bypass (100% passthrough)`: with no shot
running, mains on the pump is treated as the machine's own pump and passed through at full power.

Stop the generator, set 120 Hz, start it again. Detection runs again after the gap, and `zc` shows
`mains 60 Hz (locked auto, detected 60 Hz)`, `gate hold 8333 us (hard limit 13332 us)` and
`last period 8.333 ms`. The gate hold is the measured half cycle that the pump drive is timed by.

**Pass:** `detected 50 Hz` at 100 Hz, `detected 60 Hz` at 120 Hz, `0 rejected`, `8 / 8 samples`.
If `detected` stays `not yet`: detection needs 8 edges 7.5–11.5 ms apart. Check that GPIO 4 swings
0 → 3.3 V and that the mains frequency is not forced.

> **Keep each generator run under 2 minutes.** Mains present longer than the topology timeout
> (Settings → Advanced, default 120 s) while idle sets `FAULT 0x0400` and holds the pump drive off.
> A run longer than 5 s is also saved in the ledger as a `Passive Manual/Autofill` shot.

### 1.3 Glitch rejection

An edge closer than 6.5 ms to the last accepted edge is rejected. Switch the generator off for a
moment (the missing-pulse timeout then clears `brew_confirmed`), run it at **1 kHz** for about
10 s, and type `zc`. Only every seventh edge is 6.5 ms or more after the last accepted one, so
`rejected as glitch` climbs about six times as fast as `accepted` (≈ 860/s against 140/s). The
accepted edges are 7 ms apart, outside the 7.5–11.5 ms window, so `brew_confirmed` stays `no`.

**Pass:** rejected : accepted ≈ 6 : 1, `brew_confirmed no`.

### 1.4 Missing-pulse timeout

With 100 Hz running, note `missing-timeouts` in `zc`, then switch the generator off. Within 35 ms
`zc` shows `ac_present no`, and the log shows
`Mains AC dropped in IDLE: boiler fill / manual pump complete (dur=...ms)`. In IDLE the pulses ending
is the normal end of pump power, so it is only logged at debug level (`Zero-cross ended (pump power
released)`) and `missing-timeouts` does not change. Repeat with the generator switched off during a
controller-run shot (BREWING): that one logs
`ZC missing-pulse timeout (>35ms) while the controller drives the pump` and counts one timeout.

**Pass:** idle stops add no timeout; a loss during a controller-run shot adds exactly one.

### 1.5 Pump drive output (optional, needs a scope)

With 100 Hz running, probe GPIO 5 (dimmer gate). Idle with no command it is HIGH continuously
(full passthrough). After `duty 40` it is HIGH for whole AC cycles in 4 of every 10 cycles,
switching only at the edges on GPIO 4. With the generator off it goes LOW within 35 ms. `duty`
bypasses the pressure loop: type `duty 0` and reboot afterwards.

## Stage 2: Pressure acquisition

### 2.1 The rig

A transducer gives 0.5–4.5 V; the controller's 10 kΩ / 20 kΩ divider scales that to
0.333–3.000 V at the ADC node, and the firmware only sees the node. On the bench a potentiometer
drives the node directly:

```
3V3 ──[ 10 kΩ pot ]── GND
           │ wiper
           ├──► GPIO 1            (internal ADC1)
           └──► ADS1115 AIN0      (if fitted)
ADS1115: VDD ─ 3V3, GND ─ GND, ADDR ─ GND, SDA ─ GPIO 8, SCL ─ GPIO 9
```

Release the pressure simulator first (`sim off`, or reboot), or the real reading is ignored.

### 2.2 Zero baseline

Turn the pot until the meter reads **0.333 V** at the node, then type `status`:

```
  pressure     0.00 bar (target 0.00, node 333 mV, src INTERNAL)
```

**Pass:** 0.00 bar, drifting less than 0.02 bar over 30 s.

The zero calibration (`zero`, or Settings → Tools → Run Zero Calibration) averages 60 samples and
saves that node voltage as 0 bar. Run it only with the node at the true zero voltage.

### 2.3 Linearity and smoothing

With the default 1.2 MPa (12 bar) setting the conversion is a straight line, 222 mV per bar above
333 mV:

| Node | Transducer | Expected |
|---|---|---|
| 0.333 V | 0.5 V | 0.00 bar |
| 1.000 V | 1.5 V | 3.00 bar |
| 1.667 V | 2.5 V | 6.00 bar |
| 2.333 V | 3.5 V | 9.00 bar |
| 3.000 V | 4.5 V | 12.00 bar |

Check every row with `status`. A row off by more than 2 % points at the meter or the pot. Then
wiggle the pot while watching the dashboard gauge: it follows smoothly with a short lag. The filter
(`EMA Filter Alpha` 0.25 at 100 Hz) has a time constant of about 35 ms; the internal ADC adds a
light filter of its own.

**Pass:** ±0.05 bar at 6 bar; the reading is smooth, not jumpy.

### 2.4 ADS1115 and internal ADC

Reboot with the ADS1115 wired. The boot log shows:

```
ADS1115 probe OK at 0x48 (CONFIG=0x....)
Pressure source: ADS1115 @0x48 (continuous mode, GAIN_ONE +/-4.096V)
Pressure baseline: 0.333 V -> 0.00 bar (zero=333.3 mV, fs=3000.0 mV)
```

and `status` shows `src ADS1115`; repeat 2.3. Disconnect SDA and SCL and reboot:

```
ADS1115 not found on I2C (running at 400 kHz - falling back to the internal ADC)
Pressure source: internal ADC, oversampled
```

`status` shows `src INTERNAL` and 2.3 still holds. Addresses 0x48–0x4B are probed; Settings →
Hardware → ADC Source can force either converter.

### 2.5 Transducer faults while the machine runs its pump

The open-wire and over-range checks cut power only while a pump can be running. Start the 100 Hz
generator (the "machine" now runs its own pump), then:

| Pot | Expected |
|---|---|
| Node below 133 mV (to GND) | Warning only, at most once a minute: `Transducer open-wire while the machine is driving the pump - warning only; ...`. A machine without a transducer is a valid setup, so its pump is not cut. The same warning appears when idle without mains. |
| Node above 3050 mV (to 3V3) | After 200 ms: `FAULT 0x0100 set: transducer voltage saturated / shorted (>3.05V)`, state `FAULT`, then `PASSTHROUGH SAFETY HOLD: pump gate forced LOW (sensor saturated / shorted during machine-driven pump). ...` The drive stays off until the mains goes away. |

Stop the generator; the controller returns to `IDLE`. Set the pot back to 0.333 V.

## Stage 3: Software only: profiles and simulated shots

The commands are listed in [Operation.md](Operation.md#commands).

### 3.1 The profile loaded

Type `phase`. The default profile is Classic 9-Bar:

```
PROFILE 'Classic 9-Bar' (id=classic_9bar) target 36.0 g, max 45.0 s, 4 phases
  [0] Pre-Infusion           pressure 3.00 -> 3.00 bar (linear) ceil 60%  dur 4.0s  OR   1 trigger(s)
        pressure_ge    3.00
  [1] Ramp to 9 Bar          pressure 3.00 -> 9.00 bar (ease_in_out) ceil 100%  dur 3.0s  OR   0 trigger(s)
  [2] Peak Extraction        pressure 9.00 -> 9.00 bar (linear) ceil 100%  dur 20.0s  OR   1 trigger(s)
        weight_ge      30.00
  [3] Gentle Decline         pressure 9.00 -> 6.00 bar (ease_out) ceil 100%  dur 10.0s  OR   1 trigger(s)
        weight_ge      36.00
```

`use blooming_allonge` (or `turbo_shot`, `gentle_lever`, `italian_dark`) loads another built-in
profile and prints its table; `use classic_9bar` goes back.

### 3.2 A simulated shot

Type `sim on`, then `start`. In SIM mode the controller skips the pre-brew gate and the button
pulse and never drives the pump. Pressure, flow and weight come from the built-in model, so
`press` and `weight` have no effect. The log shows:

```
Simulation mode ENGAGED - virtual extraction flow active.
Pressure SIMULATOR engaged (software-only testing). Real sensor ignored.
[PHASE] 1/4 'Pre-Infusion' - pressure mode, target 3.00 bar, psm ceiling 60%
[BREW] Profile 'Classic 9-Bar' started - 4 phase(s), target 36.0 g, max 45.0 s
[BREW] START - simulation, profile 'Classic 9-Bar'
State IDLE -> BREWING (simulation shot started)
[PHASE] 'Pre-Infusion' -> 'Ramp to 9 Bar' after 4.0 s (simulation phase ran its 4.0 s)
[PHASE] 2/4 'Ramp to 9 Bar' - pressure mode, target 3.00 -> 9.00 bar over 3.0 s (ease_in_out), psm ceiling 100%
...
[STOP] Stop requested - weight target reached
State BREWING -> STOPPING (weight target reached)
[STOP] weight target reached - 36.0 g in 28.6 s (simulation, peak 9.02 bar)
State STOPPING -> IDLE (simulation shot complete)
```

In simulation each phase runs for its full duration and the triggers are ignored. The shot ends
when the simulated weight reaches the target, or with `profile complete` if the last phase ends
first.

**Pass:** the four phases run in order with their programmed durations, the dashboard chart
follows the dashed planned curve, and the ledger gets an entry named `Classic 9-Bar (SIM)`. The
dashboard's SIM setting does the same in the normal build. Type `sim off` when done.

### 3.3 Over-pressure interlock

`press` is clamped to the transducer's full scale (12 bar), which is below the default cut-off of
12.5 bar, so lower the cut-off for this test:

1. Settings → Advanced → **Overpressure Safety Cut-Off** = 8, **Save Advanced Settings**.
2. `press 9`. After 150 ms: `FAULT 0x0002 set: sustained over-pressure`.
3. `press 0`: `Pressure normal: FAULT_OVER_PRESSURE cleared`.
4. Start the 100 Hz generator and repeat step 2. Now the machine's pump is cut too:
   `PASSTHROUGH SAFETY HOLD: pump gate forced LOW (sustained over-pressure during machine-driven
   pump). It stays closed until the machine releases the AC.` Stop the generator to return to
   `IDLE`.
5. Set the cut-off back to 12.5 (15 for a 1.6 MPa transducer), save, and type `sim off`.

During a shot the same condition stops the shot with the reason `over-pressure cut-off`.

### 3.4 Manual override and manual duty

`manual 40`, then `zc`, shows `duty 40.00 %`; `status` shows it next to the model's feed-forward
value. `set 6.5` logs `Pressure target override -> 6.50 bar` and replaces the target of a running
phase. Both are for the HIL simulator or a jig; a reboot clears them.

## Stage 4: Pre-brew gate: hardware check, verified tare, 500 ms settle

Every real start (not SIM) goes through the gate. It checks the power stage, the transducer, the
button output and the scale, verifies the tare, waits 500 ms and only then presses the machine's
button. Keep the stage 1 LED on GPIO 6 to see that press.

### 4.1 The normal path

Set the pot to 0.333 V (or type `press 0`), make sure the generator is off, and type `start`:

```
[PRE-BREW] Hardware check starting (profile 'Classic 9-Bar', 4 phase(s), target 36.0 g, max 45.0 s)
[PRE-BREW]   power stage  ARMED - zero cross not yet seen (machine idle), mains 50 Hz
[PRE-BREW]   transducer   OK - source INTERNAL, node 333 mV, 0.00 bar
[PRE-BREW]   button       ARMED - pulse 180 ms
[PRE-BREW]   scale        NOT CONNECTED - this shot runs on time/volume limits only
[PRE-BREW] Hardware check passed
State IDLE -> PREPARING (pre-brew checks passed)
[PRE-BREW] All hardware checks passed - the brew starts in 500 ms
[PHASE] 1/4 'Pre-Infusion' - pressure mode, target 3.00 bar, psm ceiling 60%
[BREW] Profile 'Classic 9-Bar' started - 4 phase(s), target 36.0 g, max 45.0 s
[BREW] START - profile 'Classic 9-Bar', 4 phase(s), target 36.0 g
State PREPARING -> STARTING (soft start button pulsed)
FAULT 0x0020 set: no ZC burst within button ack window
State STARTING -> FAULT (soft start not acknowledged)
```

After stage 1 the power-stage line reads `zero cross proven (last 2s: N pulses, M rejected; since
boot ...)` instead. The noise check uses the last 2 s only, so an old burst cannot block a shot. If
zero-cross dropouts happened during controller-run shots, an info line gives their count.

The fault 600 ms after the button press is correct on a single board: nothing answered the button,
and the controller does not drive a pump it has not seen start. The state returns to `IDLE` once
no mains is present. With the HIL simulator connected, the simulator starts its pulses on the
button press and the shot runs to the end.

**Pass:** `[BREW] START` comes at least 500 ms after `All hardware checks passed`, and the LED
flashes at `[BREW] START`, not before.

### 4.2 The scale tare is verified

These cases need a scale paired with the controller (a real one, the companion scale or the HIL
simulator's `Bookoo_SIM`). A reading only counts if it arrived after the tare request, and the net
weight must then be within ±1.0 g of zero.

| Case | Log |
|---|---|
| A: working scale | `scale        CONNECTED - <name> [<driver>], battery <n> %, raw 12.4 g`, `Scale tare requested - waiting for the scale to confirm it`, `Scale tare VERIFIED - net 0.0 g, acknowledged by the scale` (or `confirmed by a fresh reading`), `All hardware checks passed (scale tared) - the brew starts in 500 ms` |
| B: weight keeps changing during the tare (press on the platform; or with the HIL simulator, start within its 1 s post-shot drip) | `Scale tare did not settle (net 4.7 g) - taring again (attempt 2/2)`, then `Start aborted - the scale would not tare - it still reads 4.7 g after 2 attempts. The brew was not started.` |
| C: connected but no new reading within 2 s | `Start aborted - the scale did not confirm the tare within 2000 ms. The brew was not started.` |
| Link lost during the gate | `Start aborted - the scale disconnected during preparation. The brew was not started.` |
| D: no scale | The `NOT CONNECTED` warning of 4.1; the gate continues |

### 4.3 A failed check blocks the shot

Force each condition; the shot must not start and the LED must not flash:

| Condition | How to force it | Reason in the log |
|---|---|---|
| Power stage disarmed | `disarm`, `start` (then `arm`) | `the power stage is disarmed - the pump cannot be controlled` |
| Transducer open | `sim off`, pot to GND | `no pressure transducer detected (open circuit) - a profile brew needs pressure feedback` |
| Transducer saturated | `sim off`, pot to 3V3 | `the pressure transducer is saturated - it is shorted or miswired` |
| Noisy zero-cross | Reboot, run 1 kHz for 10 s (1.3), stop it, `start` | `the zero-cross signal is too noisy - check the zero-cross wiring` (more than 4 rejected edges per accepted one since boot) |

Each ends with `[PRE-BREW] Hardware check FAILED - <reason>. The brew was not started.`

A transducer fault is confirmed over 200 ms, so one bad sample cannot refuse a shot. The log shows
`transducer   FAULT - open circuit; confirming for 200 ms`, then
`Hardware check passed (pending transducer confirmation)` and `IDLE -> PREPARING`, and 200 ms
later the FAILED line and a return to `IDLE`. A reading that recovers inside the window logs
`transducer   RECOVERED - node 333 mV, 0.00 bar` and the gate carries on.

**Machine already pumping.** With the 100 Hz generator running, `start` is refused before the gate
opens. The console prints nothing; a start from the dashboard is answered with
`machine pump active (autofill or manual)`.

### 4.4 Cancelling the gate

The gate is open for 500 ms, longer while it waits for a tare. Paste `start` and `stop` on the
console in one go:

```
[PRE-BREW] Start aborted - console. The brew was not started.
```

A WebSocket STOP logs `remote stop`, `POST /api/v1/cmd/STOP` logs `web stop`, and a FLUSH while
the gate is open logs `replaced by a grouphead flush`. The button is not pressed to stop anything:
the machine was never started.

### 4.5 No double start

If mains appears on the pump while the gate is open (someone pressed the machine's own button),
the gate cancels itself:

```
[PRE-BREW] Start aborted - the machine pump became active during preparation. The brew was not started.
```

The window is short; with the HIL simulator, press the simulator's BOOT button right after `start`.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `ERR: bench feature disabled ...` | Normal build installed | Flash `esp32s3-bench` |
| `detected not yet` in `zc` | Signal below 3.3 V, period outside 7.5–11.5 ms, or frequency forced | Check amplitude and frequency; set Mains Frequency to Auto-Detect |
| `rejected` climbing at 100 Hz | Ringing or double edges from the generator | Clean square wave; edges closer than 6.5 ms are rejected by design |
| `FAULT 0x0400` during zero-cross tests | Generator ran longer than the topology timeout | Stop it between tests, or raise the timeout (Settings → Advanced) |
| `src INTERNAL` with an ADS1115 fitted | No answer at 0x48–0x4B, or ADC Source forced | Check SDA/SCL/VDD; set ADC Source to Auto-Detect |
| Pressure stuck at the `press` value | Pressure simulator still engaged | `sim off` or reboot |
| `start` does nothing at all | Mains on the pump (generator running) | Stop the generator |
| `start` ends in `FAULT 0x0020` | Nothing answered the button | Expected on a single board; use the HIL simulator for whole shots |
| Open-wire warning once a minute | Node below 133 mV (no transducer or pot) | Expected on a bare board |

## Preview: first contact with the pump

Only after stages 1–4 pass:

1. **Pump on the bench, no machine.** Pump on its own mains supply through the dimmer, with water
   in the reservoir and the outlet into a jug (a vibratory pump must not run dry). Before applying
   mains, type `duty 30` (bench build): without it the controller treats the mains as the
   machine's own pump and passes 100 % through. Apply mains for a few seconds. On a scope the gate
   must switch only at zero crossings and stay on for whole half cycles (10 ms at 50 Hz, 8.33 ms
   at 60 Hz). Remove mains, `duty 0`, reboot.
2. **Pressure transducer.** Fit it, run the zero calibration at atmospheric pressure, check it
   reads 0 bar.
3. **Into the machine.** Wire the button relay across the machine's brew switch
   ([Hardware.md](Hardware.md)). Test the **stop** path first: the controller must always be able
   to stop a shot before you rely on it to start one.
4. **Water, no coffee.** Cup under the group, run Classic 9-Bar, watch the log and the chart.

**Keep a hand on the machine's power switch throughout.**
