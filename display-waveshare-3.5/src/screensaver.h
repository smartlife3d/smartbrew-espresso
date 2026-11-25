#pragma once

#include <Arduino.h>
#include <lvgl.h>

/**
 * Screen saver for the Waveshare ESP32-S3 3.5" client display.
 *
 * When nobody has touched the panel for SCREENSAVER_IDLE_MS the display is
 * switched to a dedicated LVGL screen showing a slowly floating espresso cup.
 * A single touch brings the cockpit back exactly as it was - including any
 * modal that was open, because the cockpit is never torn down, only covered.
 */

// Idle time before the screen saver takes over (10 minutes).
#define SCREENSAVER_IDLE_MS (10UL * 60UL * 1000UL)

/**
 * Build the screen-saver screen.
 *
 * @param cockpitScreen the LVGL screen that holds the cockpit UI.  It is kept
 *        alive and re-loaded verbatim when the user touches the display.
 */
void screensaverInit(lv_obj_t* cockpitScreen);

/**
 * Advance the screen-saver state machine.  Call it regularly (the display
 * already ticks every 30 ms).
 *
 * @param nowMs        current millis()
 * @param machineBusy  true while the machine is brewing/flushing.  The cockpit
 *                     has to stay visible then, so the saver is switched off
 *                     and the idle timer is held back.
 */
void screensaverTick(uint32_t nowMs, bool machineBusy);

/** True while the screen saver is on screen. */
bool screensaverActive();
