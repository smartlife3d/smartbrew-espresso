# Using the dashboard

The controller inside the machine runs every shot on its own. The screens only show what it is
doing and send it commands, so closing a browser or losing Wi-Fi in the middle of a shot does not
stop or change the shot. Three screens are available:

| Screen | What it is | Where it comes from |
|---|---|---|
| Web dashboard | A single page served by the controller, for any phone, tablet or computer browser | Built into the firmware |
| Android app | The same dashboard in a full-screen app, made for the Amazon Echo Show 5 | [`../android-app/README.md`](../android-app/README.md) |
| Waveshare display | A 3.5" touch screen with its own firmware and layout | [`../display-waveshare-3.5/README.md`](../display-waveshare-3.5/README.md) |

Building and flashing these is covered in [Operation.md](Operation.md). How the controller makes
its decisions is covered in [Architecture.md](Architecture.md).

## Opening the web dashboard

| Situation | Address |
|---|---|
| Controller on your Wi-Fi | `http://smartbrew.local` (or the IP shown in Settings → Hardware → Current Wi-Fi Link) |
| Controller in its setup hotspot | Join the open network `SmartBrew-XXXX`, then open `http://192.168.4.1` |

The `.local` name is set in **Settings → Hardware → Controller Hostname / mDNS** (default
`smartbrew`). First-time Wi-Fi setup is described in [Operation.md](Operation.md#first-boot-and-wi-fi).

**Install it as an app (PWA).** The page ships a web app manifest and the iOS home-screen tags:

- iPhone / iPad (Safari): Share → **Add to Home Screen**. It then opens full screen, with the
  layout kept clear of the notch and the home bar.
- Android (Chrome): menu → **Add to Home screen**. Because the controller serves plain `http`,
  Chrome may add a shortcut that opens in a browser tab rather than a full-screen app.

The page is never cached by the browser, so after a firmware update a reload always shows the new
dashboard. There is no offline mode: the dashboard needs the controller to be reachable.

## The main screen

On a wide screen the chart is on the left and the controls on the right. On a phone in portrait
the chart fills the top and the controls sit below it.

### Header

| Element | What it does |
|---|---|
| Profile pill (`☕ Classic 9-Bar ▼`) | Shows the active profile. Tap it to open the profile list. |
| State badge | `READY` when idle, the current phase name while brewing, `FLUSHING` during a rinse. |
| `↺ RESET` | Clears the chart and the shot timer. |

### The chart

| Line | Look | Axis | Meaning |
|---|---|---|---|
| Pressure | solid cyan | left, 0–12 bar | Pressure measured by the transducer |
| Flow | solid amber | right, 0–6 ml/s | Water flow estimated from the pump model (see [PumpFlow.md](PumpFlow.md)) |
| Cup Flow (scale) | dashed pale amber | right, 0–6 ml/s | Flow measured by the scale (g/s converted to ml/s). Drawn only while a scale reports; the gap to the amber line is the model's error |
| Weight | solid violet | own scale | Weight on the scale |
| Target Weight | dashed violet, labelled `Target: 36.0g` | own scale | The profile's target yield |
| Planned profile | thin dashed line, cyan (pressure phases) or amber (flow phases) | pressure or flow axis | What the profile asks for, phase by phase |

The weight lines have no printed axis. Their scale runs from 0 to 1.25 × the target weight, and
never less than 50 g, so the target line always sits at the same height relative to the top.

The time axis shows at least 40 s. It stretches to the profile's length plus 5 s, and keeps
growing if the shot runs longer.

Each phase is a lightly tinted column with its number and name at the top. A phase that ends on a
sensor condition shows a small badge, for example `⚖ 36g` (weight), `⚡ ≥1.5b` (pressure),
`💧 ≤2.0ml` (flow). The column of the running phase is highlighted.

**The floating readout** in the lower right of the chart shows the shot timer (`00:00.0`), the
live pressure and the live flow.

**After the pump stops** the weight line keeps climbing while the last coffee drips into the cup.
The pressure and flow lines end at the pump stop. The final yield is the highest weight seen in
that tail. [Architecture.md](Architecture.md) explains the rules.

**Inspecting a finished shot.** When no shot is running, drag a finger or move the mouse across
the chart. A vertical guide shows the values at that moment:
`t: 21.4s · P: 8.9b · F: 2.1 / cup 1.9ml/s · Yield: 31.2g`. Past the pump stop only the yield is
shown.

### Phase timeline

The strip under the chart lists the profile's phases with their target and duration, for example
`PRE-INFUSION 3.0b · 4s` or `RAMP TO 9 BAR 3→9b · 3s`. The running phase is highlighted. Tapping
the strip while idle opens the profile list.

### Toolbar

| Button | What it does |
|---|---|
| Portafilter pill | Green dot: connected to the controller. Red: disconnected, reconnecting. Tap to open Settings → Hardware. |
| `Scale` pill | Scale link state. Tap to open Settings → Tools, where scales are paired. |
| 📋 | Brew ledger (shot history) |
| ⚙️ | Settings |

### Weight card

The card shows the weight in large digits, the cup flow (`+2.1 g/s`) and the target (`36.0 g`).
The whole card fills with coffee colour as the weight approaches the target and glows when the
target is reached. **TARE** zeroes the scale.

### Brew and flush buttons

| Button label | Meaning |
|---|---|
| `START BREW` | Tap to start the active profile. |
| `STOP BREW` | A shot is starting or running. Tap to stop it. |
| `STOPPING…` | The controller is ending the shot. |
| `SHOT DONE` | The last shot has finished. Tap to brew again. |
| `FLUSHING…` | Brewing is disabled during a flush. |

After you tap **START BREW** the controller checks its hardware and tares the scale before the
pump starts. The shot begins about half a second after the checks pass. If a check fails, the shot
does not start and the reason appears in the log (Settings → Logs).

**GROUPHEAD FLUSH** runs the pump at a low fixed duty for the flush time (3, 5, 8 or 10 s, set in
Settings → Hardware; default 5 s). A dialog covers the chart with a countdown and live pressure and
flow. Tap **STOP FLUSH** in the dialog or on the button to end it early.

### What is locked while the machine works

| Control | Idle / shot done | Starting, brewing, stopping | Flushing |
|---|---|---|---|
| Brew button | Start | Stop | Disabled |
| Flush button | Start a flush | Disabled | Stops the flush |
| TARE | Works | Disabled | Disabled |
| Profile pill | Opens the list | Locked | Locked |
| RESET | Works | Disabled | Disabled |
| Settings, ledger | Open | Open | Open |

A refused action shows a short message such as `Cannot switch profile during extraction` or
`Cannot start brew while grouphead is flushing`.

## Profiles

The profile list (profile pill or the timeline) shows every profile stored on the controller.
Tapping a card makes it the active profile. The list also has **➕ New Profile**,
**📁 Upload JSON** (accepts native, interchange JSON and curve JSON files),
**⬇ Download Active** (interchange JSON) and **⬇ Download All** (ZIP).

**Editing profiles.** Choose **Edit** on a profile card, or **➕ New Profile**, to open the
profile designer. You drag points on the curve or use the phase cards, then press
**Save to Controller**. The designer, the phase modes and triggers, and the file formats are
described in [Profiles.md](Profiles.md).

## The brew ledger

The 📋 button opens the last 15 shots stored on the controller, newest first. Each row shows the
profile, the time, the yield in grams, the peak pressure and the duration, with a small preview of
the profile's planned curve. The yield is the settled weight in the cup, not the reading that
triggered the cutoff.

Tapping a row marked `CURVE` opens that shot in its own window: measured pressure, flow, cup flow
(when a scale was used) and weight, drawn over the profile the shot was pulled with. A tinted area
marks the time after the pump stopped. The live chart on the main screen is not touched.

The ledger reloads when you open it and whenever the browser tab comes back into view, so shots
pulled from another screen appear too. **Clear History** deletes every stored shot on the
controller after a confirmation.

## Settings

| Tab | Contents |
|---|---|
| 🔧 Hardware | Controller hostname, pump model, transducer range (1.2 or 1.6 MPa), mains frequency (auto, 50 Hz, 60 Hz), ADC source (auto, ADS1115, internal), flush duration, SIM mode. Current Wi-Fi link, **Save & Connect** for a new network, **Forget & Enter Hotspot**. |
| ⚙️ Advanced | PID gains, safety and pump-flow settings (below), and a live system table read from the controller. |
| 📜 Logs | The controller's recent log lines, with Refresh, Copy and Clear. |
| 🛠️ Tools | Zero-pressure calibration, BLE scale scan, pair and unpair, controller reboot, firmware update (OTA). |

**SIM mode** (Hardware tab) makes the controller run a simulated shot: the pump is never driven,
and pressure, flow and weight come from a built-in model. Use it to try profiles without water.

### Advanced tab

| Setting | Default | What it changes |
|---|---|---|
| Proportional / Integral / Derivative Gain | 0 = use the pump preset | Pressure loop tuning |
| Overpressure Safety Cut-Off (bar) | 12.5 (1.2 MPa sensor), 15 (1.6 MPa) | Pressure above this for 150 ms stops the pump |
| Headspace Anti-Windup Threshold (bar) | 1.5 | The integral term is held until pressure reaches this |
| EMA Filter Alpha | 0.25 | Pressure smoothing, 0.01 (heavy) to 1.0 (raw) |
| Virtual Button Pulse Width (ms) | 180 | How long the machine's brew button is "pressed" |
| Scale Drop Cutoff Lag (s) | 0.6 | How early the weight cutoff fires to allow for the drip after the pump stops |
| Learn Cutoff Lag From Each Shot | On | After each weight-cutoff shot, adjusts the lag from the measured drip |
| Weight Flow Window (ms) | 1500 | Time span used to compute cup flow from the scale, 500–4000 |
| Hydraulic Compliance (ml/bar) | 1.5 | Water stored per bar of pressure rise, used to correct the flow estimate; 0 = off |
| Pump P-Q Exponent | 0 = use the pump preset | Shape of the pump's pressure-flow curve, 0.5–4 |
| Calibrate Pump Flow From Scale | On | After a steady shot with a scale, matches the flow estimate to the cup |
| Pump Flow Calibration (×) | 1.00 | Multiplier on the pump model's flow, 0.5–1.5; updated by the calibration above |
| Machine Topology Timeout (s) | 120 | Continuous mains on the pump in idle for longer than this is treated as a wiring fault |

The pump-flow settings are explained in [PumpFlow.md](PumpFlow.md). Press **Save Advanced
Settings** to store them on the controller.

### Firmware update

Settings → Tools → **Firmware Update (OTA)**: choose a `firmware.bin` and press **Flash Firmware**.
The controller disables the pump driver for the duration of the update and restarts when done.
Details are in [Operation.md](Operation.md#updating-over-wi-fi-ota).

## The Android app

The app shows the same dashboard as the browser, full screen, and can act as the device's home
screen. It either loads its bundled copy of the dashboard or the controller's own page. If the
controller does not answer within 7 s it falls back to the bundled copy.

The app adds a **SmartBrew Controller Link** section to Settings → Hardware: the controller it is
talking to, the link status, a list of controllers found on the network, a field for a manual IP
or `.local` name, and **↻ Scan for Controllers**. The default target is `smartbrew.local`.

Installation on an Echo Show 5 is described in [`../android-app/README.md`](../android-app/README.md).

## The Waveshare display

The 3.5" display has the same layout, drawn for a 480 × 320 screen:

- **Header:** profile dropdown, state pill, controller and scale pills, reset, ledger and settings.
- **Chart:** pressure (cyan, 0–12 bar), flow (amber, 0–6 ml/s), `Cup` flow from the scale
  (dashed pale amber, same axis), weight (purple) and target weight (dashed violet), using the same
  weight scale as the web dashboard. The planned profile is a dashed sky-blue line. The legend
  reads `P(bar)`, `F(ml/s)`, `Cup`, `W(g)`, `Target`.
- **Right side:** weight card with coffee fill, flow and target, TARE, FLUSH and START/STOP BREW.

TARE, FLUSH and RESET do nothing while the display shows a running shot. The chart is cleared only
by **RESET** or by the start of a new shot. Choosing another profile moves the planned curve but
keeps the measured shot on screen.

The display's **Settings** show its Wi-Fi state, the controller host, flush duration and a SIM
toggle. **Reset Wifi** clears the display's stored network and restarts it into its own setup
hotspot `SmartBrew-Display-XXXX`; join it and open `http://192.168.4.1/wifi`.

The ledger button lists the controller's stored shots (no curves). See
[`../display-waveshare-3.5/README.md`](../display-waveshare-3.5/README.md) for details.

## Screen saver

The Android app and the Waveshare display switch to a screen saver after **10 minutes without a
touch**: an espresso cup drifting slowly across the screen. One touch brings back the previous
screen exactly as it was, including an open dialog. The touch that wakes the screen is not passed
on to the button underneath.

The screen saver stays away while the controller reports `STARTING`, `BREWING`, `FLUSHING` or
`STOPPING`. The web dashboard in a browser has no screen saver.
