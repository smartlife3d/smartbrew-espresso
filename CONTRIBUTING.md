# Contributing

Thanks for helping. This repository holds several programs that talk to each other, so a change in
one place often needs a matching change somewhere else. This page lists what to build, what to test
and which rules keep the parts compatible.

> **Safety first.** The controller switches mains power to a pump on a pressurised machine. Test
> power-stage changes on a bench with the simulator or a dummy load before they go near a real
> machine. Read the safety notice in the [README](README.md) and [`docs/Hardware.md`](docs/Hardware.md).

## Repository map

| Folder | What it is | Build |
|---|---|---|
| `/` (`src/`, `include/`, `data/`) | ESP32-S3 controller firmware and web dashboard | `pio run` (`-e esp32s3`, `-e esp32s3-bench`) |
| `display-waveshare-3.5/` | Waveshare touch display (LVGL) | `cd display-waveshare-3.5 && pio run` |
| `companion-c3-scale/` | ESP32-C3 + HX711 companion scale | `cd companion-c3-scale && pio run` |
| `simulator-esp32s3/` | Hardware-in-the-loop simulator and host tests | `cd simulator-esp32s3 && pio run` |
| `android-app/` | Android companion app (wraps the web dashboard) | `cd android-app && ./gradlew assembleDebug` |

Tools: [PlatformIO](https://platformio.org/) for the firmware; JDK 17+ and the Android SDK for the
app (`android-app/local.properties` → `sdk.dir`).

## Before you open a pull request

1. **Build what you touched**, and the controller as well if you touched `include/`, since
   `include/telemetry_wire.h` and `include/pump_physics.h` are shared by several programs.
2. **Run the host regression tests.** They need no hardware, only a C++17 compiler:
   ```bash
   cd simulator-esp32s3 && ./test/host/run.sh
   ```
   If you change one of the header-only rules (`weight_settle.h`, `shot_tail.h`, `scale_flow.h`,
   `flow_calibration.h`, `pump_physics.h`), add or update a test for it in `test/host/`.
3. **If you changed `data/index.html`**, re-sync the Android copy:
   ```bash
   ./android-app/sync_web_assets.sh
   ```
   The controller build regenerates `src/network/web_assets.h` and `data/index.html.gz` by
   itself (`tools/gen_web_assets.py`). Commit the regenerated files.
4. **Describe what you tested** in the pull request: bench, simulator, or a real machine.

## Rules that keep the parts compatible

- **Telemetry wire format.** `include/telemetry_wire.h` defines the binary frame. The controller,
  the Waveshare display and the web dashboard (and therefore the Android app) all decode it. Change
  the layout only together with every decoder, and bump `SB_TELEMETRY_VERSION`.
- **Config struct.** `SystemConfig` (`include/config_schema.h`) is stored as an NVS blob. Add new
  fields **at the end**, bump `SB_CFG_VERSION`, and add the previous version to
  `SB_CFG_APPEND_ONLY_SIZES` so existing settings are carried over. Add every new field to the JSON
  load/save in `src/config_store.cpp` and to `/api/v1/config` in `src/network/web_api.cpp`.
- **Built-in profiles.** If you change a preset in `ProfileEngine::getPresetJson()`, bump
  `SB_SEED_PROFILE_VERSION` so devices refresh their untouched copy. A user-edited profile is never
  overwritten.
- **One clock per control tick.** Inside `BrewController::tick()` and everything it calls, use the
  tick's `now`. Never read `millis()` a second time. A later reading makes stored timestamps lie in
  the future and breaks every elapsed-time check.
- **No flash writes on the control task.** Core 1 runs the control loop. Anything that writes
  LittleFS or NVS is handed to Core 0 (see how `SHOT_COMPLETED` carries the learned values).
- **No blocking work in `loop()`.** It runs on Core 1, beside the control task (see the task map at
  the top of `src/main.cpp`).

## Code style

- Follow the style of the file you are editing: 4-space indentation, `camelCase` functions,
  `_member` fields, `g_` singletons.
- Comments explain **why**. The firmware is safety-relevant, and the reasoning behind a threshold or
  an ordering matters more than a restatement of the code.
- Log lines describe machine behaviour, not wiring (no GPIO numbers in logs), and use the existing
  tags (`[PRE-BREW]`, `[BREW]`, `[PHASE]`, `[STOP]`, `[SETTLE]`, `[FLOW]`).

## Third-party code

Do not paste code from other projects. If you need a library, add it as a dependency
(`lib_deps` or Gradle), or vendor it unchanged with its license file. Then add an entry to
[`THIRD_PARTY.md`](THIRD_PARTY.md). Protocol facts (UUIDs, frame layouts, command bytes) are fine to
implement; say where you learned them in the pull request.

## License of contributions

By submitting a contribution you agree that it is licensed under the project's license,
[CC BY-NC-SA 4.0](LICENSE), and that you have the right to submit it under that license.

## Reporting bugs and safety issues

Open an issue with the firmware version (`status` on the serial console), the board, and the
relevant log lines. For anything that could make the hardware unsafe, follow
[`SECURITY.md`](SECURITY.md) instead of opening a public issue.
