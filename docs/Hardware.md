# Hardware Guide

This guide covers the parts, wiring, soldering and first power-up of the ESP32-S3 controller, plus
the supported pumps, pressure sensors, scales and the companion display. For the firmware design
see [Architecture.md](Architecture.md); for day-to-day use see [Operation.md](Operation.md).

The controller leaves the machine's own control board in charge of the boilers, solenoid and
safety cut-outs. It connects at four points:

| # | Tap point | What the controller does | Loose resistors |
|:-:|---|---|---|
| 1 | Front-panel brew button | Presses the button electronically through an opto relay | 330 Ω |
| 2 | Pump power | Switches the pump's AC half-cycles through a TRIAC dimmer and watches the zero-cross | none |
| 3 | Brew-line pressure | Reads a 0.5–4.5 V transducer through a divider into an ADS1115 and the ESP32 ADC | 10 kΩ + 20 kΩ |
| 4 | 5 V supply | Powers the ESP32-S3 and the transducer | none |

The scale (Bluetooth LE) and the displays (Wi-Fi) need no wires to the machine.

## 1. Safety

This build works inside a machine that carries 110/230 V mains and a boiler under pressure. A
mistake can cause electric shock, fire or scalding.

- **Unplug the machine from the wall before opening it**, and keep it unplugged whenever you
  touch wiring. Switching it off at the front is not enough. Let the boiler cool and depressurise
  before you open any water fitting.
- **Mains and low-voltage wiring stay apart.** Route the 3.3/5 V wires away from mains wires and
  terminals, tie them down, and never let a low-voltage wire rest on a mains screw terminal.
- **The RobotDyn board has exposed mains copper.** Mount it in an insulated enclosure, away from
  the boiler and group head. Use high-temperature (silicone or PTFE) wire and insulated crimp
  terminals for all mains connections. Leave the machine's protective earth connected to the
  chassis.
- **Fuse.** The machine's own fuse usually protects far more than the pump draws. Consider a
  small slow-blow fuse in the live feed to the dimmer (for example 1 A at 230 V, 2 A at 110–120 V;
  a 48–61 W pump draws well under 1 A).
- **RC snubber across the pump.** Fit 39 Ω (1 W, flameproof) in series with 10 nF (X2-rated
  film capacitor, 275 VAC or higher) across the pump terminals on the dimmer `LOAD` side. It damps
  the inductive kick of the pump's solenoid coil when the TRIAC turns off.
- **Galvanic isolation.** Every connection to the machine's circuits passes through an optical
  barrier: the button opto relay, and the zero-cross and gate optocouplers on the RobotDyn board. The 5 V supply
  must be an isolated type (an HLK-5M05 module or a USB charger is; a capacitive dropper is not).
- **Opto relay choice matters:**
  - `TLP222A` and `AQY212EH` are **PhotoMOS** relays. The output is a pair of MOSFETs: no
    polarity, works on AC or DC, about 1 Ω on-resistance, rated 60 V.
  - `PC817` is a **phototransistor**. Its LED side is isolated from its output, but the output is
    polarised (collector must sit on the more positive switch terminal), DC only, and drops about
    0.1–0.2 V when on. It works on most logic-level buttons; if the button is read by a resistor
    ladder or analog input, use a PhotoMOS part.
  - None of these parts may be used across a **mains-voltage** switch. The tap point must be a
    low-voltage momentary button read by the machine's control board.
- **The pump depends on the controller.** The pump power runs through the TRIAC, so while the
  controller is unpowered, or holding the gate off after a fault, the pump does not run, including boiler autofill on machines
  that use the pump for it. In idle the controller passes 100 % power whenever the machine powers
  the pump (see [Operation.md](Operation.md)).
- **3.3 V logic only.** No ESP32-S3 GPIO may see 5 V. Power the RobotDyn and ADS1115 logic from
  `3V3`, never `5V`.

## 2. What you need

