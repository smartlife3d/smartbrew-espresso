# Companion ESP32-C3 + HX711 Bluetooth Espresso Scale

> Part of the Smart Brew Controller - see the [main README](../README.md). How the controller pairs with this scale and uses its readings: [`docs/Hardware.md`](../docs/Hardware.md) (scales) and [`docs/PumpFlow.md`](../docs/PumpFlow.md) (cup flow and weight cutoff).

A drop-in replacement for the Bookoo Themis that speaks the **Bookoo BLE profile** natively:

0. **Wi-Fi OTA** — two paths: the setup portal (§6) for a plain browser upload on a
   fresh unit, and the explicit `ota` mode (§5) which joins the stored AP and
   serves ArduinoOTA on port 3232 for the bench.
1. **BLE (Bookoo-compatible)** — service `0x0FFE`, notify `0xFF11`, write
   `0xFF12`, streaming 20-byte Themis weight frames at **40 Hz**. The advertised
   name is generic and yours to set (`SMARTBREW_SCALE` by default, see §3).
   Accepted by the parent ESP32-S3 firmware and by any app or controller that
   supports Bookoo-protocol scales.

The **ESP-NOW transport has been removed**. It used to broadcast a 12-byte frame
at 40 Hz on Wi-Fi channel 1 for sub-5 ms cutoff responsiveness, but it kept a
second Wi-Fi station on the same channel as the parent MCU's dashboard telemetry
stream and competed with it for airtime — on the parent that showed up directly
as graph jitter and stalled telemetry frames. The parent MCU already parses the
Bookoo GATT profile (`src/scale/scale_bookoo.cpp`, service UUID `00000ffe`), so
the companion needs no special pairing mode: it advertises, the parent connects,
and the 40 Hz notify rate is the weight path.

Hardware: **ESP32-C3 SuperMini** + **HX711** load-cell module, RATE pin tied to
3V3 for 80 SPS sampling. No I2C, no display, no external libraries beyond
`NimBLE-Arduino`.

---

## 1. Wiring

### HX711 module → ESP32-C3

| HX711 pin | Signal       | ESP32-C3 GPIO | Notes |
| --------- | ------------ | ------------- | ----- |
| 1  | VSUP  | 3V3        | Module supply |
| 2  | VSUP  | 3V3        | Module supply |
| 3  | VGAT  | 3V3        | Analog gate |
| 4  | VFB   | GND        | |
| 5  | AGND  | GND        | Analog ground |
| 6  | VBG   | GND        | Bandgap ref |
| 7  | IN-A− | GND        | |
| 8  | IN-A+ | Load cell + | Strain-gauge input A |
| 9  | IN-B− | GND        | |
| 10 | IN-B+ | Load cell − | Strain-gauge input B |
| **11** | **DOUT / DATA** | **GPIO4** | Data out (open drain, module pull-up) |
| **12** | **SCK / PD_SCK** | **GPIO5** | Serial clock + power-down |
| 13 | XDA   | GPIO4 (tied to DOUT) | Dummy I2C pin, unused by firmware |
| 14 | XD    | GPIO4 (tied to DOUT) | Dummy I2C pin, unused by firmware |
| **15** | **RATE** | **3V3** | **HIGH ⇒ 80 SPS** (not 10 SPS) |

### ESP32-C3 → peripherals

| ESP32-C3 pin | Net name          | Destination | Notes |
| ------------ | ----------------- | ----------- | ----- |
| GPIO4        | HX711_DOUT        | HX711 pin 11 | Also tied to XDA/XD |
| GPIO5        | HX711_SCK         | HX711 pin 12 | |
| GPIO8        | LED               | SuperMini on-board LED | Active high, status indicator |
| GPIO0        | VBAT_DIV (ADC1_CH0) | Battery divider midpoint | 1 MΩ / 1 MΩ, add 100 nF to GND |
| **GPIO2**    | **OLED SDA**      | 0.91" OLED SDA | 4k7 pull-up to 3V3 |
| **GPIO3**    | **OLED SCL**      | 0.91" OLED SCL | 4k7 pull-up to 3V3 |
| **GPIO1**    | **TOUCH OUT**     | TTP223 OUT/SIG | Active high, module internal pull-up |
| 5V / VIN     | VBAT              | 3.3 V LDO input | **Not** USB 5 V directly — use a regulator |

### 0.91" OLED (IIC 12832, SSD1306)

| OLED pin | ESP32-C3 |
| -------- | -------- |
| GND      | GND |
| VCC      | 3V3 |
| SCL      | GPIO3 |
| SDA      | GPIO2 |

Firmware auto-detects the panel at **0x3C or 0x3D** over the core `Wire`
library at 400 kHz — no extra library dependency. If neither address answers,
the scale keeps running headless (BLE unaffected) and the LED slow
blinks.

### TTP223 capacitive touch key (HW-763)

| TTP223 pad | ESP32-C3 | Notes |
| ---------- | -------- | ----- |
| VCC        | 3V3      | 2.2–5.5 V |
| GND        | GND      | |
| OUT / SIG  | GPIO1    | default bridges = active-high **toggle** output |

With the factory solder bridges the module *latches*: touch → OUT high, touch
again → OUT low. The driver derives the press from the rising edge and emits the
event on the falling edge, so the **same code path** serves toggle and momentary
(`TOUCH_ACTIVE_HIGH` in `scale_config.h` sets the polarity).

