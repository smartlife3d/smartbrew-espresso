// scale_config.h - pinout, calibration, filter and transport constants for the
// companion ESP32-C3 + HX711 espresso scale.
//
// SHARED STATE CONTRACT
// ---------------------
// The HX711 task owns acquisition and publishes the fused state below. The BLE
// task and the display sample it with stateRead(). The helpers are implemented in
// src/main.cpp; every consumer therefore sees an identical, coherent snapshot.

#pragma once

#include <Arduino.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Hardware pinout (ESP32-C3 SuperMini)
// ---------------------------------------------------------------------------
#define PIN_HX711_DOUT        4   // HX711 pin 11 (DOUT / DATA) -> scale DOUT
#define PIN_HX711_SCK         5   // HX711 pin 12 (SCK / PD_SCK)  -> scale CLK
// HX711 pins 1/2 (VSUP) -> 3V3, pin 3 (VGAT) -> 3V3, pin 4 (VFB) -> GND,
// pin 5 (AGND) -> GND, pin 6 (VBG) -> GND, pin 7 (IN-A-) -> GND,
// pin 8 (IN-A+) -> cell+, pin 9 (IN-B-) -> GND, pin 10 (IN-B+) -> cell-.
// XDA(6)/XD(7) are the dummy I2C pins of the load-cell module and are tied
// together with DOUT - this firmware never drives an I2C bus.
#define PIN_LED               8   // SuperMini on-board LED (active high)
#define PIN_BATTERY_ADC       0   // ADC1_CH0, divider from the Li-coin supply

// 0.91" SSD1306 OLED (GND / VCC / SCL / SDA). GPIO2 is a C3 strapping pin, but
// the 4k7 pull-ups to 3V3 hold it high at boot, which is the safe state.
#define PIN_I2C_SDA           2
#define PIN_I2C_SCL           3
#define SSD1306_CONTRAST      0xCF
#define DISPLAY_RATE_HZ       10

// TTP223 capacitive touch key (HW-763): pads VCC / GND / OUT (and SIG).
#define PIN_TOUCH_OUT         1
#define TOUCH_ACTIVE_HIGH     true   // default solder bridges = active high
#define TOUCH_DEBOUNCE_MS     40
#define TOUCH_HOLD_MS         900   // >= this = "power" (display on/off)

// ---------------------------------------------------------------------------
// HX711 configuration
// ---------------------------------------------------------------------------
// HX711 pin 15 (RATE) is tied to 3V3 in hardware -> 80 SPS conversion rate,
// i.e. a fresh 24-bit result every 12.5 ms.
#define HX711_SPS                 80
#define HX711_CONVERSION_MS       (1000 / HX711_SPS)   // 12 ms
#define HX711_READ_TIMEOUT_MS     40   // ~3 conversions before "missing"
#define HX711_PRESENCE_FAIL_COUNT 4   // consecutive timeouts => sensor fault
#define HX711_OVER_RANGE_COUNTS   8000000L  // |raw| beyond this => over-range
#define HX711_SETTLE_CLOCKS       25   // SCK pulses per 24-bit transfer

// ---------------------------------------------------------------------------
// Calibration - replace after the real load cell + HX711 are mounted.
// Put CAL_REFERENCE_WEIGHT_G on the pan, note SCALE_CAL_COUNTS, solve:
//   SCALE_CAL_COUNTS = (raw_with_weight - raw_tared) * CAL_REFERENCE_WEIGHT_G
//                       / measured_weight_grams
// ---------------------------------------------------------------------------
#define CAL_REFERENCE_WEIGHT_G     100.0f
#define SCALE_CAL_COUNTS           23500.0f   // raw counts per gram (factory default)
#define SCALE_DEFAULT_DISPLAY_G     0.0f