| Ref | Part | Alternative | Notes |
|---|---|---|---|
| U1 | ESP32-S3-DevKitC-1, **N8R8** module (8 MB flash, 8 MB octal PSRAM) | N16R8 | Build target `esp32s3-devkitc-1`, `partitions_8MB.csv`. N16R8 needs `board_upload.flash_size` changed and a 16 MB partition table (none ships with the repo). |
| U2 | ADS1115 16-bit I²C ADC breakout | none fitted: ESP32 ADC1 on GPIO 1 | Auto-detected at boot; optional but more precise. |
| U3 | `TLP222A` PhotoMOS relay (DIP-4) | `AQY212EH` (same pinout), `PC817` | See the opto notes in §1. |
| MOD1 | RobotDyn AC Light Dimmer, 1 channel, with zero-cross | Own board: MOC3021 + BTA16-600B + zero-cross opto | Logic `VCC` from 3.3 V. |
| S1 | Pressure transducer, 5 V supply, 0.5–4.5 V output, 1.2 MPa (12 bar) or 1.6 MPa (16 bar) | — | Set `sensor_mpa` to match (§7). 1/8" thread to suit the tee. |
| T1 | Brass tee, 1/8" NPT or BSP to match machine tubing and sensor | — | Fitted in the high-pressure line after the pump. |
| PSU | Isolated 5 V supply, at least 1 A (5 W) | USB-C 5 V / 2 A charger | Feeds the DevKit `5V` pin and the transducer. |
| R1 | 10 kΩ, 1/4 W, 1 % metal film (0.1 % if you can) | — | Divider top. |
| R2 | 20 kΩ, 1/4 W, 1 % metal film (0.1 % if you can) | — | Divider bottom. Use the same tolerance as R1. |
| R3 | 330 Ω, 1/4 W | — | Opto LED series resistor. |
| C1 | 100 nF, 50 V X7R ceramic (optional) | — | Across R2: low-pass filter, about 238 Hz. |
| R4 | 10 kΩ (optional) | — | GPIO 5 to GND: holds the gate off during reset before firmware runs. |
| SN1 | 39 Ω 1 W flameproof + 10 nF X2 film | — | Pump snubber (§1). |
| — | Perfboard or terminal block / lever nuts, heat-shrink, silicone wire | — | For the power rails. |

You need no extra pull-up or pull-down resistors in the standard build. ADS1115 breakouts carry
I²C pull-ups (add 4.7 kΩ from SDA and SCL to 3V3 only if yours has none). The RobotDyn board pulls
its `ZC` output up to its own `VCC`, and the firmware also enables the ESP32 internal pull-up.

### 2.1 Reading and placing resistors

Resistors have no polarity. Read the value with a multimeter on the Ω range if the colour bands
are unclear (10 kΩ: brown-black-orange; 20 kΩ: red-black-orange; 330 Ω: orange-orange-brown, on
4-band parts). In this build they are placed in two ways:

- **In series** (R3): the resistor is a link in the wire, `GPIO 6 → R3 → opto pin 1`.
- **Voltage divider** (R1 + R2): two resistors end to end between the sensor signal and GND. The
  joint between them, the **junction**, carries a smaller voltage and goes to the ADCs.

## 3. How it connects

```
 MACHINE SIDE (mains / factory logic)            CONTROLLER SIDE (5 V / 3.3 V DC)
 ====================================            ==================================
 Brew button A/B  <-------- U3 opto relay <----- R3 330R <---- GPIO 6  PIN_BTN_TRIG
                                                 GND --------> U3 pin 2
 Pump feed L/N --> [RobotDyn AC-IN]
                   [  TRIAC + optos ] ZC ------------------------> GPIO 4  PIN_ZC_IN
                   [                ] PSM <----------------------- GPIO 5  PIN_PSM_GATE
 Pump    <-------- [RobotDyn LOAD  ] VCC/GND <---- 3V3 / GND
   |  (snubber across pump)
   v
 Brass tee --> Transducer  OUT --> R1 10k --+--> ADS1115 AIN0 --SDA--> GPIO 8  PIN_I2C_SDA
               VCC <-- 5V                   |                 --SCL--> GPIO 9  PIN_I2C_SCL
               GND --> GND                  +--> GPIO 1  PIN_ADC_FALLBACK
                                            R2 20k to GND
 Optional UART display:            RX <----------------------------- GPIO 17 PIN_AUX_TX
                                   TX -----------------------------> GPIO 18 PIN_AUX_RX
 Onboard status LED (WS2812) <-------------------------------------- GPIO 48 PIN_RGB_LED
 Scale: Bluetooth LE        Displays / web app: Wi-Fi
```

## 4. GPIO map

