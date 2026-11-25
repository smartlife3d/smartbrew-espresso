# SmartBrew Client Display (Waveshare 3.5" ESP32-S3-Touch-LCD-3.5B)

> Part of the Smart Brew Controller - see the [main README](../README.md). Using the display: [`docs/Dashboard.md`](../docs/Dashboard.md). Building and flashing all programs: [`docs/Operation.md`](../docs/Operation.md). Telemetry frame it decodes: [`docs/API.md`](../docs/API.md).

A dedicated, standalone client display firmware for the **Waveshare ESP32-S3-Touch-LCD-3.5B** development board. It connects to the SmartBrew Espresso Profile Controller over Wi-Fi via WebSocket and HTTP REST, delivering a real-time OLED cockpit experience matching the Web and Android touch displays.

---

## Hardware Specifications

- **Microcontroller**: ESP32-S3R8 (Dual-core Xtensa LX7 @ 240 MHz, 16 MB Flash, 8 MB Octal PSRAM)
- **Display**: 3.5-inch IPS LCD panel (AXS15231B controller, Hardware QSPI mode)
- **Native Panel Resolution**: 320 × 480 portrait
- **Logical Cockpit Resolution**: 480 × 320 landscape (achieved via sequential PSRAM burst rotation buffer)
- **Touch**: Capacitive Touch (AXS15231B via I2C at `0x3B`)
- **PMIC**: AXP2101 Power Management IC (I2C at `0x34`)
- **I/O Expander**: TCA9554 (I2C at `0x20`, Pin 1 controls LCD hardware reset)
- **Pinout**:
  - QSPI: `CS=12`, `CLK=5`, `D0=1`, `D1=2`, `D2=3`, `D3=4`
  - Backlight: `GPIO 6`
  - I2C Bus: `SDA=8`, `SCL=7` (Fast-mode 400 kHz)

---

## Architecture & Features

### 1. Dual-Core Pipeline
- **Core 1 (UI & Rendering)**: Drives LVGL 8.3.11 with 3× 300 KB framebuffers in PSRAM (`disp_draw_buf1`, `disp_draw_buf2`, `rotated_fb`). Performs zero-cache-miss 90° clockwise rotation writes directly to the native portrait AXS15231B panel without controller memory corruption.
- **Core 0 (Networking & Telemetry)**: Connects to Wi-Fi station, auto-discovers SmartBrew Controller via mDNS (`smartbrew.local`), maintains a full-duplex WebSocket connection (`ws://<controller-ip>/ws`), and synchronizes profiles via HTTP REST.
- **Thread Safety**: Telemetry data is transferred to the UI via a FreeRTOS mutex (`g_telemetryMutex`), ensuring stutter-free 50 FPS animations during heavy network bursts.

### 2. UI Layout & Cockpit Design
- **Top Header Bar**:
  - **Profile Selector Dropdown**: Automatically populated with profiles from the controller (`GET /api/v1/profiles`). Selecting a profile dispatches `POST /api/v1/profiles/active`.
  - **State Pill**: Displays machine state (`IDLE`, `BREWING`, `PRE-INFUSION`, `EXTRACTION`, `DONE`, `FLUSH`, `STOPPED`, `ABORT`) with dynamic color coding.
  - **Controller Portafilter Connection Pill**: Displays a custom 8-bit alpha-masked portafilter icon recolored to match the connection state (Emerald Green `#10b981` when connected, Rose Red `#f43f5e` when disconnected).
  - **Scale Connection Pill**: Shows real-time scale link status and battery percentage (`SCALE: 88%` / `SCALE: OFF`).
  - **Reset Chart Button**: Instantly clears extraction curves and resets the time axis. This is the *only* thing that clears the shot from the screen — a profile sync, a profile change and the post-shot tail all leave it alone.
  - **Settings Button**: Opens the on-device configuration modal.

- **Left Extraction Graph & In-Graph HUD**:
  - 160-point line chart displaying live **Pressure** (`0–12 bar`, Cyan `#00f0ff`), live **Flow** (`0–6 ml/s`, Amber `#f59e0b`), live **Weight** (Purple `#a855f7`), dotted **Target Profile** with full step-curve easing (`linear`, `ease_in_out`, `ease_out`, `ease_in`, `constant`, `instant`, Sky Blue `#0284c7`), and dotted **Target Weight** (Violet `#c084fc`), with dynamic mid-shot time-axis rescaling.
  - **Post-shot tail**: the curve keeps going after the pump stops, so the last of the coffee arriving in the cup is visible as the weight line climbing to its final value. The shot clock is frozen when the pump stops, so the tail is placed on the chart with the time that has passed since the stop — it continues exactly where the measured curve ended. Only the weight is plotted in that tail: the pressure and the flow curves stop at the pump stop, because past it the line is falling away and the flow reads zero, and neither of them says anything about the shot any more.
  - **In-Graph Floating HUD**: Large shot timer (`00:00.0`, held after brew completion until next shot or tare), live pressure (`9.0 bar`), and live flow rate (`2.2 ml/s`).
  - **Phase Chip / Subtitle**: Displays current brew phase name and target pressure.

