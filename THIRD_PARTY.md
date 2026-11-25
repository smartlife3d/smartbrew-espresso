# Third-Party Notices & Licenses

This repository contains several programs: the controller firmware, the Waveshare display
firmware, the companion scale firmware, the HIL simulator, the web dashboard and the Android
companion app. Each one links against, or ships alongside, open-source software from other
authors. This file records where each piece comes from, who holds its copyright, and which
license applies.

The project's own code is licensed under the terms in [`LICENSE`](LICENSE). Nothing listed below
is relicensed by that: every third-party component keeps its own license.

Full license texts that are not shipped inside a component's own folder are in
[`LICENSES/`](LICENSES/).

---

## 1. Libraries fetched at build time

These are not stored in this repository. PlatformIO and Gradle download them when you build. They
are listed because they are compiled into the firmware images and the Android app that you
distribute when you share a binary.

### Firmware (PlatformIO)

| Library | Used by | License | Copyright |
|---|---|---|---|
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) | all firmware | LGPL-2.1 | Espressif Systems and contributors |
| [ESP-IDF](https://github.com/espressif/esp-idf) (precompiled, via the Arduino core) | all firmware | Apache-2.0 | Espressif Systems |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | controller | LGPL-3.0 | Hristo Gochkov and contributors |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | controller | LGPL-3.0 | Hristo Gochkov and contributors |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) v6 (controller), v7 (display) | controller, display | MIT | Benoît Blanchon |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | controller, companion scale, simulator | Apache-2.0 | h2zero and contributors; Apache Software Foundation (Apache Mynewt NimBLE) |
| [LVGL](https://github.com/lvgl/lvgl) 8.3 | display | MIT | LVGL Kft |
| [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) | display | LGPL-2.1 | Markus Sattler and contributors |

**LGPL components.** The Arduino core, ESPAsyncWebServer, AsyncTCP and arduinoWebSockets are
LGPL. A firmware image that contains them may be distributed provided the recipient can obtain
the corresponding source of the LGPL parts and can rebuild and reflash the image with a modified
version of them. This repository publishes the complete source of every firmware and the build
configuration, and the boards can be reflashed over USB, which satisfies that. If you distribute
devices with this firmware, do not lock the bootloader or flash against reflashing.

### Android app (Gradle)

| Library | License | Copyright |
|---|---|---|
| Kotlin standard library and Kotlin Gradle plugin | Apache-2.0 | JetBrains s.r.o. and contributors |
| AndroidX Core KTX, AppCompat, WebKit | Apache-2.0 | The Android Open Source Project |
| Material Components for Android | Apache-2.0 | The Android Open Source Project |
| kotlinx-coroutines-android | Apache-2.0 | JetBrains s.r.o. and contributors |
| [OkHttp](https://github.com/square/okhttp) | Apache-2.0 | Square, Inc. |
| Gradle Wrapper (`android-app/gradle/wrapper/gradle-wrapper.jar`, stored in this repo) | Apache-2.0 | Gradle, Inc. |

Apache-2.0 text: [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).

---

## 2. Third-party code stored in this repository

### Gaggiuino default profiles — `src/control/profile_engine.cpp`
The built-in profiles `londinium`, `iuiuiu_classic`, `adaptive` and `blooming_flow` are adapted
from the default profiles of [Gaggiuino](https://github.com/Zer0-bit/gaggiuino)
(`src/eeprom_data/default_profiles.h`). Copyright the Gaggiuino contributors, licensed under
[CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/). Changes: the phase parameters
were translated into this controller's phase format (flow / pressure limits, `pause` soaks, fixed
start values where Gaggiuino continues from the current value) and the temperature setpoints were
dropped. Each profile's `author` field names the source. Non-commercial use only, as for the
rest of this project.

### Arduino_GFX — `display-waveshare-3.5/lib/GFX_Library_for_Arduino/`
- **Origin:** [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX), which
  builds on the Adafruit GFX Library.
- **License:** BSD-style license, see `lib/GFX_Library_for_Arduino/license.txt` (shipped in the folder).
- **Copyright:** Adafruit Industries; Moon On Our Nation and contributors.
- **Version and changes:** v1.5.5, with the upstream fix for the Arduino core 3.3.6+
  `spiFrequencyToClockDiv()` signature backported into `src/databus/Arduino_ESP32SPI.cpp` and
  `Arduino_ESP32SPIDMA.cpp`.
- **Bundled fonts** (`src/font/`). The library header includes them, so they stay with the library:
  - `glcdfont.h`: the classic 5×7 GLCD font from the Adafruit GFX Library, under the same BSD license.
  - `u8g2_font_unifont_*`: GNU Unifont glyphs. Copyright 1998–2021 Roman Czyborra, Paul Hardy and
    others. Dual-licensed **SIL Open Font License 1.1** or GPL-2.0-or-later with the GNU font
    embedding exception. This project uses them under the OFL; see
    [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt).
  - `u8g2_font_quan7_h_cjk.h`: Galmuri8, Chill Bitmap, diaowinner (2022), SIL Open Font License 1.1.
  - `u8g2_font_chill7_h_cjk.h`: copyright 2020–2022 warren2060. "Unlimited permission is granted
    to use, copy, and distribute them, with or without modification, either commercially or
    noncommercially." The notice is kept in the file header.
  - `u8g2_font_cubic11_h_cjk.h`: Cubic 11, same free-use notice, kept in the file header.

### XPowersLib — `display-waveshare-3.5/lib/XPowersLib/`
- **Origin:** [lewisxhe/XPowersLib](https://github.com/lewisxhe/XPowersLib)
- **License:** MIT (`lib/XPowersLib/LICENSE`, shipped in the folder).
- **Copyright:** © 2022 Lewis He.

### AXS15231B touch driver — `display-waveshare-3.5/lib/esp_lcd_touch_axs15231b/`
- **Origin:** Waveshare board support package for the ESP32-S3-Touch-LCD-3.5B.
- **License:** Apache-2.0 (SPDX header in the source; full text in the folder's `LICENSE`).
- **Copyright:** © Waveshare Electronics Co., Ltd.

### TCA9554 port-expander driver — `display-waveshare-3.5/src/TCA9554.cpp`, `include/TCA9554.h`
- **Origin:** [RobTillaart/TCA9554](https://github.com/RobTillaart/TCA9554) v0.1.2.
- **License:** MIT, see [`LICENSES/MIT-TCA9554.txt`](LICENSES/MIT-TCA9554.txt).
- **Copyright:** © 2025–2026 Rob Tillaart.

### 5×7 OLED font — `companion-c3-scale/src/ssd1306.cpp`
- The `kFont5x7` table is a variant of the widely shared classic 5×7 character-LCD glyph set.
  Where it came from was not recorded. 52 of its 95 glyphs are byte-identical to `glcdfont.c` in the
  Adafruit GFX Library, so it is attributed to that library's license as a precaution.
- **License:** BSD-style (Adafruit GFX Library license, reproduced in
  `display-waveshare-3.5/lib/GFX_Library_for_Arduino/license.txt`).
- **Copyright:** © 2012 Adafruit Industries (for the shared glyphs).

---

## 3. Protocols, formats and algorithms

- **BLE scale protocols** (Acaia, Bookoo, Timemore, Felicita / Decent / Cocinare). The scale
  drivers in `src/scale/`, `android-app/.../scale/` and the companion scale's Bookoo-compatible
  advertising implement each manufacturer's published or documented BLE message layout: service
  and characteristic UUIDs, frame layouts and command bytes. These are interface facts needed for
  interoperability. The driver code itself is this project's own.
- **Bresenham-style pulse distribution** (`src/hal/hal_psm.cpp`). The accumulator that spreads
  fired half-cycles evenly is the textbook error-accumulation technique (J. E. Bresenham, 1965),
  written for this project with zero-cross validation, mains auto-detection and the passive/profile
  ownership model.
- **Profile import formats** (`data/index.html`, `src/control/profile_engine.cpp`). The web
  dashboard and the firmware accept profile JSON written by other tools, and can export to one such
  format, so that users can move their own profiles between programs. Only the data layout is
  implemented; no code from those tools is used.

---

## 4. Trademarks

Product and company names that appear in this repository (for example the scale, pump, display and
espresso-machine brands the hardware is compatible with) are trademarks of their respective
owners. They are used only to say what the project works with. This project is independent and is
not affiliated with, endorsed by or sponsored by any of them.