This table follows `include/pins.h`. Every pin is a `#ifndef` macro, so you can change any of them
at build time without editing the source, for example
`PLATFORMIO_BUILD_FLAGS="-DPIN_ZC_IN=10" pio run`, or by adding `-DPIN_ZC_IN=10` to `build_flags`
in `platformio.ini`. No other file hard-codes a GPIO.

| GPIO | Macro | Direction / mode | Connects to | Notes |
|:-:|---|---|---|---|
| 1 | `PIN_ADC_FALLBACK` | Analog in, ADC1_CH0, 11 dB | R1/R2 junction | Used when no ADS1115 is found (or `force_ads` = 2). 32× oversampled. |
| 4 | `PIN_ZC_IN` | Input, `INPUT_PULLUP`, `RISING` interrupt | RobotDyn `ZC` | Edges closer than 6.5 ms are rejected as noise. |
| 5 | `PIN_PSM_GATE` | Output, active HIGH | RobotDyn `PSM`/`DIM`/`PWM` | Driven LOW at boot, on faults and before OTA. |
| 6 | `PIN_BTN_TRIG` | Output, active HIGH | R3 → U3 pin 1 | 180 ms pulse (`BTN_PULSE_MS`, config `buttonPulseMs`, 20–2000 ms). |
| 8 | `PIN_I2C_SDA` | I²C data, 400 kHz | ADS1115 `SDA` | |
| 9 | `PIN_I2C_SCL` | I²C clock, 400 kHz | ADS1115 `SCL` | |
| 17 | `PIN_AUX_TX` | UART1 TX, 115200 8N1 | Optional display `RX` | Newline-delimited JSON. Optional 1 kΩ series resistor. |
| 18 | `PIN_AUX_RX` | UART1 RX, 115200 8N1 | Optional display `TX` | Level-shift if the other device is 5 V. |
| 48 | `PIN_RGB_LED` | WS2812 data | Onboard RGB LED | Blue = boot, green = idle, cyan = brewing, red = fault. DevKitC-1 v1.1 boards have the LED on GPIO 38: build with `-DPIN_RGB_LED=38`. |

GPIO 7 is unused. Do not reassign pins to GPIO 19/20 (USB), 26–32 (SPI flash) or 33–37 (octal
PSRAM on N8R8), and avoid the strapping pins 0, 3, 45 and 46.

## 5. Power rails

Make three rails on perfboard or a terminal block:

| Rail | Source | Feeds |
|---|---|---|
| +5 V | PSU `+` | DevKitC `5V` pin, transducer red (`VCC`) |
| +3.3 V | DevKitC `3V3` pin (onboard regulator) | ADS1115 `VDD`, RobotDyn `VCC` |
| GND | PSU `−` | DevKitC `GND`, transducer black, R2 bottom leg, C1, ADS1115 `GND` and `ADDR`, RobotDyn `GND`, U3 pin 2, optional R4 |

The transducer needs 5 V to produce its full 0.5–4.5 V output. The RobotDyn and ADS1115 run on
3.3 V so their outputs never exceed 3.3 V. All DC grounds join on the GND rail; the machine side
of U3 is not connected to it.

Do not back-feed USB: if your DevKit has no diode between USB 5 V and the `5V` pin, disconnect the
external PSU while a USB cable is plugged in.

## 6. Module wiring

### 6.1 Brew button (U3 + R3)

The dot or notch on the chip marks pin 1. Pins 1 and 2 are the LED (ESP32 side). Pins 3 and 4 are
the switch output (machine side) and carry no resistor.

```
 GPIO 6 ---[R3 330R]---> pin 1 (anode)      pin 4 ----> button terminal A
 GND -----------------> pin 2 (cathode)     pin 3 ----> button terminal B
```

1. Solder R3 between a wire from GPIO 6 and U3 pin 1.
2. Wire U3 pin 2 to the GND rail.
3. Wire pins 3 and 4 across the two terminals of the machine's brew button, in parallel with it.
   The physical button keeps working.

LED current: (3.3 V − 1.2 V) / 330 Ω ≈ 6.4 mA. That is above the turn-on current of the PhotoMOS
parts (3 mA maximum) and, for a PC817 with the datasheet minimum 50 % current transfer ratio,
gives about 3 mA of output current, well above what a logic-level button input needs.

With a **PC817**, check two things first. Pin order on the output side varies: Sharp's PC817 has
pin 3 = emitter, pin 4 = collector, many clones are reversed (pins 1 and 2 are always anode and
cathode). And the collector must go to the button terminal that is more positive. Measure the DC
voltage across the open button with the machine on and probe only that low-voltage connector, or
use a PhotoMOS part and skip both checks.

