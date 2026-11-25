# Architecture

How the Smart Brew controller firmware is organised, how it drives the pump, and how it keeps the
machine safe. This describes firmware 1.5.0 (config version 7). Wiring, GPIO and parts are in
[Hardware.md](Hardware.md); the REST, WebSocket and telemetry formats are in [API.md](API.md).

## 1. Overview

The controller is an ESP32-S3 fitted inside an espresso machine that already has its own logic
board and a momentary brew button. It does not replace that board. The machine keeps control of the
boiler temperature, the 3-way solenoid, the boiler autofill and its own safety cut-outs. The
controller adds three things:

- a TRIAC power stage in the pump's live feed, so it can modulate pump power half-cycle by half-cycle;
- a pressure transducer on the brew line, so it can run a closed pressure or flow loop;
- an isolated relay across the brew button, so it can start and stop the machine's own brew cycle.

Design goals:

| Goal | How it is met |
| :--- | :--- |
| Even pump power at partial duty | Fired half-cycles are spread with a Bresenham accumulator, driven directly from the zero-cross interrupt. |
| Network traffic must not disturb pump timing | The zero-cross ISR and the 100 Hz control task live on Core 1; Wi-Fi, BLE, telemetry and flash writes run elsewhere. |
| No pressure overshoot on an empty puck | Feed-forward from the pump model, derivative on measurement, integral frozen below a headspace pressure. |
| The machine works normally when the controller is not brewing | In IDLE the power stage conducts every half-cycle, so autofill and the machine's own brew switch run the pump at full power. |
| Shots do not depend on a client | Profile, PID, interlocks and weight cutoff all run on the controller. A display or phone that disconnects mid-shot changes nothing. |

## 2. Components and data flow

```
                    ESPRESSO MACHINE (stock logic board stays in charge)
   brew button ◄── isolated relay ─┐    pump live ──► TRIAC ──► pump    brew line ──► transducer
                                   │                    ▲  │ zero-cross            │
                                   │                    │  ▼                       ▼
 ┌─────────────────────────────────┴────────────────────┴──────────────────────────┴──────────┐
 │ CONTROLLER (ESP32-S3)                                                                      │
 │  Core 1: zero-cross ISR · "ctrl" 100 Hz (pressure, profile, PID, interlocks) · loop()      │
 │  Core 0: "tele" (telemetry, BLE service, flash writes) · NimBLE host · log_drain           │
 │  any core: async_tcp (REST, WebSocket, OTA upload)                                         │
 └───────┬──────────────────────────────┬──────────────────────────────┬──────────────────────┘
         │ BLE                          │ Wi-Fi: REST + WebSocket       │ UART1 NDJSON (optional)
         ▼                              ▼                               ▼
   Bluetooth scale        Web dashboard / Android app           Waveshare 3.5" display
                          (the app wraps the same dashboard)    (also usable over Wi-Fi)
```

The controller is headless: it runs the shot and publishes a binary telemetry frame. The web
dashboard is compiled into the firmware and served by the controller; the Android app wraps the same
dashboard; the Waveshare 3.5" display is a separate ESP32-S3 running LVGL. All three render on their
own clocks ([Dashboard.md](Dashboard.md)). Bluetooth scales connect through NimBLE, which is only
started once a scale is configured or a discovery scan is requested. Supported scales are listed in
[Hardware.md](Hardware.md).

## 3. Tasks and cores

