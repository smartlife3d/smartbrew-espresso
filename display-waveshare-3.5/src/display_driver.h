#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "esp_lcd_touch_axs15231b.h"
#include <lvgl.h>
#include "pin_config.h"

extern Arduino_DataBus *bus;
extern Arduino_GFX *gfx;

/**
 * Timestamp (millis()) of the last touch the driver actually saw.
 *
 * The screen saver needs a reliable "the user is here" signal.  LVGL 8 has no
 * public accessor for the state of an input device, so the timestamp is taken
 * right where the panel is read - that is the only place that knows for sure
 * whether a finger is on the glass.
 */
extern volatile uint32_t g_last_touch_ms;

/**
 * True while a finger is on the glass.
 *
 * The screen saver must not take over in the middle of a gesture, otherwise
 * the cockpit underneath would receive the release of a touch that started
 * before the screen saver appeared.
 */
extern volatile bool g_touch_down;

void initDisplayAndTouch();
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data);
