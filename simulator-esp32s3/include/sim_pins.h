// =================================================================================================
//  sim_pins.h - Pin definitions for the ESP32-S3 HIL Simulator
//
//  Direct 1:1 matching with the Controller ESP32-S3:
//
//    Controller ESP32-S3 Pin       <--- Direction --->   Simulator ESP32-S3 Pin
//    ---------------------------                         ---------------------------
//    GPIO 4 (PIN_ZC_IN)            <--- [ZC Pulses] --   GPIO 4 (PIN_SIM_ZC_OUT)
//    GPIO 5 (PIN_PSM_GATE)         --- [Gate Drive] ->   GPIO 5 (PIN_SIM_GATE_IN)
//    GPIO 6 (PIN_BTN_TRIG)         --- [Relay Pulse]->   GPIO 6 (PIN_SIM_BTN_IN)
//    GPIO 1 (PIN_ADC_FALLBACK)     <--- [Analog mV] --   GPIO 7 (PIN_SIM_PRESSURE_PWM) + RC filter
//    GND                           <==================>  GND  (COMMON GROUND REQUIRED!)
//
//  Local peripherals on the Simulator:
//    GPIO 0  : Onboard BOOT button (manual barista front-panel brew button)
//    GPIO 48 : Onboard WS2812 RGB LED (status indication)
//
//  NOTE ON I2C / ADS1115:
//  The simulator deliberately does NOT implement an ADS1115. The controller probes the I2C bus at
//  boot and falls back to its internal ADC1 when no ADS1115 answers - which is exactly the path
//  the simulator exercises. If a real ADS1115 is still fitted on the controller while the simulator
//  is connected, the controller would keep using the (unconnected) ADS1115 input and read 0 bar.
//  Either unplug the ADS1115 or set the controller's pressure source to "force ADC1"
//  (config.json: "useAds1115": 2) for simulator runs.
// =================================================================================================
#pragma once

#include <Arduino.h>

#ifndef PIN_SIM_ZC_OUT
#define PIN_SIM_ZC_OUT          4   // Output: 100Hz/120Hz Zero-Cross pulses to Controller GPIO 4
#endif

#ifndef PIN_SIM_GATE_IN
#define PIN_SIM_GATE_IN         5   // Input: PSM Gate drive from Controller GPIO 5
#endif

#ifndef PIN_SIM_BTN_IN
#define PIN_SIM_BTN_IN          6   // Input: Virtual brew button pulse (180ms) from Controller GPIO 6
#endif

#ifndef PIN_SIM_PRESSURE_PWM
#define PIN_SIM_PRESSURE_PWM    7   // Output: 25 kHz PWM request (LEDC divider -> 19.5 kHz) -> RC filter -> Controller GPIO 1
#endif

#ifndef PIN_SIM_BARISTA_BTN
#define PIN_SIM_BARISTA_BTN     0   // Input: Onboard BOOT button (press to simulate barista button)
#endif

#ifndef PIN_SIM_STATUS_LED
#define PIN_SIM_STATUS_LED      48  // WS2812 RGB LED (blue=booting, green=idle, cyan=brewing)
#endif
