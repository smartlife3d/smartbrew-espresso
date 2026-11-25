# Smart Brew Cockpit — Amazon Echo Show 5 Companion App (Android)

> Part of the Smart Brew Controller - see the [main README](../README.md). Using the app: [`docs/Dashboard.md`](../docs/Dashboard.md). Building it: [`docs/Operation.md`](../docs/Operation.md).

Sub-project Android application specifically designed for the **Amazon Echo Show 5** running **LineageOS**. It transforms the device into a dedicated, countertop espresso telemetry cockpit and profiling terminal with direct **Bluetooth Low Energy (BLE) scale support** and **Wi-Fi connection to the Smart Brew Controller (ESP32-S3)**.

---

## 1. Device Context: Amazon Echo Show 5 & LineageOS

The Amazon Echo Show 5 makes a countertop espresso console:
- **Display**: 5.5-inch 960 &times; 480 capacitive touch screen (2:1 widescreen aspect ratio, ~195 dpi).
- **Form Factor**: Fixed ~30&deg; tilted desktop wedge — ideal barista viewing angle next to the E61 / dual-boiler machine.
- **Hardware**: MediaTek MT8163 / MT8516 SoC, 1 GB RAM, Dual-band Wi-Fi (802.11a/b/g/n/ac), Bluetooth 4.2/5.0 LE.
- **Operating System**: LineageOS (Android 9 Pie / Android 10 / 11 builds available on XDA for `checkov` / `sheldon`).

---

## 2. Key Capabilities

### 2.1 Pure Fullscreen Kiosk Mode
- **Immersive Sticky**: Hides status bars, clock, and software navigation bars completely (`BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE`).
- **Wake Lock (`FLAG_KEEP_SCREEN_ON`)**: Prevents the screen from sleeping or dimming during barista operation and extractions.
- **Launcher Integration**: Can be selected as the default Android Home launcher (`CATEGORY_HOME` intent filter), booting directly into the Smart Brew Cockpit when powered on.

### 2.2 Re-uses Optimized Web UI
- **Zero Latency Boot**: Pre-packages the optimized, aerospace-grade cockpit UI (`data/index.html`) directly into the app (`assets/web/`).
- **60 FPS Hardware Acceleration**: Hardware-accelerated WebView renders HTML5 canvas pressure curves, live HUD gauges, and demitasse droplet animations with zero ESP32 CPU overhead.
- **Automatic Transparent Routing**: JavaScript calls to `/api/v1/...` and `/ws` are transparently routed to the ESP32's Wi-Fi IP address without modifying core dashboard logic.
- **Dual-Mode Serving**: Switch between the bundled local assets or directly load from the Controller's HTTP server (`http://<mcu-ip>/`).

### 2.3 Direct Native BLE Scale Support
Solves the common RF problem where the ESP32 is enclosed inside a metal espresso machine chassis (Faraday cage), attenuating Bluetooth signals from the drip tray scale:
- Connects directly to BLE scales placed on the drip tray with unobstructed line-of-sight.
- Supports all four major scale driver protocols:
  1. **Bookoo Themis Ultra / Mini & ESP32-C3 Companion** (0x0FFE / 0xFF11).
  2. **Timemore Black Mirror** (Dot, Nano, Basic 0x181D / 0x2A9D).
  3. **Acaia Lunar / Pearl / Pyxis** (0x1820 / 0x2A80 with 2.5s heartbeat).
  4. **Generic Decent / Felicita Arc / Cocinare** (0xFFF0 / 0xFFE0 with XOR-validated checksums).
- Sub-5 ms instant tare execution and continuous flow rate filtering (EMA).
- Relays weight data to the ESP32 via Wi-Fi (`/api/v1/bench/sim/weight` and WebSocket).

### 2.4 Controller Wi-Fi Connection
- Auto-discovers the ESP32 on the local Wi-Fi subnet via **mDNS** (`_http._tcp.` / `smartbrew.local`).
- In-app connection configuration modal to test ping (`/api/v1/status`) and set custom IP addresses (e.g. `192.168.4.1` SoftAP mode or router-assigned static IP).
- Auto-reconnect with exponential backoff if Wi-Fi drops.