After a soft start the firmware waits up to 600 ms for zero-cross pulses. If none arrive, it
reports `FAULT_BTN_NO_ACK`; check this wiring.

### 6.2 Pump power and zero-cross (RobotDyn dimmer)

The low-voltage header (`VCC`, `GND`, `ZC`/`Z-C`, `PSM`/`DIM`/`PWM`) goes to the ESP32. The screw
terminals (`AC-IN`, `LOAD`) carry mains.

| RobotDyn | Connects to |
|---|---|
| `VCC` | +3.3 V rail (not 5 V) |
| `GND` | GND rail |
| `ZC` | GPIO 4 |
| `PSM` / `DIM` / `PWM` | GPIO 5 (optional R4 10 kΩ from GPIO 5 to GND) |
| `AC-IN` (2 terminals) | The two wires that fed the pump (switched live and neutral from the machine) |
| `LOAD` (2 terminals) | The pump terminals, with the snubber across them |

With the machine unplugged, remove the two wires from the pump, connect them to `AC-IN`, and run
new high-temperature wires from `LOAD` to the pump. `AC-IN` must see the **switched** pump feed: the
zero-cross pulses are how the controller knows the machine is driving the pump. If AC is present
for more than 120 s in idle (`topologyTimeoutSec`), the controller assumes a permanent live and
raises `FAULT_TOPOLOGY` with the gate held off.

The zero-cross output gives one pulse per half-cycle: 100 per second on 50 Hz, 120 on 60 Hz. The
firmware detects the mains frequency from the pulse spacing (or you lock it with `mains_mode`),
treats 4 valid pulses within 45 ms as "pump powered", and a 35 ms gap as "pump off". How the pulses
are used to fire whole half-cycles is described in [Architecture.md](Architecture.md).

### 6.3 Pressure transducer, divider and ADS1115

**Fitting.** Put the brass tee in the high-pressure line between the pump and the boiler or brew
path, then screw the transducer into it. Use thread sealant suited to potable water and hot
pressurised lines, and leak-test under pressure before closing the machine.

**Divider maths.** The transducer outputs 0.5 V at 0 bar and 4.5 V at full scale. R1 and R2 scale
that by 20 k / (10 k + 20 k) = 2/3:

| Sensor output | Junction voltage | Meaning in firmware |
|---|---|---|
| 0.5 V | 333 mV | `sensorZeroMv` default 333.33 (0 bar; recalibrate with zero calibration) |
| 4.5 V | 3000 mV | `sensorFsMv` default 3000 (full scale: 12 or 16 bar) |
| below 0.2 V | below 133 mV | `minSensorMv`: open wire. During a controller-run shot: `FAULT_SENSOR_OPEN`, pump cut |
| above 4.575 V | above 3050 mV | Over-range or short: `FAULT_SENSOR_OVERRANGE`, pump cut |
| 5.0 V (signal shorted to supply) | 3333 mV | Still inside both ADC input limits |

Pressure = (junction mV − 333.33) / (3000 − 333.33) × full-scale bar. On a 12 bar sensor one bar is
about 222 mV at the junction; on a 16 bar sensor about 167 mV. The 2/3 ratio keeps a sensor fault
from pushing more than 3.33 V into the ESP32 or ADS1115, so no level shifter is needed.

```
 Transducer red   ---------------------------> +5 V rail
 Transducer black ---------------------------> GND rail
 Transducer OUT ---[R1 10k]---+--------------> ADS1115 AIN0
                              +--------------> ESP32 GPIO 1
                              +---[R2 20k]---> GND rail
                              +---[C1 100n]--> GND rail (optional)
```

1. Red wire to +5 V, black to GND. The signal wire is usually yellow, green or blue; check the
   sensor's label.
2. Solder R1 to the signal wire. Join R1's other leg to R2: that joint is the junction.
3. Solder R2's free leg (and C1, if fitted) to GND. **R1 goes to the sensor, R2 to GND.** Swapping
   them puts 1.5× the intended voltage on the junction.
4. Run one wire from the junction to ADS1115 `AIN0` and one to GPIO 1.
5. ADS1115: `VDD` to +3.3 V, `GND` to GND, `ADDR` to GND (address 0x48), `SDA` to GPIO 8, `SCL` to
   GPIO 9. Leave `ALRT`, `AIN1`, `AIN2` and `AIN3` unconnected.

