#include "battery.h"

#include "scale_config.h"

static uint8_t  s_pin         = PIN_BATTERY_ADC;
static uint32_t s_lastMv      = 0;
static uint8_t  s_lastPct     = 0;
static uint32_t s_lastSample  = 0;
static bool     s_haveSample  = false;

// Piecewise-linear CR2032/CR123A discharge curve (loaded cell, ~150 mA load).
// [0] = millivolts, [1] = percent.
static const uint32_t kCurveMv[]   = {2700, 2800, 2900, 3000, 3100, 3200, 3300, 3400, 3600};
static const uint8_t  kCurvePct[]  = {  0,    3,   10,   20,   40,   60,   80,   92,  100};
static const size_t   kCurvePoints = sizeof(kCurveMv) / sizeof(kCurveMv[0]);

void batteryBegin(uint8_t adcPin) {
    s_pin = adcPin;
    analogSetPinAttenuation(s_pin, BATTERY_ADC_ATTEN);
    analogReadResolution(12);
    s_haveSample = false;
    s_lastSample = 0;
}

uint32_t batteryReadMv(void) {
    // Average a few reads to keep the ADC noise off the low-current divider.
    uint32_t acc = 0;
    const uint8_t samples = 8;
    for (uint8_t i = 0; i < samples; i++) {
        acc += (uint32_t)analogReadMilliVolts(s_pin);
        delay(1);
    }
    const float pinMv = (float)acc / (float)samples;

    // Reconstruct the supply from the divider ratio, then trim the known
    // ESP32-C3 ADC attenuation error.
    const float ratio = (BATTERY_DIVIDER_TOP_OHMS + BATTERY_DIVIDER_BOTTOM_OHMS) /
                        BATTERY_DIVIDER_BOTTOM_OHMS;
    const float supplyMv = pinMv * ratio * BATTERY_DIVIDER_TRIM;

    if (supplyMv < 0.0f) return 0;
    return (uint32_t)supplyMv;
}

uint8_t batteryPercentFromMv(uint32_t mv) {
    if (mv <= kCurveMv[0]) return kCurvePct[0];
    if (mv >= kCurveMv[kCurvePoints - 1]) return kCurvePct[kCurvePoints - 1];

    for (size_t i = 1; i < kCurvePoints; i++) {
        if (mv <= kCurveMv[i]) {
            const uint32_t spanMv = kCurveMv[i] - kCurveMv[i - 1];
            const uint32_t intoMv = mv - kCurveMv[i - 1];
            const uint32_t pctSpan = (uint32_t)(kCurvePct[i] - kCurvePct[i - 1]);
            uint32_t pct = kCurvePct[i - 1] +
                           (pctSpan * intoMv + spanMv / 2) / (spanMv ? spanMv : 1);
            if (pct > 100) pct = 100;
            return (uint8_t)pct;
        }
    }
    return kCurvePct[kCurvePoints - 1];
}

void batteryUpdate(void) {
    const uint32_t now = millis();
    if (s_haveSample && (uint32_t)(now - s_lastSample) < BATTERY_SAMPLE_INTERVAL_MS) {
        return;
    }
    s_lastSample = now;
    s_lastMv = batteryReadMv();
    s_lastPct = batteryPercentFromMv(s_lastMv);
    s_haveSample = true;
}

uint32_t batteryMv(void) { return s_lastMv; }
uint8_t  batteryPercent(void) { return s_lastPct; }