| Gesture | Action |
| ------- | ------ |
| **Tap** (< 900 ms) | Software tare (same path as the BLE tare command) |
| **Hold** (≥ 900 ms) | Display power on/off — panel sleeps, radios stay alive |

One key only, by design: GPIO 10 is the only spare pin on the SuperMini, so a
second touch pad would crowd the breakout for no useful gain. Full behaviour is
in §2.8.

Deep sleep is intentionally not used: a latched TTP223 output cannot reliably
wake the C3, so "power" means panel off, which is the part that actually draws
current.

Power notes: a 3.3 V LDO (e.g. ME6211) feeds the ESP32-C3 from the Li-coin. Draw
≤ 300 mA peak (BLE + Wi-Fi radio active); a CR2032 alone will sag, use an
**18650 holder** or two CR123A in series for sustained espresso-shot current.

Battery divider: `VBAT --[1MΩ]--+--[1MΩ]-- GND`, tap at the junction into GPIO0.
Firmware multiplies the ADC reading by 2 and applies a 1.11× ADC trim, then maps
millivolts onto a CR2032/CR123A discharge curve.

---

---

## 2. Function reference

Every feature the firmware implements, in the order the board brings it up.

### 2.1 Boot sequence (`setup()`)

1. Serial 115200, LED off, `ScaleSample` zeroed.
2. `scaleIdentityInit()` — BLE name from NVS or the compiled default, plus the
   per-unit id from the chip MAC.
3. `batteryBegin()` + first ADC sample, so the display and the transport have a
   battery value before the first weight.
4. `wifiOtaInit()` — loads the stored OTA AP from NVS; stays offline.
5. `Hx711Sensor::begin()` — pins, 2×25 SCK reset pulses, 4 probe reads, Kalman
   seeded from the first valid count.
6. `bookooInit()` — GATT server, service, characteristics, advertising.
7. `setupWebInit()` — may claim the radio for the setup portal (see §6): fresh
   unit with no stored Wi-Fi, or the touch key held through power-on.
8. `displayUiInit()` — TTP223 input, OLED probe at 0x3C/0x3D.
9. Four tasks started (below) plus the version banner. `loop()` only idles.

Any missing peripheral is non-fatal: no OLED → headless, no HX711 → LED/status
report it, the radio keeps running.

### 2.2 Tasks

| Task | Rate | Prio | Stack | Role |
| ---- | ---- | ---- | ----- | ---- |
| `hx711` | one sample per conversion (80 SPS ≈ 12.5 ms) | 5 | 4096 | acquisition → Kalman → gate → tare → publish |
| `ble` | 40 Hz | 4 | 4096 | Bookoo notify, BLE tare requests, LED, battery sampling |
| `display` | 10 Hz | 4 | 4096 | render framebuffer, poll touch, actions |
| `log` | 2 Hz | 1 | 3072 | telemetry line + serial console |
| `ota` | on demand | 3 | 8192 | Wi-Fi OTA mode only (§5); self-deletes on exit |
| `portal` | on demand | 3 | 12288 | setup portal only (§6); self-deletes when closed |
| `loop` | 1 Hz | 1 | — | idle keep-alive |

`hx711` is the **sole writer** of the fused state and the **sole executor** of
tare requests; every other task reads a spinlock-protected snapshot, so BLE and
the display can never disagree about the weight.

### 2.3 Acquisition (`hx711_sensor.cpp`)

| Function | Behaviour |
| -------- | --------- |
| `begin` | DOUT = input (module pull-up), SCK = output idle low, 2×25 SCK pulses to reset, then up to 4 probe reads (80 ms each). Seeds the Kalman filter. |
| `readRaw` | Blocks until DOUT falls (new conversion), MSB-first 24-bit transfer, 25th SCK pulse starts the next conversion. 40 ms timeout → counted, returns false. |
| Timeout policy | 4 consecutive timeouts → `hx711Present = false`, gate windows reset, LED fast blink, OLED `HX ERR`. A single late sample never blanks the display. |
| Over-range | `|raw| > 8 000 000` → keeps the last sane grams, LED fast blink, OLED `RANGE`. |
| `powerUp` / `powerDown` / `reset` | Available for a future power-saving mode; not called periodically today. |

### 2.4 Filtering (`Kalman1D`)

Predict–correct 1D Kalman applied to **raw counts, before grams**:

```
P⁻ = P + Q              K = P⁻ / (P⁻ + R)
x  = x + K(z − x)       P  = (1 − K)·P⁻
```

Q = 0.02, R = 0.15, seed variance 1.0 — chosen for a step response under 25 ms
at 80 SPS. Re-seeded (not reset) at every tare, so the filter state stays
continuous and the weight step does not ring.

### 2.5 Weight, gate and tare

