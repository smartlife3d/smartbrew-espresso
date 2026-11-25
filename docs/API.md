# Controller API

This page describes every interface a client or integrator can use to talk to the controller:
the HTTP REST API, the WebSocket telemetry stream and command channel, and the UART1 link for an
external display. The BLE link to the scale is covered in [Hardware.md](Hardware.md). Profile
documents are described in [Profiles.md](Profiles.md); the system design behind the telemetry is
in [Architecture.md](Architecture.md).

## Transports

| Transport | Address | Carries |
|---|---|---|
| HTTP | `http://smartbrew.local/` (port 80) | Cockpit web app, REST API under `/api/v1/` (JSON) |
| WebSocket | `ws://smartbrew.local/ws` (same port 80) | Binary telemetry frames out, JSON commands in, JSON replies and events out |
| UART1 | GPIO17 TX, GPIO18 RX, 115200 8N1 | NDJSON telemetry out, the same JSON commands in |
| BLE | – | Scale link only, see [Hardware.md](Hardware.md) |

The hostname is the `mcuHostname` config key (default `smartbrew`). In station mode the
controller announces `_http._tcp` and `_ws._tcp` on port 80 over mDNS. Without stored Wi-Fi
credentials, or when it cannot join within 3 minutes, it opens an open access point named `SmartBrew-XXXX` (the
last two MAC bytes in hex) at `192.168.4.1` with a catch-all DNS server; the setup page is
`http://192.168.4.1/wifi`.

## Security

The controller has **no authentication**. Anyone who can reach it on the network can start the
pump, change the configuration and flash firmware. Run it on a trusted LAN only and never expose
it to the internet (no port forwarding, no public reverse proxy).

The one protection is a cross-origin guard in `src/network/web_api.cpp`. It stops a web page from
another site, open in a browser on the same LAN, from driving the machine:

- Every state-changing POST and DELETE route, and the WebSocket handshake, is refused (HTTP 403,
  `{"ok":false,"err":"cross-origin request rejected"}`) when the request has an `Origin` header
  whose host differs from the `Host` header.
- Allowed: no `Origin` (curl, native apps), `Origin: null`, `file://`, and the Android app origin
  `appassets.androidplatform.net`.
- GET routes are not guarded. None of them changes settings or drives the machine; the only write
  is that fetching a deleted built-in profile restores its default file. Every route that changes state is a
  POST, PUT or DELETE (a browser always sends an `Origin` header with a cross-origin request of
  that kind, so the guard can refuse it). JSON and text responses carry an
  `Access-Control-Allow-Origin` header only for this device itself and the Android app's local
  origin, never `*`.

## REST API

Responses are JSON with `Cache-Control: no-cache` unless noted. Parameters are query-string or
form fields. Command routes answer HTTP 200 with `{"ok":false,"err":...}` when the controller
refuses, so check `ok`.

### Status and system

| Method | Path | Purpose and parameters |
|---|---|---|
| GET | `/api/v1/status` | Health and live state: `fw`, `hostname`, `chip`, heap and PSRAM, `state`, `mode`, `pump_owner`, `faults`, control-loop timing, Wi-Fi power save, `ws_clients`, `reset_reason_str`, `adc_source`, `p_bar`, `p_target`, `ac_hz`, zero-cross counters, `duty_pct`, PID gains, scale state, `profile`, `profile_id`, `phase`, `shot_count`, `pump`, `sensor_mpa`, `sim`, `override`, `manual`, `bench`, `flush_rem_ms`, `sensor_open_warn`. |
| GET | `/api/v1/logs` | Recent log lines as `text/plain`. With `?json` or `Accept: application/json`: `{"ok":true,"logs":"...","drops":n}`. |
| DELETE | `/api/v1/logs` | Clear the log buffer. |
| POST | `/api/v1/time` | `epoch` = Unix time in ms (must be > 10¹²). Sets the wall clock used for shot timestamps. |
| POST | `/api/v1/reboot` | Restart after 400 ms. |
| GET | `/`, `/index.html`, `/manifest.json` | Cockpit web app (gzipped) and its PWA manifest. See [Dashboard.md](Dashboard.md). |

