// battery.h - CR2032 / CR123A Li-coin state-of-charge estimate.

#pragma once

#include <Arduino.h>
#include <stdint.h>

// Configures the ADC pin, divider and sample cadence.
void     batteryBegin(uint8_t adcPin);

// Re-samples the divider if the interval has elapsed; call from any task.
void     batteryUpdate(void);

// Forces an immediate ADC read (blocking, ~10 ms).
uint32_t batteryReadMv(void);

// Piecewise-linear Li-coin discharge curve, clamped to 0..100.
uint8_t  batteryPercentFromMv(uint32_t mv);

// Cached values from the last batteryUpdate().
uint32_t batteryMv(void);
uint8_t  batteryPercent(void);