| Task | Core | Priority | Rate | Work |
| :--- | :---: | :---: | :--- | :--- |
| Zero-cross ISR | 1 | interrupt | every mains half-cycle | Glitch filter, 50/60 Hz detection, brew-burst detection, writes the TRIAC gate. Runs from IRAM, integer-only. |
| `ctrl` (`taskControl`) | 1 | 5 | 100 Hz | Commands, pressure sampling, profile engine, PID, interlocks, virtual button, zero-cross watchdog, scale cutoff, telemetry snapshot. |
| Arduino `loop()` | 1 | 1 | ~5 ms | Wi-Fi reconnect and liveness, captive-portal DNS, serial console, status LED. |
| `tele` (`taskTelemetry`) | 0 | 3 | 20 Hz active / 10 Hz settling / 2 Hz idle | Drains controller events, broadcasts telemetry over WebSocket and UART1, services the BLE scale, writes the ledger and config. |
| NimBLE host | 0 | (stack) | event-driven | Scale radio stack. |
| `log_drain` | 0 | 2 | event-driven | Drains the log ring buffer to USB CDC and UART0. |
| `cal_zero`, `ota_pull` | 0 | 1 | on demand | Atmospheric zero calibration; firmware download from a URL. |
| `async_tcp` | any | (library) | event-driven | REST, WebSocket and OTA upload handlers. |

- `loop()` is on Core 1 because the precompiled Arduino-ESP32 `sdkconfig` sets
  `ARDUINO_RUNNING_CORE=1`. `ctrl` outranks it (5 vs 1) and always preempts it. Do not add slow or
  busy-waiting work to `loop()` on the assumption that it runs beside the radio.
- The zero-cross interrupt is bound to Core 1. ESP-IDF runs a GPIO interrupt on the core that
  installed the interrupt service, so `PsmDimmer::arm()` always performs the attach on Core 1:
  directly at boot, and through an IPC call when it is re-armed from another core (for example after
  a failed or aborted OTA).
- The task watchdog is 5 s, set in `setup()` with `esp_task_wdt_init(5, true)`. `ctrl`, `tele`,
  `loop()` and `log_drain` subscribe to it.
- **One clock per tick.** `tick(now)` uses its `now` argument everywhere and `processCommands(now)`
  does not re-read `millis()`, so no timestamp can lie in the future of the tick that compares it.
- **Cross-core traffic.** Commands reach `ctrl` through a FreeRTOS queue; events (`STATE_CHANGED`,
  `FAULT_SET`, `SHOT_COMPLETED`) come back through another. Telemetry is read from a lock-free
  seqlock double buffer, so readers never block the control task.
- **No flash writes on the control task during a shot.** The ledger entry, the learned drip lag and
  the pump-flow calibration are computed on Core 1 and written by `tele` on Core 0 when it receives
  `SHOT_COMPLETED`. The one exception: a profile switch requested mid-shot is deferred and applied by
  the control task when the machine returns to IDLE (`loadById` reads the profile file).
- `ConfigStore::save()` is serialised with a mutex, because the telemetry task, web handlers, console
  and zero calibration can all save. The control task never calls it.

## 4. Brew state machine

`BrewController` owns one state variable, changed only on the control task. Every transition is
logged with its reason.

| State | Pump owner | Meaning |
| :--- | :--- | :--- |
| `IDLE` | machine (passive) | No controller shot. The gate conducts every half-cycle, so autofill and the machine's own brew switch work untouched. |
| `PREPARING` | machine (passive) | Pre-brew gate: hardware check, scale tare verification, settle delay. Nothing is driven. |
| `STARTING` | controller | The start pulse has been sent; waiting for the machine to route AC to the pump. |
| `BREWING` | controller | Profile engine and PID own the pump. |
| `FLUSHING` | controller | Grouphead rinse: fixed 45 % duty for 1–60 s, no profile, no PID, no weight cutoff. |
| `STOPPING` | controller | Duty is zero, the stop pulse is queued, waiting for the AC to drop. |
| `FAULT` | gate held LOW | A safety condition latched the gate off. |

On the wire `PREPARING` is reported as `STARTING`, so clients show a pending start without a new
state code.

Transitions:

