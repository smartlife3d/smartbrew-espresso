#include "screensaver.h"
#include "screensaver_art.h"
#include "display_driver.h"

#include <math.h>

// =================================================================================================
// Screen saver - a slowly floating espresso cup
//
// The cockpit is a single LVGL screen whose modals are children of it, so the
// screen saver is a *second* screen that is loaded on top.  Nothing in the
// cockpit is touched while the saver is up, which is what makes "touch to
// resume" restore the previous screen exactly - including an open modal.
//
// All art is an 8-bit alpha mask (see tools/gen_screensaver_art.py).  LVGL
// renders an ALPHA_8BIT image as a solid colour modulated by the mask, so each
// layer is tinted at runtime with img_recolor.
// =================================================================================================

// --- Motion tuning ------------------------------------------------------------------------------

// The cup drifts on two independent sine paths of different length, so it
// traces a slow Lissajous figure instead of a straight line.
static const uint32_t SAVER_DRIFT_X_MS = 17000;
static const uint32_t SAVER_DRIFT_Y_MS = 11000;

static const int32_t SAVER_CUP_X_MIN = 34;
static const int32_t SAVER_CUP_X_MAX = 250;
static const int32_t SAVER_CUP_Y_MIN = 42;
static const int32_t SAVER_CUP_Y_MAX = 132;

// Steam.  The offsets are in the 96 px design space of the generated art: the
// curl sits just above the rim and drifts only a little further up while it
// fades out, so the vapour always stays at the top of the cup.
static const uint32_t SAVER_STEAM_RISE_MS = 3600;
static const int32_t  SAVER_STEAM_Y_START = -16;
static const int32_t  SAVER_STEAM_Y_END   = -36;
static const int32_t  SAVER_STEAM_X_BASE  = 23;
static const int32_t  SAVER_STEAM_X_SWAY  = 27;
static const int32_t  SAVER_STEAM_OPA     = 105;
static const int32_t  SAVER_STEAM_WISPS   = 2;

// --- House palette (see data/index.html) ---------------------------------------------------------

#define SAVER_BG_TOP      0x07090E  // deep OLED black
#define SAVER_BG_BOTTOM   0x2E1409  // espresso, rgba(46,20,9)
#define SAVER_CERAMIC     0xEDE0D2
#define SAVER_CERAMIC_SH  0x966A46
#define SAVER_ESPRESSO    0x78350F  // rgba(120,53,15)
#define SAVER_CREMA       0xF59E0B  // rgba(245,158,11)
#define SAVER_CREMA_HI    0xFDE047  // rgba(253,224,71)
#define SAVER_HINT        0x8A5A2B

// --- State ---------------------------------------------------------------------------------------

static lv_obj_t* s_cockpit   = nullptr;
static lv_obj_t* s_screen    = nullptr;
static lv_obj_t* s_float     = nullptr;              // the group that carries the cup
static lv_obj_t* s_wisp[SAVER_STEAM_WISPS] = { nullptr, nullptr };
static lv_indev_t* s_indev   = nullptr;              // the touch panel
static bool      s_active    = false;
static bool      s_ready     = false;
static uint32_t  s_anchorMs  = 0;                    // last moment the user was known to be around
static uint32_t  s_seenTouch = 0;

// =================================================================================================
// Animation helpers
// =================================================================================================

/**
 * Smooth there-and-back path.
 *
 * LVGL calls the path callback with the animation descriptor and expects the
 * final value back (not a 0..1000 progress).  act_time runs from 0 to time and
 * the animation then repeats, so one full cosine produces a seamless round
 * trip between start_value and end_value.
 */
static int32_t saver_sine_path(const lv_anim_t* a) {
    const float phase = (float)a->act_time / (float)a->time * 2.0f * 3.14159265f;
    const float s = 0.5f - 0.5f * cosf(phase);
    return a->start_value + (int32_t)(s * (float)(a->end_value - a->start_value));
}

static void saver_exec_x(void* var, int32_t value) {
    lv_obj_set_x((lv_obj_t*)var, (lv_coord_t)value);
}

