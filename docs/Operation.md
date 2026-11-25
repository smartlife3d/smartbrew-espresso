# Building, flashing and running the controller

This page covers the tools side: building and flashing every program in the repository, keeping
the recorded shots safe, first boot and Wi-Fi, firmware updates, the serial console and the log.
Using the dashboard is described in [Dashboard.md](Dashboard.md); verifying a new build on the
bench is described in [Testing.md](Testing.md). Wiring is in [Hardware.md](Hardware.md).

## Prerequisites

| Need | For | Notes |
|---|---|---|
| [PlatformIO Core](https://platformio.org/install/cli) (`pio` on `PATH`) | All ESP32 firmware | Fetches the pioarduino `espressif32` platform 55.03.313 (Arduino core 3.3.13, ESP-IDF 5.5.6) and the libraries itself |
| Python 3 | The scripts in `tools/` | PlatformIO's own interpreter (`~/.platformio/penv/bin/python`) works and already has pyserial |
| bash, macOS or Linux | `flash.sh`, `monitor.sh`, `tools/brewmon.py` | `brewmon.py` uses `termios`, so it does not run on native Windows |
| A data USB-C cable | Flashing | Charge-only cables enumerate nothing |
| JDK 17 and the Android SDK | Android app only | The SDK path goes in `android-app/local.properties` (`sdk.dir=...`) |
| A C++17 compiler | Host regression tests only | See [Testing.md](Testing.md) |

The controller targets an **ESP32-S3-WROOM-1-N8R8** (8 MB flash, 8 MB octal PSRAM). For an N16R8
module set `board_upload.flash_size = 16MB` in `platformio.ini`. The repository ships only
`partitions_8MB.csv`, which also fits a 16 MB chip and leaves the upper 8 MB unused.

## Build and flash

### The controller

```bash
./flash.sh                 # find the board, build, flash the firmware, open the console
./monitor.sh               # open the console only
pio run                    # build only (env esp32s3)
pio run -t upload          # build and flash with PlatformIO's own port choice
```

`flash.sh` and `monitor.sh` share `scripts/portlib.sh`. It ranks serial ports by USB VID:PID
(Espressif native USB, CP210x, CH340/CH9102, FTDI, PL2303), skips system ports such as Bluetooth,
and polls four times because macOS sometimes drops a freshly attached board for a second or two.

| `flash.sh` option | Effect |
|---|---|
| *(none)* | Firmware only. Prompts only if more than one board could be the controller. |
| `--list` | Show every port, with the ESP32 boards marked, and exit |
| `-p <port>` / `-p <n>` / `-i <n>` | Use this port, or the n-th entry of `--list` |
| `--fs` | Also replace the LittleFS image (**destroys shots and profiles**, asks and backs up first) |
| `--fs-only` | Replace only the LittleFS image (same safeguards) |
| `--erase` | Erase the whole flash before writing (same safeguards) |
| `--no-backup` | Skip the backup before a destructive step |
| `--yes` | Do not ask before a destructive step (required when there is no terminal) |
| `--no-reset` | Flash without toggling DTR/RTS, using env `esp32s3-bootloader` |
| `--no-monitor` | Do not open the console afterwards |
| `--dry-run` | Print the commands and run nothing |

A successful flash opens the console on the port it just wrote. If a console is already holding
that port, `flash.sh` stops it first. Exit codes: 0 success, 1 usage error, 2 no port found,
3 upload failed.

The dashboard is compiled into the firmware. Every build runs `tools/gen_web_assets.py`, which
gzips `data/index.html` into `src/network/web_assets.h`, so a firmware flash always ships the
current dashboard and no filesystem upload is needed.

### Build environments

| Env | Use | Build and flash |
|---|---|---|
| `esp32s3` | Normal firmware (`SB_BENCH=0`). Default for `pio run` and `flash.sh`. | `./flash.sh` |
| `esp32s3-bench` | Bench firmware (`SB_BENCH=1`): adds the bench console commands and the pressure, weight and duty injection endpoints | `pio run -e esp32s3-bench -t upload --upload-port <port>` |
| `esp32s3-uart` | Removes the USB CDC console flags, so `Serial` is UART0 (the console listens on both either way) | `pio run -e esp32s3-uart -t upload --upload-port <port>` |
| `esp32s3-bootloader` | Uploads with `--before no_reset`, for a board already held in the ROM downloader | `./flash.sh --no-reset` |

`flash.sh` always builds `esp32s3` (or `esp32s3-bootloader` with `--no-reset`). To run the bench
firmware, flash it with the `pio` command above and open the console with `./monitor.sh -p <port>`.
**Never install the bench firmware in a machine that is in use:** `duty` and `manual` drive the
pump with the pressure loop bypassed.

### The other programs

| Program | Build | Flash |
|---|---|---|
| Waveshare 3.5" display | `pio run -d display-waveshare-3.5` | `./display-waveshare-3.5/flash.sh` (asks for the port; `--list`, `-p <port\|n>`, `--no-monitor`) |
| HIL simulator | `cd simulator-esp32s3 && pio run` | `cd simulator-esp32s3 && ./flash.sh` (always asks for the port; `-p`, `--build-only`, `--no-monitor`) |
| Companion scale (ESP32-C3) | `pio run -d companion-c3-scale` | `pio run -d companion-c3-scale -t upload --upload-port <port>` |
| Android app | `cd android-app && ./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk` | `./upload_echo5.sh` |

The simulator and display scripts always ask for the port, because all three ESP32-S3 boards look
identical to the computer. `./upload_echo5.sh` (also `android-app/upload.sh`) copies
`data/index.html` into the app, builds the debug APK, installs it over ADB and starts it; options
`-s <serial|ip:port>`, `-c` (clean), `--no-sync`, `--no-launch`, `-l` (follow logcat). Details:
[`../android-app/README.md`](../android-app/README.md), [`../display-waveshare-3.5/README.md`](../display-waveshare-3.5/README.md),
[`../simulator-esp32s3/README.md`](../simulator-esp32s3/README.md), [`../companion-c3-scale/README.md`](../companion-c3-scale/README.md).

## What a flash erases

The controller keeps the brew ledger (`/history`), the profiles (`/profiles`) and a copy of its
settings (`/config.json`) in the LittleFS partition (`0x610000`, 1.875 MB). The settings are also
stored in NVS, together with the Wi-Fi credentials.

| Operation | Writes | Ledger and profiles | Settings and Wi-Fi |
|---|---|---|---|
| `./flash.sh`, `pio run -t upload`, OTA update | Firmware slot only | Kept | Kept |
| Restart, power cycle | Nothing | Kept | Kept |
| `./flash.sh --fs`, `--fs-only`, `pio run -t uploadfs` | A new LittleFS image over the whole partition | **Lost** (the image replaces, it does not merge) | Kept in NVS, restored at boot |
| `./flash.sh --erase`, `pio run -t erase` | Wipes the whole flash | **Lost** | **Lost** |

After a filesystem wipe the controller writes the five built-in profiles back at the next boot.
Edited built-in profiles and your own profiles are gone.

**Use `flash.sh` for anything destructive.** It asks you to type `wipe`, saves the partition to
`backups/littlefs-<time>.bin` first, and stops if the backup fails. By hand (console closed):

```bash
tools/fs_backup.py backup  --port /dev/cu.usbmodem101
tools/fs_backup.py restore --port /dev/cu.usbmodem101 --file backups/littlefs-<time>.bin
```

### Checking what is on the chip

```bash
~/.platformio/penv/bin/python tools/verify_flash.py [--port <port>] [--fs]
```

This reads the flash back and checks the chip identity, the partition table, the SHA-256 of the
whole app image against `.pio/build/esp32s3/firmware.bin`, and that the gzipped dashboard is
embedded. The LittleFS partition is only reported, because it is expected to differ from a local
build. Pass `--fs` only when you flashed the filesystem and want it to match. The tool compares
against the `esp32s3` build, so after flashing the bench firmware it reports a mismatch.

### When the upload keeps failing

| Symptom | Fix |
|---|---|
| `Could not configure port: Device not configured` halfway through | Native USB dropped during the long write. Flash in chunks: `tools/flash_chunked.sh <port> .pio/build/esp32s3/firmware.bin 0x10000 64` (retries each 64 KB chunk and checks the digest; needs `esptool.py` on `PATH`) |
| `multiple access on port` | A console still holds the port. Quit `monitor.sh`/`brewmon.py`. |
| No port found | Hold BOOT, tap RESET, release BOOT, then `./flash.sh --list` and `-p <port>` |
| Console shows `waiting for download` and nothing else | The chip is in the ROM downloader. Tap RESET on its own, without BOOT. |

## First boot and Wi-Fi

1. With no stored network the controller opens an open hotspot named `SmartBrew-XXXX` (the last
   four hex digits of its MAC) on channel 1. The console prints `SETUP HOTSPOT ACTIVE`.
2. Join it from a phone. The setup page opens by itself; if not, open `http://192.168.4.1/wifi`.
   The dashboard itself is at `http://192.168.4.1`.
3. Enter the network name and password (2.4 GHz only). The controller saves them and restarts
   after 1.2 s.
4. Once connected the console prints `Wi-Fi connected: <ip> ... http://smartbrew.local`. The
   dashboard is then at `http://smartbrew.local` (the name is set in Settings → Hardware) or the IP.

If the stored network cannot be reached for 3 minutes, the controller falls back to the setup
hotspot. It also syncs its clock from `pool.ntp.org` (UTC) once connected, so shots are stamped
with the real time.

| Console | Effect |
|---|---|
| `wifi` | Show mode, IP and signal |
| `wifi <ssid> <password>` | Save credentials and restart |
| `portal` | Open the setup hotspot now |
| `scan` | List nearby 2.4 GHz networks |

The dashboard's **Forget & Enter Hotspot** (Settings → Hardware) clears the credentials and opens
the hotspot.

## Updating over Wi-Fi (OTA)

Build with `pio run`, then in the dashboard open Settings → Tools → **Firmware Update (OTA)**,
choose `.pio/build/esp32s3/firmware.bin` and press **Flash Firmware**. From a terminal:

```bash
curl -F "firmware=@.pio/build/esp32s3/firmware.bin" http://smartbrew.local/api/v1/ota/upload
```

The controller stops any shot and disables the pump driver before the first flash sector is
written, writes the inactive app slot, and restarts. The new image marks itself good after its
control loop has run cleanly for 30 s; if it resets before that, the bootloader returns to the
previous image. An upload that stalls for more than 30 s is aborted and the pump driver is
re-enabled. OTA only replaces the firmware, so the ledger and profiles stay. The REST endpoints,
including the URL-based update, are listed in [API.md](API.md).

## Serial console

`./monitor.sh` opens the console through `tools/brewmon.py` at 115200 baud. It reconnects by
itself when the board resets (native USB disappears for about a second), timestamps every line,
colours warnings and shot events, and recognises the ROM downloader. `pio device monitor` works
too but gives up at every reset.

| `monitor.sh` option | Effect |
|---|---|
| `--list`, `-p <port\|n>` | Same port selection as `flash.sh` |
| `--reset` | Reset the board on connect and capture the boot banner |
| `--log` | Also write the session to `logs/monitor-*.log` |
| `--send <command>` | Send one command after connecting |
| `--duration <s>` | Stop after that many seconds |

Keys: `Ctrl-C` / `Ctrl-]` quit, `Ctrl-R` reset the board, Up/Down history, `Tab` completes
commands. The console also listens on UART0 at 115200 baud.

### Commands

**Bench** commands exist only in the `esp32s3-bench` build. In the normal build they answer
`ERR: bench feature disabled in production build (-DSB_BENCH=1 required)`.

| Command | Build | Effect |
|---|---|---|
| `help` (or `?`) | all | List the commands |
| `status` | all | State, faults, pressure, duty and PID terms, zero-cross, phase, scale, button, flow, memory |
| `zc` | all | Zero-cross and power-stage diagnostics |
| `disarm` / `arm` | all | Stop any shot and switch the power stage and the button output off / back on |
| `start` / `stop` / `tare` / `next` | all | Start a shot, stop it, tare the scale, skip to the next phase |
| `profiles` | all | List the profile files, marking the active one |
| `use <id>` | all | Load a profile by id and make it the default |
| `phase` | all | Print the active profile's phase table |
| `scale` | all | Scale state, MAC, battery, update count, cutoff events, cup flow |
| `scale scan` / `scale pair <mac\|AUTO>` / `scale disc` | all | Scan 8 s for scales / pair / disconnect |
| `wifi`, `wifi <ssid> <pw>`, `portal`, `scan` | all | Wi-Fi, see above |
| `led <r> <g> <b>` / `led bright <0-100>` / `led auto` | all | Force the status LED colour / set and save its brightness / return to automatic |
| `heap` | all | Free heap, minimum ever, PSRAM, largest block |
| `clear` | all | Clear faults |
| `sim on` / `sim off` | bench | Simulated shots from the built-in model (the dashboard's SIM setting does the same in every build) |
| `press <bar>` | bench | Replace the transducer reading with this pressure |
| `weight <g>` | bench | Replace the scale weight with this value |
| `zero` | bench | Run the zero-pressure calibration (idle only) |
| `btn` | bench | Pulse the brew-button output once |
| `duty <0-100>` / `manual <0-100>` | bench | Force the pump duty, bypassing the pressure loop |
| `set <bar>` | bench | Override the pressure target of the running phase |

What each command should print on a bench is shown in [Testing.md](Testing.md).

## Log reference

Every log line starts with a level, `I|`, `W|` or `E|` (info, warning, error). The same lines
appear in the dashboard under Settings → Logs. Shot events carry a tag, so one shot can be read
from start to finish:

| Tag | Covers |
|---|---|
| `[PRE-BREW]` | The hardware check before a shot, the scale tare and its verification, the 500 ms settle |
| `[BREW]` | The start of a shot and the profile that was loaded |
| `[PHASE]` | Each phase's target, and why each phase ended |
| `[STOP]` | Why the shot ended, with weight, duration and peak pressure |
| `[SETTLE]` | The weight tail after the pump stops, the final yield, the learned cutoff lag |
| `[FLOW]` | The pump-flow calibration against the scale after a shot |
| `[SCALE]` | A tare requested with no scale connected |
| `CLOCK:` | Bench build only: a timing self-check in the control loop. It should never appear. |

Untagged lines worth knowing: `State A -> B (reason)`, `FAULT 0x.... set: <reason>`,
`WEIGHT CUTOFF: ...`, `Mains AC active in IDLE ...` (the machine is running its own pump).

A shot with a connected scale (level prefixes left out):

```
[PRE-BREW] Hardware check starting (profile 'Classic 9-Bar', 4 phase(s), target 36.0 g, max 45.0 s)
[PRE-BREW]   power stage  ARMED - zero cross not yet seen (machine idle), mains 50 Hz
[PRE-BREW]   transducer   OK - source ADS1115, node 333 mV, 0.00 bar
[PRE-BREW]   button       ARMED - pulse 180 ms
[PRE-BREW]   scale        CONNECTED - Bookoo_SIM [Bookoo], battery 100 %, raw 0.0 g
[PRE-BREW] Hardware check passed
[PRE-BREW] Scale tare requested - waiting for the scale to confirm it
[PRE-BREW] Scale tare VERIFIED - net 0.0 g, confirmed by a fresh reading
[PRE-BREW] All hardware checks passed (scale tared) - the brew starts in 500 ms
[PHASE] 1/4 'Pre-Infusion' - pressure mode, target 3.00 bar, psm ceiling 60%
[BREW] Profile 'Classic 9-Bar' started - 4 phase(s), target 36.0 g, max 45.0 s
[BREW] START - profile 'Classic 9-Bar', 4 phase(s), target 36.0 g
[PHASE] 'Pre-Infusion' -> 'Ramp to 9 Bar' after 2.6 s (pressure 3.01 bar >= 3.00 bar)
...
WEIGHT CUTOFF: 34.9 g >= 36.0 g target - 1.14 g predicted (slope 1.90 g/s)
[STOP] Stop requested - weight target reached
[SETTLE] Post-shot weight settling started at 35.0 g - watching the cup fill
[STOP] weight target reached - 35.0 g in 27.8 s (peak 9.12 bar, ended in phase 3 'Peak Extraction')
[SETTLE] Final yield 36.3 g (the weight stopped changing)
```

## Tools

| Tool | Purpose | Run |
|---|---|---|
| `tools/brewmon.py` | The serial console behind `monitor.sh` | `./monitor.sh` |
| `tools/fs_backup.py` | Save / restore the LittleFS partition | `tools/fs_backup.py backup --port <port>` |
| `tools/verify_flash.py` | Read the flash back and compare it with the local build | `~/.platformio/penv/bin/python tools/verify_flash.py` |
| `tools/flash_chunked.sh` | Write an image in chunks with retries | `tools/flash_chunked.sh <port> <bin> <offset> [chunk_kb]` (default 96 KB) |
| `tools/wsprobe.py` | Measure the WebSocket telemetry rate, gaps and stalls | see below |
| `tools/wsping.py` | Measure the WebSocket command round trip | see below |
| `tools/gen_web_assets.py` | Gzip the dashboard into `src/network/web_assets.h` | Runs on every build |
| `tools/gen_screensaver_art.py` | Draw the screen-saver art for the display and the Android app (needs Pillow) | `python3 tools/gen_screensaver_art.py --preview` |
| `tools/test_brewmon.py` | Unit tests for the console | `~/.platformio/penv/bin/python tools/test_brewmon.py` |
| `android-app/sync_web_assets.sh` | Copy `data/index.html` into the Android app | Run by `upload_echo5.sh` |

### Measuring telemetry latency

The dashboard's chart is only as smooth as the telemetry stream: 20 frames/s during a shot, 10/s
while the post-shot weight settles, 2/s when idle. Both tools use only the Python standard library.
Their default host is `brew-mcu.local`, so pass `--host`:

```bash
python3 tools/wsprobe.py --host smartbrew.local --duration 30 --gap-ms 150          # rate, gaps, stalls
python3 tools/wsprobe.py --host smartbrew.local --duration 30 --sim --start --stop  # during a simulated shot
python3 tools/wsping.py  --host smartbrew.local --count 40                          # command round trip
```

`wsprobe.py` reports the frame rate, the gap distribution and every gap above `--gap-ms`, with the
controller's shot clock and frame sequence number, so a late frame can be told from a lost one.
Gaps while idle are counted separately. If `wsping.py` stays smooth while `wsprobe.py` shows
stalls, the delay is in the network, not the controller. `GET /api/v1/status` also reports the
controller's own timing counters ([API.md](API.md)).

### Flash and RAM usage

Every `pio run` ends with the RAM and flash usage of that build (`RAM: [==] x% ...`,
`Flash: [===] y% ...`). The app slot is 3 MB (`board_upload.maximum_size = 3145728`).
`pio run -t size` prints the same summary for the last build; add `-d <project>` for the display,
simulator or scale.

## Troubleshooting

| Problem | Likely cause | What to do |
|---|---|---|
| Bench command answers `ERR: bench feature disabled ...` | Normal firmware installed | Flash `esp32s3-bench` (see Build environments) |
| Shots and profiles gone after flashing | `uploadfs`, `--fs` or `--erase` was used | Restore the backup from `backups/` with `tools/fs_backup.py restore` |
| `smartbrew.local` does not resolve | mDNS blocked by the network or not supported by the client | Use the IP printed on the console (`wifi`) or shown by the router |
| Controller reappears as `SmartBrew-XXXX` hotspot | Stored network unreachable for 3 minutes, or wrong password | Re-enter credentials; the console prints the disconnect reason |
| `start` does nothing and logs nothing | Mains is already present on the pump (machine running its own pump) | Wait for the machine to finish, then start |
| `[PRE-BREW] Hardware check FAILED - ...` | A blocking check failed; the reason names the part | Fix that part; see [Testing.md](Testing.md) |
| `FAULT 0x0400 set: suspected permanent mains AC in IDLE` | Mains present on the pump for longer than the topology timeout (120 s) while idle | Check the wiring ([Hardware.md](Hardware.md)); on a bench, stop the pulse generator between tests |
| `Transducer open-wire while the machine is driving the pump - warning only` once a minute | No transducer, or its input below 133 mV | Connect the transducer; on a bare bench board this warning is expected |
| Dashboard shows an old layout after an update | The page in the browser predates the update | Reload; the page is sent with `no-cache` |
| OTA upload fails at once | Another update is running, or the file is not a controller `firmware.bin` | Wait 30 s or reboot, and upload `.pio/build/esp32s3/firmware.bin` |
