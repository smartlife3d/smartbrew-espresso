// =================================================================================================
//  sim_pressure.h - Simulated Pressure Transducer Output (PWM -> RC filter -> controller ADC)
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <atomic>
#include "sim_pins.h"

class SimPressure {
public:
    void begin();

    // Set current simulated pressure in bar
    void setBar(float bar);
    float currentBar() const { return _currentBar.load(std::memory_order_relaxed); }
    float currentMv() const { return _currentMv.load(std::memory_order_relaxed); }

    // ---- Manual override (locks the pressure at a fixed value) ---------------------------------
    // Unlike the hydraulic model, the override is allowed to exceed the transducer's nominal full
    // scale: that is how the controller's over-range protection (>3050 mV) is exercised.
    void setManualOverride(bool enable, float bar = 0.0f);
    bool isManualOverride() const { return _manualOverride; }
    float manualOverrideBar() const { return _overrideBar; }

    // ---- Raw node voltage injection (fault simulation) -----------------------------------------
    // Drives the ADC node with an arbitrary millivolt value, bypassing the bar <-> mV mapping.
    // Used to exercise the controller's open-wire (<minSensorMv) and over-range (>3050 mV)
    // detection, neither of which a healthy 0.5-4.5 V transducer can ever produce.
    void setRawMvOverride(bool enable, float mv = 0.0f);
    bool isRawMvOverride() const { return _rawMvOverride; }

    // Update hardware outputs (called from the main loop)
    void update();

    // ---- Transducer full scale ----------------------------------------------------------------
    // The simulated transducer drives the controller's ADC node directly (i.e. it already
    // includes the 10k/20k divider that sits in front of the real transducer). The controller
    // converts that node voltage back to bar with
    //
    //     bar = (mV - sensorZeroMv) / (sensorFsMv - sensorZeroMv) * FS_bar
    //
    // so the simulator has to use the SAME full scale the controller is configured for, otherwise
    // the controller reads every pressure wrong. The controller defaults to 1.2 MPa (12 bar); set
    // 16 here when the controller is configured for 1.6 MPa (the physical 0-16 bar transducer).
    void setFullScaleBar(float bar);
    float fullScaleBar() const { return _fullScaleBar; }

    // ---- Node voltage endpoints (identical to the controller's config defaults) ----------------
    static constexpr float NODE_ZERO_MV = 333.33f;   // 0 bar    (500 mV transducer / 1.5)
    static constexpr float NODE_FS_MV   = 3000.0f;   // full scale (4500 mV transducer / 1.5)
    static constexpr float DEFAULT_FS_BAR = 12.0f;   // controller default: SENSOR_1_2MPA

private:
    // Drive the PWM with a given node voltage. The duty saturates at the 3.3 V rail.
    void emitMv(float mv);

    std::atomic<float> _currentBar{0.0f};
    std::atomic<float> _currentMv{NODE_ZERO_MV};

    bool  _manualOverride = false;
    float _overrideBar    = 0.0f;

    bool  _rawMvOverride  = false;
    float _rawMv          = 0.0f;

    float _fullScaleBar = DEFAULT_FS_BAR;

    // LEDC PWM configuration for the filtered analog voltage
    static constexpr uint8_t  PWM_CHANNEL    = 0;
    static constexpr uint32_t PWM_FREQ_HZ    = 25000; // requested; the LEDC divider lands nearer 19 kHz
    static constexpr uint8_t  PWM_RESOLUTION = 10;    // 10-bit (0..1023)
    static constexpr float    PWM_RAIL_MV    = 3300.0f;
};

extern SimPressure g_simPressure;