### 2.5 Screen Saver
- **Burn-in protection**: after **10 minutes without interaction** the cockpit is covered by a screen saver showing an espresso cup that drifts slowly across the panel on a warm halo, with steam curling out of the cup.
- **Touch to resume**: a single touch brings the cockpit back exactly as it was, including an open modal. The touch is swallowed by the app, so it can never reach the web UI underneath.
- **Native overlay, not a web page**: the screen saver is a `ScreensaverView` on top of the WebView, so it works identically whether the WebView shows the bundled local assets or the Controller's remote page — and the Controller's own web UI is left untouched.
- **Never hides a shot**: while the controller reports `STARTING`, `BREWING`, `FLUSHING` or `STOPPING`, the screen saver is switched off and the idle timer is held back.

---

## 3. Project Architecture

```
android-app/
├── app/
│   ├── build.gradle                 # Android SDK 34, minSdk 26 (Android 8.0+)
│   ├── src/main/
│   │   ├── AndroidManifest.xml      # Fullscreen theme, BLE & Wi-Fi permissions, Home launcher
│   │   ├── assets/web/
│   │   │   ├── index.html           # Re-used Smart Brew cockpit UI
│   │   │   ├── echo5_style.css      # 960x480 landscape layout optimization & neon modals
│   │   │   └── echo5_bridge.js      # JS <-> Android BLE scale & Wi-Fi proxy bridge
│   │   ├── java/com/smartbrew/echo5/
│   │   │   ├── MainActivity.kt      # Immersive kiosk lifecycle, permissions, WebView host
│   │   │   ├── WebAppInterface.kt   # @JavascriptInterface methods exposed to Web UI
│   │   │   ├── screensaver/
│   │   │   │   ├── ScreensaverView.kt       # Floating espresso cup drawn over the WebView
│   │   │   │   └── ScreenSaverController.kt # Idle timer, touch-to-resume, brew override
│   │   │   ├── scale/
│   │   │   │   ├── BleScaleManager.kt  # Central BLE scanner, GATT lifecycle, EMA flow
│   │   │   │   ├── ScaleReading.kt     # Weight, flow, battery, stable payload
│   │   │   │   ├── ScaleDriver.kt      # Abstract driver interface
│   │   │   │   ├── BookooDriver.kt     # Bookoo / ESP32-C3 scale driver
│   │   │   │   ├── TimemoreDriver.kt   # Timemore Black Mirror driver
│   │   │   │   ├── AcaiaDriver.kt      # Acaia Lunar / Pearl driver
│   │   │   │   └── GenericDriver.kt    # Decent / Felicita / Cocinare driver
│   │   │   ├── network/
│   │   │   │   └── McuBridge.kt        # mDNS auto-discovery, HTTP health check & weight relay
│   │   │   └── utils/
│   │   │       └── AppPreferences.kt   # SharedPreferences persistence (Controller IP, paired scale)
│   │   └── res/                     # Dark OLED colors, fullscreen styles, strings
│   │       └── drawable-nodpi/      # Screen-saver art (espresso_cup.png, espresso_steam.png)
│   └── proguard-rules.pro           # Preserves JavaScript interfaces
├── sync_web_assets.sh               # One-click script to copy updated root data/index.html
├── build.gradle                     # Project-level Gradle build file
└── settings.gradle
```

### The screen saver in detail

The screen saver is a native Android view, not a page of the web UI. That is deliberate: the app
serves either the bundled local assets *or* the Controller's remote page, and an overlay behaves
identically in both cases. It also means the Controller's own web UI (`data/index.html`) stays
exactly as it is.

| Piece | Role |
| --- | --- |
| `ScreensaverView` | Draws the background, the warm halo, the floating cup and the steam curls. |
| `ScreenSaverController` | Owns the idle timer, the touch-to-resume rule and the "machine is busy" override. |
| `MainActivity.dispatchTouchEvent` | Counts every touch as activity, and swallows the touch that wakes the display up so it never reaches the cockpit underneath. |

The cup drifts on two sine paths of different length, so it traces a slow Lissajous figure rather
than a straight line, and the steam curls rise and fade on their own cycle.