// ---------------------------------------------------------------------------
// Kalman filter (fast 1D) applied to RAW HX711 COUNTS, before grams conversion.
// Q = 0.02 / R = 0.15 keeps step-response latency well under 25 ms at 80 SPS.
// ---------------------------------------------------------------------------
#define SCALE_KALMAN_Q              0.02f
#define SCALE_KALMAN_R              0.15f
#define SCALE_KALMAN_SEED_VARIANCE  1.0f

// ---------------------------------------------------------------------------
// Stability gate (scale side vibration lockout)
// ---------------------------------------------------------------------------
#define GATE_MEDIAN_WINDOW          5     // samples in the running-median window
#define GATE_OUTLIER_G              40.0f  // deviation from median that counts
#define GATE_MAX_REJECTS            3      // consecutive rejects before accepting

// ---------------------------------------------------------------------------
// Auto tare on boot
// ---------------------------------------------------------------------------
#define AUTO_TARE_DELAY_MS          2000   // min time before the auto tare
#define AUTO_TARE_WINDOW            20     // samples inspected for "settled"
#define AUTO_TARE_SPREAD_G          0.5f   // max peak-to-peak spread, grams

// ---------------------------------------------------------------------------
// Transport 1: Standard 0x0FFE BLE Scale Service
// ---------------------------------------------------------------------------
// The ADVERTISED NAME is configurable: it carries no protocol meaning and no
// third-party brand. A generic default keeps the unit neutral for distribution, and
// `name <text>` over the serial console (or the NVS key) sets a per-unit name
// without reflashing.
//
// The GATT UUIDs below use the common 0x0FFE espresso scale service (0xFF11 notify,
// 0xFF12 write) - making the scale interoperable with the parent ESP32-S3 controller
// as well as any app or logging tool that supports Bookoo-protocol scales.
//
#ifndef SCALE_DEVICE_NAME
#define SCALE_DEVICE_NAME          "SMARTBREW_SCALE"
#endif

#define SCALE_NAME_MAX              28   // BLE advertised-name limit is 29 chars
#define SCALE_SERVICE_UUID          0x0FFE
#define BOOKOO_SERVICE_UUID         0x0FFE
#define BOOKOO_NOTIFY_UUID          0xFF11
#define BOOKOO_WRITE_UUID           0xFF12
#define BOOKOO_FRAME_LEN            20
// 40 Hz notify rate.
//
// The companion used to publish the weight twice: once over BLE at 20 Hz and once over ESP-NOW at
// 40 Hz. ESP-NOW is gone - it put a second Wi-Fi station on the same channel as the controller's
// telemetry stream and competed with it for airtime. The BLE link is the only path now, so it
// carries the full rate: 40 Hz over BLE is 800 B/s, which is nothing for a 185 byte MTU link,
// and it restores exactly the granularity the ESP-NOW path used to provide.
#define BOOKOO_RATE_HZ              40
#define BOOKOO_SIGN_POS             0x2B   // '+'
#define BOOKOO_SIGN_NEG             0x2D   // '-'
// Bookoo tare command: {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08}
#define BOOKOO_TARE_CMD_0           0x03
#define BOOKOO_TARE_CMD_LEN         6

// ---------------------------------------------------------------------------
// Wi-Fi channel handling (OTA / setup portal only)
//
// The ESP-NOW transport that used to live here is gone. See the README: the weight now reaches
// the controller over BLE, which was already a working transport, so a second Wi-Fi station no
// longer has to share the channel with the controller's telemetry stream.
#define SCALE_WIFI_CHANNEL          1       // Fallback channel if no AP found
#define SCALE_WIFI_SECOND_CHAN      WIFI_SECOND_CHAN_NONE

// ---------------------------------------------------------------------------
// Battery (CR2032 / CR123A Li-coin behind a resistive divider)
// ---------------------------------------------------------------------------
#define BATTERY_DIVIDER_TOP_OHMS    1000000.0f
#define BATTERY_DIVIDER_BOTTOM_OHMS 1000000.0f
#define BATTERY_DIVIDER_TRIM        1.11f    // compensates the ESP ADC network
#define BATTERY_SAMPLE_INTERVAL_MS  5000
#define BATTERY_ADC_ATTEN           ADC_11db

