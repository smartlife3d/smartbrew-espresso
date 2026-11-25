// =================================================================================================
//  sim_ble_scale.h - Bookoo Themis BLE Scale Emulator
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>

class SimBleScale {
public:
    void begin(const char* deviceName = "Bookoo_SIM");

    // Update weight and send notifications if client subscribed (called from loop)
    void update();

    // Push the current weight immediately instead of waiting for the next 33 Hz tick. Used when
    // the cup is emptied so the controller sees the 0 g value without delay.
    void notifyNow();

    // Set live weight in grams (e.g. from hydraulic physics engine)
    void setWeight(float grams);
    float weightGrams() const { return _weightGrams.load(std::memory_order_relaxed); }

    // Tare the scale (zero out weight)
    void tare();

    // Status
    bool isConnected() const;
    bool isSubscribed() const;
    uint32_t notifyCount() const;
    uint32_t tareCount() const;

private:
    std::atomic<float>    _weightGrams{0.0f};
    uint32_t              _lastNotifyMs = 0;
};

extern SimBleScale g_simBleScale;

