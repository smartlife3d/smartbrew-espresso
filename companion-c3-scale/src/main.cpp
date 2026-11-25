// main.cpp - companion ESP32-C3 + HX711 espresso scale.
//
// Task layout
//   hx711Task   : bit-banged 80 SPS acquisition -> Kalman -> gate -> state
//                 (sole owner of tare, window and state publication)
//   bleTask     : 20 Hz Bookoo 0xFF11 notify frames, tare requests, LED status
//   displayTask : 10 Hz SSD1306 render loop + TTP223 touch actions
//   logTask     : 2 Hz serial telemetry
//
// All readers sample the same fused snapshot published by hx711Task.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include "battery.h"
#include "bookoo_ble.h"
#include "display_ui.h"
#include "hx711_sensor.h"
#include "scale_config.h"
#include "scale_identity.h"
#include "setup_web.h"
#include "wifi_ota.h"
#include <WiFi.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED;
static ScaleSample  s_state;
static Hx711Sensor  s_sensor;

// Tare requests come from the BLE write handler and are executed by hx711Task.
static volatile bool s_tarePending     = false;
static volatile bool s_tarePendingAuto = false;

// ---------------------------------------------------------------------------
// Stability gate windows (owned by hx711Task)
// ---------------------------------------------------------------------------
static float    s_medianWindow[GATE_MEDIAN_WINDOW] = {0.0f};
static uint8_t  s_medianIndex  = 0;
static uint8_t  s_medianFilled = 0;
static uint8_t  s_rejectStreak = 0;

static float    s_settleWindow[AUTO_TARE_WINDOW];
static uint8_t  s_settleIndex  = 0;
static uint8_t  s_settleFilled = 0;

// ---------------------------------------------------------------------------
// State publish / read (spinlock-guarded snapshot copy)
// ---------------------------------------------------------------------------
void statePublish(const ScaleSample& s) {
    portENTER_CRITICAL(&s_stateMux);
    s_state = s;
    portEXIT_CRITICAL(&s_stateMux);
}

ScaleSample stateRead(void) {
    ScaleSample out;
    portENTER_CRITICAL(&s_stateMux);
    out = s_state;
    portEXIT_CRITICAL(&s_stateMux);
    return out;
}