### Configuration and calibration

| Method | Path | Purpose and parameters |
|---|---|---|
| GET | `/api/v1/config` | All config keys (table below). |
| POST | `/api/v1/config` | Form field `json` holding a JSON object with any subset of the writable keys. Refused with 409 unless the machine is IDLE. Reply `{"ok":true\|false}` (false = flash write failed). |
| POST | `/api/v1/calibrate/zero` | Start atmospheric zero calibration; `n` = samples, 8–200, default 60. 409 if not IDLE or already running. |
| GET | `/api/v1/calibrate/zero` | `{"ok":true,"state":"idle\|running\|ok\|error","msg":...,"sensorZeroMv":...}` |

### Profiles

| Method | Path | Purpose and parameters |
|---|---|---|
| GET | `/api/v1/profiles` | `{"active":"<id>","items":[...]}`. Each item is the full stored document plus `id` and `active`. Built-ins come first, then user profiles. |
| GET | `/api/v1/profiles/detail` | `id`. One profile document; 404 if unknown. A missing built-in is re-created and returned. |
| POST | `/api/v1/profiles` | Save a profile. Body: raw JSON, or form field `json`. Max 16 KB (413 above). Strips `seed_version`, sanitises the id, makes the profile active and queues it for loading. Reply `{"ok":true,"id":"<id>"}`; 400 on parse errors. |
| POST | `/api/v1/profiles/active` | `id`. Make a profile active. Reply `{"ok":true,"id":...,"deferred":bool}`; `deferred` is true when the machine is busy and the profile will load on return to IDLE. 503 if the command queue is full. |
| DELETE | `/api/v1/profiles` | `id`. Delete `/profiles/<id>.json`. Built-ins are re-seeded at the next boot. |