| Stage | Rule |
| ----- | ---- |
| Grams | `(filtered − tareCounts) / SCALE_CAL_COUNTS` |
| Gate window | running median of the last 5 accepted readings |
| Outlier | `|grams − median| > 40 g` |
| Reject | up to **3 consecutive** outliers hold the previous weight and set `outlierRejected` (OLED `HOLD`); the 4th is accepted, so a genuine fast pour is never lost |
| Auto tare | **once**, after ≥ 2000 ms from boot **and** when the last 20 accepted readings span ≤ 0.5 g; publishes 0.00 g with an empty cup |
| Manual tare | BLE write to `0xFF12` starting with `0x03`, or a **tap** on the TTP223 |
| Tare effect | `tareCounts = filtered`, Kalman re-seeded, median/settle/reject windows cleared, `[tare] auto|manual at <n> counts` logged |

Both tare paths call `scaleRequestTare()` and are executed inside the HX711 task,
so a tare can never interleave with a half-finished conversion.

### 2.6 Battery (`battery.cpp`)

| Aspect | Value |
| ------ | ----- |
| Input | GPIO0 / ADC1_CH0, 12-bit, `ADC_11db` attenuation |
| Averaging | 8 reads, 1 ms apart |
| Reconstruction | `pin_mV × (R_top+R_bot)/R_bot × 1.11` (ADC trim) |
| Curve | 9-point piecewise-linear CR2032/CR123A: 2700 mV→0 %, 3000→20, 3100→40, 3200→60, 3300→80, 3400→92, 3600→100, linear between, clamped 0–100 |
| Cadence | every 5 s, driven from the BLE task; the cached percent is shown on the OLED and on the setup portal page. The 20-byte Bookoo frame has no battery field, so it is not carried over BLE. |

### 2.7 Display (`ssd1306.cpp`, `display_ui.cpp`)

| Feature | Detail |
| ------- | ------ |
| Panel | SSD1306 128×32, auto-detected at **0x3C or 0x3D**, 400 kHz, contrast `0xCF` |
| Driver | 512-byte page framebuffer in RAM, own 5×7 ASCII font, drawn at 10 Hz and flushed in 32-byte chunks |
| Weight | 2× glyphs, right-aligned, 2 decimals, `-` when below tare |
| Flow | OLS slope over the last 10 samples (1 s), forced to `0.00` below 0.5 g or on a negative slope |
| Battery | percent text + battery outline with proportional fill bar |
| Status line | `OFF` · `RANGE` · `HX ERR` · `WAIT HX` · `HOLD` · `TARE`, overridden for 1 s by the last touch action (`TARE`/`ON`/`OFF`) |
| Power | hold-to-off sends the panel sleep command and stops flushing; radios and HX711 keep running, wake on the next hold |

### 2.8 Touch — single TTP223 key

| Gesture | Action |
| ------- | ------ |
| Tap (< 900 ms) | Software tare, OLED shows `TARE` for 1 s |
| Hold (≥ 900 ms) | Display power toggle |

40 ms debounce. The module latches by default, so the driver starts the press on
the **rising** edge and emits the event on the **falling** edge — identical
behaviour for the factory toggle configuration and for a momentary (bridge-cut)
module. Only one key is supported by design; GPIO 10 is the single spare pin on
the SuperMini, so a second key is deliberately left out rather than crowding the
breakout.

### 2.9 Transport 1 — BLE (`bookoo_ble.cpp`)

| Function | Behaviour |
| -------- | --------- |
| `bookooInit` | `NimBLEDevice::init(name)`, `ESP_PWR_LVL_P9`, server with advertise-on-disconnect, service `0x0FFE`, `0xFF11` (NOTIFY+READ), `0xFF12` (WRITE+WRITE_NR+READ), advertising payload = flags + name + complete service list |
| `bookooTick` | 40 Hz; **only notifies when a central has subscribed**; frame built, `setValue` + `notify`, counter incremented |
| `bookooBuildFrame` | 20 bytes: `0x02`, five zero bytes, sign `0x2B`/`0x2D` at `[6]`, hundredths-of-a-gram big-endian at `[7..9]` clamped to 24 bits, remaining bytes zero-padded |
| Tare write | Exact `{0x03,0x0A,0x01,0x00,0x00,0x08}` **or any write whose first byte is `0x03`** → `scaleRequestTare(false)`; other writes are logged and ignored |
| Name | `scaleDeviceName()` — generic by default, per-unit override in NVS (§3) |

`notify()` fans out to every subscribed central, so the parent MCU and any other
app on the phone see the same 40 Hz stream.

### 2.11 LED (`main.cpp`)

| Pattern | Condition |
| ------- | --------- |
| Solid | HX711 healthy, panel awake — i.e. connected and notifying |
| Slow blink (1 s) | `hx711Waiting` — no successful sample yet since boot |
| Fast blink (100 ms) | Fault: over-range, or HX711 lost after a good start |

Precedence: fault → slow blink → solid.

### 2.12 Wi-Fi OTA (`wifi_ota.cpp`)

Explicit, opt-in update mode — see §5 for the full workflow.

| Step | Behaviour |
| ---- | --------- |
| Storage | AP SSID/password in NVS namespace `wifiota`; survives reflash, OTA and power cycles |
| `ota` | Associates with the stored AP (15 s timeout), starts the ArduinoOTA server on port 3232 |
| Transfer | Progress logged every 10 %; on success the unit reboots into the new slot |
| Exit | 120 s idle timeout, or an AP-join failure → drops the AP |
| Safety | The update is written to the *other* app slot and only becomes bootable after `esp_ota_end()` validates it; a failed attempt never disturbs the running image |
| Indication | OLED shows `OTA MODE`; `status` prints `ota=READY (192.168.x.x:3232)` |

