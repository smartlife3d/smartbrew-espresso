#include "display_ui.h"

#include <stdio.h>
#include <string.h>

#include "scale_config.h"
#include "ssd1306.h"
#include "setup_web.h"
#include "wifi_ota.h"
#include "touch_button.h"

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static Ssd1306     s_oled;
static TouchButton s_touch;

static bool     s_present     = false;
static bool     s_powered      = true;
static uint32_t s_touchEventAt = 0;
static const char* s_touchMsg   = "";

// Flow-rate estimation: ordinary least squares over (t, w) of the last second.
static const uint8_t FLOW_WINDOW = 10;
static uint32_t s_flowMs[FLOW_WINDOW];
static float    s_flowG[FLOW_WINDOW];
static uint8_t  s_flowIndex = 0;
static uint8_t  s_flowCount = 0;
static float    s_flowGs    = 0.0f;

static float flowRateGps(uint32_t nowMs, float grams) {
    s_flowMs[s_flowIndex] = nowMs;
    s_flowG[s_flowIndex]  = grams;
    s_flowIndex = (uint8_t)((s_flowIndex + 1) % FLOW_WINDOW);
    if (s_flowCount < FLOW_WINDOW) s_flowCount++;

    if (s_flowCount < 3) {
        s_flowGs = 0.0f;
        return 0.0f;
    }

    // Use the newest samples only; the ring is already in order if we walk it
    // backwards from the last written index.
    double sumT = 0.0, sumW = 0.0, sumTT = 0.0, sumTW = 0.0;
    for (uint8_t i = 0; i < s_flowCount; i++) {
        const uint8_t idx = (uint8_t)((s_flowIndex + FLOW_WINDOW - 1 - i) % FLOW_WINDOW);
        const double t = (double)(s_flowMs[idx] - nowMs) / 1000.0;   // <= 0
        const double w = s_flowG[idx];
        sumT += t;
        sumW += w;
        sumTT += t * t;
        sumTW += t * w;
    }
    const double n = (double)s_flowCount;
    const double denom = (n * sumTT) - (sumT * sumT);
    if (denom <= 1e-9) {
        s_flowGs = 0.0f;
        return 0.0f;
    }
    const double slope = ((n * sumTW) - (sumT * sumW)) / denom;   // g/s
    s_flowGs = (float)slope;
    return s_flowGs;
}

// ---------------------------------------------------------------------------
// Status text
// ---------------------------------------------------------------------------
static const char* statusText(const ScaleSample& s, bool displayOn) {
    if (!displayOn) return "OFF";
    if (setupWebActive()) return "SETUP AP";                  // setup portal is up
    if (wifiOtaState() != OtaState::Idle) return "OTA MODE";  // update mode is up
    if (s.overRange) return "RANGE";
    if (!s.hx711Present && !s.hx711Waiting) return "HX ERR";
    if (s.hx711Waiting) return "WAIT HX";
    if (s.outlierRejected) return "HOLD";
    return "TARE";
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void render(const ScaleSample& s, uint32_t nowMs) {
    s_oled.clear();

    if (!s_powered) {
        s_oled.drawText(2, 13, "OFF", 1, true);
        s_oled.flush();
        return;
    }

    // --- weight (2x glyphs) ---
    s_oled.drawNumberRight(74, 1, s.grams, 2, 2, true);
    s_oled.drawText(78, 1, "g", 1, false);

    // --- battery: percent + icon ---
    char pctText[4];
    snprintf(pctText, sizeof(pctText), "%u", (unsigned)s.batteryPct);
    s_oled.drawTextRight(106, 1, pctText, 1, false);
    s_oled.drawBattery(110, 1, s.batteryPct);

    // --- flow rate ---
    const float flow = (s.grams > 0.5f && s_flowGs > 0.02f) ? s_flowGs : 0.0f;
    char flowText[16];
    snprintf(flowText, sizeof(flowText), "F %5.2fg/s", (double)flow);
    s_oled.drawText(0, 17, flowText, 1, false);

    // --- status line: touch feedback overrides the sensor status for 1 s ---
    const char* status = s_touchMsg;
    if (status[0] == '\0' || (uint32_t)(nowMs - s_touchEventAt) > 1000) {
        status = statusText(s, s_powered);
    }
    s_oled.drawText(0, 25, status, 1, false);

    s_oled.flush();
}

// ---------------------------------------------------------------------------
// Touch actions
// ---------------------------------------------------------------------------
static void handleTouch(TouchEvent event) {
    switch (event) {
        case TouchEvent::Tap:
            scaleRequestTare(false);
            s_touchMsg = "TARE";
            s_touchEventAt = millis();
            Serial.println(F("[ui] touch tap -> tare"));
            break;

        case TouchEvent::Hold:
            s_powered = !s_powered;
            s_oled.displayOn(s_powered);
            s_touchMsg = s_powered ? "ON" : "OFF";
            s_touchEventAt = millis();
            Serial.printf("[ui] touch hold -> display %s\n", s_powered ? "on" : "off");
            break;

        case TouchEvent::None:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool displayUiInit(void) {
    s_touch.begin(PIN_TOUCH_OUT, TOUCH_ACTIVE_HIGH);
    s_present = s_oled.begin(PIN_I2C_SDA, PIN_I2C_SCL, SSD1306_CONTRAST);
    if (!s_present) {
        Serial.println(F("[ui] OLED not found at 0x3C/0x3D - continuing headless"));
    }
    return s_present;
}

bool displayPresent(void) { return s_present; }
bool displayPowered(void) { return s_powered; }

void displayTask(void* /*arg*/) {
    const TickType_t period = pdMS_TO_TICKS(1000 / DISPLAY_RATE_HZ);

    for (;;) {
        const uint32_t now = millis();

        handleTouch(s_touch.poll(now));

        const ScaleSample s = stateRead();
        flowRateGps(now, s.grams);
        render(s, now);

        vTaskDelay(period);
    }
}