// ---------------------------------------------------------------------------
// LED status patterns (milliseconds)
// ---------------------------------------------------------------------------
#define LED_FAULT_BLINK_MS          100     // fast blink = fault
#define LED_WAIT_BLINK_MS           1000    // slow blink = waiting for HX711
#define LED_SOLID                   true

// ---------------------------------------------------------------------------
// Setup portal (SoftAP + browser): first-boot Wi-Fi setup, BLE name, browser OTA
// ---------------------------------------------------------------------------
// WPA2 needs >= 8 characters. Change this before selling: anyone who knows it can
// reconfigure or reflash a unit that is in setup mode.
#define SETUP_AP_PASSWORD            "password"
#define SETUP_AP_CHANNEL             1
#define SETUP_AP_MAX_CONNECTIONS     4
#define SETUP_WEB_PORT               80
#define SETUP_WEB_TASK_STACK         12288
#define SETUP_AUTO_WHEN_UNCONFIGURED 1   // fresh unit -> AP up until Wi-Fi is saved
#define SETUP_HOLD_BOOT_TO_ENTER     1   // hold the touch key while powering on
#define SETUP_BOOT_HOLD_MIN_MS       1200
#define SETUP_IDLE_TIMEOUT_MS        300000  // leave setup after 5 min idle (battery)

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define SCALE_FW_VERSION            "1.1.0"
#define SCALE_FW_BUILD              __DATE__ " " __TIME__

// ---------------------------------------------------------------------------
// Wi-Fi OTA (framework-provided ArduinoOTA; no lib_deps entry needed)
// The scale stays unassociated by default and only joins an AP when explicitly
// put into update mode with `ota`.
// ---------------------------------------------------------------------------
#define OTA_NVS_NAMESPACE           "wifiota"
#define OTA_NVS_KEY_SSID            "apssid"
#define OTA_NVS_KEY_PASS            "appass"
#define OTA_NVS_KEY_PASSWASHED      0   // set to 1 to store an MD5, not the SSID
#define OTA_PORT                    3232
#define OTA_HOSTNAME_PREFIX         "espresso-scale-"
#define OTA_PASSWORD                "espresso"    // change before shipping
#define OTA_TIMEOUT_MS              120000       // idle timeout -> back to broadcast
#define OTA_CONNECT_TIMEOUT_MS      15000
#define OTA_TASK_STACK              8192          // OTA image buffering + TLS-free TCP

// ---------------------------------------------------------------------------
// Task priorities / stacks
// ---------------------------------------------------------------------------
#define TASK_HX711_PRIO             5
#define TASK_BLE_PRIO               4
#define TASK_HX711_STACK            4096
#define TASK_TRANSPORT_STACK        4096

// ---------------------------------------------------------------------------
// Fused scale state published by the HX711 task
// ---------------------------------------------------------------------------
struct ScaleSample {
    float     grams;            // stability-gated, tare-corrected weight
    float     rawGrams;         // un-gated weight (diagnostics)
    float     batteryMv;        // measured supply voltage
    int32_t   rawCounts;        // last raw HX711 count
    int32_t   tareCounts;       // filtered-count tare offset
    uint32_t  seq;              // monotonically increasing sample counter
    uint8_t   batteryPct;       // 0..100, clamped
    bool      tareValid;        // a tare offset is in effect
    bool      autoTared;        // the boot auto tare already happened
    bool      outlierRejected;  // this sample was held back by the gate
    bool      hx711Present;     // HX711 answering
    bool      hx711Waiting;     // no successful sample yet since boot
    bool      overRange;        // |raw| beyond HX711_OVER_RANGE_COUNTS
};

// Implemented in src/main.cpp.
void        statePublish(const ScaleSample& s);
ScaleSample stateRead(void);

// Queues a software tare; executed by the HX711 task, which owns the Kalman
// state. Used by the BLE tare command and by the TTP223 tap.
void        scaleRequestTare(bool automatic);