- **Right Metrics & Controls**:
  - **Weight Container Card with Coffee Liquid Fill Effect**: The entire container card acts as an espresso cup. As coffee is extracted, a rich roasted coffee gradient (`#260e04` to `#78350f`) fills from bottom to top, topped with a golden crema head line (`#fde047` / `#f59e0b`). When target weight is reached, the card border illuminates with a golden amber glow.
  - Large Montserrat 36 weight digits (`36.0 g`), target weight (`Target: 36.0g`), and weight flow rate (`+2.2 g/s`).
  - **Tare Button**: Dispatches `{"cmd":"TARE"}` over WebSocket.
  - **Flush Button**: Starts grouphead rinse with an interactive countdown overlay.
  - **Start / Stop Brew Button**: Large prominent touch button (Emerald Green `START BREW` toggling to Rose Red `STOP BREW`).

---

## What the display keeps on screen

The display polls the controller for the profile list every few seconds, so a **profile sync is not
a reset**. The sync refreshes the dropdown and the target weight, and nothing else: the measured
curve, the yield in the cup, the shot timer and the coffee fill all stay exactly where they were.

Only two things clear the screen:

- the **RESET CHART** button, and
- the **start of a new brew** (the chart has to show the new shot).

Selecting a different profile from the dropdown moves the *planned* curve behind the measured one —
it does not throw away the shot that is on screen.

---

## The shot ledger

The ledger button opens the list of recorded shots. That list is **not** a built-in example: the
display asks the controller for its ledger (`GET /api/v1/history`) when the ledger is opened, and
again every 20 seconds, so what is on screen is what the controller has actually recorded.

Each entry is laid out like a web ledger row, minus the little curve:

```
┌──────────────────────────────────────────────────────┐
│ #3  Turbo Shot                            32.0 g     │
│ 22:33:12 · COMPLETED                  8.0 bar · 20.5 s │
└──────────────────────────────────────────────────────┘
```

The newest shot comes first and the numbers count up from the oldest, exactly like the web ledger.
The time is the one the controller stamped the shot with — the display has no clock of its own, so
a shot recorded before the controller kept one shows `--:--:--` rather than a made-up time.

The list scrolls — the controller keeps 15 shots, which is more than the 3.5" screen shows at once.

If the controller cannot be reached, the display says so rather than showing an empty or a stale
ledger. The fetch never runs while a shot is in progress, so it cannot delay the live telemetry
the chart is drawn from.

---

## The screen saver

After **10 minutes without a touch** the display switches to a screen saver: an espresso cup that
drifts slowly across the panel on a warm halo, with steam curling out of the cup. The motion is the
point — a static image would burn into the panel just as badly as the cockpit would.

**A single touch brings the cockpit back exactly as it was**, including a modal that was open. The
screen saver is a *second* LVGL screen that is loaded on top of the cockpit (`lv_scr_load`); the
cockpit is never torn down, so nothing about it can be lost.

The screen saver also steps aside while the machine is working. While the controller reports
`STARTING`, `BREWING`, `FLUSHING` or `STOPPING`, the cockpit stays on screen and the idle timer is
held back, so a shot is never hidden behind a floating cup.

### The art

All sprites are 8-bit **alpha masks** (`LV_IMG_CF_ALPHA_8BIT`), one per layer of the cup:

| Sprite | Size | Tint |
| --- | --- | --- |
| `saver_cup` | 96 × 96 | `#EDE0D2` ceramic |
| `saver_cup_shade` | 96 × 96 | `#966A46` ceramic shadow |
| `saver_liquid` | 96 × 96 | `#78350F` espresso |
| `saver_crema` | 96 × 96 | `#F59E0B` crema |
| `saver_crema_hi` | 96 × 96 | `#FDE047` crema highlight |
| `saver_steam` | 40 × 56 | `#EDE0D2` steam |
| `saver_glow` | 128 × 128 | `#F59E0B` halo |

LVGL draws an alpha-mask image as the `img_recolor` colour modulated by the mask, so the tint is
applied at runtime and the whole set costs one byte per pixel of flash and **no LVGL heap at all** —
which matters on a board whose `LV_MEM_SIZE` is only 64 kB.

The drawing is shared with the Android app: `tools/gen_screensaver_art.py` is the single source of
truth and emits both the LVGL masks (`src/screensaver_art.c`) and the Android PNGs. Re-run it after
changing the art:

```bash
python3 tools/gen_screensaver_art.py --preview
```

`--preview` also refreshes `tools/art/screensaver_preview.png`, a mock of both screens.

---

## Building and Flashing

### 1. Build Firmware
From the repository root:
```bash
pio run -d display-waveshare-3.5
```

### 2. Flash to Waveshare Board
Connect the board to your computer via USB-C, then run:
```bash
# Interactive selection menu (lists all detected ports)
./display-waveshare-3.5/flash.sh

# Or select by list number
./display-waveshare-3.5/flash.sh 2

# Or specify a direct device path
./display-waveshare-3.5/flash.sh /dev/cu.usbmodem832301

# List detected ports without flashing
./display-waveshare-3.5/flash.sh --list

# Flash and open the console on the same port (this is the default)
./display-waveshare-3.5/flash.sh 2

# Flash without opening the console
./display-waveshare-3.5/flash.sh 2 --no-monitor
```

After a successful flash the script **opens the console on the port it just wrote to**. The console
is the repository's own (`tools/brewmon.py`), the same one the controller and the simulator use: it
reconnects by itself when the board resets on native USB, which is exactly when `pio device monitor`
gives up and leaves you staring at a dead terminal.

### 3. Wi-Fi Configuration
By default, the display loads credentials from NVS (`sb_display` or `sbwifi`). You can update credentials or the target controller IP anytime by tapping the **Settings** gear icon on the top right of the screen.