// ---------------------------------------------------------------------------
// Window helpers
// ---------------------------------------------------------------------------
static float windowMedian() {
    if (s_medianFilled == 0) return 0.0f;

    float sorted[GATE_MEDIAN_WINDOW];
    for (uint8_t i = 0; i < s_medianFilled; i++) {
        sorted[i] = s_medianWindow[i];
    }
    for (uint8_t i = 1; i < s_medianFilled; i++) {
        const float key = sorted[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return sorted[s_medianFilled / 2];
}

static void medianPush(float value) {
    s_medianWindow[s_medianIndex] = value;
    s_medianIndex = (uint8_t)((s_medianIndex + 1) % GATE_MEDIAN_WINDOW);
    if (s_medianFilled < GATE_MEDIAN_WINDOW) s_medianFilled++;
}

static void settlePush(float value) {
    s_settleWindow[s_settleIndex] = value;
    s_settleIndex = (uint8_t)((s_settleIndex + 1) % AUTO_TARE_WINDOW);
    if (s_settleFilled < AUTO_TARE_WINDOW) s_settleFilled++;
}

// True when the last AUTO_TARE_WINDOW readings have settled (spread <= 0.5 g).
static bool readingsSettled() {
    if (s_settleFilled < AUTO_TARE_WINDOW) return false;
    float lo = s_settleWindow[0];
    float hi = s_settleWindow[0];
    for (uint8_t i = 1; i < s_settleFilled; i++) {
        if (s_settleWindow[i] < lo) lo = s_settleWindow[i];
        if (s_settleWindow[i] > hi) hi = s_settleWindow[i];
    }
    return (hi - lo) <= AUTO_TARE_SPREAD_G;
}

static void resetWindows() {
    s_medianIndex = 0;
    s_medianFilled = 0;
    s_settleIndex = 0;
    s_settleFilled = 0;
    s_rejectStreak = 0;
}

// ---------------------------------------------------------------------------
// Tare request (called from the BLE task and the touch UI, executed in hx711Task)
// ---------------------------------------------------------------------------
void scaleRequestTare(bool automatic) {
    portENTER_CRITICAL(&s_stateMux);
    s_tarePending = true;
    s_tarePendingAuto = automatic;
    portEXIT_CRITICAL(&s_stateMux);
}

// ---------------------------------------------------------------------------
// HX711 acquisition task - runs as fast as the 80 SPS hardware allows
// ---------------------------------------------------------------------------
static void hx711Task(void* /*arg*/) {
    const uint32_t bootMs   = millis();
    int32_t  tareCounts     = 0;
    bool     tareValid      = false;
    bool     autoTared      = false;
    bool     everPresent    = false;
    uint32_t seq            = 0;

    for (;;) {
        int32_t raw = 0;
        const bool readOk = s_sensor.readRaw(raw, HX711_READ_TIMEOUT_MS);

        // --- execute any pending tare against the current filtered value ---
        bool doTare = false, doAutoTare = false;
        portENTER_CRITICAL(&s_stateMux);
        if (s_tarePending) {
            doTare = true;
            doAutoTare = s_tarePendingAuto;
            s_tarePending = false;
        }
        portEXIT_CRITICAL(&s_stateMux);

        if (doTare) {
            tareCounts = (int32_t)s_sensor.filtered();
            tareValid  = true;
            if (doAutoTare) autoTared = true;
            resetWindows();
            Serial.printf("[tare] %s at %ld counts\n", doAutoTare ? "auto" : "manual",
                          (long)tareCounts);
        }

        if (!readOk) {
            s_state.hx711Present = everPresent && s_sensor.present();
            s_state.hx711Waiting = !everPresent;
            s_state.outlierRejected = true;
            statePublish(s_state);
            if (everPresent && !s_sensor.present()) {
                resetWindows();     // re-arm the gate once the cell recovers
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        everPresent = true;

        // 1) Kalman-filter the RAW COUNTS, then convert to grams.
        const float filtered = s_sensor.filter(raw);
        s_state.rawCounts    = raw;
        s_state.hx711Present = true;
        s_state.hx711Waiting = false;
        s_state.overRange    = Hx711Sensor::isOverRange(raw);

        float grams = s_sensor.toGrams(filtered - (float)tareCounts);
        if (s_state.overRange) {
            grams = s_state.grams;        // hold the last sane value
        }

        s_state.tareCounts  = tareCounts;
        s_state.tareValid   = tareValid;
        s_state.autoTared   = autoTared;
        s_state.batteryMv   = batteryMv();
        s_state.batteryPct  = batteryPercent();
        s_state.seq         = seq++;

        // 2) Stability gate: hold updates while a reading is a vibration spike.
        const bool haveMedian = (s_medianFilled >= 3);
        const bool isOutlier  = haveMedian &&
                                (fabsf(grams - windowMedian()) > GATE_OUTLIER_G);

        if (isOutlier && s_rejectStreak < GATE_MAX_REJECTS) {
            s_rejectStreak++;
            s_state.outlierRejected = true;   // grams keeps its previous value
            statePublish(s_state);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        s_rejectStreak = 0;
        s_state.outlierRejected = false;
        s_state.rawGrams = grams;
        s_state.grams     = grams;
        medianPush(grams);
        settlePush(grams);

        // 3) Auto tare once, after 2.0 s of settled readings.
        if (!autoTared && !tareValid &&
            (uint32_t)(millis() - bootMs) >= AUTO_TARE_DELAY_MS && readingsSettled()) {
            tareCounts = (int32_t)s_sensor.filtered();
            tareValid  = true;
            autoTared  = true;
            resetWindows();
            s_state.tareCounts = tareCounts;
            s_state.tareValid  = true;
            s_state.autoTared  = true;
            s_state.grams      = 0.0f;
            s_state.rawGrams   = 0.0f;
            Serial.printf("[tare] auto at %ld counts\n", (long)tareCounts);
        }

        statePublish(s_state);

        // readRaw() already blocks on DOUT; this only yields between samples.
        taskYIELD();
    }
}

// ---------------------------------------------------------------------------
// LED status: solid = connected/notifying, slow blink = waiting for HX711,
// fast blink = fault (HX711 lost or over-range)
// ---------------------------------------------------------------------------
static void ledUpdate(bool connected, bool present, bool waiting, bool overRange) {
    static uint32_t lastToggle = 0;
    static bool     level      = false;

    const bool fault  = overRange || (present == false && waiting == false);
    const bool slowBl = (waiting && !overRange);

    if (fault) {
        if ((uint32_t)(millis() - lastToggle) >= LED_FAULT_BLINK_MS) {
            lastToggle = millis();
            level = !level;
            digitalWrite(PIN_LED, level ? HIGH : LOW);
        }
    } else if (slowBl) {
        if ((uint32_t)(millis() - lastToggle) >= LED_WAIT_BLINK_MS) {
            lastToggle = millis();
            level = !level;
            digitalWrite(PIN_LED, level ? HIGH : LOW);
        }
    } else {
        digitalWrite(PIN_LED, LED_SOLID ? HIGH : LOW);
        lastToggle = millis();
    }
    (void)connected;   // a connected central keeps the LED solid
}

// ---------------------------------------------------------------------------
// BLE task - 20 Hz Bookoo notifications + tare requests
// ---------------------------------------------------------------------------
static void bleTask(void* /*arg*/) {
    const TickType_t period = pdMS_TO_TICKS(1000 / BOOKOO_RATE_HZ);
    for (;;) {
        if (bookooTakeTareRequest()) {
            Serial.println(F("[ble] tare command -> software tare"));
            scaleRequestTare(false);
        }

        batteryUpdate();
        const ScaleSample s = stateRead();
        bookooTick(s);
        ledUpdate(bookooIsConnected(), s.hx711Present, s.hx711Waiting, s.overRange);

        vTaskDelay(period);
    }
}

// ---------------------------------------------------------------------------
// Serial console (115200 baud)
//   v                 firmware version / build
//   status            one-line health snapshot
//   name [text|reset] per-unit advertised BLE name
//   wifi [ssid pass]  store (or show) the OTA access point
//   wifi clear        forget the OTA access point
//   ota               enter update mode: join the AP, serve :3232
//   ap                enter setup portal (SoftAP + web page + browser OTA)
//   ap exit           leave the portal and resume advertising
// ---------------------------------------------------------------------------
static void printVersion(void) {
    Serial.printf("[fw] %s  built %s  id=%s  name=\"%s\"\n",
                  SCALE_FW_VERSION, SCALE_FW_BUILD, scaleIdentityShortId(),
                  scaleDeviceName());
}

static void printStatus(void) {
    const ScaleSample s = stateRead();
    Serial.printf("[status] fw=%s id=%s name=\"%s\" w=%.2fg batt=%u%% hx=%s gate=%s\n",
                  SCALE_FW_VERSION, scaleIdentityShortId(), scaleDeviceName(),
                  (double)s.grams, (unsigned)s.batteryPct,
                  s.hx711Present ? "up" : "down",
                  s.outlierRejected ? "hold" : "ok");
    Serial.printf("[status] ota=%s (%s)  ble=%s notifies=%lu  ip=%s\n",
                  wifiOtaStateName(), wifiOtaStatus(),
                  bookooIsConnected() ? "connected" : "advertising",
                  (unsigned long)bookooNotifyCount(),
                  WiFi.SSID().c_str());
    if (setupWebActive()) {
        Serial.printf("[status] setup portal: ssid=\"%s\" password=\"%s\" clients=%u\n",
                      setupWebApSsid(), SETUP_AP_PASSWORD, (unsigned)setupWebClientCount());
    }
}

static void runCommand(const char* line) {
    if (strcmp(line, "v") == 0 || strcmp(line, "version") == 0) {
        printVersion();
    } else if (strcmp(line, "status") == 0) {
        printStatus();
    } else if (strcmp(line, "help") == 0) {
        Serial.println(F("[cmd] v | status | name [text|reset] | wifi [ssid pass] | "
                         "wifi clear | ota | ap [exit]"));
    } else if (strncmp(line, "wifi clear", 10) == 0) {
        wifiOtaClearCredentials();
    } else if (strncmp(line, "wifi ", 5) == 0) {
        // "wifi <ssid> <password>" - split on the first space after the ssid.
        const char* ssid = line + 5;
        while (*ssid == ' ') ssid++;
        const char* pass = strchr(ssid, ' ');
        char ssidBuf[33];
        const size_t ssidLen = pass ? (size_t)(pass - ssid) : strlen(ssid);
        if (ssidLen == 0 || ssidLen >= sizeof(ssidBuf)) {
            Serial.println(F("[cmd] ERROR: usage: wifi <ssid> [password]"));
            return;
        }
        memcpy(ssidBuf, ssid, ssidLen);
        ssidBuf[ssidLen] = '\0';
        while (pass && *pass == ' ') pass++;
        if (wifiOtaSetCredentials(ssidBuf, pass)) {
            Serial.println(F("[cmd] OK - now type: ota"));
        }
    } else if (strcmp(line, "wifi") == 0) {
        if (wifiOtaHasCredentials()) {
            Serial.printf("[wifi] OTA AP configured (enter `ota` to join)\n");
        } else {
            Serial.println(F("[wifi] no AP stored - usage: wifi <ssid> [password]"));
        }
    } else if (strcmp(line, "ota") == 0) {
        wifiOtaRequest();
    } else if (strcmp(line, "ap") == 0) {
        setupWebEnter();
    } else if (strcmp(line, "ap exit") == 0) {
        setupWebLeave(false);
    } else if (strncmp(line, "name reset", 10) == 0) {
        scaleIdentityClearName();
    } else if (strncmp(line, "name ", 5) == 0) {
        const char* wanted = line + 5;
        if (scaleIdentitySetName(wanted)) {
            Serial.printf("[cmd] OK, advertised as \"%s\" after reboot\n", wanted);
        } else {
            Serial.println(F("[cmd] ERROR: name must be 1-28 printable ASCII chars"));
        }
    } else if (strcmp(line, "name") == 0) {
        Serial.printf("[cmd] name=\"%s\" unit=%s\n",
                      scaleDeviceName(), scaleIdentityShortId());
    } else {
        Serial.printf("[cmd] unknown command \"%s\" (type: help)\n", line);
    }
}

static void pollConsole(uint32_t now) {
    static char     line[80];
    static uint8_t  len = 0;
    static uint32_t lastRx = 0;

    while (Serial.available() > 0) {
        const char c = (char)Serial.read();
        lastRx = now;

        if (c == '\r' || c == '\n') {
            if (len == 0) continue;
            line[len] = '\0';
            len = 0;
            runCommand(line);
            continue;
        }

        if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }

    // Nudge the monitor: a bare CR often never arrives from some terminals.
    if (len > 0 && (uint32_t)(now - lastRx) > 2000) {
        len = 0;
    }
}

// ---------------------------------------------------------------------------
// Serial telemetry task
// ---------------------------------------------------------------------------
static void logTask(void* /*arg*/) {
    for (;;) {
        const uint32_t now = millis();
        pollConsole(now);

        const ScaleSample s = stateRead();
        Serial.printf("[scale] %8.2f g  raw=%8ld  batt=%3u%% (%4lumV)  gate=%-4s hx=%-3s "
                      "ble=%-3s notif=%lu  id=%s\n",
                      (double)s.grams, (long)s.rawCounts, (unsigned)s.batteryPct,
                      (unsigned long)s.batteryMv,
                      s.outlierRejected ? "hold" : "ok",
                      s.hx711Present ? "up" : "down",
                      bookooIsConnected() ? "conn" : "adv",
                      (unsigned long)bookooNotifyCount(),
                      scaleIdentityShortId());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    memset(&s_state, 0, sizeof(s_state));
    s_state.grams = SCALE_DEFAULT_DISPLAY_G;
    s_state.hx711Waiting = true;

    batteryBegin(PIN_BATTERY_ADC);
    batteryUpdate();

    Serial.printf("\n[boot] companion ESP32-C3 scale: %u SPS, Kalman Q=%g R=%g\n",
                  (unsigned)HX711_SPS, (double)SCALE_KALMAN_Q, (double)SCALE_KALMAN_R);

    const bool sensorOk = s_sensor.begin(PIN_HX711_DOUT, PIN_HX711_SCK,
                                         SCALE_KALMAN_Q, SCALE_KALMAN_R);
    s_state.hx711Present = sensorOk;
    s_state.hx711Waiting = !sensorOk;
    Serial.println(sensorOk ? "[boot] HX711 responding"
                            : "[boot] HX711 MISSING - check DOUT/SCK wiring");

    printVersion();
    scaleIdentityInit();
    wifiOtaInit();
    setupWebInit();          // may claim the radio for the setup portal
    bookooInit();
    displayUiInit();

    xTaskCreatePinnedToCore(hx711Task,   "hx711",  TASK_HX711_STACK,     nullptr, TASK_HX711_PRIO,  nullptr, 0);
    xTaskCreatePinnedToCore(bleTask,     "ble",    TASK_TRANSPORT_STACK, nullptr, TASK_BLE_PRIO,    nullptr, 0);
    xTaskCreatePinnedToCore(displayTask, "display", TASK_TRANSPORT_STACK, nullptr, TASK_BLE_PRIO,  nullptr, 0);
    xTaskCreatePinnedToCore(logTask,     "log",    3072,                 nullptr, 1,                nullptr, 0);
}

void loop() {
    // All work happens in the tasks above; keep the Arduino task alive.
    vTaskDelay(pdMS_TO_TICKS(1000));
}