### 2.13 Setup portal (`setup_web.cpp`)

Opt-in SoftAP + one-page web UI — full workflow in §6.

| Aspect | Behaviour |
| ------ | --------- |
| Entry | Fresh unit (no stored Wi-Fi), touch key held ≥ 1.2 s at power-on, or `ap` on the console |
| AP | `espresso-scale-<unit id>`, WPA2 with `SETUP_AP_PASSWORD`, captive portal at `192.168.4.1` |
| Page | Live weight + battery (1 s poll), Wi-Fi SSID/password form, firmware upload with progress, BLE name form, reboot |
| Upload | multipart streamed to `Update.begin/write/end` — validated into the spare slot, then reboots |
| Exit | 5 min idle timeout, or `ap exit` |
| Indication | OLED status line shows `SETUP AP`; `status` prints the AP SSID and client count |

### 2.14 Serial console (115200 baud)

| Command | Effect |
| ------- | ------ |
| `v` / `version` | firmware version, build date/time, unit id, BLE name |
| `status` | weight, battery, HX711 state, OTA state, current SSID |
| `name` | print `name="…" unit=XXXX` |
| `name <text>` | validate (1–28 printable ASCII, no edge spaces), store in NVS, **takes effect after a reboot** |
| `name reset` | delete the stored name; compiled default returns |
| `wifi <ssid> [pass]` | store the OTA access point (NVS); `wifi clear` forgets it |
| `ota` | enter Wi-Fi update mode (§5) |
| `ap` | enter the setup portal (§6) |
| `ap exit` | close the portal and resume advertising |
| `help` | command list |
| anything else | `[cmd] unknown command …` |

A partial line is discarded after 2 s of silence, so a stray character cannot
wedge the console. The 2 Hz telemetry line is prefixed `[scale]` and carries
weight, raw counts, battery, gate state, HX711 state, BLE state and the unit id.

### 2.15 Configuration reference (`include/scale_config.h`)

| Macro | Default | Purpose |
| ----- | ------- | ------- |
| `PIN_HX711_DOUT` / `PIN_HX711_SCK` | 4 / 5 | HX711 data / clock |
| `PIN_LED` / `PIN_BATTERY_ADC` | 8 / 0 | status LED / battery divider tap |
| `PIN_I2C_SDA` / `PIN_I2C_SCL` | 2 / 3 | OLED |
| `PIN_TOUCH_OUT` | 1 | TTP223 output |
| `SSD1306_CONTRAST` / `DISPLAY_RATE_HZ` | `0xCF` / 10 | panel contrast / refresh rate |
| `TOUCH_ACTIVE_HIGH` | `true` | invert for a bridged-low module |
| `TOUCH_DEBOUNCE_MS` / `TOUCH_HOLD_MS` | 40 / 900 | debounce / tap-vs-hold |
| `HX711_SPS` / `HX711_CONVERSION_MS` | 80 / 12 | hardware rate (RATE pin on 3V3) |
| `HX711_READ_TIMEOUT_MS` | 40 | DOUT wait before a miss is declared |
| `HX711_PRESENCE_FAIL_COUNT` | 4 | consecutive misses before "missing" |
| `HX711_OVER_RANGE_COUNTS` | 8 000 000 | ±full-scale threshold |
| `CAL_REFERENCE_WEIGHT_G` | 100.0 | calibration reference |
| `SCALE_CAL_COUNTS` | 23500.0 | **raw counts per gram — calibrate this** |
| `SCALE_DEFAULT_DISPLAY_G` | 0.0 | weight shown before the first sample |
| `SCALE_KALMAN_Q` / `_R` / `_SEED_VARIANCE` | 0.02 / 0.15 / 1.0 | filter tuning |
| `GATE_MEDIAN_WINDOW` / `GATE_OUTLIER_G` / `GATE_MAX_REJECTS` | 5 / 40.0 / 3 | stability gate |
| `AUTO_TARE_DELAY_MS` / `_WINDOW` / `_SPREAD_G` | 2000 / 20 / 0.5 | auto tare |
| `SCALE_DEVICE_NAME` | `SMARTBREW_SCALE` | advertised BLE name (neutral default for commercial distribution) |
| `SCALE_NAME_MAX` | 28 | name validation limit |
| `SCALE_SERVICE_UUID` / `BOOKOO_NOTIFY_UUID` / `_WRITE_UUID` | `0x0FFE` / `0xFF11` / `0xFF12` | GATT identifiers |
| `BOOKOO_FRAME_LEN` / `BOOKOO_RATE_HZ` | 20 / 40 | frame size / notify rate |
| `BOOKOO_SIGN_POS` / `_NEG` | `0x2B` / `0x2D` | sign bytes |
| `SCALE_WIFI_CHANNEL` | 1 | fallback AP channel for the setup portal and OTA mode |
| `BATTERY_DIVIDER_TOP_OHMS` / `_BOTTOM_OHMS` / `_TRIM` | 1 M / 1 M / 1.11 | divider + ADC correction |
| `BATTERY_SAMPLE_INTERVAL_MS` | 5000 | ADC cadence |
| `LED_FAULT_BLINK_MS` / `LED_WAIT_BLINK_MS` / `LED_SOLID` | 100 / 1000 / true | LED patterns |
| `SCALE_FW_VERSION` / `SCALE_FW_BUILD` | `1.1.0` / build stamp | reported by `v` and `status` |
| `OTA_PORT` | 3232 | ArduinoOTA TCP port |
| `OTA_HOSTNAME_PREFIX` | `espresso-scale-` | hostname is prefix + unit id |
| `OTA_PASSWORD` | `espresso` | **change before selling** |
| `OTA_TIMEOUT_MS` / `OTA_CONNECT_TIMEOUT_MS` | 120000 / 15000 | idle exit / AP join timeout |
| `OTA_TASK_STACK` | 8192 | OTA task stack |
| `SETUP_AP_PASSWORD` | `password` | **change before selling** — WPA2 key of the setup portal |
| `SETUP_AP_CHANNEL` / `SETUP_AP_MAX_CONNECTIONS` | 1 / 4 | portal AP radio settings |
| `SETUP_WEB_PORT` / `SETUP_WEB_TASK_STACK` | 80 / 12288 | HTTP port / portal task stack |
| `SETUP_AUTO_WHEN_UNCONFIGURED` | 1 | fresh unit starts the portal |
| `SETUP_HOLD_BOOT_TO_ENTER` / `SETUP_BOOT_HOLD_MIN_MS` | 1 / 1200 | hold the touch key at power-on to re-enter |
| `SETUP_IDLE_TIMEOUT_MS` | 300000 | auto-exit the portal (battery) |
| `TASK_*_PRIO` / `TASK_*_STACK` | see header | FreeRTOS tuning |
---