| From | To | When |
| :--- | :--- | :--- |
| IDLE | PREPARING | `START` with a profile loaded and no AC present. |
| IDLE | STARTING | `FLUSH` (start pulse sent). |
| PREPARING | STARTING | All checks pass, tare verified, 500 ms settle elapsed; start pulse sent. |
| PREPARING | IDLE | A check fails, the machine's pump turns on, the scale disconnects, `STOP`, or a `FLUSH` replaces the start. |
| STARTING | BREWING / FLUSHING | AC present and a burst of 4 valid zero-cross pulses seen. |
| STARTING | FAULT | No burst within `buttonAckTimeoutMs` (600 ms). |
| BREWING | STOPPING | `STOP`, weight cutoff, profile complete, 120 s limit, an interlock, or AC lost more than 500 ms into the shot (the machine's button was pressed). |
| FLUSHING | STOPPING | Flush time elapsed, or AC lost. |
| STOPPING | IDLE | AC drops. The shot is frozen and queued for the ledger. |
| STOPPING | FAULT | AC still present 5 s after the stop pulse (one retry at 3 s). |
| IDLE | FAULT | AC present continuously for `topologyTimeoutSec` (120 s), or an over-pressure / over-range hold while the machine drives the pump. |
| FAULT | IDLE | AC absent for more than 1 s, or the operator clears faults once the condition is gone. |

A machine-driven pump run in IDLE (autofill or the machine's own button) lasting at least 5 s is
recorded in the ledger as a "Passive Manual/Autofill" entry. The controller does not tare the scale
or apply a profile in that case.

## 5. Power stage

### Zero-cross and brew detection

The machine's logic board only switches AC to the pump while it is pumping, so the dimmer's
zero-cross output doubles as a "the pump is powered" signal. The ISR (`PsmDimmer::onZeroCross`):

1. Rejects edges closer than 6.5 ms to the previous one (TRIAC commutation noise).
2. Tracks a burst: 4 consecutive edges with 7.5–11.5 ms spacing inside 45 ms confirm the pump is
   powered (`brewConfirmed`).
3. Auto-detects mains frequency from the first 8 valid periods: an average of 9.1 ms or less means
   60 Hz, otherwise 50 Hz. The frequency can also be locked in the config.
4. Writes the gate level for the coming half-cycle.

The control task runs the missing-pulse watchdog every tick. No edge for 35 ms clears "AC present"
and the burst state, and forces the gate LOW.

### Passive mode and profile mode

| Mode | Used in | Gate behaviour |
| :--- | :--- | :--- |
| `PASSIVE` | IDLE, PREPARING | Gate HIGH on every valid edge: 100 % passthrough, no handshake. |
| `PROFILE` | STARTING, BREWING, FLUSHING, STOPPING, FAULT | Gate stays LOW until the 4-pulse burst is confirmed, then follows the Bresenham accumulator. STOPPING and FAULT also set a force-off latch. |

### Bresenham pulse skipping

A vibratory pump has an internal diode and only strokes on one half-cycle. Since Live and Neutral
can be wired either way, the accumulator runs once per **full** AC cycle (every second edge):
`acc += duty`, and if `acc >= 100` the cycle fires and `acc -= 100`. A fired cycle drives the gate
HIGH on both of its half-cycles, so the half-cycle that powers the pump is always covered whatever
the polarity. Fired cycles are spread evenly, which avoids the pressure ringing of bunched pulses.

The gate is driven HIGH only by the ISR. Task code may only force it LOW (`forceGateOff`, the
watchdog, `disarm`). If the zero-cross signal stops, the pump cannot be energised.

## 6. Pressure acquisition

The 0.5–4.5 V transducer passes through a 10 kΩ / 20 kΩ divider (×1/1.5) to a 0.333–3.000 V node.

- **Source** is chosen at boot (`useAds1115`): auto-detect probes an ADS1115 at 0x48–0x4B and falls
  back to the internal ADC1; it can also be forced to either. There is no runtime failover.
- **ADS1115**: continuous mode, AIN0, ±4.096 V range, 860 SPS, 0.125 mV per code. The control task
  reads the conversion register once per tick.
- **Internal ADC1**: 32 readings averaged per tick, then a light pre-filter (α = 0.15).
- **Conversion** (node millivolts): `P = max(0, (V − zero) / (fs − zero) × P_fs)`, with zero =
  333.3 mV, fs = 3000 mV and `P_fs` = 12 or 16 bar for a 1.2 or 1.6 MPa transducer. The zero point
  can be recalibrated at atmospheric pressure.
- **Filter**: EMA at 100 Hz, `emaAlpha` default 0.25.
- **Health**: below `minSensorMv` (133 mV at the node, 200 mV at the transducer) is open-wire; above
  3050 mV at the node is over-range (shorted or saturated). See §11 for what each triggers.

## 7. Control loop

Each `BREWING` tick asks the profile engine for a target and turns it into a duty:

| Phase mode | Duty |
| :--- | :--- |
| `PRESSURE` | PID: `u = clamp(ff(target) + Kp·e + Ki·∫e − Kd·dP/dt, 0, phase ceiling)` |
| `FLOW` | Pump-model feed-forward for the target flow at the current pressure, plus 12 % per ml/s of flow error. |
| `POWER` | The target duty, capped by the phase ceiling. |
| `PAUSE` | 0. |

PID details (`src/control/pid_controller.cpp`): the **feed-forward** is a line through
`(0 bar, minActivationPct)` and `(9 bar, holdNineBarPct)` from the pump preset. The derivative acts
on the measurement, low-pass filtered (α = 0.2), so setpoint steps do not kick the output. The
integral is **frozen below 1.5 bar** (`headspaceAntiWindupBar`) so it cannot wind up while the puck
fills, and it unwinds when the output is saturated and the error pushes further into the rail. Gains
default to the pump preset; non-zero config values override them.

Flow is estimated two ways. The **virtual flow** comes from the pump's pressure-flow model with a
hydraulic compliance correction. The **cup flow** is the regression slope of the scale weight.
FLOW phases and flow triggers use the cup flow when a scale reports one, and the virtual flow
otherwise. The **predictive weight cutoff** stops the pump early by `slope × scaleLagSec` so the
drip brings the cup to target. The drip lag and a pump-flow scale factor are learned from each
suitable shot. All of this is documented in [PumpFlow.md](PumpFlow.md). Profile structure, phases
and triggers are in [Profiles.md](Profiles.md).

## 8. Pre-brew gate

`START` never pulses the machine immediately. It opens the `PREPARING` state, which runs on the
control tick and only launches the shot once the hardware is proven:

| Check | Start refused when |
| :--- | :--- |
| Profile | Not loaded, or has no phases. |
| Power stage | Disarmed. |
| Zero-cross | Pulses seen and rejected glitches exceed 4× accepted pulses (noisy wiring). |
| Pressure | No source; or open-wire / over-range that persists for 200 ms (a single bad sample does not block). |
| Virtual button | Disarmed. |
| Scale tare | A scale is connected and the tare cannot be confirmed (below). |

Tare verification, when a scale is connected:

1. The gate sends a tare and records the scale's update counter.
2. After a 150 ms grace period (or the scale's own tare acknowledgement), a **fresh** reading must
   arrive and the net weight must be within ±1.0 g of zero.
3. If a fresh reading is off zero after 450 ms, the gate tares once more. A second failure, or no
   confirmation within 2 s of a tare, aborts the start with a reason.

When everything passes, the shot starts **500 ms later** so the scale rests at zero and the display
shows the pending start. If the machine's pump turns on while the gate is open, the start is
cancelled, so the controller never adds a second brew to a running one. With no scale connected
the shot runs on its time and volume limits only. Everything is logged under `[PRE-BREW]`.

## 9. Shot end: the post-shot weight tail and the ledger

The reading that trips the cutoff is not the yield. Coffee keeps draining from the grouphead for a
second or two after the pump stops.

**Settling window** (`src/control/weight_settle.h`). When a real shot with a scale enters
`STOPPING` (not a flush, not a simulation), the controller keeps watching the scale. Telemetry sets
`SB_FLAG_WEIGHT_SETTLING` and stays at 10 Hz instead of 2 Hz. The yield is the **highest** weight
seen, so lifting the cup does not pull it down. The window closes when the weight has not risen by
more than 0.2 g for 2 s, when it drops by more than 1.0 g (cup lifted), when the scale disconnects,
or after 10 s. The log reports `[SETTLE] Final yield ... g (reason)`.

**Ledger** (`src/control/shot_history.*`). When the AC drops, `freezeShot()` copies the curve into
a separate record, so the next shot can start capturing at once. While the window is open, tail
samples go to that frozen record at 10 Hz (`addTailSample`). They carry the weight live but hold
pressure and flow at their values when the pump stopped, so the curve has no collapse to zero.
When the window closes, the settled yield is recorded and `SHOT_COMPLETED` is posted; Core 0 then
writes the entry. The ledger, `GET /api/v1/history` and the `shot_end` message all report the
settled yield. Duration and peak pressure are the shot's own: the tail does not stretch the brewing
time. Routing rules are in `src/control/shot_tail.h` and covered by host tests ([Testing.md](Testing.md)).

Clients follow the same rule on screen: the weight line keeps climbing after the pump stop, placed
by the time elapsed since the stop, while pressure and flow curves end at the stop. Ledger graphs
and how the clients draw them are described in [Dashboard.md](Dashboard.md).

## 10. Telemetry and client smoothing

Each `tele` cycle builds one binary frame (`include/telemetry_wire.h`, version 2): a 36-byte numeric
head, three length-prefixed strings (phase name, scale name, profile id) and a CRC-8/ATM. WebSocket
clients receive it as a binary message; the UART1 bridge renders an NDJSON line from the same bytes.
JSON text messages on the WebSocket carry command replies and events. Field layout: [API.md](API.md).

What keeps the stream smooth: Nagle is disabled on each WebSocket connection (`setNoDelay(true)`);
a client more than two frames behind is skipped for that frame (`ws_skipped`) rather than sent a
backlog; Wi-Fi modem sleep is off (`WIFI_PS_NONE`) once connected; and BLE stays down unless a
scale is configured, because ESP-IDF coexistence does not allow BLE with modem sleep disabled.

The remaining jitter is the radio hop to the access point. Frames arrive in order and are not lost,
but a client can fall a few hundred milliseconds behind and catch up in a burst. The `seq` field
tells a delayed frame from a dropped one. `/api/v1/status` reports `ctrl_us_max`,
`tele_period_us_max`, `tele_body_us_max`, `wifi_ps` and `ws_skipped`; how to measure them is in
[Operation.md](Operation.md).

Clients never draw at packet rate. They plot against the controller's shot clock (`t_ms`), not
arrival time, so a burst of frames stays time-accurate; the shot timer advances on the client's own
clock; gaps are interpolated and numbers eased. `t_ms` reads 0 while a shot is pending (PREPARING or
STARTING), the elapsed time while it runs, and the frozen final duration afterwards. The Waveshare
display receives on a Core 0 task at 100 Hz and updates LVGL every 30 ms on its other core. Details:
[Dashboard.md](Dashboard.md).

## 11. Safety interlocks and fail-safes

Interlocks are evaluated every control tick (skipped in simulation, where the gate is held LOW).
"Owned" means the controller drives the pump (STARTING, BREWING, FLUSHING).

| Condition | Threshold | Action |
| :--- | :--- | :--- |
| Over-pressure | ≥ `maxSafetyPressureBar` for > 150 ms (default 12.5 bar for 1.2 MPa, 15.0 bar for 1.6 MPa) | Owned: gate LOW, stop pulse, STOPPING. Machine-driven: latched gate hold, FAULT. |
| Transducer open-wire | node < `minSensorMv` (133 mV) for > 200 ms | Owned brew: gate LOW, stop. Machine-driven: warning only (logged at most once a minute), because no transducer fitted is a valid setup. |
| Transducer over-range | node > 3050 mV for > 200 ms while the pump runs | Same as over-pressure. |
| Zero-cross lost | no edge for 35 ms | Gate LOW. Mid-shot (after 500 ms): STOPPING. |
| Start not acknowledged | no 4-pulse burst within 600 ms of the start pulse | `FAULT_BTN_NO_ACK`, FAULT. |
| Stop not acknowledged | AC still present 3 s after the stop pulse | Retry the pulse; FAULT at 5 s. |
| Maximum brew time | 120 s | `FAULT_MAX_TIME`, stop. |
| Permanent AC in IDLE | > `topologyTimeoutSec` (120 s) | `FAULT_TOPOLOGY`, gate LOW. |
| Firmware flash | OTA starts | Power stage disarmed (below). |
| Task stalled | not fed for 5 s | Task watchdog panic and reset. |

The latched machine-driven hold is released only when the machine drops the AC or the operator
clears the fault with the condition gone. Releasing it as soon as the reading dips would make the
gate chatter and re-run a dead-headed pump.

**Network self-heal.** Once a minute the controller probes its gateway; after 3 consecutive failures
it re-associates to get a fresh DHCP address, but never while a shot is running.

## 12. Storage, OTA and partitions

`partitions_8MB.csv`:

| Partition | Offset | Size | Contents |
| :--- | :--- | :--- | :--- |
| `nvs` | 0x9000 | 20 KB | Config mirror (binary blob). |
| `otadata` | 0xE000 | 8 KB | Active OTA slot. |
| `app0` / `app1` | 0x10000 / 0x310000 | 3 MB each | Firmware, including the gzipped dashboard. |
| `spiffs` (LittleFS) | 0x610000 | 1.875 MB | `/config.json`, `/profiles`, `/history` (15 shots, round robin). |
| `coredump` | 0x7F0000 | 64 KB | Crash dump. |

The config is loaded from NVS and overlaid with `/config.json`; `save()` writes both. A firmware
flash leaves LittleFS alone. `pio run -t uploadfs` replaces the whole partition and erases the
ledger; see [Operation.md](Operation.md) and [../README.md](../README.md).

**OTA** (browser upload or download from a URL):

1. `OtaService::disarmForFlash()` requests a stop, detaches the zero-cross ISR, aborts the virtual
   button, sets duty to 0 and drives the gate and button pins LOW before any flash is written.
2. An upload that stalls for 30 s, or a download that runs over 5 minutes, is aborted and the power
   stage re-armed.
3. The image is finished with `Update.end(false)`, so it boots pending verification.
4. After 30 s of control-loop operation, the control task calls
   `esp_ota_mark_app_valid_cancel_rollback()`.

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the bootloader reverts to the previous image only if
the new one resets before it is marked valid. An image that runs 30 s and misbehaves later is not
rolled back.

## 13. Simulation mode

Simulation mode (`SET_SIM`, console `sim on`) runs a complete shot with no water. The gate is held
LOW the whole time and the pre-brew gate and interlocks are skipped. Each control tick:

- a first-order lag moves pressure toward the target implied by the phase (a FLOW target is
  converted with `P = Q² · R`, capped at 11.5 bar; POWER maps duty to up to 9.5 bar);
- flow follows `Q = √(P / R)` through an EMA (α = 0.04 at 100 Hz);
- puck resistance `R` starts at 0.11 and rises with delivered water (swelling) and with pressure
  (compaction);
- cup weight starts once about 11.5 ml has filled the headspace and reaches full rate by about 21 ml;
- the shot stops when the weight reaches the target, the profile ends, or 120 s pass.

Telemetry carries `SB_FLAG_SIMULATION`, the shot is recorded with "(SIM)" after the profile name,
and there is no post-shot tail. Test procedures are in [Testing.md](Testing.md).
