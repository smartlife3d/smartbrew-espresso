// =================================================================================================
//  pins.h - ESP32-S3 GPIO map for the Universal Espresso Profiling Controller
//  docs/Hardware.md
//
//  EVERY pin below is overridable at build time, e.g.:
//      pio run --target upload --project-option="build_flags=-DPIN_ZC_IN=10"
//  or by editing the values here. Nothing else in the firmware hard-codes a GPIO.
// =================================================================================================
#pragma once
// Set by tools/gen_build_info.py (the git commit); a fallback for builds outside PlatformIO.
#ifndef SB_BUILD_REV
#define SB_BUILD_REV "unknown"
#endif

#include <Arduino.h>

// -------------------------------------------------------------------------------------------------
// Zero-cross / PSM power stage (RobotDyn AC Dimmer)
// -------------------------------------------------------------------------------------------------
#ifndef PIN_ZC_IN
#define PIN_ZC_IN 4          // RobotDyn ZC output -> RISING edge interrupt (true AC state detect)
#endif
#ifndef PIN_PSM_GATE
#define PIN_PSM_GATE 5       // RobotDyn PSM/DIM input -> Active-HIGH TRIAC gate drive
#endif

// -------------------------------------------------------------------------------------------------
// Virtual machine brew button (PhotoMOS relay across the machine's momentary button)
// -------------------------------------------------------------------------------------------------
#ifndef PIN_BTN_TRIG
#define PIN_BTN_TRIG 6       // -> 330R -> U3 pin1 (Anode); pin2 (Cathode) -> GND
#endif
#ifndef BTN_PULSE_MS
#define BTN_PULSE_MS 180     // docs/Architecture.md: 180 ms momentary press
#endif

// -------------------------------------------------------------------------------------------------
// Pressure acquisition
// -------------------------------------------------------------------------------------------------
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 8        // ADS1115 SDA
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 9        // ADS1115 SCL
#endif
#ifndef PIN_ADC_FALLBACK
#define PIN_ADC_FALLBACK 1   // Divided pressure node -> internal ADC1_CH0
#endif

// -------------------------------------------------------------------------------------------------
// Auxiliary external-display bridge (UART1, NDJSON both directions)
// -------------------------------------------------------------------------------------------------
#ifndef PIN_AUX_TX
#define PIN_AUX_TX 17        // MCU TX -> external display RX
#endif
#ifndef PIN_AUX_RX
#define PIN_AUX_RX 18        // external display TX -> MCU RX
#endif
#ifndef AUX_UART_NUM
#define AUX_UART_NUM 1
#endif
#ifndef AUX_UART_BAUD
#define AUX_UART_BAUD 115200
#endif

// -------------------------------------------------------------------------------------------------
// Onboard status LED (WS2812 on most S3 dev boards; plain GPIO on clones)
// -------------------------------------------------------------------------------------------------
#ifndef PIN_RGB_LED
#define PIN_RGB_LED 48
#endif

// -------------------------------------------------------------------------------------------------
// Bench notes
//
// There is no separate "ZC mirror" output pin: GPIO 4 (PIN_ZC_IN) already carries the raw zero-cross
// signal, so a scope probe on GPIO 4 shows exactly the pulse train the firmware sees. To watch it on
// a second channel, use a scope maths function or a logic analyser - an output mirror would add an
// ISR write per edge for no extra information.
//
// -------------------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------------------
// Timing constants shared by the HAL
// -------------------------------------------------------------------------------------------------
#ifndef ZC_GLITCH_US
#define ZC_GLITCH_US 6500UL     // docs/Architecture.md: ignore edges closer than 6.5 ms
#endif
#ifndef ZC_MISSING_TIMEOUT_US
#define ZC_MISSING_TIMEOUT_US 35000UL  // docs/Architecture.md: no pulse for 35 ms -> ac_power_present = false
#endif
#ifndef ZC_BURST_PULSES
#define ZC_BURST_PULSES 4       // docs/Architecture.md: 4 consecutive valid pulses within 45 ms -> BREWING
#endif
#ifndef ZC_BURST_WINDOW_US
#define ZC_BURST_WINDOW_US 45000UL
#endif

// In-RAM telemetry sample rate (Core 1 control loop)
#ifndef CONTROL_HZ
#define CONTROL_HZ 100
#endif
#ifndef WS_HZ_BREW
#define WS_HZ_BREW 20
#endif
#ifndef WS_HZ_IDLE
#define WS_HZ_IDLE 2
#endif
// The rate used while the post-shot weight is still settling. The last of the coffee is still
// arriving in the cup then, and the idle rate of 2 Hz is too coarse to follow it.
#ifndef WS_HZ_SETTLE
#define WS_HZ_SETTLE 10
#endif