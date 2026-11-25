#include "hx711_sensor.h"

#include "scale_config.h"

// ---------------------------------------------------------------------------
// Kalman1D
// ---------------------------------------------------------------------------
Kalman1D::Kalman1D(float q, float r, float p0)
    : _q(q), _r(r), _p0(p0), _x(0.0f), _p(p0), _primed(false) {}

void Kalman1D::reset() {
    _x = 0.0f;
    _p = _p0;
    _primed = false;
}

void Kalman1D::seed(float value) {
    _x = value;
    _p = _p0;
    _primed = true;
}

float Kalman1D::update(float z) {
    if (!_primed) {
        seed(z);
        return _x;
    }
    const float pPred = _p + _q;
    const float k     = pPred / (pPred + _r);
    _x += k * (z - _x);
    _p  = (1.0f - k) * pPred;
    return _x;
}

// ---------------------------------------------------------------------------
// Hx711Sensor - 80 SPS bit-banged readout
// ---------------------------------------------------------------------------
Hx711Sensor::Hx711Sensor()
    : _dout(PIN_HX711_DOUT), _sck(PIN_HX711_SCK), _lastRaw(0),
      _filtered(0.0f), _timeouts(0), _consecTimeouts(0),
      _kalman(SCALE_KALMAN_Q, SCALE_KALMAN_R, SCALE_KALMAN_SEED_VARIANCE) {}

bool Hx711Sensor::begin(uint8_t doutPin, uint8_t sckPin, float q, float r) {
    _dout = doutPin;
    _sck  = sckPin;
    pinMode(_dout, INPUT);      // external pull-up on the module
    pinMode(_sck, OUTPUT);
    digitalWrite(_sck, LOW);

    reset();
    powerUp();

    // Probe the load cell: a dead HX711 leaves DOUT high forever.
    int32_t probe = 0;
    bool ok = false;
    for (uint8_t i = 0; i < HX711_PRESENCE_FAIL_COUNT; i++) {
        if (readRaw(probe, HX711_READ_TIMEOUT_MS * 2)) {
            ok = true;
            break;
        }
    }
    if (ok) {
        _kalman.seed((float)probe);
        _filtered = (float)probe;
        _consecTimeouts = 0;
    }
    return ok;
}

void Hx711Sensor::powerUp() {
    digitalWrite(_sck, LOW);
    delay(1);
}

void Hx711Sensor::powerDown() {
    digitalWrite(_sck, LOW);
    delay(1);
}

void Hx711Sensor::reset() {
    // 25 SCK pulses (>= 21) reset/calibrate the internal counter and shift the
    // 80 SPS conversion pipeline back to a known state.
    for (uint8_t i = 0; i < 2; i++) {
        for (uint8_t p = 0; p < HX711_SETTLE_CLOCKS; p++) {
            digitalWrite(_sck, LOW);
            delayMicroseconds(2);
            digitalWrite(_sck, HIGH);
            delayMicroseconds(2);
        }
    }
    digitalWrite(_sck, LOW);
    delay(1);
}

bool Hx711Sensor::ready() const {
    return digitalRead(_dout) == LOW;
}

bool Hx711Sensor::readRaw(int32_t& raw, uint32_t timeoutMs) {
    // Wait for DOUT to fall = a 24-bit result is ready (80 SPS => 12.5 ms).
    const uint32_t start = millis();
    while (digitalRead(_dout) != LOW) {
        if ((uint32_t)(millis() - start) > timeoutMs) {
            _timeouts++;
            _consecTimeouts++;
            return false;
        }
        delayMicroseconds(150);
    }

    // MSB first, 24 bits, three bytes.
    // C-10: Guard the 25-pulse SCK bit-bang loop with a critical section so a BLE or Wi-Fi ISR
    // cannot stall SCK HIGH > 60 us (which forces the HX711 into power-down mode).
    static portMUX_TYPE s_hxMux = portMUX_INITIALIZER_UNLOCKED;
    uint8_t data[3] = {0, 0, 0};
    portENTER_CRITICAL(&s_hxMux);
    for (uint8_t i = 0; i < 3; i++) {
        for (uint8_t b = 0; b < 8; b++) {
            digitalWrite(_sck, LOW);
            delayMicroseconds(1);
            const bool bit = digitalRead(_dout) != LOW;
            digitalWrite(_sck, HIGH);
            delayMicroseconds(1);
            data[i] = (uint8_t)((data[i] << 1) | (bit ? 1 : 0));
        }
    }
    // 25th pulse: starts the next conversion in 80 SPS mode.
    digitalWrite(_sck, LOW);
    delayMicroseconds(1);
    portEXIT_CRITICAL(&s_hxMux);

    int32_t value = ((int32_t)data[0] << 16) | ((int32_t)data[1] << 8) | (int32_t)data[2];
    if (value & 0x00800000) {                 // sign-extend the 24-bit result
        value |= (int32_t)0xFF000000;
    }

    raw = value;
    _lastRaw = value;
    _timeouts = 0;
    _consecTimeouts = 0;
    return true;
}

float Hx711Sensor::filter(int32_t raw) {
    _filtered = _kalman.update((float)raw);
    return _filtered;
}

void Hx711Sensor::reseed(float value) {
    _kalman.seed(value);
    _filtered = value;
}