## 3. Naming, identity and resale

The **advertised BLE name is yours** — it carries no protocol meaning and no
third-party brand. What makes the scale interoperable is the *protocol*
(service `0x0FFE`, notify `0xFF11`, write `0xFF12`, 20-byte frame), which is
independent of the name.

| | Value |
| - | ----- |
| Compiled default | `SMARTBREW_SCALE` |
| Default name | `SMARTBREW_SCALE` (configurable via `scale_config.h`) |
| Override | `name <text>` on the serial console, stored in NVS |
| Per-unit id | last 2 bytes of the chip MAC, logged as `id=XXXX` and printed at boot |

```bash
pio device monitor      # 115200 baud
name                   # -> name="SMARTBREW_SCALE" unit=A3F2
name MYCOFFEE_01       # stored in NVS, takes effect after a reboot
name reset             # back to the compiled default
```

Rules the firmware enforces: 1–28 printable ASCII characters, no leading or
trailing space. Names survive power cycles and OTA reflashes (NVS namespace
`scaleid`).

> **Integration note.** The parent ESP32-S3 firmware identifies this scale by the
> **advertised GATT service UUID**, not by name — `src/scale/scale_manager.cpp`,
> `matchScale()`:
> `if (has("00000ffe") || has("0ffe") || hasName("bookoo") || hasName("themis")) { outLink = ScaleLink::BOOKOO; ... }`
> The companion advertises service `0x0FFE`, so it is recognised under any name,
> including `SMARTBREW_SCALE`, `ESPRESSO_SCALE`, and per-unit overrides.

> **Commercial note.** Shipping with a third-party brand in the advertised name
> is the part that carries the most trademark / passing-off risk; the GATT
> compatibility itself is ordinary reverse-engineering interop. This is not
> legal advice — have your own IP attorney confirm the labelling, packaging and
> app-listing wording before selling.

---

## 4. Build, flash, monitor

```bash
cd companion-c3-scale
pio run                 # build
pio run -t upload       # flash over USB-CDC
pio device monitor      # 115200 baud telemetry
```

Targets `esp32-c3-devkitm-1`, framework Arduino, partition table
`partitions_4MB.csv` (nvs, otadata, app0 1.5 MB, app1 1.5 MB, littlefs 512 KB,
coredump 64 KB). `lib_deps` contains only `h2zero/NimBLE-Arduino@^2.5.2`; the HX711
driver and the SSD1306 driver are in-tree (the latter uses the core `Wire`
library that ships with `framework-arduinoespressif32`). Build flags include
`-DCONFIG_ESP_TASK_WDT_TIMEOUT_S=15`.

---

---

## 5. OTA updates over Wi-Fi

The scale does not join an AP in normal operation — Wi-Fi stays unassociated and
the BLE link carries the weight. **Update mode is therefore explicit**, entered
from the serial console.

The OTA server is the framework's own `ArduinoOTA` (no `lib_deps` entry), and the
two 1.5 MB app slots plus `otadata` are already in `partitions_4MB.csv`, so a new
image is written to the *other* slot and only becomes active after it validates.

### 5.1 One-time setup per unit (stored in NVS)

```
> wifi MyAP mypassword
[ota] AP stored: ssid="MyAP" pass=set
```

The AP details survive reflash, OTA and power cycles. `wifi clear` forgets them.

### 5.2 Enter update mode

```
> ota
[ota] joining "MyAP" as espresso-scale-A3F2 ...
[ota] READY at 192.168.1.50:3232 - run: pio run -t upload --upload-port 192.168.1.50
```

While update mode is active:

* BLE keeps running at its normal rate — NimBLE and Wi-Fi coexist on the C3's one radio and
  time-slice it, so the scale stays usable and the parent MCU keeps seeing weights;
* the OLED status line shows `OTA MODE`;
* `status` reports the live state: `IDLE` / `CONNECTING` / `READY` / `TRANSFER` / `FAILED`;
* if the AP is unreachable, or nothing arrives for `OTA_TIMEOUT_MS` (120 s), the unit
  drops the AP and **resumes advertising automatically**.

### 5.3 Push the new image from your machine

```bash
# Arduino CLI (network upload)
arduino-cli compile --fqbn esp32:esp32:esp32c3 .
arduino-cli upload  --fqbn esp32:esp32:esp32c3 -p 192.168.1.50

# or Espressif's espota.py against the same protocol
python3 espota.py -i 192.168.1.50 -p 3232 -f .pio/build/esp32-c3-devkitm-1/firmware.bin
```

The password is `OTA_PASSWORD` (`espresso` in `scale_config.h`) — **change it before
you ship units**. Any ESP32 OTA client works: Arduino IDE's *Upload using Network*
(port 3232) works too.

> `pio run -t upload --upload-port <ip>` does **not** work here: PlatformIO drives
> esptool over a serial port, so it cannot speak the OTA protocol.

### 5.4 After the update

The unit reboots into the new slot, resumes BLE advertising and drops the AP.
Confirm with:

```
> v
[fw] 1.1.0  built Oct 17 2026 21:12:03  id=A3F2  name="SMARTBREW_SCALE"
```

### 5.5 Practical notes

| Topic | Note |
| ----- | ---- |
| Power | Keep USB connected during the update. A ~1 MB image takes 1–3 minutes; a CR2032 will not survive it. |
| Failure | A failed or aborted update leaves the running slot untouched — the unit keeps advertising. USB is the recovery path if a bad image is ever set as boot. |
| Headroom | Current image ≈ 1.04 MB of a 1.5 MB slot. |
| Customers | Wi-Fi OTA needs an AP on the customer's network, so it is a *you* update path, not a customer self-service one. |
---

---

## 6. Setup portal (SoftAP + browser)

A customer needs no laptop, no terminal and no internet: the scale hosts its own
Wi-Fi hotspot and serves one web page for setup and firmware updates.

### 6.1 When it runs

| Trigger | Behaviour |
| ------- | --------- |
| **No Wi-Fi stored** (fresh unit) | Enters setup mode automatically after boot — this is the normal customer path |
| **Touch key held during power-on** (≥ 1.2 s) | Enters setup mode; use this later to reconfigure or update a unit that already has Wi-Fi |
| `ap` on the serial console | Enters setup mode |

It leaves by itself after `SETUP_IDLE_TIMEOUT_MS` (5 min) with no client attached,
and `ap exit` returns it to normal operation.

### 6.2 Using it

```
1. Join the Wi-Fi network:  espresso-scale-XXXX     password: password
2. Open http://192.168.4.1/    (phones may show a "sign in to network" popup first)
3. Live card: current weight and battery, refreshed every second
4. "Home Wi-Fi": enter SSID + password, tap Save Wi-Fi  -> stored in NVS
5. "Firmware": choose firmware.bin, tap Upload & install -> streamed to the spare
   app slot, progress bar, then an automatic reboot into the new image
6. "Device": change the Bluetooth name (max 28 chars, applies after reboot) or
   reboot the unit
```

The page is responsive, so a phone works as well as a laptop. All assets are
inline — the portal needs no internet connection at all.

### 6.3 Details

| Aspect | Detail |
| ------ | ------ |
| Access point | `espresso-scale-<unit id>`, WPA2, `SETUP_AP_PASSWORD` (**`password`**), channel 1, 4 clients max |
| Address | `192.168.4.1`, captive portal via `DNSServer` so any hostname resolves to it |
| Server | `WebServer` on port 80; endpoints `/`, `/api/state`, `/wifi`, `/name`, `/reboot`, `/update` |
| Firmware upload | multipart streamed into `Update.begin/write/end` → validated, then the new slot becomes bootable. A failed upload keeps the running image. |
| Radios | BLE and the SoftAP share the C3's one radio and time-slice it. The portal still advertises, so a scale can be configured and paired at the same time. |
| Nothing to lose | Wi-Fi credentials and the BLE name live in NVS, so they survive the reboot that follows an upload. |

### 6.4 Security notes

* `password` is a weak WPA2 key — anyone nearby who knows it can reconfigure or
  reflash a unit that is sitting in setup mode. Set a private
  `SETUP_AP_PASSWORD` before you ship.
* The portal has no login beyond the WPA2 key, which is deliberate: it is only
  reachable when the unit is explicitly in setup mode, i.e. unconfigured or with
  the touch key held at power-on.
* `SetUpgradeEnable` is not used — the OTA server is never exposed while the unit
  is advertising, so there is no remote-update door in normal operation.

---

## 7. Calibration

`include/scale_config.h`:

```c
#define CAL_REFERENCE_WEIGHT_G   100.0f
#define SCALE_CAL_COUNTS         23500.0f   // raw counts per gram
```

Place `CAL_REFERENCE_WEIGHT_G` on the pan, read `raw=` from the serial log after
taring, then:

```
SCALE_CAL_COUNTS = raw_with_weight * CAL_REFERENCE_WEIGHT_G / measured_weight_grams
```

---

## 8. Signal chain

```
HX711 24-bit raw counts  (80 SPS, 12.5 ms)
   -> Kalman 1D on RAW COUNTS (Q = 0.02, R = 0.15, step latency < 25 ms)
   -> grams = (filtered - tareCounts) / SCALE_CAL_COUNTS
   -> stability gate: reject up to 3 consecutive readings > 40 g from the
      running median of the last 5 samples (vibration lockout, scale side)
   -> statePublish()  -->  BLE task (40 Hz)
```

**Auto tare:** once, 2.0 s after boot, when the last 20 accepted readings span
≤ 0.5 g — the scale reads `0.00 g` with an empty cup.

**BLE tare:** any write to `0xFF12` starting with `0x03` (in particular the
Bookoo command `{0x03, 0x0A, 0x01, 0x00, 0x00, 0x08}`) triggers a software
tare.

---

## 9. Protocol — Transport 1: BLE (0x0FFE)

Advertised name: whatever `scaleDeviceName()` resolves to — `SMARTBREW_SCALE` by
default, overridable per unit (see §3). The name is independent of this table;
the UUIDs and frame layout are what the apps parse.

| UUID | Properties | Direction | Payload |
| ---- | ---------- | --------- | ------- |
| `0x0FFE` | — | service | — |
| `0xFF11` | NOTIFY, READ | scale → app | 20-byte weight frame, 40 Hz |
| `0xFF12` | WRITE, WRITE_NR, READ | app → scale | tare command |

### 20-byte weight frame

| Offset | Size | Field | Value |
| ------ | ---- | ----- | ----- |
| `[0]` | 1 | frame type | `0x02` |
| `[1..5]` | 5 | reserved | `0x00` |
| `[6]` | 1 | **sign** | `0x2B` = `'+'`, `0x2D` = `'-'` |
| `[7]` | 1 | magnitude MSB | `((b7<<16) \| (b8<<8) \| b9) >> 16` |
| `[8]` | 1 | magnitude | |
| `[9]` | 1 | magnitude LSB | |
| `[10..19]` | 10 | padding | `0x00` |

Decode exactly as the parent firmware does:

```c
float grams = ((frame[7] << 16) | (frame[8] << 8) | frame[9]) / 100.0f;
if (frame[6] == 0x2D) grams = -grams;   // 0x2B = '+'
```

The value field is hundredths of a gram in a 24-bit big-endian integer
(max 167 772.15 g).

---

## 10. What the ESP-NOW transport used to be, and why it is gone

The companion used to carry the weight on **two** radios at once:

1. the Bookoo BLE GATT profile above, at 20 Hz; and
2. an ESP-NOW broadcast on Wi-Fi channel 1 — a 12-byte frame at 40 Hz, sender only,
   no promiscuous mode, no receive callback, destination a compile-time constant
   (`-D SCALE_ESPNOW_PEER_MAC=\"24:6f:28:aa:bb:cc\"`, broadcast by default).

The ESP-NOW frame carried `magic 0xA5`, an 8-bit sequence counter, flags, the battery
percentage, the net weight as a little-endian float32, a status byte and a CRC-8/ATM
over bytes `[0..10]` (polynomial `0x07`, init `0x00`, check value for `"123456789"`
is `0xF4`).

**It has been removed, on both sides.** The reasons were practical, not theoretical:

* The companion's ESP-NOW sender put a **second Wi-Fi station on the same channel** as the
  parent MCU's dashboard telemetry stream. Both radios then competed for the same airtime,
  and the parent showed it directly as jitter and stalls in the graph.
* The ESP-NOW link also **forced the Wi-Fi radio on** for a scale that has no reason to be
  on Wi-Fi at all — a battery-powered device burning its radio to serve one peer.
* It bought nothing the BLE link did not already provide. The parent MCU already parses
  the Bookoo GATT profile (`src/scale/scale_bookoo.cpp`, service UUID `00000ffe`), and
  `bookoo_ble.cpp` uses `notify()`, which fans out to every subscribed central. The BLE
  notify rate was raised from 20 Hz to **40 Hz** to recover the granularity the ESP-NOW
  path used to provide.

The weight path is now the Bookoo BLE profile alone, at 40 Hz.

---

## 11. Display (SSD1306 128×32) and touch

```
 y 0..13    [  12.34 g ]      [86] [batt]     weight, 2x glyphs
 y 17..23   F  3.21 g/s                        flow rate, 1 s OLS window
 y 25..31   TARE / HOLD / HX ERR / RANGE / OFF status line
```

* **Weight** — the same stability-gated value sent over BLE, 2 decimals,
  `-` sign when the pan is lighter than the tare.
* **Flow** — ordinary least squares slope over the last 10 samples (1 s at the
  10 Hz render rate), forced to `0.00` below 0.5 g or a negative slope.
* **Battery** — cached Li-coin percent plus a proportional battery icon.
* **Status** — `WAIT HX` (no sample yet), `HX ERR` (lost after a good start),
  `RANGE` (over-range), `HOLD` (stability gate is suppressing an outlier),
  `TARE`, or the last touch action for 1 s.
* Tap = tare, hold = panel off/on (see §1).