**ADC selection.** At boot the firmware probes 0x48, 0x49, 0x4A and 0x4B (ADDR to GND, VDD, SDA or
SCL) and uses the first ADS1115 it finds: continuous mode, AIN0 to GND, `GAIN_ONE` (±4.096 V),
860 samples/s, read at 100 Hz. Without one it falls back to the internal ADC on GPIO 1. The config
key `force_ads` overrides this: 0 = auto, 1 = ADS1115 only, 2 = internal ADC only. Wiring the
junction to both inputs costs nothing and keeps the fallback working.

## 7. Supported pumps and sensors

The controller drives **vibratory pumps** (solenoid pumps with an internal diode). It does not
support rotary pumps. Presets live in `include/pump_presets.h`; select one with the `pump` config
key.

| Key | Pump | Power | Free flow at 0 bar | Stall pressure |
|---|---|---|---|---|
| `ULKA_EX5` (default) | Ulka EX5 / EP5 | 48 W | 10.8 ml/s | 15.0 bar |
| `ULKA_EX4` | Ulka EX4 / EP4 | 48 W | 7.5 ml/s | 18.5 bar |
| `OLAB_SILENT` | Olab Silent Green | 61 W | 9.5 ml/s | 14.5 bar |
| `CUSTOM` | Your own values | — | 9.0 ml/s start value | 15.5 bar start value |

The pump flow model and its calibration against the scale are explained in
[PumpFlow.md](PumpFlow.md).

**Pressure sensors.** Two ranges are supported through the `sensor_mpa` config key:

| `sensor_mpa` | Full scale | Default safety cut-off | Over-range fault (3050 mV) trips at |
|---|---|---|---|
| `"1.2"` (default) | 12 bar | 12.5 bar | about 12.2 bar |
| `"1.6"` | 16 bar | 15.0 bar | about 16.3 bar |

Pressure at or above the safety cut-off for more than 150 ms cuts the pump and stops the shot
(`maxSafetyPressureBar` overrides the default). With a 12 bar sensor the over-range fault trips
first, at about 12.2 bar. A 16 bar sensor gives readings with headroom above the cut-off, which is
the better choice if your machine's OPV is set near 12 bar. **Set `sensor_mpa` to match the part you
fit**; a 16 bar sensor read as 12 bar under-reads every pressure by 25 %. The fault handling is
described in [Architecture.md](Architecture.md).

## 8. Scales

### 8.1 Supported BLE scales

The controller is a Bluetooth LE central (NimBLE). In auto-pair mode it matches scales by
advertised service UUID or by name. These are the UUIDs in `src/scale/*.h`; short forms expand to
`0000xxxx-0000-1000-8000-00805f9b34fb`.

