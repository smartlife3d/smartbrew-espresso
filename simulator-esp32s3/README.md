# ESP32-S3 Hardware-In-The-Loop (HIL) Test Rig & Hardware Simulator

> Part of the Smart Brew Controller - see the [main README](../README.md). Bench test procedure and the host regression tests: [`docs/Testing.md`](../docs/Testing.md).

This firmware turns a second **ESP32-S3** board into an automated **Hardware-In-The-Loop (HIL) Test Rig** for the **Universal Espresso Pressure & Flow Profiling Controller**.

Instead of risking high-voltage AC mains, plumbing high-pressure water, or needing an actual physical scale on your development bench, this simulator **mimics every hardware peripheral** with real-time physical feedback:

1. **RobotDyn AC Dimmer (Zero-Cross & PSM Gate)**:
   - Emulates AC mains zero-crossing pulses (**100 Hz** for 50 Hz mains, **120 Hz** for 60 Hz mains).
   - Generates ZC pulses **only when the machine is energized** (mimicking the espresso machine's switched pump phase).
   - Measures the controller's **PSM gate drive conduction and duty cycle** (0% to 100%) in real time.
2. **0.5V – 4.5V Pressure Transducer**:
   - Converts the simulated hydraulic pressure into the **post-divider ADC node voltage** (333 mV to 3000 mV, exactly what the controller's `10 kΩ / 20 kΩ` divider produces from a real transducer).
   - Outputs a high-frequency PWM signal on GPIO 7 (25 kHz requested, 19.5 kHz as actually clocked) that must be **RC filtered** before it reaches the controller's `PIN_ADC_FALLBACK` (GPIO 1).
3. **Bookoo Themis BLE Espresso Scale**:
   - Runs a NimBLE BLE Peripheral advertising as `Bookoo_SIM` under service `0x0FFE`.
   - Streams 20-byte weight packets at **33 Hz** over characteristic `0xFF11`.
   - Receives and executes the controller's tare commands over characteristic `0xFF12`.
4. **Virtual Brew Button & Front-Panel Paddle**:
   - Detects the controller's virtual button pulse on `PIN_BTN_TRIG` (GPIO 6) and reacts on the **rising edge**, like the real momentary brew switch does.
   - Supports the onboard `BOOT` button (GPIO 0) as a manual barista button to test manual machine paddle extractions and passive shot detection.
5. **Real-Time Hydraulic & Puck Extraction Physics**:
   - Models pump pressure build-up, headspace filling (12 ml), puck resistance (`R_puck`), water flow (`Q = √(P/R)`), swelling, compaction, erosion and gravimetric weight accumulation in the cup.

---

## 1. Wiring & Pin Interconnection

Connect the two ESP32-S3 boards using jumper wires:

```
   SIMULATOR ESP32-S3                        CONTROLLER ESP32-S3
  ┌────────────────────────┐               ┌────────────────────────┐
  │ GPIO 4  ZC out         │── 100/120 Hz ▶│ GPIO 4  PIN_ZC_IN      │
  │ GPIO 5  gate in        │◀─ gate drive ─│ GPIO 5  PIN_PSM_GATE   │
  │ GPIO 6  button in      │◀─ 180 ms relay│ GPIO 6  PIN_BTN_TRIG   │
  │                        │               │                        │
  │ GPIO 7  PWM out        │               │                        │
  │      │                 │               │                        │
  │    [R 10k]             │               │                        │
  │      │                 │               │                        │
  │      ●─────────────────┼───────────────│ GPIO 1 PIN_ADC_FALLBACK│
  │      │                 │               │                        │
  │    [C 1µF]             │               │                        │
  │      │                 │               │                        │
  │ GND ─┴─────────────────┼───────────────│ GND                    │
  └────────────────────────┘               └────────────────────────┘

  The ● node is a single junction: the far end of the 10k resistor, the top of the
  1uF capacitor, and the controller's GPIO 1 all meet there.
  GPIO 7 is a push-pull PWM output - it is NEVER connected directly to GND.
```

### Parts list

| Part | Value | Notes |
| :--- | :--- | :--- |
| Resistor | **10 kΩ** | Series element, between simulator GPIO 7 and the ● node |
| Capacitor | **10 µF** (1 µF – 47 µF all fine) | From the ● node to GND. Ceramic or film preferred; if you use an electrolytic, its **+** goes to the ● node |
| Jumper wires | — | Plus a **common ground** between the two boards |

> [!WARNING]
> **Do not tie simulator GPIO 7 to GND.** It is a push-pull output driving the PWM. Grounding it
> holds the node at 0 mV, the controller sees an open-wire transducer (< 133 mV) and raises
> `FAULT_SENSOR_OPEN` instead of reading any pressure.

### Complete Pin Table

| Controller ESP32-S3 | Simulator ESP32-S3 | Direction | Signal Function | Notes |
| :--- | :--- | :---: | :--- | :--- |
| **GPIO 4** | **GPIO 4** | Sim $\to$ Ctrl | Zero-Cross Pulse Train | 100 Hz (50 Hz AC) or 120 Hz (60 Hz AC), 150 µs active-HIGH pulses |
| **GPIO 5** | **GPIO 5** | Ctrl $\to$ Sim | PSM Dimmer Gate Drive | Controller's TRIAC gate conduction pulses |
| **GPIO 6** | **GPIO 6** | Ctrl $\to$ Sim | Virtual Brew Button | Controller's relay pulse (180 ms default) |
| **GPIO 1** | **GPIO 7** | Sim $\to$ Ctrl | Pressure Sensor Analog mV | **10 kΩ in series** (GPIO 7 → ● node), **1 µF from the ● node to GND**, then the ● node → GPIO 1. The RC filter is mandatory, see below |
| **GND** | **GND** | Both | **Common Ground** | **MANDATORY**: Connect grounds together. The capacitor's ground leg and the two boards' grounds are all the same node |

> [!IMPORTANT]
> **The RC filter is MANDATORY — a direct wire will not work.**
> The controller reads GPIO 1 with the ESP32-S3's internal ADC, which *samples* the pin voltage. It takes four oversampling reads, but they are back-to-back, so they all land within a few microseconds of each other and therefore sample essentially the same instant — they do **not** average a PWM period. A 25 kHz PWM square wave sampled at 100 Hz gives essentially random results (0 V or 3.3 V, never the average), so the controller sees the sensor as over-range or open-wire, the pressure never settles, and the shot never runs.
> With the **10 kΩ series resistor and a capacitor from the ● node to GND**, the RC time constant turns the PWM into a clean DC level that is indistinguishable from a real transducer.
>
> **Any capacitance from 1 µF to 47 µF works.** Measured ripple and closed-loop error:
>
> | C | τ = 10 kΩ × C | PWM ripple | Error the PID sees |
> | ---: | ---: | ---: | ---: |
> | 1 µF | 10 ms | 13 mV (0.06 bar) | 0.019 bar |
> | **10 µF** | **100 ms** | **1.3 mV (0.006 bar)** | **0.016 bar** |
> | 47 µF | 470 ms | 0.3 mV | 0.003 bar |
>
> A bigger capacitor lowers the ripple but adds measurement lag. That lag is harmless up to about
> 47 µF because the simulated hydraulics already have a ~400 ms time constant of their own — the
> filter is not what sets the loop's speed. **10 µF is the sweet spot**: an order of magnitude less
> ripple than 1 µF and no measurable difference in how well the profile is traced.

> [!WARNING]
> **Check the filter before blaming the firmware.** If the controller reports
> *"Transducer open-wire while the machine is driving the pump"* while the simulator's own status
> line shows a healthy node voltage, the RC filter is not actually in the circuit. Measure the DC
> voltage at the **controller's GPIO 1** with a multimeter:
>
> | Reading at controller GPIO 1 | Meaning |
> | :--- | :--- |
> | **≈ 0.33 V** (idle, 0 bar) | Correct — the filter is working |
> | **≈ 0 V** | The PWM output is shorted to GND (capacitor on the wrong side of the resistor, or GPIO 7 grounded) |
> | **A fast 0–3.3 V square wave** | No series resistor — the ADC is sampling the raw PWM |

> [!NOTE]
> **No ADS1115 is emulated on purpose.** The controller probes the I2C bus at boot and automatically falls back to its internal ADC1 when nothing answers — that is exactly the path the simulator wants to exercise. If a real ADS1115 is still fitted on the controller while the simulator is connected, the controller would keep reading the (now unconnected) ADS1115 input and see 0 bar. Either unplug the ADS1115, or force the internal ADC in the controller's `config.json` (`"useAds1115": 2`) for simulator runs.

---

## 2. Quick Start

### 1. Flash the Simulator Firmware
Connect the Simulator ESP32-S3 to your computer via USB:
```bash
cd simulator-esp32s3
./flash.sh
```

`flash.sh` **asks which port to use** before it flashes — the controller, the simulator and the
Waveshare display are all ESP32-S3 boards that look identical to the operating system, so the
choice is never made silently. It then builds, flashes, and **opens the console on that same port**.

```bash
./flash.sh --list                    # show every port with the ESP32 ones marked
./flash.sh -p /dev/cu.usbmodem101    # pick the port yourself, no question asked
./flash.sh --build-only              # build without flashing
./flash.sh --no-monitor              # flash without opening the console
```

### 2. Flash and Run the Controller Firmware
Connect the Controller ESP32-S3 via its USB port:
```bash
cd ..
./flash.sh
```

The controller's script asks the same question and opens its console on the port it flashed. The
console is the repository's own (`tools/brewmon.py`), which reconnects by itself when the board
resets on native USB — something `pio device monitor` cannot do.

### 3. Check the sensor range on both sides

The simulator emits the **post-divider** node voltage, so the controller must be configured with the
same full scale or it will read every pressure wrong:

| Controller `sensor_mpa` | Controller full scale | Simulator command |
| :--- | :---: | :--- |
| `1.2` (**default**) | 12 bar | *(nothing — this is the simulator default)* |
| `1.6` | 16 bar | `range 16` on the simulator serial console |

The simulator's 333.33 mV / 3000 mV endpoints match the controller's default `sensorZeroMv` and
`sensorFsMv`. Do **not** run the controller's *zero calibration* against the simulator — the
simulator already outputs exactly the configured zero voltage, and recalibrating would teach the
controller a wrong zero.

---

## 3. How the Closed-Loop Test Works

### Phase 1: Idle & Discovery
1. On boot, the Simulator advertises as **`Bookoo_SIM`** over BLE (Service `0x0FFE`).
2. The Controller automatically discovers and pairs with `Bookoo_SIM` within a few seconds.
3. The Simulator's Zero-Cross generator is **inactive (LOW)** because the simulated machine is currently IDLE. The Controller sees `ac_power_present = false`.
4. The simulated pressure outputs 333 mV (0.0 bar atmospheric baseline).

### Phase 2: Start Extraction
1. In the Controller's Web Cockpit (or companion display), click **"Start Brew"** (or send a `START` command via REST/WebSocket).
2. The Controller pulses **GPIO 6 HIGH for 180 ms**.
3. The Simulator reacts on the **rising edge** of that pulse (20 ms debounce) and toggles the machine state to **`BREWING`**, which starts the Zero-Cross pulses on GPIO 4.
4. The Controller receives the ZC pulses on GPIO 4:
   - Verifies 4 valid pulses within 45 ms (`brewConfirmed()`).
   - Transitions state from `STARTING` $\to$ **`BREWING`**.
   - Sends a **Tare command** to the scale (`0xFF12`) — the simulator really zeroes the cup.
   - Begins modulating pump power via Bresenham PSM on **GPIO 5**.

### Phase 3: Profiling & Pressure Control
1. The Simulator measures the controller's gate pulses on **GPIO 5** over a 40-cycle window and derives the duty cycle (0% to 100%). The window is a trailing average, so the duty the plant sees lags the command by up to 400 ms and is quantised to ±2.5 percentage points.
2. The Simulator's hydraulic model responds:
   - For the first 12 ml, the headspace fills (pressure stays below ~0.5 bar, nothing reaches the cup).
   - Once filled, the pressure follows the flow balance $\dot P = (Q_{pump} - Q_{puck}) / C$.
   - Liquid flows through the puck ($Q = \sqrt{P/R}$).
   - Weight accumulates into the cup (g).
3. The Simulator streams the live weight at **33 Hz** over BLE.
4. The Controller's closed-loop PID reads the pressure on GPIO 1, adjusts the dimmer duty on GPIO 5, and traces the programmed profile!

### Phase 4: Gravimetric Cutoff & Stop
1. When the cup weight reaches the profile target (e.g., 36.0 g):
   - The Controller's scale manager triggers gravimetric cutoff!
   - The Controller cuts gate power immediately on GPIO 5.
   - The Controller pulses **GPIO 6 (180 ms)** to turn off the machine.
2. The Simulator detects the stop pulse on GPIO 6:
   - Toggles state back to **`IDLE`**.
   - Cuts the Zero-Cross pulses on GPIO 4.
   - Starts the **post-shot weight tail** (see below): the scale keeps climbing for ~1 s.
3. The Controller detects AC loss:
   - Finalizes shot statistics and returns cleanly to **`IDLE`**.
4. The tail runs out and the cup comes off the scale: the scale reads 0.00 g again.

### The Post-Shot Weight Tail

The reading that trips the cutoff is **not** the yield. When the pump stops, the last of the coffee
is still draining out of the grouphead, so the cup keeps getting heavier for a second or two after
the shot has ended.

The simulator models this, because the Controller's settling window
(`src/control/weight_settle.h`) follows exactly this tail and the clients show the yield climbing
past the cutoff reading:

- The residual flow out of the grouphead decays exponentially (3.0 ml/s with a 0.5 s time
  constant), which adds roughly **1.3 g** to the cup over the default 1 s tail — the amount that
  actually ends up under the basket.
- The Controller sees `SB_FLAG_WEIGHT_SETTLING` on the wire, keeps the telemetry at 10 Hz, and
  holds the **highest** weight it has seen. The dashboard and the Waveshare display then show the
  yield ticking up to the real figure instead of freezing at the cutoff value.
- At the end of the tail the cup comes off the scale, so the scale is back to 0 g for the next shot.

The tail is visible on the simulator's own console, so you can confirm it is really being sent:

```
[PHYSICS] Post-shot weight tail started at 34.21 g (1000 ms)
[PHYSICS] Tail  200 ms: scale sends 34.62 g (drip 2.44 ml/s)
[PHYSICS] Tail  400 ms: scale sends 34.96 g (drip 1.98 ml/s)
[PHYSICS] Tail  600 ms: scale sends 35.22 g (drip 1.61 ml/s)
[PHYSICS] Tail  800 ms: scale sends 35.41 g (drip 1.31 ml/s)
[PHYSICS] Post-shot weight tail over at 35.52 g - cup removed from the scale
```

The live status line also refreshes at 150 ms while the tail is running (it would otherwise
refresh every 1.5 s, which is longer than the whole tail, and the tail would be invisible). The
`*` next to the weight marks the tail:

```
[IDLE] Dimmer:   0.0% | P:  0.3 bar ( 333 mV) | Q:  1.2 ml/s | W: 35.22 g* | BLE: CONN(SUB) | ZC: 0
```

```bash
tail          # show the current tail length
tail 1500     # 1.5 s tail
tail 0        # disable the tail (the old behaviour: the cup comes off instantly)
```

> [!NOTE]
> **Why the cup still comes off the scale when the machine stops.**
> The Controller's scale manager keeps a *software* tare offset (`_tareOffsetG = _rawGrams`) in
> addition to sending the hardware tare command. It captures that offset from the last raw value
> it received, so the scale must read 0 g whenever the machine is idle. If the simulator only
> dropped the raw weight to 0 at the *start* of the next shot, the Controller would still be
> holding the previous shot's offset and would read a large negative weight for the whole shot —
> which would also stop the gravimetric cut-off from ever firing.
>
> That is why the tail ends with the cup coming off the scale rather than the coffee sitting in it
> for ever. The trade-off is that the next shot has to start *after* the tail has run out — if you
> start one within the tail, the Controller tares a moving value and will (correctly) refuse the
> brew with `the scale would not tare`.

---

## 4. The Simulated Pump Model

The simulated pump is **not** an arbitrary curve — it is derived from the same constants the
controller uses in `include/pump_presets.h`, because the controller's PID feed-forward line is
drawn through `(0 bar, minActivationPct)` and `(9 bar, holdNineBarPct)`.

| Preset | minActivation | 9 bar at | free flow | stall |
| :--- | ---: | ---: | ---: | ---: |
| `ULKA_EX5` (default) | 14 % | 36 % | 10.8 ml/s | 15.0 bar |
| `ULKA_EX4` | 16 % | 32 % | 7.5 ml/s | 18.5 bar |
| `OLAB_SILENT` | 18 % | 40 % | 9.5 ml/s | 14.5 bar |

The plant is a flow-balance model:

```
Q_pump(duty, P) = (duty/100) · maxFlow · max(0, 1 - (P/stall)^1.35)     [ml/s]
Q_puck(P)       = √(P / R)                                             [ml/s]
dP/dt           = (Q_pump - Q_puck) / C                                 [bar/s]
```

The puck resistance `R` is calibrated per pump preset so that the MEDIUM grind sits **on the
controller's feed-forward line** at the start of a shot (at `holdNineBarPct` duty the equilibrium
pressure is 9 bar). The grind presets scale that value:

| Grind | Resistance factor | Result at 9 bar | Shot time to 36 g |
| :--- | ---: | :--- | :---: |
| `coarse` | 0.65 × | 9.0 bar, ~2.4 ml/s | ~18 s |
| `medium` | 1.00 × | 9.0 bar, ~1.9 ml/s | ~21 s |
| `fine` | 1.83 × | 9.0 bar, ~1.4 ml/s | ~28 s |
| `choked` | 3.65 × | 9.0 bar, ~1.0 ml/s | ~37 s |

The bed swells, compacts under pressure and erodes as channels open up, so the resistance drifts
upwards by roughly 12 % over a 25 s shot and the flow eases off towards the end — like a real
extraction. That drift moves the equilibrium slightly off the feed-forward line, and the
controller's integral term absorbs it (the resulting pressure error stays below ~0.2 bar).

> [!NOTE]
> The controller's PID is tuned for *this* class of pump. A simulator whose duty-to-pressure
> curve disagreed with the controller's feed-forward would force the controller to fight a large
> steady-state error, and every HIL result would be meaningless. If you change the pump model,
> change it here **and** in the controller's `pump_presets.h`.

---

## 5. Interactive Serial CLI Commands

Open the serial monitor on the Simulator ESP32-S3 (`pio device monitor` @ 115200 baud). The simulator provides an interactive console:

| Command | Action |
| :--- | :--- |
| `start` / `brew` | Simulates the barista pressing the front panel button to start extraction (starts ZC pulses). |
| `stop` | Simulates the barista pressing the front panel button to stop extraction (stops ZC pulses). |
| `toggle` | Toggles the simulated machine brew state. |
| `tare` | Resets the simulated scale weight to $0.00\text{ g}$. |
| `tail <ms>` | Sets the post-shot weight tail in milliseconds (default 1000, `tail 0` disables it). See *The Post-Shot Weight Tail* below. |
| `weight <grams>` | Injects an arbitrary weight (e.g. `weight 35.8` to test instant cutoff). |
| `press <bar>` | Overrides the hydraulic model and locks pressure to a fixed value (e.g. `press 9.0`). Values above the transducer's full scale are allowed on purpose — see *Testing the protection paths* below. |
| `press auto` | Restores the automated hydraulic physics model. |
| `raw <mv>` | Forces the ADC node voltage directly, bypassing the bar ↔ mV mapping (e.g. `raw 50` for an open wire, `raw 3200` for over-range). |
| `raw off` | Clears the raw node-voltage injection. |
| `puck <coarse\|medium\|fine\|choked>` | Switches puck resistance to test fast-flow, 9-bar normal, or choked shots. |
| `puck <ohms>` | Sets an explicit puck resistance (survives a shot restart). |
| `pump <ulka_ex5\|ulka_ex4\|olab_silent>` | Switches the simulated pump model. **Must match the controller's `pumpModel`.** |
| `range <12\|16>` | Sets the transducer full scale. **Must match the controller's `sensor_mpa`.** |
| `50hz` / `60hz` | Toggles mains frequency between 50 Hz (100 Hz ZC) and 60 Hz (120 Hz ZC). |
| `status` | Prints a detailed live telemetry snapshot. |
| `help` | Displays available commands. |

All commands are case-insensitive.

### Physical Barista Button (BOOT Button on GPIO 0)
Pressing the physical **`BOOT` button** on the Simulator ESP32-S3 board instantly toggles machine extraction:
- Press while IDLE: starts ZC pulses immediately (tests **autofill / manual paddle extraction** on the controller).
- Press while BREWING: stops ZC pulses immediately (tests **manual barista abort** on the controller).

### Status LED (GPIO 48)
Blue = booting, green = idle, cyan = brewing.

### Host-side regression tests

Two of the nastiest bugs in this simulator were invisible to the compiler: the hydraulic plant
silently never advancing, and the brew button honouring only the *first* controller pulse (so the
machine could be started but never stopped). Both are timing/ordering bugs that only appear when
the module is driven the way the firmware actually drives it.

```bash
./test/host/run.sh
```

This compiles the **real** `src/sim_physics.cpp` and `src/sim_button.cpp` natively against a small
set of stubs and exercises them exactly as `main.cpp` does — no ESP32 hardware required. Run it
after touching the plant model or the button logic.

A third test compiles the **real** controller header `include/profile_types.h` and checks the phase
transition *reason* — the text the operator reads in the log to find out why a phase ended (for
example `weight 36.2 g >= 12.0 g`, or `phase timeout after 8.0 s`). A wrong or silent reason would
quietly defeat the whole point of the brew log, so it is covered here too.

A fourth test compiles the **real** controller header `src/control/weight_settle.h` and checks the
timing rules of the post-shot weight tail — when the yield in the cup counts as final, what happens
when the cup is lifted, and what happens when the scale link drops. Those are timing rules that a
compile check cannot verify.

A fifth test compiles the **real** controller header `src/control/shot_tail.h` and checks the rules
that route a post-shot tail sample to the shot it belongs to: the tail always goes to the finished
shot, it can only raise the recorded weight and peak, and it never stretches the recorded brewing
time. A sixth, integration test drives the real simulator physics into the real settling rules, so the
tail the simulator produces is the tail the ledger records.

A seventh test (`test_pump_flow.cpp`) compiles the controller's `include/pump_physics.h`,
`src/control/scale_flow.h` and `src/control/flow_calibration.h` and checks the pump flow model and
its inverse, the compliance correction under pressure ripple, the time-windowed cup-flow slope, the
learned drip lag and the pump-flow calibration. See [`docs/PumpFlow.md`](../docs/PumpFlow.md).

---

## 6. Testing the Protection Paths

A healthy 0.5–4.5 V transducer physically cannot leave the 0.333–3.000 V window at the ADC node,
so the controller's analogue protections can never trip on healthy hardware — they exist for
*electrical* faults. The simulator can inject those faults on purpose:

| Controller protection | Trigger | Simulator command |
| :--- | :--- | :--- |
| `FAULT_SENSOR_OPEN` (open wire) | node voltage < `minSensorMv` (133 mV) | `raw 50` |
| `FAULT_SENSOR_OVERRANGE` (saturated) | node voltage > 3050 mV | `raw 3200` |
| `FAULT_OVER_PRESSURE` | sustained pressure above `safetyPressureBar` | `press 14` (with a 12 bar transducer) |

The same applies to the **passive** scenario. While the controller sits in IDLE with the machine
energised it drives the gate 100 % (passthrough), so the simulated machine pressurises to the
transducer's full scale and stops there — exactly as a real pump running against a closed OPV
would. The node voltage then sits at 3000 mV, just below the 3050 mV over-range threshold, so the
controller does **not** fault. If you want the passive case to stay well clear of the threshold,
use `range 16` together with the controller's `1.6 MPa` setting.

> [!NOTE]
> The RC filter means the controller sees a change at the node only after ~5 × RC. With 10 kΩ and
> 10 µF that is **≈0.5 s** — so after a `press` or `raw` command, give the controller half a second
> before judging what it reads. With 1 µF it would be ≈50 ms.

---

## 7. Known Limitations

- **Duty measurement latency and quantisation.** The gate duty is measured over a 40 zero-crossing window (400 ms at 50 Hz mains). It is a trailing average, so the plant sees the command up to 400 ms late, and the estimate is quantised to ±2.5 percentage points (the controller's PSM resolves one full AC cycle out of 20 in that window). The resulting pressure ripple is about 0.1 bar.
- **The pump model is a single lumped model.** It reproduces the pressure/flow behaviour the controller's PID was tuned against, not the full electromechanical behaviour of a vibratory pump (no solenoid bounce, no per-stroke ripple).
- **The headspace phase is a flat plateau.** The first 12 ml (≈3 s) sit at ~0.5 bar, whereas a real pre-infusion ramps to 2–3 bar. The controller's headspace anti-windup freezes its integral below 1.5 bar, so this is conservative rather than misleading.
- **No boiler / steam wand model.** Only the brew path (grouphead, puck, cup) is simulated.
- **The post-shot tail is a model, not a measurement.** The residual flow out of the grouphead is modelled as an exponential decay (3.0 ml/s, 0.5 s time constant) rather than derived from the hydraulic model, because once the pump stops there is no pressure source left to drive the real flow balance. The resulting ~1.3 g of tail coffee is in the right range for a real machine, but the exact shape will differ.
- **BLE range is the real one.** The controller has to be within radio range of the simulator; the Bookoo GATT dialect (`0x0FFE` / `0xFF11` / `0xFF12`) is byte-for-byte the real one, but the radio link is not emulated.