Display refresh is 10 Hz; a full frame is 512 bytes pushed in 32-byte I2C
chunks, so the bus is idle most of the time and the radio is unaffected.

| Pattern | Meaning |
| ------- | ------- |
| **Solid** | Scale healthy and connected / notifying |
| **Slow blink** (1 s) | Waiting for the HX711 — no successful sample yet |
| **Fast blink** (100 ms) | Fault: HX711 missing after a good start, or over-range |

---

## 12. Troubleshooting

| Symptom | Check |
| ------- | ----- |
| LED fast-blinks, weight frozen | DOUT/SCK swapped or HX711 unpowered. HX711 pin 15 (RATE) must be on 3V3 for 80 SPS — on 10 SPS the loop still works but is 8× slower. |
| LED slow-blinks forever | HX711 never answered. Verify 3V3 on pins 1–3, GND on 4–7, GPIO4→pin 11, GPIO5→pin 12. |
| `raw=0` constantly | Load cell not connected (pins 8/10) or module unpowered. |
| Weight drifts / jumps 40 g+ | Mechanical play in the pan, or a marginal USB supply. The gate holds only 3 consecutive outliers; anything beyond is a genuine reading. |
| Battery % looks wrong | Divider ratio wrong (firmware assumes 1 MΩ/1 MΩ) or `BATTERY_DIVIDER_TRIM` needs calibration against a DMM. |
| BLE name not visible | Pairing cache — forget the unit and re-scan; the scale advertises continuously. The name in use is printed at boot and via the `name` command (§3). |
| Parent MCU does not see the scale | The parent matches the Bookoo service UUID `0x0FFE`; check the MCU's `scale_mac` setting is `AUTO` (scan) or this unit's MAC, and that the scale is advertising (`status` shows `ble=advertising`). |
| OLED stays blank, log says `not found at 0x3C/0x3D` | Check VCC=3V3, GND, SDA→GPIO2, SCL→GPIO3 and 4k7 pull-ups to 3V3. Some modules ship with the 4k7 resistors already on the module. |
| OLED garbled / half the screen wrong | 128×64 module fitted instead of 128×32 — the init sets multiplex 32 and COM pins for 32 rows. |
| Touch does nothing | TTP223 OUT must go to GPIO1; if the module is strapped active-low, set `TOUCH_ACTIVE_HIGH false`. Check `rawPressed()` in the log. |
| Tap tares twice (or the long press needs two touches) | Expected with the latched/toggle configuration: the event fires on the falling edge, so a toggle module needs a second touch to release. Cut the "toggle" solder bridge for momentary mode and the same firmware gives single-gesture behaviour. |
| Flow stuck at 0.00 | Normal below 0.5 g or on a non-positive slope; the window needs 3 rendered samples (~300 ms). |
| S3 never finds the scale after renaming | The parent matches the advertised service UUID `0x0FFE`, not the name, so a rename cannot break it. If it still is not found, check the MCU's `scale_mac` setting and that the scale is advertising (`status` shows `ble=advertising`). |
| `[cmd] ERROR: name must be...` | Serial command rejected: 1–28 printable ASCII chars, no leading/trailing space. |
| `ota` says `could not join` | SSID/password wrong or the AP is out of range: re-store with `wifi <ssid> <pass>`. The unit returns to advertising by itself. |
| Update fails with an error code | `ota_error_t` value is printed; 2 = auth failed (check `OTA_PASSWORD`), 4/5 = connection lost (AP or power). |
| `pio run -t upload --upload-port <ip>` does nothing | Expected — PlatformIO only uploads over serial. Use `arduino-cli upload -p <ip>` or `espota.py`, see §5.3. |
| Portal not visible after joining the AP | Phone may still be on cellular data; the "sign in to network" popup must be accepted, or open `http://192.168.4.1/` manually. |
| Portal page loads but upload stalls | Expected on a 1 MB image — 1–3 minutes over the soft AP. Keep USB power on. |
| BLE weight stalls during an OTA | The C3 shares one radio between NimBLE and Wi-Fi, so the notify rate dips while the transfer is running. It returns to 40 Hz when update mode ends. |

---

## 13. Layout

```
companion-c3-scale/
├── platformio.ini
├── partitions_4MB.csv
├── README.md
├── include/
│   └── scale_config.h     pins, calibration, filter constants, shared state
└── src/
    ├── main.cpp           setup/loop: HX711, BLE, display, LED, log tasks
    ├── hx711_sensor.h/.cpp    bit-banged 80 SPS driver + Kalman1D
    ├── bookoo_ble.h/.cpp      NimBLE 0x0FFE service, frame builder, tare
    ├── ssd1306.h/.cpp         128x32 OLED driver (core Wire) + 5x7 font
    ├── display_ui.h/.cpp      weight/flow/battery layout, touch actions, task
    ├── touch_button.h/.cpp    TTP223 debounce + tap/hold events
    ├── scale_identity.h/.cpp   per-unit BLE name (NVS) + unit id
    ├── wifi_ota.h/.cpp         OTA mode: AP credentials, ArduinoOTA server
    ├── setup_web.h/.cpp        SoftAP setup portal: Wi-Fi, BLE name, browser OTA
    └── battery.h/.cpp         divider read + Li-coin discharge curve
```