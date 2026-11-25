// hx711_sensor.h - bit-banged HX711 driver (no external library) plus the fast
// 1D Kalman filter applied to the raw counts.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "scale_config.h"

// Fast 1D Kalman filter: x' = x + K(z - x), P' = (1-K)(P+Q), K = (P+Q)/(P+Q+R).
class Kalman1D {
public:
    Kalman1D(float q, float r, float p0);

    void  reset();              // drop the estimate (next update re-seeds)
    void  seed(float value);    // re-seed with a known value (tare / boot)
    float update(float z);      // feed a measurement, returns the estimate
    float value() const { return _x; }
    float variance() const { return _p; }
    bool  primed() const { return _primed; }

private:
    float _q, _r, _p0;
    float _x, _p;
    bool  _primed;
};

class Hx711Sensor {
public:
    Hx711Sensor();

    // Configures the pins, verifies the load cell answers and primes the filter.
    bool begin(uint8_t doutPin, uint8_t sckPin, float q, float r);

    bool     ready() const;                                  // DOUT low == data ready
    bool     readRaw(int32_t& raw, uint32_t timeoutMs);      // bit-banged 24-bit read
    void     powerUp();
    void     powerDown();
    void     reset();                                        // 25+ SCK pulses

    // Feeds a raw count through the Kalman filter, returns the filtered count.
    float    filter(int32_t raw);
    void     reseed(float value);
    float    filtered() const { return _filtered; }
    float    variance() const { return _kalman.variance(); }

    int32_t  lastRaw() const { return _lastRaw; }
    uint32_t timeouts() const { return _timeouts; }
    uint32_t consecutiveTimeouts() const { return _consecTimeouts; }
    bool     present() const { return _consecTimeouts < (uint32_t)HX711_PRESENCE_FAIL_COUNT; }
    static bool isOverRange(int32_t raw) {
        return raw > HX711_OVER_RANGE_COUNTS || raw < -HX711_OVER_RANGE_COUNTS;
    }

    // Converts a filtered count to grams.
    float    toGrams(float filteredCounts) const { return filteredCounts / SCALE_CAL_COUNTS; }

private:
    uint8_t  _dout, _sck;
    int32_t  _lastRaw;
    float    _filtered;
    uint32_t _timeouts;
    uint32_t _consecTimeouts;
    Kalman1D _kalman;
};