Id sanitising, storage and the seeding rules are in [Profiles.md](Profiles.md#storage).

### Shot history

| Method | Path | Purpose and parameters |
|---|---|---|
| GET | `/api/v1/history` | Summary of stored shots (format below). With `slot` = 0–14: the full shot, 404 if empty. |
| POST | `/api/v1/history` | Store a shot recorded by a client. JSON body (max 32 KB): `profileId`, `profileName`, `peakBar`, `doseG` (or `dose_g`), `durationMs` (or `duration_s`), `epochMs`, `s` (sample array). Reply `{"ok":true,"slot":n}`; also broadcasts a `shot_end` event. |
| DELETE | `/api/v1/history` | `slot` deletes one shot; without `slot` all 15 are deleted. |

### Brew control

| Method | Path | Purpose and parameters |
|---|---|---|
| POST | `/api/v1/cmd/START` | Start a shot with the active profile. Refused when not idle, when the machine's own pump is running, or with no profile loaded. |
| POST | `/api/v1/cmd/STOP` | Stop the running shot or flush. |
| POST | `/api/v1/cmd/FLUSH` | `sec` = flush length, clamped to 1–60, default 5. |
| POST | `/api/v1/cmd/TARE` | Tare the scale. 409 when busy. |
| POST | `/api/v1/cmd/NEXT_PHASE` | Skip to the next profile phase. |
| POST | `/api/v1/bench/sim` | `on` = `1`, `true` or `on` enables the software simulator; anything else disables it. Available in all builds. Reply `{"ok":true,"simulator":bool}`. |

What these commands do on the machine is described in [Operation.md](Operation.md).

### Scale

| Method | Path | Purpose and parameters |
|---|---|---|
| POST | `/api/v1/scale/scan` | `mac` = `AA:BB:CC:DD:EE:FF` or `AUTO` (default): scan and pair. `discover=1`: 6 s discovery scan without pairing. |
| GET | `/api/v1/scale/discovered` | `scanning`, `discovering`, `connected`, `device_name`, `device_mac`, `target_mac`, `battery`, `scales:[{mac,name,link,rssi}]`. |
| POST | `/api/v1/scale/disconnect` | Drop the scale connection. |
| POST | `/api/v1/scale/tare` | Same as `/api/v1/cmd/TARE`. |

### Wi-Fi

| Method | Path | Purpose and parameters |
|---|---|---|
| GET | `/api/v1/wifi` | `{"ok":true,"mode":"AP\|STA","ssid":...,"ip":...,"rssi":n,"portal":bool}` |
| POST | `/api/v1/wifi` | `ssid`, `pass` (or form field `json` with both). Stores the credentials and restarts after 1.2 s. |
| GET | `/wifi` | HTML setup page. |
| POST | `/save` | HTML form target: `ssid`, `pass`. Stores the credentials and restarts after 1.2 s. |
| POST | `/forget` | Clear the credentials and enter the setup portal. JSON reply with `Accept: application/json`, otherwise a redirect to `/wifi`. |
| GET | `/generate_204`, `/gen_204`, `/ncsi.txt`, `/hotspot-detect.html`, `/library/test/success.html`, `/canonical.html`, `/connecttest.txt` | Captive-portal probes; redirect to `http://192.168.4.1/wifi`. Any unknown path also redirects there while the portal is active. |

### Firmware update

| Method | Path | Purpose and parameters |
|---|---|---|
| POST | `/api/v1/ota/upload` | Multipart upload of a firmware image. Plain-text reply `OK` (then reboot after 500 ms) or HTTP 500 with the error. |
| POST | `/api/v1/ota/github` | `url` (default: the `otaGitHubUrl` key). Starts an HTTPS pull in the background. 409 if an update is running. |
| POST | `/api/v1/ota/abort` | Abort a running update. |

### Bench-only routes (`SB_BENCH` builds only)

| Method | Path | Purpose and parameters |
|---|---|---|
| POST | `/api/v1/cmd/MANUAL_POWER` | Form field `pct`: fixed pump duty; a negative value returns control to the profile. |
| POST | `/api/v1/cmd/OVERRIDE_PRESSURE` | `bar`: override the current phase's pressure target; without `bar` the override is cleared. |
| POST | `/api/v1/bench/sim/bar` | `bar`: inject a simulated pressure reading. |
| POST | `/api/v1/bench/sim/weight` | `g`: inject a simulated weight. |
| POST | `/api/v1/bench/duty` | `pct`: set the pump duty directly. |

In normal builds `MANUAL_POWER` and `OVERRIDE_PRESSURE` answer 403 `bench feature disabled`, and
the three `/bench/sim/*` and `/bench/duty` routes do not exist. See [Testing.md](Testing.md).

## Configuration keys

The configuration is stored twice: as a binary blob in NVS and as `/config.json` on LittleFS
(config version 7). At boot the NVS copy is loaded and `/config.json` is laid over it, so a
hand-edited file wins. `POST /api/v1/config` writes both.

| Key | Default | Range / values | Notes |
|---|---|---|---|
| `pump` | `ULKA_EX5` | `ULKA_EX5`, `ULKA_EX4`, `OLAB_SILENT`, `CUSTOM` | Pump preset. Unknown keys leave the setting unchanged. |
| `pumpModel` | 0 | 0–3 | Read-only index of `pump`. |
| `sensor_mpa` | `"1.2"` | `"1.2"`, `"1.6"` | Transducer range (12 / 16 bar full scale). |
| `mains_mode` | 0 | 0 auto, 1 = 50 Hz, 2 = 60 Hz | |
| `mains_hz` | – | – | Read-only, measured. |
| `force_ads` | 0 | 0 auto, 1 ADS1115, 2 internal ADC | ADC source. |
| `sensorZeroMv` | 333.33 | – | ADC-node mV at 0 bar. Set by zero calibration. |
| `sensorFsMv` | 3000.00 | – | ADC-node mV at full scale. |
| `emaAlpha` | 0.25 | 0.01–1.0 | Pressure smoothing. |
| `maxSafetyPressureBar` | 12.5 | 0 = derive from range; otherwise 1–20 | POST accepts 0–25; the next boot clamps to 1–20. Derived values: 12.5 bar (1.2 MPa), 15.0 bar (1.6 MPa). |
| `safetyBar`, `pressureFsBar` | – | – | Read-only: effective safety cut-off and sensor full scale. |
| `minSensorMv` | 133.33 | 50–1000 | Below this the transducer counts as open. |
| `kp`, `ki`, `kd` | 0 | 0–50 | 0 = use the pump preset's gain. |
| `loopHz` | 100 | – | Read-only control rate. |
| `headspaceBar` | 1.5 | 0–9.9 | PID integral is frozen below this pressure. |
| `buttonPulseMs` | 180 | 20–2000 | Soft brew-button pulse. |
| `buttonAckTimeoutMs` | 600 | 200–3000 | Read-only over the API; set in `/config.json`. |
| `buttonMinRetriggerMs` | 250 | 100–2000 | Read-only over the API; set in `/config.json`. |
| `scaleLagSec` | 0.6 | 0–3 | Predictive cutoff lag. |
| `scaleMac` | `""` | MAC or `AUTO` | Paired scale. |
| `scaleLockoutSec` | 3 | 0–30 | |
| `preInfusionProtectSec` | 3 | 0–30 | |
| `otaGitHubUrl` | `""` | ≤ 127 chars | Default URL for `/api/v1/ota/github`. |
| `mcuHostname` | `smartbrew` | ≤ 31 chars of `a-z`, `0-9`, `-` | POST lower-cases, filters and re-announces mDNS. A stored `brew-mcu` is migrated to `smartbrew`. |
| `verboseLogging` | 1 | 0 quiet, 1 info, 2 debug | Read-only over the API; set in `/config.json`. |
| `ledBrightness` | 10 | 0–100 | `/config.json` only; not in GET. |
| `topologyTimeoutSec` | 120 | 10–3600 | |
| `complianceMlPerBar` | 1.5 | 0–10 | See [PumpFlow.md](PumpFlow.md). |
| `pumpPqExponent` | 0 | 0 = preset, or 0.5–4.0 | See [PumpFlow.md](PumpFlow.md). |
| `pumpPqExponentActive` | – | – | Read-only: the exponent in use. |
| `scaleSlopeWindowMs` | 1500 | 500–4000 | See [PumpFlow.md](PumpFlow.md). |
| `scaleLagAutoLearn` | 1 | 0 / 1 (POST: boolean) | See [PumpFlow.md](PumpFlow.md). |
| `pumpFlowScale` | 1.000 | 0.5–1.5 | See [PumpFlow.md](PumpFlow.md). |
| `pumpFlowAutoCal` | 1 | 0 / 1 (POST: boolean) | See [PumpFlow.md](PumpFlow.md). |
| `pressureCtrlMode` | 1 | 0 / 1 | Pressure loop. 1 = adaptive: shaped setpoint (1 Hz, max 9 bar/s), integral only near the target, feed-forward cut above the target. 0 = classic feed-forward + PID. See `src/control/pid_controller.h`. |

POST clamps out-of-range values to the range shown.

## WebSocket

Connect to `ws://<host>/ws`. The server keeps at most 4 clients, sends the current frame on
connect, and disables Nagle on the socket. Binary messages are telemetry; text messages are JSON.

Telemetry rate: 20 Hz while a shot or flush is in progress, 10 Hz while the post-shot weight is
settling or a scale scan is running, 2 Hz idle. A client with more than two frames queued is
skipped until it catches up; gaps in `seq` show the skipped frames.

### Telemetry frame (version 2)

Defined in `include/telemetry_wire.h`. All multi-byte fields are little-endian. Frame length is
40 to 126 bytes.

| Offset | Size | Field | Type | Meaning |
|---|---|---|---|---|
| 0 | 1 | `magic` | u8 | `0x53` (`'S'`) |
| 1 | 1 | `version` | u8 | `2` |
| 2 | 2 | `seq` | u16 | Frame counter, wraps at 65536 |
| 4 | 4 | `t_ms` | u32 | Shot clock in ms (see below) |
| 8 | 1 | `state` | u8 | State code |
| 9 | 1 | `mode` | u8 | 0 passive, 1 profile |
| 10 | 1 | `pump_owner` | u8 | Owner code |
| 11 | 1 | `flags` | u8 | Flag bits |
| 12 | 1 | `phase_idx` | u8 | Running phase index |
| 13 | 1 | `phase_pct` | u8 | Progress through the phase, 0–100 |
| 14 | 1 | `psm_pct` | u8 | Pump duty % |
| 15 | 1 | `scale_bat` | i8 | Scale battery %, −1 unknown |
| 16 | 2 | `ac_hz` | u16 | Mains frequency |
| 18 | 4 | `faults` | u32 | Fault bitmask |
| 22 | 2 | `p_bar` | u16 | Pressure, 0.01 bar |
| 24 | 2 | `p_target` | u16 | Current phase target × 100, in the phase's unit (bar, ml/s or %) |
| 26 | 2 | `flow_virt` | u16 | Pump-model flow, 0.01 ml/s |
| 28 | 2 | `flow_weight` | i16 | Cup flow from the scale, 0.01 g/s |
| 30 | 2 | `weight_g` | i16 | Weight, 0.1 g, signed (range -3276.7 to 3276.7 g) |
| 32 | 2 | `duty_ff` | i16 | Feed-forward duty for `p_target`, 0.1 % |
| 34 | 2 | `flow_slope` | i16 | Scale weight slope, 0.01 g/s |
| 36 | 1+n | `phase_name` | string | ≤ 27 chars; `-` when none |
| … | 1+n | `scale_name` | string | ≤ 27 chars; may be empty |
| … | 1+n | `profile_id` | string | ≤ 32 chars; `default` when none |
| last | 1 | `crc8` | u8 | CRC-8/ATM over all preceding bytes |

Strings are a length byte followed by that many characters, with no terminator.

| Code | `state` | `pump_owner` |
|---|---|---|
| 0 | IDLE | PASSIVE |
| 1 | STARTING (also sent for the pre-brew gate) | PROFILE |
| 2 | BREWING | STOPPING |
| 3 | FLUSHING | FAULT |
| 4 | STOPPING | – |
| 5 | FAULT | – |

| Flag | Bit | Meaning |
|---|---|---|
| `SB_FLAG_SCALE_CONNECTED` | `0x01` | A scale is connected. |
| `SB_FLAG_SENSOR_OPEN` | `0x02` | Pressure transducer reads as open. |
| `SB_FLAG_SIMULATION` | `0x04` | Simulator active. |
| `SB_FLAG_WEIGHT_SETTLING` | `0x08` | Post-shot drip still arriving. Keep showing the live (highest) weight until it clears instead of latching the cutoff value. |

Fault bits (`FaultBits` in `include/config_schema.h`): `0x001` sensor open, `0x002` over-pressure,
`0x004` over-temperature, `0x008` no scale, `0x010` profile missing, `0x020` button not
acknowledged, `0x040` zero-cross lost, `0x080` scale stale, `0x100` sensor over-range, `0x200` max
brew time (120 s), `0x400` wiring topology.

`t_ms` is 0 while a shot is pending, the elapsed time while it runs, and the frozen final duration
once the machine is idle again. During a flush it carries the **remaining** flush time in ms.

**Decoding.** C/C++ clients include `telemetry_wire.h` and call `sbTelemetryDecode()`. Other
clients follow the same steps: check that the length is 40–126, byte 0 is `0x53` and byte 1 is `2`;
compute CRC-8/ATM (polynomial `0x07`, initial value 0, no reflection, no final XOR) over bytes
`0 … n−2` and compare with byte `n−1`; read the 36-byte head; read the three strings; reject the
frame unless the last string ends exactly on the CRC byte.

### Text messages from the controller

- Command replies, e.g. `{"ok":true,"cmd":"START"}` or `{"ok":false,"cmd":"START","err":"machine not idle (BREWING)"}`.
- Shot end, to every client: `{"evt":"shot_end","duration_ms":n,"state":"IDLE","peak_bar":x,"dose_g":x,"shots":n,"faults":n}`.

### Commands

Send one JSON object per text message (max 255 bytes). The same commands work on UART1.

| Command | Arguments | Effect |
|---|---|---|
| `{"cmd":"START"}` | – | Start a shot. |
| `{"cmd":"STOP"}` | `reason` (optional, logged) | Stop the shot or flush. |
| `{"cmd":"FLUSH"}` | `sec` (1–60, default 5) | Run a group flush. |
| `{"cmd":"TARE"}` | – | Tare the scale. |
| `{"cmd":"NEXT_PHASE"}` | – | Skip to the next phase. Always replies ok. |
| `{"cmd":"USE"}` or `LOAD_PROFILE` | `id` | Queue the profile and write it to `/profiles/index.json`. `ok` means queued; loading is deferred while the machine is busy. |
| `{"cmd":"SIM"}` | `on` (bool) | Enable or disable the simulator. |
| `{"cmd":"CLEAR_FAULTS"}` | – | Clear latched faults. |
| `{"cmd":"OVERRIDE_PRESSURE"}` | `bar`; omit to clear | `SB_BENCH` builds only. |
| `{"cmd":"MANUAL_POWER"}` | `pct`; omit to clear | `SB_BENCH` builds only. |

Unknown commands get `{"ok":false,"err":"unknown command"}`, malformed JSON
`{"ok":false,"err":"malformed json"}`.

## UART1 NDJSON

For an external display wired to GPIO17 (TX) and GPIO18 (RX) at 115200 8N1, newline-terminated
JSON in both directions (`src/network/aux_bridge.cpp`). There is no wireless display transport
besides the WebSocket.

| Direction | Line |
|---|---|
| Out, at boot | `{"evt":"hello","dev":"brew-mcu","fw":"<version>"}` |
| Out, 10 Hz in BREWING, 2 Hz otherwise | `{"t_ms","state","mode","pump_owner","phase_idx","phase_name","p_bar","p_target","flow_virt","psm_pct","weight_g","flow_weight","scale_conn","scale_name","scale_bat","ac_hz","phase_pct","faults","duty_ff","flow_slope","sensor_open_warn","sim","weight_settling","profile","profile_id"}` with values in real units (bar, ml/s, g, g/s, %), decoded from the same binary frame |
| Out, after a shot | `{"evt":"shot_end","duration_ms":n,"peak_bar":x,"dose_g":x,"faults":n}` |
| In | Any command from the table above, one per line, max 255 characters |
| Out, reply | Accepted: `{"evt":"ack","raw":<reply>}`. Rejected: the reply object itself. |

A telemetry line is skipped, never split, when the 1 KB transmit buffer cannot take it whole.

## Shot history format

Shots are stored as `/history/<slot>.json` in a ring of 15 slots. `GET /api/v1/history` returns
the headers only:

```json
{"ok":true,"count":2,"shots":[
  {"slot":0,"id":"0-1718000000000","profileId":"classic_9bar","profileName":"Classic 9-Bar",
   "epochMs":1718000000000,"durationMs":31200,"peakBar":9.05,"doseG":36.40,"dose_g":36.40,"count":312}
]}
```

`GET /api/v1/history?slot=0` returns the same header plus `"s":[...]`, one row per 100 ms:

| Index | Field | Unit |
|---|---|---|
| 0 | `tMs` | ms since shot start |
| 1 | `pBar` | bar |
| 2 | `pTarget` | phase target (bar, ml/s or %) |
| 3 | `flowVirt` | ml/s, pump model |
| 4 | `psmPct` | pump duty % |
| 5 | `weightG` | g |
| 6 | `flowWeight` | g/s, from the scale |
| 7 | `phaseIdx` | phase index |

`doseG` is the settled yield in the cup, and `dose_g` repeats it. Shots posted by a client keep
the client's `s` array as sent and have `count` 0.

See also: [Operation.md](Operation.md), [Testing.md](Testing.md), [../README.md](../README.md).