The art is shared with the Waveshare display: `tools/gen_screensaver_art.py` is the single source of
truth and emits both the Android PNGs (`res/drawable-nodpi/`) and the LVGL alpha masks. Re-run it
after changing the drawing:

```bash
python3 tools/gen_screensaver_art.py --preview
```

The screen saver cannot be dismissed on hardware without a touchscreen — the hint then reads
`SMARTBREW` instead of `TOUCH TO RESUME`, and the saver steps aside on its own as soon as the
controller reports a running shot. The LineageOS Echo Show 5 exposes a Goodix (`goodix-ts`)
touch digitizer, so tap-to-resume works there.

---

## 4. Build Instructions

### Prerequisites
- JDK 17 or JDK 21 (e.g., `/opt/homebrew/opt/openjdk@21`).
- Android SDK (installed at `/opt/homebrew/share/android-commandlinetools` or via Android Studio).

### Building the APK
From the `android-app` directory:

```bash
# Debug APK
./gradlew assembleDebug

# Output location:
# app/build/outputs/apk/debug/app-debug.apk
```

```bash
# Release APK
./gradlew assembleRelease

# Output location:
# app/build/outputs/apk/release/app-release-unsigned.apk
```

---

## 5. Amazon Echo Show 5 Installation & Deployment

### Step 1: Enable ADB on Echo Show 5 (LineageOS)
On the Echo Show 5 running LineageOS:
1. Open **Settings** &rarr; **About Tablet**.
2. Tap **Build Number** 7 times to enable Developer Options.
3. Open **Settings** &rarr; **System** &rarr; **Developer Options**.
4. Enable **USB Debugging** and optionally **Network Debugging (ADB over Wi-Fi)**.

### Step 2: One-Step Build & Sideload via Script
Connect via micro-USB or over Wi-Fi, then run the upload script:

```bash
# Connect over Wi-Fi (if network debugging is enabled on port 5555):
adb connect <ECHO5_IP_ADDRESS>:5555

# Run the automated build & upload script (from workspace root):
./upload_echo5.sh

# Or from inside the android-app/ directory:
./upload.sh
```

The script automatically:
1. Syncs the latest web assets (`data/index.html` -> Android `assets/web/`).
2. Compiles the Android APK via Gradle.
3. Detects your connected Echo Show 5 device.
4. Installs the APK and restarts the app.


### Step 3: Grant Permissions Automatically (Optional)
To skip runtime prompt dialogs on the countertop display, grant BLE and Location permissions via ADB:

```bash
adb shell pm grant com.smartbrew.echo5 android.permission.ACCESS_FINE_LOCATION
adb shell pm grant com.smartbrew.echo5 android.permission.BLUETOOTH_CONNECT
adb shell pm grant com.smartbrew.echo5 android.permission.BLUETOOTH_SCAN
```

### Step 4: Make Smart Brew the Default Kiosk Launcher (Optional)
To make the Echo Show 5 automatically launch Smart Brew on power-up:

```bash
adb shell cmd package set-home-activity com.smartbrew.echo5/.MainActivity
```

---

## 6. Pairing & Configuration

1. **Controller Wi-Fi Link**:
   - Tap the **📡 Controller** button in the dashboard toolbar (or use the main settings menu).
   - Tap **DISCOVER (mDNS)** or enter `smartbrew.local` / `192.168.4.1` / `<static-ip>`.
   - Tap **TEST PING** to confirm connectivity.
   - Tap **CONNECT**.

2. **BLE Scale Pairing**:
   - Turn on your BLE scale (Bookoo, Timemore, Acaia, Decent, or Felicita).
   - Tap the **⚖️ Scale** button in the toolbar.
   - Tap **RE-SCAN**; the device will appear with its signal strength (RSSI).
   - Tap **CONNECT**; the app remembers this scale and will automatically reconnect on startup.
   - Live weight and flow rate will begin updating immediately on the weight card, HUD, and animated demitasse cup!

3. **Re-syncing Web UI Updates & Deploying**:
   Whenever you make updates to the root Controller dashboard (`../data/index.html`), simply run:
   ```bash
   ./upload_echo5.sh
   ```