| Family | Service | Notify | Write | Name match | Tare |
|---|---|---|---|---|---|
| Bookoo Themis Ultra / Mini, companion scale | `0x0FFE` | `0xFF11` | `0xFF12` | `bookoo`, `themis`, `smartbrew`, `espresso_scale` | `03 0A 01 00 00 08` |
| Timemore Black Mirror Dot (TES017), Basic3 Link (TES016) - [open protocol](https://github.com/TIMEMORE-COFFEE/open-scale-protocol) | `0xFFF0` | `0xFFF1` (`A5 5A 01 01 00 09`, int32 BE weight 0.1 g, int16 flow 0.1 g/s, timer, overload) | `0xFFF2` (write without response) | `dot`, `timemore` | `A5 5A 03 0D 00 00` + CRC-16; software tare if not acknowledged |
| Timemore Black Mirror / Nano (older) | `0x181D` | `0x2A9D` | vendor `553f4e49-bf21-4468-9c6c-0e4fb5b17697` (then `0x2A9E`) | `black mirror`, `nano`, `timemore` | `00` |
| Acaia Lunar / Pearl / Pyxis | `0x1820` or vendor `49535343-fe7d-4ae5-8fa9-9fafd205e455` | `0x2A80` | `0x2A80` | `lunar`, `pearl`, `pyxis`, `acaia` | `EF DD 04` + 7 zero bytes; 3 s heartbeat |
| Decent Scale, Felicita (Arc), Cocinare, Skale | `0xFFF0` or `0xFFE0` | `0xFFF4` or `0xFFE1` | same as notify | `decent`, `felicita`, `arc`, `cocinare`, `skale`, `scale` | `03 0F 01 00 00`, then `03 0A 01 00 00 08`, then `'T'` |

The last family is matched by name in auto-pair mode. Its service UUIDs are only accepted when you
pair a specific MAC address. Frames from that family are checked with a trailing XOR byte.

### 8.2 Companion ESP32-C3 + HX711 scale

`companion-c3-scale/` is a separate PlatformIO project for a home-built scale. It speaks the Bookoo
profile (service `0x0FFE`, 20-byte frames on `0xFF11` at 40 Hz, tare on `0xFF12`), so the controller
treats it like any Bookoo scale and no special pairing is needed. The default advertised name is
`SMARTBREW_SCALE`, changeable per unit.

| ESP32-C3 SuperMini part | Connection (`companion-c3-scale/include/scale_config.h`) |
|---|---|
| HX711 module | DOUT → GPIO 4, SCK → GPIO 5, **RATE (pin 15) → 3V3 for 80 samples/s** |
| Load cell | HX711 inputs (see the companion README) |
| 0.91" SSD1306 OLED | SDA → GPIO 2, SCL → GPIO 3 (4.7 kΩ pull-ups to 3V3) |
| TTP223 touch key | OUT → GPIO 1 (tap = tare, hold ≥ 900 ms = display on/off) |
| Battery divider | 1 MΩ / 1 MΩ, junction → GPIO 0 |
| Status LED | GPIO 8 (onboard) |

The firmware runs a Kalman filter on the raw counts (Q = 0.02, R = 0.15) and a stability gate
against vibration. Power it through a 3.3 V regulator from a battery that can supply about 300 mA
peaks; a CR2032 alone sags under radio load. Full wiring, calibration and OTA:
[../companion-c3-scale/README.md](../companion-c3-scale/README.md).

## 9. Companion display

`display-waveshare-3.5/` is firmware for the **Waveshare ESP32-S3-Touch-LCD-3.5B**. It talks to the
controller over Wi-Fi (WebSocket and REST, found by mDNS), so it needs no wires to the machine;
only USB-C power.

| Item | Value (`display-waveshare-3.5/include/pin_config.h`) |
|---|---|
| MCU | ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM |
| Panel | 3.5" IPS, AXS15231B over QSPI, 320 × 480 native, used as 480 × 320 landscape |
| QSPI | CS 12, CLK 5, D0 1, D1 2, D2 3, D3 4 |
| Backlight | GPIO 6 |
| I²C bus (400 kHz) | SDA 8, SCL 7: touch 0x3B, TCA9554 expander 0x20 (pin 1 = LCD reset), AXP2101 PMIC 0x34 |

These pins belong to the display board and are unrelated to the controller's GPIO map. Build and
setup: [../display-waveshare-3.5/README.md](../display-waveshare-3.5/README.md). A UART display can
use GPIO 17/18 instead (§4).

## 10. Soldering checklist

Tick off each joint. Before you start: insulate every joint with heat-shrink, strain-relieve wires
where they leave the enclosure, and keep the machine unplugged throughout.

| ☐ | From | Via | To |
|:-:|---|---|---|
| 1 | PSU `+` | — | +5 V rail → DevKitC `5V` |
| 2 | PSU `−` | — | GND rail → DevKitC `GND` |
| 3 | DevKitC `3V3` | — | +3.3 V rail |
| 4 | GPIO 6 | R3 330 Ω | U3 pin 1 |
| 5 | U3 pin 2 | — | GND rail |
| 6 | U3 pin 3 | — | Brew button terminal B (PC817: emitter, more negative side) |
| 7 | U3 pin 4 | — | Brew button terminal A (PC817: collector, more positive side) |
| 8 | Transducer red | — | +5 V rail |
| 9 | Transducer black | — | GND rail |
| 10 | Transducer signal | R1 10 kΩ | Junction |
| 11 | Junction | R2 20 kΩ (and C1) | GND rail |
| 12 | Junction | — | ADS1115 `AIN0` and GPIO 1 |
| 13 | ADS1115 `VDD` | — | +3.3 V rail |
| 14 | ADS1115 `GND` and `ADDR` | — | GND rail |
| 15 | ADS1115 `SDA` | — | GPIO 8 |
| 16 | ADS1115 `SCL` | — | GPIO 9 |
| 17 | RobotDyn `VCC` | — | +3.3 V rail |
| 18 | RobotDyn `GND` | — | GND rail |
| 19 | RobotDyn `ZC` | — | GPIO 4 |
| 20 | RobotDyn `PSM` | (R4 10 kΩ to GND) | GPIO 5 |
| 21 | Machine pump feed (live, neutral) | mains wire | RobotDyn `AC-IN` |
| 22 | RobotDyn `LOAD` | mains wire | Pump terminals |
| 23 | Snubber 39 Ω + 10 nF | — | Across the pump terminals |

## 11. First power-up and verification

Test the low-voltage side first, with the machine **unplugged from the wall**.

1. **Resistance, everything unpowered.** With the transducer signal wire disconnected from R1:
   junction to GND reads about 20 kΩ; junction to R1's free leg about 10 kΩ. ADS1115 `ADDR` to GND
   reads 0 Ω. +3.3 V to GND and +5 V to GND must not read near 0 Ω (that is a short). Reconnect
   the signal wire.
2. **Low-voltage power on** (PSU or USB only, machine still unplugged):
   - The status LED shows blue, then green.
   - +3.3 V rail to GND: about 3.3 V, never 5 V. Transducer red to black: about 5 V.
   - Junction to GND at 0 bar: about 0.33 V. 0.00 V means the sensor's power or signal wire is
     open. About 0.5 V means R2 is not connected to GND.
   - Serial log at 115200 baud: `ADS1115 probe OK at 0x48` and `Pressure source: ADS1115`, or
     `Pressure source: internal ADC` if none is fitted.
   - `GET /api/v1/status` (or the dashboard) reports `adc_source` and `adc_mv`. `adc_mv` should
     agree with your meter reading at the junction to within a few mV. If it does not, check the
     wiring and the ADC selection before brewing.
   - Run zero-pressure calibration with the line at atmospheric pressure (`POST
     /api/v1/calibrate/zero` or the dashboard); it stores the measured `sensorZeroMv`.
3. **Mains test.** Close the machine so no mains part is exposed, plug it in and switch on.
   - Press the machine's own brew button. The pump runs at full power (idle passthrough), the log
     shows `Mains AC active in IDLE` and `Mains detected: 50Hz` (or 60 Hz), and the pressure reading
     rises.
   - Start a shot from the dashboard. The opto relay presses the button for 180 ms, the zero-cross
     burst confirms the start and the profile runs. `FAULT_BTN_NO_ACK` means no pump power followed
     the press: recheck §6.1.
   - Check for water leaks at the tee and sensor under pressure.

Bench tests without a machine are covered in [Testing.md](Testing.md).

## 12. Pin-to-pin reference

| Module pin | Goes to | Level | Note |
|---|---|---|---|
| DevKitC `5V` / `3V3` / `GND` | +5 V rail (input) / +3.3 V rail (output) / GND rail | 5 V / 3.3 V | |
| U3 pin 1 / pin 2 | R3 → GPIO 6 / GND | 3.3 V logic | ~6.4 mA when on |
| U3 pins 3, 4 | Brew button terminals | Machine logic | Isolated from the ESP32 |
| RobotDyn `VCC` / `GND` / `ZC` / `PSM` | +3.3 V / GND / GPIO 4 / GPIO 5 | 3.3 V logic | `PSM` high = TRIAC on |
| RobotDyn `AC-IN` / `LOAD` | Pump feed / pump | **Mains** | Snubber across pump |
| Transducer red / black | +5 V / GND | 5 V | |
| Transducer signal | R1 → junction | 0.5–4.5 V | Never directly to a GPIO |
| Junction | ADS1115 `AIN0`, GPIO 1, R2 → GND | 0.33–3.00 V | |
| ADS1115 `VDD` / `GND` / `ADDR` / `SDA` / `SCL` | +3.3 V / GND / GND / GPIO 8 / GPIO 9 | 3.3 V | `ADDR` low = 0x48; I²C 400 kHz |
| ADS1115 `ALRT`, `AIN1`–`AIN3` | unconnected | — | |
| UART display `RX` / `TX` | GPIO 17 / GPIO 18 | 3.3 V | Optional |
| Onboard WS2812 | GPIO 48 (38 on DevKitC-1 v1.1) | 3.3 V | Status LED |

Project overview and build instructions: [../README.md](../README.md).