static void saver_exec_y(void* var, int32_t value) {
    lv_obj_set_y((lv_obj_t*)var, (lv_coord_t)value);
}

static void saver_exec_img_opa(void* var, int32_t value) {
    lv_obj_set_style_img_opa((lv_obj_t*)var, (lv_opa_t)value, LV_PART_MAIN);
}

// =================================================================================================
// Object helpers
// =================================================================================================

static void saver_hide(uint32_t nowMs);

/**
 * Turn a plain container into an invisible, padding-free box.
 *
 * The theme gives every `lv_obj` a background, a border and a padding; the
 * screen saver needs containers that are completely transparent.
 */
static void saver_make_transparent(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* saver_tinted_img(lv_obj_t* parent, const lv_img_dsc_t* src, uint32_t rgb, lv_opa_t opa) {
    lv_obj_t* img = lv_img_create(parent);
    lv_img_set_src(img, src);
    // recolor_opa has to be non-zero: an ALPHA_8BIT image is drawn as the
    // recolour itself, modulated by the mask.
    lv_obj_set_style_img_recolor(img, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_img_opa(img, opa, LV_PART_MAIN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static void saver_touch_cb(lv_event_t* e) {
    (void)e;
    // LVGL re-searches the object under the finger on every read while the
    // finger is down.  Without this the cockpit underneath would get the
    // very same tap and could press a button.  Asking LVGL to wait for the
    // release swallows the rest of the gesture.
    if (s_indev) lv_indev_wait_release(s_indev);
    saver_hide(millis());
}

// =================================================================================================
// Animation start / stop
// =================================================================================================

static void saver_start_animations(void) {
    // --- The cup drifts across the panel ---------------------------------------------------------
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_float);
        lv_anim_set_exec_cb(&a, saver_exec_x);
        lv_anim_set_values(&a, SAVER_CUP_X_MIN, SAVER_CUP_X_MAX);
        lv_anim_set_time(&a, SAVER_DRIFT_X_MS);
        lv_anim_set_path_cb(&a, saver_sine_path);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_float);
        lv_anim_set_exec_cb(&a, saver_exec_y);
        lv_anim_set_values(&a, SAVER_CUP_Y_MIN, SAVER_CUP_Y_MAX);
        lv_anim_set_time(&a, SAVER_DRIFT_Y_MS);
        lv_anim_set_path_cb(&a, saver_sine_path);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    // --- Steam: a couple of curls that rise out of the cup and fade away -------------------------
    for (int i = 0; i < SAVER_STEAM_WISPS; i++) {
        const uint32_t phase = (uint32_t)i * (SAVER_STEAM_RISE_MS / 3);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_wisp[i]);
        lv_anim_set_exec_cb(&a, saver_exec_y);
        lv_anim_set_values(&a, SAVER_STEAM_Y_START, SAVER_STEAM_Y_END);
        lv_anim_set_time(&a, SAVER_STEAM_RISE_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, phase);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_wisp[i]);
        lv_anim_set_exec_cb(&a, saver_exec_x);
        lv_anim_set_values(&a, SAVER_STEAM_X_BASE, SAVER_STEAM_X_SWAY);
        lv_anim_set_time(&a, SAVER_STEAM_RISE_MS / 2);
        lv_anim_set_path_cb(&a, saver_sine_path);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, phase);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_wisp[i]);
        lv_anim_set_exec_cb(&a, saver_exec_img_opa);
        lv_anim_set_values(&a, 0, SAVER_STEAM_OPA);
        lv_anim_set_time(&a, SAVER_STEAM_RISE_MS);
        lv_anim_set_path_cb(&a, saver_sine_path);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, phase);
        lv_anim_start(&a);
    }
}

static void saver_stop_animations(void) {
    if (s_float) lv_anim_del(s_float, nullptr);
    for (int i = 0; i < SAVER_STEAM_WISPS; i++) {
        if (s_wisp[i]) lv_anim_del(s_wisp[i], nullptr);
    }
}

// =================================================================================================
// Show / hide
// =================================================================================================

static void saver_show(void) {
    if (s_active || !s_ready) return;
    s_active = true;
    saver_start_animations();
    lv_scr_load(s_screen);
}

