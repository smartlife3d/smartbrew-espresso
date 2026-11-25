// display_ui.h - 128x32 OLED front end (weight / flow / battery) plus the
// TTP223 touch actions.
//
// Layout (128 x 32, 4 SSD1306 pages):
//
//   y 0..13   [ 12.34 ] [86] [battery icon]   <- weight (2x), state of charge
//   y 17..23  F  3.2 g/s                      <- flow rate, 1 s OLS window
//   y 25..31  TARE / HOLD / HX ERR / OFF      <- status line
//
// Touch:  tap  -> software tare
//         hold -> display power on/off (panel sleep, radios stay alive)

#pragma once

#include <Arduino.h>

// Brings up the SSD1306 and the TTP223 input.
bool displayUiInit(void);

// True when the OLED answered on the I2C bus.
bool displayPresent(void);

// True when the panel is switched off by a long press.
bool displayPowered(void);

// FreeRTOS task entry (10 Hz render loop).
void displayTask(void* arg);