static void saver_hide(uint32_t nowMs) {
    if (!s_active) return;
    s_active = false;
    saver_stop_animations();
    // The cockpit was never torn down, so this restores it exactly as it was -
    // including whichever modal was open.
    lv_scr_load(s_cockpit);
    s_anchorMs = nowMs;
}

// =================================================================================================
// Public API
// =================================================================================================

void screensaverInit(lv_obj_t* cockpitScreen) {
    s_cockpit = cockpitScreen;
    if (s_cockpit == nullptr) return;

    // A screen of its own: the cockpit is covered, not rebuilt.
    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(SAVER_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_screen, lv_color_hex(SAVER_BG_BOTTOM), LV_PART_MAIN);

    // The floating group: glow first so it ends up behind the cup.
    s_float = lv_obj_create(s_screen);
    saver_make_transparent(s_float);
    // The halo is bigger than the group and the steam curls rise above it, so
    // the children must not be clipped to the group's box.
    lv_obj_add_flag(s_float, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_size(s_float, 96, 96);
    lv_obj_set_pos(s_float, SAVER_CUP_X_MIN, SAVER_CUP_Y_MIN);

    lv_obj_t* glow = saver_tinted_img(s_float, &img_saver_glow, SAVER_CREMA, 95);
    lv_obj_set_pos(glow, -16, -16);

    saver_tinted_img(s_float, &img_saver_cup, SAVER_CERAMIC, LV_OPA_COVER);
    saver_tinted_img(s_float, &img_saver_cup_shade, SAVER_CERAMIC_SH, LV_OPA_COVER);
    saver_tinted_img(s_float, &img_saver_liquid, SAVER_ESPRESSO, LV_OPA_COVER);
    saver_tinted_img(s_float, &img_saver_crema, SAVER_CREMA, LV_OPA_COVER);
    saver_tinted_img(s_float, &img_saver_crema_hi, SAVER_CREMA_HI, LV_OPA_COVER);

    // Steam curls - children of the floating group so they travel with the cup.
    for (int i = 0; i < SAVER_STEAM_WISPS; i++) {
        lv_obj_t* wisp = saver_tinted_img(s_float, &img_saver_steam, SAVER_CERAMIC, 0);
        lv_obj_set_pos(wisp, SAVER_STEAM_X_BASE, SAVER_STEAM_Y_START);
        s_wisp[i] = wisp;
    }

    // Hint
    lv_obj_t* hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "TOUCH TO RESUME");
    lv_obj_set_style_text_color(hint, lv_color_hex(SAVER_HINT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_opa(hint, 190, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    // Invisible full-screen catcher: any touch anywhere wakes the display up.
    lv_obj_t* catcher = lv_obj_create(s_screen);
    saver_make_transparent(catcher);
    lv_obj_set_size(catcher, 480, 320);
    lv_obj_set_pos(catcher, 0, 0);
    lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(catcher, saver_touch_cb, LV_EVENT_PRESSED, nullptr);

    s_anchorMs = millis();
    s_seenTouch = g_last_touch_ms;

    // Remember the touch panel so the waking gesture can be swallowed.
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            s_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }

    s_ready = true;
}

void screensaverTick(uint32_t nowMs, bool machineBusy) {
    if (!s_ready) return;

    // Any new touch means the user is here.
    const uint32_t touch = g_last_touch_ms;
    if (touch != s_seenTouch) {
        s_seenTouch = touch;
        s_anchorMs = touch;
    }

    // While the machine is working the cockpit has to stay readable.
    if (machineBusy) {
        if (s_active) saver_hide(nowMs);
        s_anchorMs = nowMs;
        return;
    }

    // Never take over in the middle of a gesture: the cockpit underneath would
    // then receive the release of a touch that started before the screen saver
    // appeared.
    if (g_touch_down) {
        s_anchorMs = nowMs;
        return;
    }

    if (!s_active && (uint32_t)(nowMs - s_anchorMs) >= SCREENSAVER_IDLE_MS) {
        saver_show();
    }
}

bool screensaverActive() {
    return s_active;
}
