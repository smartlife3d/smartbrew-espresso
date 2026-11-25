#include "ui_screen.h"
#include <algorithm>
#include <cmath>

// -----------------------------------------------------------------------------
// UI Element Handles
// -----------------------------------------------------------------------------
// Top Header Bar
static lv_obj_t* dropdown_profiles = nullptr;
static lv_obj_t* badge_state       = nullptr;
static lv_obj_t* dot_state         = nullptr;
static lv_obj_t* lbl_state         = nullptr;
static lv_obj_t* btn_reset_chart   = nullptr;

static lv_obj_t* dot_mcu           = nullptr;
static lv_obj_t* badge_scale       = nullptr;
static lv_obj_t* dot_scale         = nullptr;
static lv_obj_t* lbl_scale         = nullptr;
static lv_obj_t* btn_history       = nullptr;
static lv_obj_t* btn_settings      = nullptr;

// Left Main Area: Extraction Chart, In-Graph HUD, Legend
static lv_obj_t* chart_brew         = nullptr;
static lv_chart_series_t* ser_target_weight  = nullptr;
static lv_chart_series_t* ser_target_profile = nullptr;
static lv_chart_series_t* ser_weight         = nullptr;
static lv_chart_series_t* ser_flow           = nullptr;
// The pump flow is the controller's model estimate, not a measurement: a dimmed amber, so the
// measured pressure and scale readings carry the chart. Matches the web dashboard.
static constexpr uint32_t kFlowEstColor = 0xb8760f;
// The flow the scale measures in the cup (g/s / espresso density), on the same ml/s axis as the
// model flow above. Matches the dashed pale-amber "Cup Flow (scale)" line of the web dashboard.
static lv_chart_series_t* ser_cupflow        = nullptr;
static constexpr float    kEspressoDensityGPerMl = 1.03f;   // same as the controller and the web UI
static lv_chart_series_t* ser_pressure       = nullptr;

static lv_obj_t* lbl_hud_timer    = nullptr;
static lv_obj_t* lbl_hud_pressure = nullptr;
static lv_obj_t* lbl_hud_flow     = nullptr;

// Right Sidebar:
// 1. Weight (Yield) Card
static lv_obj_t* weight_card       = nullptr;
static lv_obj_t* coffee_fill_obj   = nullptr;
static lv_obj_t* crema_head_obj    = nullptr;
static lv_obj_t* btn_tare          = nullptr;
static lv_obj_t* lbl_weight        = nullptr;

// 2. Separate Flow & Target Metrics Box
static lv_obj_t* metrics_box       = nullptr;
static lv_obj_t* lbl_weight_flow   = nullptr;
static lv_obj_t* lbl_target_weight = nullptr;

// 3. Primary Brew Button
static lv_obj_t* btn_brew          = nullptr;
static lv_obj_t* lbl_brew_icon     = nullptr;
static lv_obj_t* lbl_brew_title    = nullptr;
static lv_obj_t* lbl_brew_sub      = nullptr;

// 4. Secondary Flush Button
static lv_obj_t* btn_flush         = nullptr;
static lv_obj_t* lbl_flush_title   = nullptr;
static lv_obj_t* lbl_flush_sub     = nullptr;

// Modals
static lv_obj_t* flush_modal            = nullptr;
static lv_obj_t* lbl_flush_modal_timer  = nullptr;
static lv_obj_t* lbl_flush_modal_telem  = nullptr;

static lv_obj_t* portal_modal       = nullptr;
static lv_obj_t* lbl_portal_ssid    = nullptr;

static lv_obj_t* settings_modal     = nullptr;
static lv_obj_t* lbl_set_wifi_status= nullptr;
static lv_obj_t* lbl_set_mcu_ip     = nullptr;
static lv_obj_t* dd_flush_dur       = nullptr;
static lv_obj_t* btn_reset_wifi     = nullptr;
static lv_obj_t* lbl_reset_wifi_btn = nullptr;

static lv_obj_t* history_modal      = nullptr;
static lv_obj_t* lbl_history_body   = nullptr;
static lv_obj_t* history_list       = nullptr;

static lv_obj_t* btn_toggle_sim     = nullptr;
static lv_obj_t* lbl_sim_btn        = nullptr;

// Internal state tracking
static bool is_brewing_active  = false;
static bool is_flushing_active = false;
static bool s_mcu_flush_seen   = false;
static uint32_t s_local_stop_pending_ms = 0;
static uint32_t flush_start_time = 0;
static float flush_duration_seconds = 5.0f;

static float last_displayed_wt  = -999.0f;
static float last_displayed_tw  = -999.0f;
static float last_displayed_p   = -999.0f;
static float last_displayed_f   = -999.0f;
static bool  last_mcu_conn      = false;
static bool  last_scale_conn    = false;
static bool  last_portal_mode   = false;
static bool  s_hold_shot_weight = false;
static float s_held_weight      = 0.0f;
static bool  s_hold_shot_time   = false;
static float s_held_time        = 0.0f;
static int   s_last_point_idx   = -1;
static lv_coord_t s_last_val_p  = 0;
static lv_coord_t s_last_val_f  = 0;
static lv_coord_t s_last_val_w  = 0;
// The pressure and the flow curves have their own last index. They stop at the pump stop, so they
// cannot share the weight curve's interpolation state - otherwise a tail sample would drag them
// along the time axis.
static int   s_last_gauge_idx    = -1;
// The cup-flow curve keeps its own interpolation state: it is only drawn while a scale reports,
// so it can have gaps that the model-flow curve does not have.
static int        s_last_cup_idx = -1;
static lv_coord_t s_last_val_cf  = 0;

static float s_smooth_wt = 0.0f;
static float s_smooth_p  = 0.0f;
static float s_smooth_f  = 0.0f;
static float s_smooth_wf = 0.0f;
static float s_max_shot_weight = 0.0f;
static float s_max_shot_time_observed = 0.0f;

static uint32_t lastChartRefreshMs = 0;
static float currentMaxTime = 40.0f;
static float s_last_drawn_max_time = 0.0f;
static char  s_last_drawn_profile_id[40] = "";

// The post-shot tail on the chart.
//
// The shot clock stops when the pump stops, so the tail cannot be placed with the controller's
// timestamp - every sample of it would land on the same x and the curve would simply stop. The
// tail is therefore placed with the time that has passed since the stop, measured on the display's
// own clock.
static bool     s_tail_active  = false;
static uint32_t s_tail_startMs = 0;
static float    s_tail_baseT   = 0.0f;

static constexpr int kChartPoints = 160;
struct WaveShotSample {
    float t;
    float p;
    float f;
    float w;
    // True while the pump was running.
    //
    // Past the pump stop the gauges are not recorded: the pressure falls away and the flow reads
    // zero, and neither of them says anything about the shot any more. The tail carries the weight
    // alone, so the pressure and the flow curves end at the pump stop instead of trailing away
    // into a collapse of zeros.
    bool gauges;
    // Cup flow measured by the scale, ml/s. Negative when no scale was reporting: the cup-flow
    // curve has a gap there instead of dropping to zero.
    float cf;
};
static WaveShotSample s_shot_samples[1200];
static int            s_shot_sample_count = 0;

static ProfileStepData s_ui_steps[MAX_PROFILE_STEPS];
static int             s_ui_step_count = 0;
static float           s_ui_target_weight = 36.0f;
static float           s_ui_total_duration = 35.0f;

static float applyStepCurve(const char* curve, float u) {
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (!curve || curve[0] == '\0') return u;
    if (strcasecmp(curve, "constant") == 0 || strcasecmp(curve, "flat") == 0) return 0.0f;
    if (strcasecmp(curve, "instant") == 0) return (u >= 1.0f) ? 1.0f : 0.0f;
    if (strcasecmp(curve, "ease_in") == 0) return u * u;
    if (strcasecmp(curve, "ease_out") == 0) return 1.0f - (1.0f - u) * (1.0f - u);
    if (strcasecmp(curve, "ease_in_out") == 0) return u * u * (3.0f - 2.0f * u);
    return u;
}

static float calculateProfilePressure(float t) {
    if (s_ui_step_count <= 0) return 9.0f;
    float cum = 0.0f;
    for (int i = 0; i < s_ui_step_count; i++) {
        float dur = s_ui_steps[i].duration;
        if (dur <= 0.0f) dur = 1.0f;
        if (t <= cum + dur) {
            if (strcasecmp(s_ui_steps[i].type, "pause") == 0) return 0.0f;
            float prog = (t - cum) / dur;
            float shaped = applyStepCurve(s_ui_steps[i].curve, prog);
            float s = s_ui_steps[i].start_val;
            float e = s_ui_steps[i].end_val;
            return s + (e - s) * shaped;
        }
        cum += dur;
    }
    return s_ui_steps[s_ui_step_count - 1].end_val;
}

static lv_coord_t mapPressureCoord(float pBar) {
    float p = std::min(12.0f, std::max(0.0f, pBar));
    return (lv_coord_t)(p * 10.0f + 0.5f);
}

static lv_coord_t mapFlowCoord(float flowMls) {
    // Match Web / Android scale: 0..6 ml/s spans the full 0..120 Y-axis height (1 ml/s = 2 bar height)
    float f = std::min(6.0f, std::max(0.0f, flowMls));
    return (lv_coord_t)(f * 20.0f + 0.5f);
}

// Plots one cup-flow point at chart index `idx`. `cupFlowMls < 0` means no scale reading: the curve
// breaks there. Skipped indices between two readings are interpolated like the other curves.
static void plotCupFlow(int idx, float cupFlowMls) {
    if (!chart_brew || !ser_cupflow) return;
    if (cupFlowMls < 0.0f) { s_last_cup_idx = -1; return; }
    const lv_coord_t c = mapFlowCoord(cupFlowMls);
    if (s_last_cup_idx >= 0 && idx > s_last_cup_idx + 1) {
        for (int k = s_last_cup_idx + 1; k < idx; k++) {
            const float interp = (float)(k - s_last_cup_idx) / (float)(idx - s_last_cup_idx);
            lv_chart_set_value_by_id(chart_brew, ser_cupflow, k,
                                     (lv_coord_t)(s_last_val_cf + (c - s_last_val_cf) * interp));
        }
    }
    lv_chart_set_value_by_id(chart_brew, ser_cupflow, idx, c);
    s_last_cup_idx = idx;
    s_last_val_cf = c;
}

static lv_coord_t mapWeightCoord(float weightG, float targetWt) {
    // Match Web / Android scale: maxWeightScale = max(targetW * 1.25, 50)
    float tw = (targetWt > 0.0f) ? targetWt : 36.0f;
    float maxWeightScale = std::max(tw * 1.25f, 50.0f);
    float w = std::min(maxWeightScale, std::max(0.0f, weightG));
    return (lv_coord_t)((w / maxWeightScale) * 120.0f + 0.5f);
}

// -----------------------------------------------------------------------------
// Chart Reset
// -----------------------------------------------------------------------------
//
// The chart holds two different things, and they must not be thrown away together:
//
//   * the MEASURED curve - the pressure, flow and weight of the shot that is on screen, and
//   * the TARGET guideline - the planned profile curve drawn behind it.
//
// Clearing the screen is one thing; redrawing the guideline because the active profile changed is
// another. The display syncs the profile list from the controller every few seconds, so a sync
// must never wipe the shot the operator is looking at.
static void redrawProfileGuide() {
    if (!chart_brew) return;

    float targetWt = (s_ui_target_weight > 0.0f) ? s_ui_target_weight : 36.0f;
    float totalDur = (s_ui_total_duration > 5.0f) ? s_ui_total_duration : 35.0f;
    if (totalDur + 5.0f > currentMaxTime) {
        currentMaxTime = totalDur + 5.0f;
    }
    s_last_drawn_max_time = currentMaxTime;

    // Plot target curves across kChartPoints
    lv_coord_t twCoord = mapWeightCoord(targetWt, targetWt);
    for (int i = 0; i < kChartPoints; i++) {
        float t = ((float)i / (float)(kChartPoints - 1)) * currentMaxTime;
        float plannedP = calculateProfilePressure(t);
        lv_chart_set_value_by_id(chart_brew, ser_target_profile, i, mapPressureCoord(plannedP));

        // Flat horizontal target weight guideline
        lv_chart_set_value_by_id(chart_brew, ser_target_weight, i, twCoord);
    }
    lv_chart_refresh(chart_brew);
}

void resetShotChart() {
    if (!chart_brew) return;
    currentMaxTime = 40.0f;
    s_shot_sample_count = 0;

    lv_chart_set_point_count(chart_brew, kChartPoints);
    lv_chart_set_all_value(chart_brew, ser_pressure, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart_brew, ser_flow, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart_brew, ser_cupflow, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart_brew, ser_weight, LV_CHART_POINT_NONE);

    // The planned curve is drawn on top of the cleared chart.
    redrawProfileGuide();

    if (lbl_hud_timer)    lv_label_set_text(lbl_hud_timer, "00:00.0");
    if (lbl_hud_pressure) lv_label_set_text(lbl_hud_pressure, "0.0 bar");
    if (lbl_hud_flow)     lv_label_set_text(lbl_hud_flow, "0.0 ml/s");

    s_hold_shot_weight = false;
    s_held_weight = 0.0f;
    s_hold_shot_time = false;
    s_held_time = 0.0f;
    s_last_point_idx = -1;
    s_last_gauge_idx = -1;
    s_last_cup_idx = -1;
    s_last_val_cf = 0;
    s_last_val_p = 0;
    s_last_val_f = 0;
    s_last_val_w = 0;
    s_smooth_wt = 0.0f;
    s_smooth_p  = 0.0f;
    s_smooth_f  = 0.0f;
    s_smooth_wf = 0.0f;
    s_max_shot_weight = 0.0f;
    s_max_shot_time_observed = 0.0f;
    last_displayed_wt = 0.0f;
    last_displayed_p  = 0.0f;
    last_displayed_f  = 0.0f;
    if (lbl_weight) lv_label_set_text(lbl_weight, "0.0 g");
    if (lbl_weight_flow) lv_label_set_text(lbl_weight_flow, "+0.0 g/s");

    if (coffee_fill_obj) {
        lv_obj_set_height(coffee_fill_obj, 0);
    }
    if (weight_card) {
        lv_obj_set_style_border_color(weight_card, lv_color_hex(0x1e293b), 0);
        lv_obj_set_style_border_width(weight_card, 1, 0);
    }
}

// -----------------------------------------------------------------------------
// Chart Draw Event: Custom Ticks and Dashed Target Guides
// -----------------------------------------------------------------------------
static void chart_draw_event_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
    if (!dsc) return;

    if (dsc->part == LV_PART_TICKS && dsc->text != nullptr) {
        if (dsc->label_dsc) {
            dsc->label_dsc->font = &lv_font_montserrat_10;
            dsc->label_dsc->color = lv_color_hex(0x94a3b8);
        }
        if (dsc->line_dsc) {
            dsc->line_dsc->color = lv_color_hex(0x1e293b);
        }

        if (dsc->id == LV_CHART_AXIS_PRIMARY_Y) {
            int p = (dsc->value + 5) / 10;
            snprintf(dsc->text, dsc->text_length, "%db", p);
        } else if (dsc->id == LV_CHART_AXIS_PRIMARY_X) {
            int t = (int)(((float)dsc->value / 4.0f) * currentMaxTime + 0.5f);
            snprintf(dsc->text, dsc->text_length, "%ds", t);
        }
    } else if (dsc->part == LV_PART_ITEMS) {
        if (dsc->sub_part_ptr == ser_target_profile) {
            if (dsc->line_dsc) {
                dsc->line_dsc->color = lv_color_hex(0x0284c7);
                dsc->line_dsc->opa = LV_OPA_60; // Subtle sky-blue dashed curve
                dsc->line_dsc->width = 2;
                dsc->line_dsc->dash_width = 4;
                dsc->line_dsc->dash_gap = 4;
            }
        } else if (dsc->sub_part_ptr == ser_flow) {
            if (dsc->line_dsc) {
                dsc->line_dsc->color = lv_color_hex(0xf59e0b);
                dsc->line_dsc->opa = LV_OPA_50;   // estimate: amber at half strength
                dsc->line_dsc->dash_width = 0;
                dsc->line_dsc->dash_gap = 0;
            }
        } else if (dsc->sub_part_ptr == ser_cupflow) {
            if (dsc->line_dsc) {
                dsc->line_dsc->color = lv_color_hex(0xfde68a);
                dsc->line_dsc->opa = LV_OPA_90;   // pale amber, dashed: measured, not modelled
                dsc->line_dsc->width = 2;
                dsc->line_dsc->dash_width = 5;
                dsc->line_dsc->dash_gap = 3;
            }
        } else if (dsc->sub_part_ptr == ser_target_weight) {
            if (dsc->line_dsc) {
                dsc->line_dsc->color = lv_color_hex(0xc084fc);
                dsc->line_dsc->opa = LV_OPA_70; // Subtle violet dashed line
                dsc->line_dsc->width = 2;
                dsc->line_dsc->dash_width = 4;
                dsc->line_dsc->dash_gap = 4;
            }
        } else {
            if (dsc->line_dsc) {
                dsc->line_dsc->dash_width = 0;
                dsc->line_dsc->dash_gap = 0;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Interactive Control Callbacks
// -----------------------------------------------------------------------------
static void btn_brew_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!is_brewing_active) {
            is_brewing_active = true;
            s_local_stop_pending_ms = 0;
            s_hold_shot_weight = false;
            s_held_weight = 0.0f;
            s_hold_shot_time = false;
            s_held_time = 0.0f;
            s_max_shot_time_observed = 0.0f;
            s_last_point_idx = -1;
            last_displayed_wt = 0.0f;
            if (lbl_weight) lv_label_set_text(lbl_weight, "0.0 g");
            if (lbl_weight_flow) lv_label_set_text(lbl_weight_flow, "+0.0 g/s");
            if (coffee_fill_obj) lv_obj_set_height(coffee_fill_obj, 0);
            resetShotChart();
            sendStartBrew();
            lv_label_set_text(lbl_brew_icon, LV_SYMBOL_STOP);
            lv_label_set_text(lbl_brew_title, "STOP BREW");
            lv_label_set_text(lbl_brew_sub, "Tap to Stop");
            lv_obj_set_style_bg_color(btn_brew, lv_color_hex(0xf43f5e), 0); // Rose Red
        } else {
            is_brewing_active = false;
            s_local_stop_pending_ms = millis();
            if (last_displayed_wt > 0.1f) {
                s_hold_shot_weight = true;
                s_held_weight = last_displayed_wt;
            }
            TelemetryData snap{};
            getTelemetrySnapshot(snap);
            float finalShotTime = std::max(snap.shot_time, s_max_shot_time_observed);
            if (finalShotTime > 0.1f) {
                s_hold_shot_time = true;
                s_held_time = finalShotTime;
            }
            sendStopBrew();
            lv_label_set_text(lbl_brew_icon, LV_SYMBOL_PLAY);
            lv_label_set_text(lbl_brew_title, "START BREW");
            lv_label_set_text(lbl_brew_sub, "Auto Extraction");
            lv_obj_set_style_bg_color(btn_brew, lv_color_hex(0x10b981), 0); // Emerald Green
        }
    }
}

static void btn_flush_event_cb(lv_event_t* e) {
    if (is_brewing_active) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!is_flushing_active) {
            is_flushing_active = true;
            s_mcu_flush_seen = false;
            flush_start_time = millis();
            sendFlush(flush_duration_seconds);
            lv_obj_set_style_bg_color(btn_flush, lv_color_hex(0x0284c7), 0);
            lv_label_set_text(lbl_flush_title, LV_SYMBOL_STOP "  STOP FLUSH");
            if (flush_modal) {
                lv_obj_clear_flag(flush_modal, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(flush_modal);
            }
        } else {
            is_flushing_active = false;
            s_mcu_flush_seen = false;
            sendStopBrew();
            lv_obj_set_style_bg_color(btn_flush, lv_color_hex(0x1e293b), 0);
            lv_label_set_text(lbl_flush_title, LV_SYMBOL_TINT "  FLUSH");
            if (flush_modal) lv_obj_add_flag(flush_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void btn_tare_event_cb(lv_event_t* e) {
    if (is_brewing_active) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_hold_shot_weight = false;
        s_held_weight = 0.0f;
        s_hold_shot_time = false;
        s_held_time = 0.0f;
        s_max_shot_time_observed = 0.0f;
        last_displayed_wt = 0.0f;
        if (lbl_hud_timer) lv_label_set_text(lbl_hud_timer, "00:00.0");
        if (lbl_weight) lv_label_set_text(lbl_weight, "0.0 g");
        if (lbl_weight_flow) lv_label_set_text(lbl_weight_flow, "+0.0 g/s");
        if (coffee_fill_obj) lv_obj_set_height(coffee_fill_obj, 0);
        sendTare();
    }
}

// Clears the shot on screen: the measured curves, the timer and the held yield. The planned
// curve of the active profile is redrawn on the empty chart. Does not tare the scale.
static void clearShotOnScreen() {
    s_hold_shot_weight = false;
    s_held_weight = 0.0f;
    s_hold_shot_time = false;
    s_held_time = 0.0f;
    s_max_shot_time_observed = 0.0f;
    last_displayed_wt = 0.0f;
    if (lbl_hud_timer) lv_label_set_text(lbl_hud_timer, "00:00.0");
    if (lbl_weight) lv_label_set_text(lbl_weight, "0.0 g");
    if (lbl_weight_flow) lv_label_set_text(lbl_weight_flow, "+0.0 g/s");
    if (coffee_fill_obj) lv_obj_set_height(coffee_fill_obj, 0);
    resetShotChart();
}
static void btn_reset_chart_event_cb(lv_event_t* e) {
    if (is_brewing_active) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        clearShotOnScreen();
        sendTare();
    }
}

static void dropdown_profile_event_cb(lv_event_t* e) {
    if (is_brewing_active) return;
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        uint16_t opt = lv_dropdown_get_selected(dropdown_profiles);
        applySelectedProfile(opt);
        TelemetryData snap{};
        if (getTelemetrySnapshot(snap)) {
            if (snap.step_count > 0) {
                s_ui_step_count = snap.step_count;
                for (int i = 0; i < snap.step_count && i < MAX_PROFILE_STEPS; i++) {
                    s_ui_steps[i] = snap.steps[i];
                }
                s_ui_target_weight = (snap.target_weight > 0.0f) ? snap.target_weight : 36.0f;
                s_ui_total_duration = (snap.total_profile_duration > 5.0f) ? snap.total_profile_duration : 35.0f;
                strlcpy(s_last_drawn_profile_id, snap.active_profile_id, sizeof(s_last_drawn_profile_id));
            }
            if (lbl_target_weight) {
                char twBuf[24];
                snprintf(twBuf, sizeof(twBuf), "%.1f g", snap.target_weight);
                lv_label_set_text(lbl_target_weight, twBuf);
            }
        }
        // Picking another profile starts over: the last shot's curves, timer and yield belong to
        // the old profile and would be drawn against the new profile's planned curve. Clear them
        // and draw the new plan on an empty chart.
        clearShotOnScreen();
        sendTare();
    }
}

static void btn_toggle_sim_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        bool newState = !isSimulationMode();
        setSimulationMode(newState);
        if (lbl_sim_btn && btn_toggle_sim) {
            if (newState) {
                lv_label_set_text(lbl_sim_btn, LV_SYMBOL_PLAY " SIMULATION: ON");
                lv_obj_set_style_bg_color(btn_toggle_sim, lv_color_hex(0x00f0ff), 0);
                lv_obj_set_style_text_color(lbl_sim_btn, lv_color_hex(0x07090e), 0);
            } else {
                lv_label_set_text(lbl_sim_btn, LV_SYMBOL_STOP " SIMULATION: OFF");
                lv_obj_set_style_bg_color(btn_toggle_sim, lv_color_hex(0x1e293b), 0);
                lv_obj_set_style_text_color(lbl_sim_btn, lv_color_hex(0x94a3b8), 0);
            }
        }
    }
}

static void btn_settings_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && settings_modal) {
        if (lv_obj_has_flag(settings_modal, LV_OBJ_FLAG_HIDDEN)) {
            if (lbl_set_wifi_status) {
                if (isWifiConnected()) {
                    char wBuf[64];
                    snprintf(wBuf, sizeof(wBuf), "Wi-Fi: %s (Connected)", getWifiSsid());
                    lv_label_set_text(lbl_set_wifi_status, wBuf);
                } else {
                    lv_label_set_text(lbl_set_wifi_status, "Wi-Fi: Not Connected");
                }
            }
            lv_obj_clear_flag(settings_modal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(settings_modal);
        } else {
            lv_obj_add_flag(settings_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void btn_close_settings_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && settings_modal) {
        lv_obj_add_flag(settings_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void btn_reset_wifi_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lbl_reset_wifi_btn) {
            lv_label_set_text(lbl_reset_wifi_btn, LV_SYMBOL_REFRESH " RESETTING...");
        }
        if (btn_reset_wifi) {
            lv_obj_set_style_bg_color(btn_reset_wifi, lv_color_hex(0xf43f5e), 0);
        }
        if (lbl_set_wifi_status) {
            lv_label_set_text(lbl_set_wifi_status, "Wi-Fi: Resetting Credentials...");
        }
        Serial.println("[System] Reset Wifi clicked - clearing stored credentials and rebooting into setup portal...");
        resetWifiConfig();
        delay(350);
        ESP.restart();
    }
}

static void btn_exit_portal_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        requestStopSetupPortal();
        if (portal_modal) lv_obj_add_flag(portal_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void btn_history_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && history_modal) {
        if (lv_obj_has_flag(history_modal, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(history_modal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(history_modal);
            // Ask the network task for the Controller's ledger. The list is drawn as soon as the
            // answer arrives, so what is on screen is what the Controller has recorded.
            requestHistoryRefresh();
        } else {
            lv_obj_add_flag(history_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void dd_flush_dur_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && dd_flush_dur) {
        uint16_t sel = lv_dropdown_get_selected(dd_flush_dur);
        flush_duration_seconds = (sel == 0) ? 3.0f : ((sel == 1) ? 5.0f : 8.0f);
        if (lbl_flush_sub) {
            char subBuf[32];
            snprintf(subBuf, sizeof(subBuf), "%.1fs Rinse", flush_duration_seconds);
            lv_label_set_text(lbl_flush_sub, subBuf);
        }
    }
}

// -----------------------------------------------------------------------------
// Modals
// -----------------------------------------------------------------------------
static void createPortalModal(lv_obj_t* parentScr) {
    portal_modal = lv_obj_create(parentScr);
    lv_obj_set_size(portal_modal, 480, 320);
    lv_obj_center(portal_modal);
    lv_obj_set_style_bg_color(portal_modal, lv_color_hex(0x060a12), 0);
    lv_obj_set_style_bg_opa(portal_modal, LV_OPA_90, 0);
    lv_obj_set_style_border_width(portal_modal, 0, 0);
    lv_obj_set_style_pad_all(portal_modal, 0, 0);
    lv_obj_clear_flag(portal_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(portal_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* card = lv_obj_create(portal_modal);
    lv_obj_set_size(card, 390, 240);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_icon = lv_label_create(card);
    lv_label_set_text(lbl_icon, LV_SYMBOL_WIFI "  PHONE WI-FI SETUP ACTIVE");
    lv_obj_set_style_text_font(lbl_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_icon, lv_color_hex(0x00f0ff), 0);

    lbl_portal_ssid = lv_label_create(card);
    lv_label_set_text(lbl_portal_ssid, "1. Connect phone to: SmartBrew-Display");
    lv_obj_set_style_text_font(lbl_portal_ssid, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_portal_ssid, lv_color_hex(0xffffff), 0);

    lv_obj_t* lbl_p2 = lv_label_create(card);
    lv_label_set_text(lbl_p2, "2. Setup page opens automatically\n   (or browse to http://192.168.4.1/wifi)");
    lv_obj_set_style_text_font(lbl_p2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_p2, lv_color_hex(0x94a3b8), 0);

    lv_obj_t* lbl_p3 = lv_label_create(card);
    lv_label_set_text(lbl_p3, "3. Enter your Wi-Fi SSID & password");
    lv_obj_set_style_text_font(lbl_p3, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_p3, lv_color_hex(0x94a3b8), 0);

    lv_obj_t* btn_exit = lv_btn_create(card);
    lv_obj_set_size(btn_exit, 160, 38);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_exit, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(btn_exit, 1, 0);
    lv_obj_set_style_radius(btn_exit, 8, 0);
    lv_obj_set_ext_click_area(btn_exit, 8);
    lv_obj_add_event_cb(btn_exit, btn_exit_portal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, "EXIT SETUP");
    lv_obj_set_style_text_font(lbl_exit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_exit, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_exit);
}

static void createSettingsModal(lv_obj_t* parentScr) {
    settings_modal = lv_obj_create(parentScr);
    lv_obj_set_size(settings_modal, 464, 304);
    lv_obj_center(settings_modal);
    lv_obj_set_style_bg_color(settings_modal, lv_color_hex(0x090d16), 0);
    lv_obj_set_style_border_color(settings_modal, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_border_width(settings_modal, 2, 0);
    lv_obj_set_style_radius(settings_modal, 10, 0);
    lv_obj_set_style_pad_all(settings_modal, 8, 0);
    lv_obj_clear_flag(settings_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_modal, LV_OBJ_FLAG_HIDDEN);

    // Modal Header
    lv_obj_t* modal_hdr = lv_obj_create(settings_modal);
    lv_obj_set_size(modal_hdr, 448, 36);
    lv_obj_align(modal_hdr, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_opa(modal_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modal_hdr, 0, 0);
    lv_obj_set_style_pad_all(modal_hdr, 0, 0);
    lv_obj_clear_flag(modal_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_title = lv_label_create(modal_hdr);
    lv_label_set_text(lbl_title, LV_SYMBOL_SETTINGS "  DISPLAY & CONTROLLER SETTINGS");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x00f0ff), 0);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t* btn_close = lv_btn_create(modal_hdr);
    lv_obj_set_size(btn_close, 70, 28);
    lv_obj_align(btn_close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x0284c7), 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x0369a1), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_close, 6, 0);
    lv_obj_set_ext_click_area(btn_close, 14);
    lv_obj_add_event_cb(btn_close, btn_close_settings_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "CLOSE");
    lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_close);

    auto createSettingRow = [](lv_obj_t* parent, lv_coord_t yPos) -> lv_obj_t* {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, 448, 48);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, yPos);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0f172a), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x1e293b), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        return row;
    };

    // Row 1: Wi-Fi Status & Phone Portal Button (y=54 for generous gap below close button)
    lv_obj_t* r1 = createSettingRow(settings_modal, 54);
    lbl_set_wifi_status = lv_label_create(r1);
    lv_label_set_text(lbl_set_wifi_status, "Wi-Fi: Connecting...");
    lv_obj_set_style_text_font(lbl_set_wifi_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_set_wifi_status, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_set_wifi_status, LV_ALIGN_LEFT_MID, 8, 0);

    btn_reset_wifi = lv_btn_create(r1);
    lv_obj_set_size(btn_reset_wifi, 140, 34);
    lv_obj_align(btn_reset_wifi, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_reset_wifi, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_reset_wifi, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_border_width(btn_reset_wifi, 1, 0);
    lv_obj_set_style_radius(btn_reset_wifi, 6, 0);
    lv_obj_set_ext_click_area(btn_reset_wifi, 6);
    lv_obj_add_event_cb(btn_reset_wifi, btn_reset_wifi_cb, LV_EVENT_CLICKED, NULL);

    lbl_reset_wifi_btn = lv_label_create(btn_reset_wifi);
    lv_label_set_text(lbl_reset_wifi_btn, LV_SYMBOL_REFRESH " Reset Wifi");
    lv_obj_set_style_text_font(lbl_reset_wifi_btn, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_reset_wifi_btn, lv_color_hex(0x00f0ff), 0);
    lv_obj_center(lbl_reset_wifi_btn);

    // Row 2: Controller Host/IP
    lv_obj_t* r2 = createSettingRow(settings_modal, 108);
    lbl_set_mcu_ip = lv_label_create(r2);
    lv_label_set_text(lbl_set_mcu_ip, "Controller Host: smartbrew.local");
    lv_obj_set_style_text_font(lbl_set_mcu_ip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_set_mcu_ip, lv_color_hex(0x38bdf8), 0);
    lv_obj_align(lbl_set_mcu_ip, LV_ALIGN_LEFT_MID, 8, 0);

    // Row 3: Flush Duration
    lv_obj_t* r3 = createSettingRow(settings_modal, 162);
    lv_obj_t* lbl_f_title = lv_label_create(r3);
    lv_label_set_text(lbl_f_title, "Grouphead Flush Duration");
    lv_obj_set_style_text_font(lbl_f_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_f_title, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_f_title, LV_ALIGN_LEFT_MID, 8, 0);

    dd_flush_dur = lv_dropdown_create(r3);
    lv_obj_set_size(dd_flush_dur, 160, 34);
    lv_obj_align(dd_flush_dur, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_dropdown_set_options(dd_flush_dur, "3 Seconds\n5 Seconds\n8 Seconds");
    lv_dropdown_set_selected(dd_flush_dur, 1);
    lv_obj_set_style_bg_color(dd_flush_dur, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(dd_flush_dur, lv_color_hex(0x334155), 0);
    lv_obj_set_style_text_font(dd_flush_dur, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dd_flush_dur, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_ext_click_area(dd_flush_dur, 6);
    lv_obj_add_event_cb(dd_flush_dur, dd_flush_dur_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Row 4: Simulation Mode (SIM Mode)
    lv_obj_t* r4 = createSettingRow(settings_modal, 216);
    lv_obj_t* lbl_sim_title = lv_label_create(r4);
    lv_label_set_text(lbl_sim_title, "Simulation Mode (SIM)");
    lv_obj_set_style_text_font(lbl_sim_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_sim_title, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_sim_title, LV_ALIGN_LEFT_MID, 8, 0);

    btn_toggle_sim = lv_btn_create(r4);
    lv_obj_set_size(btn_toggle_sim, 160, 34);
    lv_obj_align(btn_toggle_sim, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_toggle_sim, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_toggle_sim, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(btn_toggle_sim, 1, 0);
    lv_obj_set_style_radius(btn_toggle_sim, 6, 0);
    lv_obj_set_ext_click_area(btn_toggle_sim, 6);
    lv_obj_add_event_cb(btn_toggle_sim, btn_toggle_sim_cb, LV_EVENT_CLICKED, NULL);

    lbl_sim_btn = lv_label_create(btn_toggle_sim);
    bool initSim = isSimulationMode();
    lv_label_set_text(lbl_sim_btn, initSim ? (LV_SYMBOL_PLAY " SIMULATION: ON") : (LV_SYMBOL_STOP " SIMULATION: OFF"));
    lv_obj_set_style_text_font(lbl_sim_btn, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_sim_btn, lv_color_hex(initSim ? 0x07090e : 0x94a3b8), 0);
    if (initSim) {
        lv_obj_set_style_bg_color(btn_toggle_sim, lv_color_hex(0x00f0ff), 0);
    }
    lv_obj_center(lbl_sim_btn);
}

static void createFlushModal(lv_obj_t* parentScr) {
    flush_modal = lv_obj_create(parentScr);
    lv_obj_set_size(flush_modal, 480, 320);
    lv_obj_center(flush_modal);
    lv_obj_set_style_bg_color(flush_modal, lv_color_hex(0x060a12), 0);
    lv_obj_set_style_bg_opa(flush_modal, LV_OPA_90, 0);
    lv_obj_set_style_border_width(flush_modal, 0, 0);
    lv_obj_clear_flag(flush_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(flush_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* card = lv_obj_create(flush_modal);
    lv_obj_set_size(card, 340, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0d131f), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x0284c7), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_icon = lv_label_create(card);
    lv_label_set_text(lbl_icon, LV_SYMBOL_TINT "  GROUPHEAD FLUSH");
    lv_obj_set_style_text_font(lbl_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_icon, lv_color_hex(0x38bdf8), 0);

    lbl_flush_modal_timer = lv_label_create(card);
    lv_label_set_text(lbl_flush_modal_timer, "05.0s");
    lv_obj_set_style_text_font(lbl_flush_modal_timer, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_flush_modal_timer, lv_color_hex(0x38bdf8), 0);

    lbl_flush_modal_telem = lv_label_create(card);
    lv_label_set_text(lbl_flush_modal_telem, "9.0 BAR  |  RINSE CYCLE");
    lv_obj_set_style_text_font(lbl_flush_modal_telem, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_flush_modal_telem, lv_color_hex(0x94a3b8), 0);

    lv_obj_t* btn_stop_flush = lv_btn_create(card);
    lv_obj_set_size(btn_stop_flush, 150, 40);
    lv_obj_set_style_bg_color(btn_stop_flush, lv_color_hex(0xf43f5e), 0);
    lv_obj_set_style_radius(btn_stop_flush, 8, 0);
    lv_obj_set_ext_click_area(btn_stop_flush, 8);
    lv_obj_add_event_cb(btn_stop_flush, btn_flush_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_stop = lv_label_create(btn_stop_flush);
    lv_label_set_text(lbl_stop, "STOP FLUSH");
    lv_obj_set_style_text_font(lbl_stop, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_stop, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_stop);
}

static void createHistoryModal(lv_obj_t* parentScr) {
    history_modal = lv_obj_create(parentScr);
    lv_obj_set_size(history_modal, 440, 260);
    lv_obj_center(history_modal);
    lv_obj_set_style_bg_color(history_modal, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_color(history_modal, lv_color_hex(0xa855f7), 0);
    lv_obj_set_style_border_width(history_modal, 2, 0);
    lv_obj_set_style_radius(history_modal, 10, 0);
    lv_obj_set_style_pad_all(history_modal, 12, 0);
    lv_obj_clear_flag(history_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(history_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_h_title = lv_label_create(history_modal);
    lv_label_set_text(lbl_h_title, LV_SYMBOL_LIST "  EXTRACTION SHOT LEDGER");
    lv_obj_set_style_text_font(lbl_h_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_h_title, lv_color_hex(0xc084fc), 0);
    lv_obj_align(lbl_h_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* btn_close_h = lv_btn_create(history_modal);
    lv_obj_set_size(btn_close_h, 70, 28);
    lv_obj_align(btn_close_h, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_close_h, lv_color_hex(0xa855f7), 0);
    lv_obj_set_style_radius(btn_close_h, 6, 0);
    lv_obj_set_ext_click_area(btn_close_h, 6);
    lv_obj_add_event_cb(btn_close_h, btn_history_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_close_h = lv_label_create(btn_close_h);
    lv_label_set_text(lbl_close_h, "CLOSE");
    lv_obj_set_style_text_font(lbl_close_h, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_close_h, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_close_h);

    lbl_history_body = lv_label_create(history_modal);
    lv_label_set_text(lbl_history_body, "Reading the ledger from the Controller...");
    lv_obj_set_style_text_font(lbl_history_body, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_history_body, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_size(lbl_history_body, 416, 200);
    lv_obj_align(lbl_history_body, LV_ALIGN_TOP_LEFT, 0, 36);

    // The ledger entries themselves, laid out like the web ledger rows.
    history_list = lv_obj_create(history_modal);
    lv_obj_set_size(history_list, 416, 200);
    lv_obj_align(history_list, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_set_style_bg_opa(history_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(history_list, 0, 0);
    lv_obj_set_style_pad_all(history_list, 0, 0);
    lv_obj_set_style_pad_row(history_list, 4, 0);
    lv_obj_set_flex_flow(history_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(history_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(history_list, LV_OBJ_FLAG_HIDDEN);
}

// The shot's wall-clock time, the way the web ledger shows it. The controller stamps each shot
// with the moment it was pulled, and the time of day is rendered in UTC - the time zone the
// bench and the dashboards run in. A shot that carries no real timestamp (one taken before the
// controller kept a clock) shows a dash instead of a made-up time.
static void formatShotTime(uint64_t epochMs, char* out, size_t n) {
    if (epochMs < 1000000000000ULL) {
        snprintf(out, n, "--:--:--");
        return;
    }
    uint32_t secs = (uint32_t)((epochMs / 1000ULL) % 86400ULL);
    snprintf(out, n, "%02u:%02u:%02u", secs / 3600, (secs % 3600) / 60, secs % 60);
}

// One ledger entry, in the same shape as a web ledger row: the shot's number and profile on the
// left, the yield in the corner, and the time it was pulled with the peak and the brewing time
// underneath.
static void createHistoryRow(lv_obj_t* parent, int number, const HistoryShotData& s) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, 398, 36);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // "#3  Turbo Shot" - a long profile name is cut at a word boundary and marked, so the line
    // never ends halfway through a word.
    char name[40];
    strlcpy(name, s.profile_name, sizeof(name));
    if (strlen(s.profile_name) >= 30) {
        char* space = strrchr(name, ' ');
        if (space) *space = '\0';
        strlcat(name, "...", sizeof(name));
    }
    char title[64];
    snprintf(title, sizeof(title), "#%d  %s", number, name);

    lv_obj_t* lbl_title = lv_label_create(row);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_width(lbl_title, 240);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 8, 3);

    // The yield - the number the shot is really about.
    char dose[24];
    snprintf(dose, sizeof(dose), "%.1f g", s.dose_g);
    lv_obj_t* lbl_dose = lv_label_create(row);
    lv_label_set_text(lbl_dose, dose);
    lv_obj_set_style_text_font(lbl_dose, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_dose, lv_color_hex(0xc084fc), 0);
    lv_obj_align(lbl_dose, LV_ALIGN_TOP_RIGHT, -8, 3);

    // When it was pulled, and how it ended.
    char when[32];
    formatShotTime(s.epoch_ms, when, sizeof(when));
    char stamp[48];
    snprintf(stamp, sizeof(stamp), "%s  -  COMPLETED", when);
    lv_obj_t* lbl_when = lv_label_create(row);
    lv_label_set_text(lbl_when, stamp);
    lv_obj_set_style_text_font(lbl_when, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_when, lv_color_hex(0x64748b), 0);
    lv_obj_align(lbl_when, LV_ALIGN_TOP_LEFT, 8, 20);

    // The peak and the brewing time, in the corner under the yield.
    char time_s[24];
    snprintf(time_s, sizeof(time_s), "|  %.1f s", s.duration_ms / 1000.0f);
    lv_obj_t* lbl_time = lv_label_create(row);
    lv_label_set_text(lbl_time, time_s);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0x94a3b8), 0);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_RIGHT, -8, 20);

    char peak[24];
    snprintf(peak, sizeof(peak), "%.1f bar", s.peak_bar);
    lv_obj_t* lbl_peak = lv_label_create(row);
    lv_label_set_text(lbl_peak, peak);
    lv_obj_set_style_text_font(lbl_peak, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_peak, lv_color_hex(0x00f0ff), 0);
    lv_obj_align_to(lbl_peak, lbl_time, LV_ALIGN_OUT_LEFT_MID, -5, 0);
}

// The ledger the display shows. The Controller holds the real recorded shots; this only formats
// what was fetched, so the numbers on the screen are the recorded ones.
bool historyLedgerVisible() {
    return history_modal && !lv_obj_has_flag(history_modal, LV_OBJ_FLAG_HIDDEN);
}

void updateEspressoUiHistory(const HistoryData& h) {
    if (!lbl_history_body || !history_list) return;
    // No point drawing the ledger while it is hidden - it is drawn when it is opened.
    if (!historyLedgerVisible()) return;

    // Redraw only when the ledger actually changed. The UI runs at ~33 Hz, and rebuilding the
    // rows means new objects, so there is no reason to do that between fetches.
    static uint32_t drawn_updated_ms = 0;
    static bool     drawn_any = false;
    static bool     drawn_valid = false;
    static bool     drawn_failed = false;
    if (drawn_any && h.updated_ms == drawn_updated_ms &&
        h.valid == drawn_valid && h.failed == drawn_failed) {
        return;
    }
    drawn_any = true;
    drawn_updated_ms = h.updated_ms;
    drawn_valid = h.valid;
    drawn_failed = h.failed;

    // The message label and the list are never both on screen.
    auto showMessageOnly = [&](const char* text) {
        lv_obj_add_flag(history_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_history_body, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_history_body, text);
    };

    if (!h.valid) {
        showMessageOnly("Reading the ledger from the Controller...");
        return;
    }
    if (h.failed) {
        showMessageOnly("The Controller cannot be reached, so the ledger cannot be read.");
        return;
    }
    if (h.count <= 0) {
        showMessageOnly("No shots recorded yet.\nPull a shot to see it here.");
        return;
    }

    // The rows are rebuilt from scratch. The newest shot comes first, and the numbers count up
    // from the oldest, exactly like the web ledger.
    lv_obj_clean(history_list);
    for (int i = 0; i < h.count; ++i) {
        createHistoryRow(history_list, h.count - i, h.shots[i]);
    }
    lv_obj_clear_flag(history_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_history_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(history_list, LV_ANIM_OFF);
}

// -----------------------------------------------------------------------------
// UI Construction (Matching Web & Android Display Cockpit)
// -----------------------------------------------------------------------------
void createEspressoUi() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07090e), 0); // Deep OLED Black

    // =========================================================================
    // 1. Top Header Bar (Height 42px, Width 480px)
    // =========================================================================
    lv_obj_t* header = lv_obj_create(scr);
    lv_obj_set_size(header, 480, 42);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0d131f), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Left Header Container (x=4, w=296)
    lv_obj_t* left_hdr = lv_obj_create(header);
    lv_obj_set_size(left_hdr, 296, 40);
    lv_obj_align(left_hdr, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(left_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_hdr, 0, 0);
    lv_obj_set_style_pad_all(left_hdr, 0, 0);
    lv_obj_set_flex_flow(left_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_hdr, 4, 0);
    lv_obj_clear_flag(left_hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Profile Selector Dropdown Pill (w=224, h=38) - expanded to give ample room for profile name
    TelemetryData initSnap{};
    getTelemetrySnapshot(initSnap);
    dropdown_profiles = lv_dropdown_create(left_hdr);
    lv_obj_set_size(dropdown_profiles, 224, 38);
    if (initSnap.total_profiles > 0) {
        char optsBuf[1024] = {0};
        for (int i = 0; i < initSnap.total_profiles; i++) {
            const char* pName = initSnap.profile_names[i];
            if (strlen(pName) == 0 || strcasecmp(pName, "Profile") == 0) {
                pName = initSnap.profile_ids[i];
            }
            if (strlen(pName) == 0 || strcasecmp(pName, "Profile") == 0) {
                if (i == 0) pName = "Classic 9-Bar";
                else if (i == 1) pName = "Turbo Shot";
                else if (i == 2) pName = "Blooming Espresso";
                else if (i == 3) pName = "Gentle Lever Profile";
                else if (i == 4) pName = "Italian Dark Roast";
                else pName = "Custom Profile";
            }
            if (i > 0) strncat(optsBuf, "\n", sizeof(optsBuf) - strlen(optsBuf) - 1);
            strncat(optsBuf, pName, sizeof(optsBuf) - strlen(optsBuf) - 1);
        }
        lv_dropdown_set_options(dropdown_profiles, optsBuf);
        lv_dropdown_set_selected(dropdown_profiles, initSnap.active_profile_idx);
    } else {
        lv_dropdown_set_options(dropdown_profiles, "Classic 9-Bar\nTurbo Shot\nBlooming Espresso\nGentle Lever Profile\nItalian Dark Roast");
        lv_dropdown_set_selected(dropdown_profiles, 0);
    }
    lv_obj_set_style_bg_color(dropdown_profiles, lv_color_hex(0x0d1e28), 0);
    lv_obj_set_style_bg_color(dropdown_profiles, lv_color_hex(0x132a38), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(dropdown_profiles, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_border_width(dropdown_profiles, 1, 0);
    lv_obj_set_style_radius(dropdown_profiles, 8, 0);
    lv_obj_set_style_text_font(dropdown_profiles, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dropdown_profiles, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_text_color(dropdown_profiles, lv_color_hex(0x00f0ff), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(dropdown_profiles, lv_color_hex(0x00f0ff), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(dropdown_profiles, lv_color_hex(0x00f0ff), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(dropdown_profiles, 8, 0);
    lv_obj_set_style_pad_ver(dropdown_profiles, 8, 0);
    lv_obj_set_ext_click_area(dropdown_profiles, 10);
    lv_obj_add_event_cb(dropdown_profiles, dropdown_profile_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Style the dropdown popup list with guaranteed visible text colors
    lv_obj_t* list = lv_dropdown_get_list(dropdown_profiles);
    if (list) {
        lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_line_space(list, 14, 0);
        lv_obj_set_style_pad_all(list, 10, 0);
        lv_obj_set_style_max_height(list, 230, 0);
        // Normal list items
        lv_obj_set_style_bg_color(list, lv_color_hex(0x0b1120), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(list, lv_color_hex(0xf8fafc), LV_PART_MAIN);
        lv_obj_set_style_border_color(list, lv_color_hex(0x00f0ff), LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
        // Highlighted / selected list item
        lv_obj_set_style_bg_color(list, lv_color_hex(0x0284c7), LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
        lv_obj_set_style_text_color(list, lv_color_hex(0xffffff), LV_PART_SELECTED);
    }

    // Reset Chart Button (w=66, h=38)
    btn_reset_chart = lv_btn_create(left_hdr);
    lv_obj_set_size(btn_reset_chart, 66, 38);
    lv_obj_set_style_bg_color(btn_reset_chart, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_color(btn_reset_chart, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(btn_reset_chart, 1, 0);
    lv_obj_set_style_radius(btn_reset_chart, 8, 0);
    lv_obj_set_style_pad_all(btn_reset_chart, 0, 0);
    lv_obj_set_ext_click_area(btn_reset_chart, 10);
    lv_obj_add_event_cb(btn_reset_chart, btn_reset_chart_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_reset = lv_label_create(btn_reset_chart);
    lv_label_set_text(lbl_reset, LV_SYMBOL_REFRESH " RESET");
    lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_reset, lv_color_hex(0x94a3b8), 0);
    lv_obj_center(lbl_reset);

    // Right Header Container (w=176)
    lv_obj_t* right_hdr = lv_obj_create(header);
    lv_obj_set_size(right_hdr, 176, 40);
    lv_obj_align(right_hdr, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_opa(right_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_hdr, 0, 0);
    lv_obj_set_style_pad_all(right_hdr, 0, 0);
    lv_obj_set_flex_flow(right_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_hdr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_hdr, 6, 0);
    lv_obj_clear_flag(right_hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Controller Connection Indicator Dot (Green dot when connected, Red when disconnected, NO text)
    dot_mcu = lv_obj_create(right_hdr);
    lv_obj_set_size(dot_mcu, 14, 14);
    lv_obj_set_style_radius(dot_mcu, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_mcu, lv_color_hex(0xf43f5e), 0); // Red initially until connected
    lv_obj_set_style_border_color(dot_mcu, lv_color_hex(0x9f1239), 0);
    lv_obj_set_style_border_width(dot_mcu, 2, 0);
    lv_obj_set_style_pad_all(dot_mcu, 0, 0);
    lv_obj_clear_flag(dot_mcu, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Scale Connection Pill (w=46, h=38)
    badge_scale = lv_obj_create(right_hdr);
    lv_obj_set_size(badge_scale, 46, 38);
    lv_obj_set_style_bg_color(badge_scale, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(badge_scale, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(badge_scale, 1, 0);
    lv_obj_set_style_radius(badge_scale, 8, 0);
    lv_obj_set_style_pad_all(badge_scale, 0, 0);
    lv_obj_set_flex_flow(badge_scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge_scale, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(badge_scale, 3, 0);
    lv_obj_clear_flag(badge_scale, LV_OBJ_FLAG_SCROLLABLE);

    dot_scale = lv_obj_create(badge_scale);
    lv_obj_set_size(dot_scale, 7, 7);
    lv_obj_set_style_radius(dot_scale, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_scale, lv_color_hex(0x64748b), 0);
    lv_obj_set_style_border_width(dot_scale, 0, 0);

    lbl_scale = lv_label_create(badge_scale);
    lv_label_set_text(lbl_scale, "WT");
    lv_obj_set_style_text_font(lbl_scale, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_scale, lv_color_hex(0x64748b), 0);

    // Ledger / History Button (w=44, h=38)
    btn_history = lv_btn_create(right_hdr);
    lv_obj_set_size(btn_history, 44, 38);
    lv_obj_set_style_bg_color(btn_history, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_history, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(btn_history, 1, 0);
    lv_obj_set_style_radius(btn_history, 8, 0);
    lv_obj_set_style_pad_all(btn_history, 0, 0);
    lv_obj_set_ext_click_area(btn_history, 10);
    lv_obj_add_event_cb(btn_history, btn_history_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_hist = lv_label_create(btn_history);
    lv_label_set_text(lbl_hist, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(lbl_hist, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_hist, lv_color_hex(0xc084fc), 0);
    lv_obj_center(lbl_hist);

    // Settings Button (w=46, h=38)
    btn_settings = lv_btn_create(right_hdr);
    lv_obj_set_size(btn_settings, 46, 38);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_settings, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(btn_settings, 1, 0);
    lv_obj_set_style_radius(btn_settings, 8, 0);
    lv_obj_set_style_pad_all(btn_settings, 0, 0);
    lv_obj_set_ext_click_area(btn_settings, 10);
    lv_obj_add_event_cb(btn_settings, btn_settings_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_set = lv_label_create(btn_settings);
    lv_label_set_text(lbl_set, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lbl_set, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_set, lv_color_hex(0x94a3b8), 0);
    lv_obj_center(lbl_set);

    // =========================================================================
    // 2. Left Main Area: Extraction Graph, In-Graph HUD, Legend
    // =========================================================================
    lv_obj_t* left_panel = lv_obj_create(scr);
    lv_obj_set_size(left_panel, 314, 272);
    lv_obj_align(left_panel, LV_ALIGN_TOP_LEFT, 4, 44);
    lv_obj_set_style_bg_color(left_panel, lv_color_hex(0x0e131f), 0);
    lv_obj_set_style_border_color(left_panel, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(left_panel, 1, 0);
    lv_obj_set_style_radius(left_panel, 8, 0);
    lv_obj_set_style_pad_all(left_panel, 2, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Y-Axis Unit Header Tag ("bar" above Y-axis ticks)
    lv_obj_t* lbl_axis_p = lv_label_create(left_panel);
    lv_label_set_text(lbl_axis_p, "bar");
    lv_obj_set_style_text_font(lbl_axis_p, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_axis_p, lv_color_hex(0x00f0ff), 0);
    lv_obj_align(lbl_axis_p, LV_ALIGN_TOP_LEFT, 6, 4);

    // Extraction Line Chart (w=276, h=214) aligned TOP_RIGHT to create 34px left margin for Y ticks
    chart_brew = lv_chart_create(left_panel);
    lv_obj_set_size(chart_brew, 276, 214);
    lv_obj_align(chart_brew, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_chart_set_type(chart_brew, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_brew, kChartPoints);
    lv_chart_set_div_line_count(chart_brew, 6, 6);
    lv_chart_set_range(chart_brew, LV_CHART_AXIS_PRIMARY_Y, 0, 120);

    lv_obj_set_style_bg_color(chart_brew, lv_color_hex(0x060a12), 0);
    lv_obj_set_style_border_width(chart_brew, 0, 0);
    lv_obj_set_style_line_color(chart_brew, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_size(chart_brew, 0, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(chart_brew, &lv_font_montserrat_10, LV_PART_TICKS);
    lv_obj_set_style_text_color(chart_brew, lv_color_hex(0x94a3b8), LV_PART_TICKS);

    lv_chart_set_axis_tick(chart_brew, LV_CHART_AXIS_PRIMARY_Y, 4, 2, 5, 1, true, 28);
    lv_chart_set_axis_tick(chart_brew, LV_CHART_AXIS_PRIMARY_X, 4, 2, 6, 1, true, 20);
    lv_obj_add_event_cb(chart_brew, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    ser_target_weight  = lv_chart_add_series(chart_brew, lv_color_hex(0xc084fc), LV_CHART_AXIS_PRIMARY_Y);
    ser_target_profile = lv_chart_add_series(chart_brew, lv_color_hex(0x0284c7), LV_CHART_AXIS_PRIMARY_Y);
    ser_weight         = lv_chart_add_series(chart_brew, lv_color_hex(0xa855f7), LV_CHART_AXIS_PRIMARY_Y);
    ser_flow           = lv_chart_add_series(chart_brew, lv_color_hex(kFlowEstColor), LV_CHART_AXIS_PRIMARY_Y);
    ser_cupflow        = lv_chart_add_series(chart_brew, lv_color_hex(0xfde68a), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart_brew, ser_cupflow, LV_CHART_POINT_NONE);
    ser_pressure       = lv_chart_add_series(chart_brew, lv_color_hex(0x00f0ff), LV_CHART_AXIS_PRIMARY_Y);

    // In-Graph Brew Status Badge Pill (Top Left of Chart)
    badge_state = lv_obj_create(chart_brew);
    lv_obj_set_size(badge_state, LV_SIZE_CONTENT, 22);
    lv_obj_align(badge_state, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(badge_state, lv_color_hex(0x0b1120), 0);
    lv_obj_set_style_bg_opa(badge_state, LV_OPA_90, 0);
    lv_obj_set_style_border_color(badge_state, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_border_width(badge_state, 1, 0);
    lv_obj_set_style_radius(badge_state, 6, 0);
    lv_obj_set_style_pad_hor(badge_state, 8, 0);
    lv_obj_set_style_pad_ver(badge_state, 2, 0);
    lv_obj_set_flex_flow(badge_state, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge_state, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(badge_state, 4, 0);
    lv_obj_clear_flag(badge_state, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    dot_state = lv_obj_create(badge_state);
    lv_obj_set_size(dot_state, 6, 6);
    lv_obj_set_style_radius(dot_state, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_state, lv_color_hex(0x00f0ff), 0);
    lv_obj_set_style_border_width(dot_state, 0, 0);

    lbl_state = lv_label_create(badge_state);
    lv_label_set_text(lbl_state, "READY");
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x00f0ff), 0);

    // In-Graph Floating HUD Overlay (Bottom Right of Chart, elevated above X ticks)
    lv_obj_t* hud_overlay = lv_obj_create(chart_brew);
    lv_obj_set_size(hud_overlay, 114, 48);
    lv_obj_align(hud_overlay, LV_ALIGN_BOTTOM_RIGHT, -4, -20);
    lv_obj_set_style_bg_color(hud_overlay, lv_color_hex(0x0b1120), 0);
    lv_obj_set_style_bg_opa(hud_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(hud_overlay, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(hud_overlay, 1, 0);
    lv_obj_set_style_radius(hud_overlay, 8, 0);
    lv_obj_set_style_pad_all(hud_overlay, 4, 0);
    lv_obj_clear_flag(hud_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lbl_hud_timer = lv_label_create(hud_overlay);
    lv_label_set_text(lbl_hud_timer, "00:00.0");
    lv_obj_set_style_text_font(lbl_hud_timer, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hud_timer, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_hud_timer, LV_ALIGN_TOP_MID, 0, 1);

    lv_obj_t* hud_row = lv_obj_create(hud_overlay);
    lv_obj_set_size(hud_row, 106, 16);
    lv_obj_align(hud_row, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_style_bg_opa(hud_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hud_row, 0, 0);
    lv_obj_set_style_pad_all(hud_row, 0, 0);
    lv_obj_set_flex_flow(hud_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hud_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hud_row, 4, 0);
    lv_obj_clear_flag(hud_row, LV_OBJ_FLAG_SCROLLABLE);

    lbl_hud_pressure = lv_label_create(hud_row);
    lv_label_set_text(lbl_hud_pressure, "0.0 bar");
    lv_obj_set_style_text_font(lbl_hud_pressure, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_hud_pressure, lv_color_hex(0x00f0ff), 0);

    lv_obj_t* lbl_div = lv_label_create(hud_row);
    lv_label_set_text(lbl_div, "|");
    lv_obj_set_style_text_font(lbl_div, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_div, lv_color_hex(0x64748b), 0);

    lbl_hud_flow = lv_label_create(hud_row);
    lv_label_set_text(lbl_hud_flow, "0.0 ml/s");
    lv_obj_set_style_text_font(lbl_hud_flow, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_hud_flow, lv_color_hex(kFlowEstColor), 0);

    // Chart Legend Row (w=300, h=22, positioned cleanly at bottom with no X-axis overlap)
    lv_obj_t* legend_row = lv_obj_create(left_panel);
    lv_obj_set_size(legend_row, 300, 22);
    lv_obj_align(legend_row, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_bg_opa(legend_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(legend_row, 0, 0);
    lv_obj_set_style_pad_all(legend_row, 0, 0);
    lv_obj_set_flex_flow(legend_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend_row, 10, 0);
    lv_obj_clear_flag(legend_row, LV_OBJ_FLAG_SCROLLABLE);

    auto addLegendItem = [](lv_obj_t* parent, lv_color_t color, const char* text) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, color, 0);
        lv_obj_set_style_border_width(dot, 0, 0);

        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x94a3b8), 0);
    };

    addLegendItem(legend_row, lv_color_hex(0x00f0ff), "P(bar)");
    addLegendItem(legend_row, lv_color_hex(kFlowEstColor), "F(est)");
    addLegendItem(legend_row, lv_color_hex(0xfde68a), "Cup");
    addLegendItem(legend_row, lv_color_hex(0xa855f7), "W(g)");
    addLegendItem(legend_row, lv_color_hex(0xc084fc), "Target");

    // =========================================================================
    // 3. Right Sidebar: Weight, Flow & Target Box, Start Brew, Flush
    // =========================================================================
    lv_obj_t* right_panel = lv_obj_create(scr);
    lv_obj_set_size(right_panel, 154, 272);
    lv_obj_align(right_panel, LV_ALIGN_TOP_RIGHT, -4, 44);
    lv_obj_set_style_bg_opa(right_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_panel, 0, 0);
    lv_obj_set_style_pad_all(right_panel, 0, 0);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Box 1: Weight (Yield) Card with Espresso Coffee Fill (h=72)
    // Make entire weight card act as Tare button for easy tapping
    weight_card = lv_obj_create(right_panel);
    lv_obj_set_size(weight_card, 154, 72);
    lv_obj_set_style_bg_color(weight_card, lv_color_hex(0x131a29), 0);
    lv_obj_set_style_border_color(weight_card, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(weight_card, 1, 0);
    lv_obj_set_style_radius(weight_card, 10, 0);
    lv_obj_set_style_pad_all(weight_card, 0, 0);
    lv_obj_set_style_clip_corner(weight_card, true, 0);
    lv_obj_clear_flag(weight_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(weight_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(weight_card, lv_color_hex(0xa855f7), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(weight_card, lv_color_hex(0x1e293b), LV_STATE_PRESSED);
    lv_obj_add_event_cb(weight_card, btn_tare_event_cb, LV_EVENT_CLICKED, NULL);

    // Coffee Liquid Fill Object
    coffee_fill_obj = lv_obj_create(weight_card);
    lv_obj_set_size(coffee_fill_obj, 154, 0);
    lv_obj_align(coffee_fill_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(coffee_fill_obj, lv_color_hex(0x260e04), 0);
    lv_obj_set_style_bg_grad_color(coffee_fill_obj, lv_color_hex(0x78350f), 0);
    lv_obj_set_style_bg_grad_dir(coffee_fill_obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(coffee_fill_obj, 0, 0);
    lv_obj_set_style_radius(coffee_fill_obj, 0, 0);
    lv_obj_set_style_pad_all(coffee_fill_obj, 0, 0);
    lv_obj_clear_flag(coffee_fill_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Golden Crema Head Line
    crema_head_obj = lv_obj_create(coffee_fill_obj);
    lv_obj_set_size(crema_head_obj, 154, 3);
    lv_obj_align(crema_head_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(crema_head_obj, lv_color_hex(0xf59e0b), 0);
    lv_obj_set_style_border_color(crema_head_obj, lv_color_hex(0xfde047), 0);
    lv_obj_set_style_border_width(crema_head_obj, 1, 0);
    lv_obj_set_style_border_side(crema_head_obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(crema_head_obj, 0, 0);
    lv_obj_clear_flag(crema_head_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Weight Card Top Header: Title Left, Tare Button Right
    lv_obj_t* wt_header = lv_obj_create(weight_card);
    lv_obj_set_size(wt_header, 146, 24);
    lv_obj_align(wt_header, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_set_style_bg_opa(wt_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wt_header, 0, 0);
    lv_obj_set_style_pad_all(wt_header, 0, 0);
    lv_obj_clear_flag(wt_header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wt_header, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* lbl_wt_title = lv_label_create(wt_header);
    lv_label_set_text(lbl_wt_title, "WEIGHT");
    lv_obj_set_style_text_font(lbl_wt_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_wt_title, lv_color_hex(0x94a3b8), 0);
    lv_obj_align(lbl_wt_title, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_clear_flag(lbl_wt_title, LV_OBJ_FLAG_CLICKABLE);

    btn_tare = lv_btn_create(wt_header);
    lv_obj_set_size(btn_tare, 50, 22);
    lv_obj_align(btn_tare, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_tare, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_tare, lv_color_hex(0xa855f7), 0);
    lv_obj_set_style_border_width(btn_tare, 1, 0);
    lv_obj_set_style_radius(btn_tare, 6, 0);
    lv_obj_set_style_pad_all(btn_tare, 0, 0);
    lv_obj_set_ext_click_area(btn_tare, 6);
    lv_obj_add_event_cb(btn_tare, btn_tare_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_tare_text = lv_label_create(btn_tare);
    lv_label_set_text(lbl_tare_text, "TARE");
    lv_obj_set_style_text_font(lbl_tare_text, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_tare_text, lv_color_hex(0xc084fc), 0);
    lv_obj_center(lbl_tare_text);

    // Large Weight digits
    lbl_weight = lv_label_create(weight_card);
    lv_label_set_text(lbl_weight, "0.0 g");
    lv_obj_set_style_text_font(lbl_weight, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(lbl_weight, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_weight, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_clear_flag(lbl_weight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl_weight, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_move_foreground(wt_header);
    lv_obj_move_foreground(lbl_weight);

    // Box 2: Dedicated Flow & Target Metrics Box (h=44, separate box!)
    metrics_box = lv_obj_create(right_panel);
    lv_obj_set_size(metrics_box, 154, 44);
    lv_obj_set_style_bg_color(metrics_box, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_color(metrics_box, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(metrics_box, 1, 0);
    lv_obj_set_style_radius(metrics_box, 8, 0);
    lv_obj_set_style_pad_all(metrics_box, 2, 0);
    lv_obj_set_flex_flow(metrics_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics_box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(metrics_box, LV_OBJ_FLAG_SCROLLABLE);

    // Left Column: FLOW
    lv_obj_t* col_flow = lv_obj_create(metrics_box);
    lv_obj_set_size(col_flow, 70, 38);
    lv_obj_set_style_bg_opa(col_flow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_flow, 0, 0);
    lv_obj_set_style_pad_all(col_flow, 0, 0);
    lv_obj_set_flex_flow(col_flow, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_flow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col_flow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_fl_h = lv_label_create(col_flow);
    lv_label_set_text(lbl_fl_h, "FLOW");
    lv_obj_set_style_text_font(lbl_fl_h, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_fl_h, lv_color_hex(0xf59e0b), 0);

    lbl_weight_flow = lv_label_create(col_flow);
    lv_label_set_text(lbl_weight_flow, "+0.0 g/s");
    lv_obj_set_style_text_font(lbl_weight_flow, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_weight_flow, lv_color_hex(0xffffff), 0);

    // Divider line between FLOW and TARGET
    lv_obj_t* div_line = lv_obj_create(metrics_box);
    lv_obj_set_size(div_line, 1, 28);
    lv_obj_set_style_bg_color(div_line, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(div_line, 0, 0);

    // Right Column: TARGET
    lv_obj_t* col_target = lv_obj_create(metrics_box);
    lv_obj_set_size(col_target, 70, 38);
    lv_obj_set_style_bg_opa(col_target, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_target, 0, 0);
    lv_obj_set_style_pad_all(col_target, 0, 0);
    lv_obj_set_flex_flow(col_target, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_target, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col_target, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_tg_h = lv_label_create(col_target);
    lv_label_set_text(lbl_tg_h, "TARGET");
    lv_obj_set_style_text_font(lbl_tg_h, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_tg_h, lv_color_hex(0xc084fc), 0);

    lbl_target_weight = lv_label_create(col_target);
    char twInit[24];
    snprintf(twInit, sizeof(twInit), "%.1f g", initSnap.target_weight > 0.0f ? initSnap.target_weight : 36.0f);
    lv_label_set_text(lbl_target_weight, twInit);
    lv_obj_set_style_text_font(lbl_target_weight, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_target_weight, lv_color_hex(0xc084fc), 0);

    // Box 3: START / STOP BREW Button (h=78)
    btn_brew = lv_btn_create(right_panel);
    lv_obj_set_size(btn_brew, 154, 78);
    lv_obj_set_style_bg_color(btn_brew, lv_color_hex(0x10b981), 0); // Emerald Green
    lv_obj_set_style_radius(btn_brew, 10, 0);
    lv_obj_set_style_pad_all(btn_brew, 4, 0);
    lv_obj_set_flex_flow(btn_brew, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn_brew, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(btn_brew, 6);
    lv_obj_add_event_cb(btn_brew, btn_brew_event_cb, LV_EVENT_CLICKED, NULL);

    lbl_brew_icon = lv_label_create(btn_brew);
    lv_label_set_text(lbl_brew_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_brew_icon, &lv_font_montserrat_16, 0);

    lbl_brew_title = lv_label_create(btn_brew);
    lv_label_set_text(lbl_brew_title, "START BREW");
    lv_obj_set_style_text_font(lbl_brew_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_brew_title, lv_color_hex(0xffffff), 0);

    lbl_brew_sub = lv_label_create(btn_brew);
    lv_label_set_text(lbl_brew_sub, "Auto Extraction");
    lv_obj_set_style_text_font(lbl_brew_sub, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_brew_sub, lv_color_hex(0xd1fae5), 0);

    // Box 4: GROUPHEAD FLUSH Button (h=54)
    btn_flush = lv_btn_create(right_panel);
    lv_obj_set_size(btn_flush, 154, 54);
    lv_obj_set_style_bg_color(btn_flush, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_color(btn_flush, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(btn_flush, 1, 0);
    lv_obj_set_style_radius(btn_flush, 10, 0);
    lv_obj_set_style_pad_all(btn_flush, 4, 0);
    lv_obj_set_flex_flow(btn_flush, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn_flush, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(btn_flush, 6);
    lv_obj_add_event_cb(btn_flush, btn_flush_event_cb, LV_EVENT_CLICKED, NULL);

    lbl_flush_title = lv_label_create(btn_flush);
    lv_label_set_text(lbl_flush_title, LV_SYMBOL_TINT "  FLUSH");
    lv_obj_set_style_text_font(lbl_flush_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_flush_title, lv_color_hex(0x38bdf8), 0);

    lbl_flush_sub = lv_label_create(btn_flush);
    lv_label_set_text(lbl_flush_sub, "5.0s Rinse");
    lv_obj_set_style_text_font(lbl_flush_sub, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_flush_sub, lv_color_hex(0x94a3b8), 0);

    // Construct Modals
    createPortalModal(scr);
    createSettingsModal(scr);
    createFlushModal(scr);
    createHistoryModal(scr);

    // Initial Chart Reset
    resetShotChart();
}

// -----------------------------------------------------------------------------
// UI Telemetry Refresh Loop
// -----------------------------------------------------------------------------
void updateEspressoUiTelemetry(const TelemetryData& data) {
    if (!badge_state) return;

    if (data.step_count > 0) {
        s_ui_step_count = data.step_count;
        for (int i = 0; i < data.step_count && i < MAX_PROFILE_STEPS; i++) {
            s_ui_steps[i] = data.steps[i];
        }
        s_ui_target_weight = (data.target_weight > 0.0f) ? data.target_weight : 36.0f;
        s_ui_total_duration = (data.total_profile_duration > 5.0f) ? data.total_profile_duration : 35.0f;
    }

    // 0. Wi-Fi Setup Portal Overlay State
    if (data.in_setup_portal != last_portal_mode) {
        last_portal_mode = data.in_setup_portal;
        if (portal_modal) {
            if (data.in_setup_portal) {
                if (lbl_portal_ssid && strlen(data.portal_ssid) > 0) {
                    char sBuf[64];
                    snprintf(sBuf, sizeof(sBuf), "1. Connect phone to: %s", data.portal_ssid);
                    lv_label_set_text(lbl_portal_ssid, sBuf);
                }
                lv_obj_clear_flag(portal_modal, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(portal_modal);
            } else {
                lv_obj_add_flag(portal_modal, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // 1. Controller Connection Status Indicator: Green dot when connected, Red dot when disconnected (NO text)
    if (data.mcu_connected != last_mcu_conn) {
        last_mcu_conn = data.mcu_connected;
        if (dot_mcu) {
            lv_obj_set_style_bg_color(dot_mcu, lv_color_hex(data.mcu_connected ? 0x10b981 : 0xf43f5e), 0);
            lv_obj_set_style_border_color(dot_mcu, lv_color_hex(data.mcu_connected ? 0x047857 : 0x9f1239), 0);
        }
    }

    // 2. Scale Connection Status Pill (Dot & Text)
    if (data.scale_conn != last_scale_conn) {
        last_scale_conn = data.scale_conn;
        if (data.scale_conn) {
            lv_obj_set_style_border_color(badge_scale, lv_color_hex(0x10b981), 0);
            lv_obj_set_style_bg_color(dot_scale, lv_color_hex(0x10b981), 0);
            lv_obj_set_style_text_color(lbl_scale, lv_color_hex(0x10b981), 0);
        } else {
            lv_obj_set_style_border_color(badge_scale, lv_color_hex(0x334155), 0);
            lv_obj_set_style_bg_color(dot_scale, lv_color_hex(0x64748b), 0);
            lv_obj_set_style_text_color(lbl_scale, lv_color_hex(0x64748b), 0);
        }
    }

    // 3. Profiles sync from controller (NO emoji in options)
    if (data.profiles_updated && dropdown_profiles != nullptr && data.total_profiles > 0) {
        char optsBuf[1024] = {0};
        for (int i = 0; i < data.total_profiles; i++) {
            const char* pName = data.profile_names[i];
            if (strlen(pName) == 0 || strcasecmp(pName, "Profile") == 0) {
                pName = data.profile_ids[i];
            }
            if (strlen(pName) == 0 || strcasecmp(pName, "Profile") == 0) {
                if (i == 0) pName = "Classic 9-Bar";
                else if (i == 1) pName = "Turbo Shot";
                else if (i == 2) pName = "Blooming Espresso";
                else if (i == 3) pName = "Gentle Lever Profile";
                else if (i == 4) pName = "Italian Dark Roast";
                else pName = "Custom Profile";
            }
            if (i > 0) strncat(optsBuf, "\n", sizeof(optsBuf) - strlen(optsBuf) - 1);
            strncat(optsBuf, pName, sizeof(optsBuf) - strlen(optsBuf) - 1);
        }
        lv_dropdown_set_options(dropdown_profiles, optsBuf);
        lv_dropdown_set_selected(dropdown_profiles, data.active_profile_idx);

        if (lbl_target_weight) {
            char twBuf[24];
            snprintf(twBuf, sizeof(twBuf), "%.1f g", data.target_weight);
            lv_label_set_text(lbl_target_weight, twBuf);
        }

        // Was the ACTIVE profile actually changed by this sync? The sync runs every few seconds and
        // normally brings the same profile back, so the answer is almost always no.
        const bool profileChanged = (data.active_profile_id[0] != '\0' &&
                                     strcasecmp(data.active_profile_id, s_last_drawn_profile_id) != 0);
        strlcpy(s_last_drawn_profile_id, data.active_profile_id, sizeof(s_last_drawn_profile_id));

        // A sync that brings the same profile back changes nothing. When the active profile was
        // changed elsewhere (dashboard, app), clear the old shot like a local change does - but
        // never during a shot or while its yield is still settling.
        if (profileChanged) {
            if (!is_brewing_active && !data.brew_active && !data.weight_settling) clearShotOnScreen();
            else redrawProfileGuide();
        }
    } else if (dropdown_profiles != nullptr && !is_brewing_active && data.total_profiles > 0) {
        uint16_t curSelected = lv_dropdown_get_selected(dropdown_profiles);
        bool profileChanged = (data.active_profile_id[0] != '\0' && strcasecmp(data.active_profile_id, s_last_drawn_profile_id) != 0);
        if ((data.active_profile_idx >= 0 && data.active_profile_idx < data.total_profiles && data.active_profile_idx != curSelected) || profileChanged) {
            if (data.active_profile_idx >= 0 && data.active_profile_idx < data.total_profiles) {
                lv_dropdown_set_selected(dropdown_profiles, data.active_profile_idx);
            }
            if (lbl_target_weight) {
                char twBuf[24];
                snprintf(twBuf, sizeof(twBuf), "%.1f g", data.target_weight);
                lv_label_set_text(lbl_target_weight, twBuf);
            }
            strlcpy(s_last_drawn_profile_id, data.active_profile_id, sizeof(s_last_drawn_profile_id));
            // A different profile: clear the old shot (this branch only runs when not brewing).
            if (profileChanged && !data.brew_active && !data.weight_settling) clearShotOnScreen();
            else redrawProfileGuide();
        }
    }

    // 4. Synchronize Brew Button with machine state
    bool effectiveBrewActive = data.brew_active;
    if (s_local_stop_pending_ms != 0) {
        if (!data.brew_active || (millis() - s_local_stop_pending_ms > 2500)) {
            s_local_stop_pending_ms = 0;
        } else {
            effectiveBrewActive = false;
        }
    }

    if (effectiveBrewActive && !is_brewing_active) {
        is_brewing_active = true;
        s_hold_shot_weight = false;
        s_held_weight = 0.0f;
        s_hold_shot_time = false;
        s_held_time = 0.0f;
        s_max_shot_time_observed = 0.0f;
        s_last_point_idx = -1;
        resetShotChart();
        lv_label_set_text(lbl_brew_icon, LV_SYMBOL_STOP);
        lv_label_set_text(lbl_brew_title, "STOP BREW");
        lv_label_set_text(lbl_brew_sub, "Tap to Stop");
        lv_obj_set_style_bg_color(btn_brew, lv_color_hex(0xf43f5e), 0); // Red
    } else if (!effectiveBrewActive && is_brewing_active) {
        is_brewing_active = false;
        if (last_displayed_wt > 0.1f) {
            s_hold_shot_weight = true;
            s_held_weight = last_displayed_wt;
        }
        float finalShotTime = std::max(data.shot_time, s_max_shot_time_observed);
        if (finalShotTime > 0.1f) {
            s_hold_shot_time = true;
            s_held_time = finalShotTime;
        }
        lv_label_set_text(lbl_brew_icon, LV_SYMBOL_PLAY);
        lv_label_set_text(lbl_brew_title, "START BREW");
        lv_label_set_text(lbl_brew_sub, "Auto Extraction");
        lv_obj_set_style_bg_color(btn_brew, lv_color_hex(0x10b981), 0); // Green
    }

    // 5. Handle Flush countdown modal (including flushes initiated externally on Web/Android)
    const bool mcuFlushing = (strcmp(data.state, "FLUSHING") == 0 || strcmp(data.state, "FLUSH") == 0);
    if (mcuFlushing) {
        s_mcu_flush_seen = true;
    }
    if (mcuFlushing && !is_flushing_active) {
        is_flushing_active = true;
        flush_start_time = millis();
        lv_obj_set_style_bg_color(btn_flush, lv_color_hex(0x0284c7), 0);
        lv_label_set_text(lbl_flush_title, LV_SYMBOL_STOP "  STOP FLUSH");
        if (flush_modal) {
            lv_obj_clear_flag(flush_modal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(flush_modal);
        }
    } else if (!mcuFlushing && is_flushing_active && (s_mcu_flush_seen || (millis() - flush_start_time > 1200))) {
        is_flushing_active = false;
        s_mcu_flush_seen = false;
        lv_obj_set_style_bg_color(btn_flush, lv_color_hex(0x1e293b), 0);
        lv_label_set_text(lbl_flush_title, LV_SYMBOL_TINT "  FLUSH");
        if (flush_modal) lv_obj_add_flag(flush_modal, LV_OBJ_FLAG_HIDDEN);
    }

    if (is_flushing_active) {
        uint32_t elapsed = millis() - flush_start_time;
        uint32_t totalMs = (uint32_t)(flush_duration_seconds * 1000.0f);
        if (elapsed >= totalMs && !mcuFlushing) {
            is_flushing_active = false;
            s_mcu_flush_seen = false;
            lv_obj_set_style_bg_color(btn_flush, lv_color_hex(0x1e293b), 0);
            lv_label_set_text(lbl_flush_title, LV_SYMBOL_TINT "  FLUSH");
            if (flush_modal) lv_obj_add_flag(flush_modal, LV_OBJ_FLAG_HIDDEN);
        } else {
            uint32_t rxAge = (data.rx_millis > 0 && millis() > data.rx_millis)
                             ? std::min<uint32_t>(600U, millis() - data.rx_millis)
                             : 0;
            float rem = (mcuFlushing && data.t_ms > 0)
                        ? ((data.t_ms > rxAge) ? ((data.t_ms - rxAge) / 1000.0f) : 0.0f)
                        : ((totalMs > elapsed) ? ((totalMs - elapsed) / 1000.0f) : 0.0f);
            char fBuf[16];
            snprintf(fBuf, sizeof(fBuf), "%04.1fs", rem);
            if (lbl_flush_modal_timer) lv_label_set_text(lbl_flush_modal_timer, fBuf);
            if (lbl_flush_modal_telem) {
                char ftBuf[48];
                snprintf(ftBuf, sizeof(ftBuf), "%.1f BAR  |  %.1f ML/S", data.pressure, data.flow_rate);
                lv_label_set_text(lbl_flush_modal_telem, ftBuf);
            }
        }
    }

    // 6. Shot Timer display (Strictly monotonic forward latching: time NEVER steps backward)
    if (!effectiveBrewActive && s_hold_shot_time && data.shot_time > s_held_time) {
        s_held_time = data.shot_time;
    }
    float baseT = effectiveBrewActive ? data.shot_time : (s_hold_shot_time ? s_held_time : 0.0f);
    float displayT = baseT;
    if (effectiveBrewActive && data.rx_millis > 0) {
        uint32_t deltaMs = millis() - data.rx_millis;
        if (deltaMs < 600) {
            displayT += (float)deltaMs / 1000.0f;
        }
    }
    if (effectiveBrewActive) {
        if (displayT > s_max_shot_time_observed) {
            s_max_shot_time_observed = displayT;
        }
        displayT = s_max_shot_time_observed;
    } else {
        s_max_shot_time_observed = 0.0f;
    }
    if (lbl_hud_timer) {
        int m = (int)(displayT / 60.0f);
        float s = displayT - (m * 60.0f);
        char tBuf[24];
        snprintf(tBuf, sizeof(tBuf), "%02d:%04.1f", m, s);
        lv_label_set_text(lbl_hud_timer, tBuf);
    }

    // 7. Weight Card & Liquid Fill (h=72)
    // Skip scale weight measurement and post-shot tail updates while flushing.
    if (!is_flushing_active && !mcuFlushing) {
        // During STARTING or before extraction begins (t < 0.15s), scale is auto-taring.
        // Suppress residual pre-tare weight so previous shot's brown coffee fill never flashes.
        bool inStartingPhase = (strcmp(data.state, "STARTING") == 0 || (data.brew_active && data.shot_time < 0.15f));
        float rawWt = inStartingPhase ? 0.0f : data.weight;

        float targetDisplayWt = rawWt;
        if (s_hold_shot_weight) {
            if (rawWt > s_held_weight) {
                s_held_weight = rawWt;
            }
            targetDisplayWt = s_held_weight;
        }
        if (data.brew_active && !inStartingPhase) {
            if (rawWt > s_max_shot_weight) {
                s_max_shot_weight = rawWt;
            }
            // Protect against scale droplet bounce dips: weight accumulates monotonically
            float stableTarget = (rawWt < s_max_shot_weight - 2.0f) ? rawWt : s_max_shot_weight;
            s_smooth_wt += (stableTarget - s_smooth_wt) * 0.45f;
            s_smooth_p  += (data.pressure - s_smooth_p) * 0.40f;
            s_smooth_f  += (data.flow_rate - s_smooth_f) * 0.40f;
            s_smooth_wf += (data.weight_flow - s_smooth_wf) * 0.40f;
        } else {
            s_smooth_wt = inStartingPhase ? 0.0f : targetDisplayWt;
            s_smooth_p  = data.pressure;
            s_smooth_f  = data.flow_rate;
            s_smooth_wf = (s_hold_shot_weight && !data.weight_settling) ? 0.0f : data.weight_flow;
            if (!data.brew_active) {
                s_max_shot_weight = 0.0f;
            }
        }

        if (std::abs(s_smooth_wt - last_displayed_wt) >= 0.03f || !data.brew_active) {
            last_displayed_wt = s_smooth_wt;
            char wtBuf[16];
            snprintf(wtBuf, sizeof(wtBuf), "%.1f g", s_smooth_wt);
            lv_label_set_text(lbl_weight, wtBuf);

            char wfBuf[16];
            // While the post-shot weight is settling the drip rate is still meaningful, so it stays
            // live instead of being zeroed with the rest of the held readout.
            snprintf(wfBuf, sizeof(wfBuf), "+%.1f g/s",
                     (s_hold_shot_weight && !data.weight_settling) ? 0.0f : s_smooth_wf);
            lv_label_set_text(lbl_weight_flow, wfBuf);

            float targetWt = (data.target_weight > 0.0f) ? data.target_weight : 40.0f;
            float pct = s_smooth_wt / targetWt;
            if (pct > 1.0f) pct = 1.0f;
            if (pct < 0.0f) pct = 0.0f;
            int fillHeight = (int)(pct * 72.0f);
            if (coffee_fill_obj) {
                lv_obj_set_height(coffee_fill_obj, fillHeight);
            }

            if (s_smooth_wt >= targetWt && targetWt > 0.0f && data.brew_active) {
                lv_obj_set_style_border_color(weight_card, lv_color_hex(0x10b981), 0);
                lv_obj_set_style_border_width(weight_card, 2, 0);
            }
        }
    } else {
        s_smooth_p = data.pressure;
        s_smooth_f = data.flow_rate;
    }

    // 8. Target Weight display
    if (std::abs(data.target_weight - last_displayed_tw) >= 0.1f) {
        last_displayed_tw = data.target_weight;
        char twBuf[24];
        snprintf(twBuf, sizeof(twBuf), "%.1f g", data.target_weight);
        lv_label_set_text(lbl_target_weight, twBuf);
        redrawProfileGuide();
    }

    // 9. Floating In-Graph HUD updates (using smoothed values for jitter-free display)
    if (std::abs(s_smooth_p - last_displayed_p) >= 0.03f || !data.brew_active) {
        last_displayed_p = s_smooth_p;
        if (lbl_hud_pressure) {
            char pBuf[16];
            snprintf(pBuf, sizeof(pBuf), "%.1f bar", s_smooth_p);
            lv_label_set_text(lbl_hud_pressure, pBuf);
        }
    }

    if (std::abs(s_smooth_f - last_displayed_f) >= 0.03f || !data.brew_active) {
        last_displayed_f = s_smooth_f;
        if (lbl_hud_flow) {
            char fBuf[16];
            snprintf(fBuf, sizeof(fBuf), "%.1f ml/s", s_smooth_f);
            lv_label_set_text(lbl_hud_flow, fBuf);
        }
    }

    // Simulation button visual sync in settings
    if (lbl_sim_btn && btn_toggle_sim) {
        if (data.simulation_mode) {
            lv_label_set_text(lbl_sim_btn, LV_SYMBOL_PLAY " SIMULATION: ON");
            lv_obj_set_style_bg_color(btn_toggle_sim, lv_color_hex(0x00f0ff), 0);
            lv_obj_set_style_text_color(lbl_sim_btn, lv_color_hex(0x07090e), 0);
        } else {
            lv_label_set_text(lbl_sim_btn, LV_SYMBOL_STOP " SIMULATION: OFF");
            lv_obj_set_style_bg_color(btn_toggle_sim, lv_color_hex(0x1e293b), 0);
            lv_obj_set_style_text_color(lbl_sim_btn, lv_color_hex(0x94a3b8), 0);
        }
    }

    // 10. State Badge Pill
    char stateDisplayBuf[32];
    const bool hasFault = (strcmp(data.state, "FAULT") == 0);
    const char* baseState = hasFault ? "FAULT" : ((strlen(data.phase_name) > 0 && data.brew_active) ? data.phase_name : data.state);
    if (data.simulation_mode && !hasFault) {
        snprintf(stateDisplayBuf, sizeof(stateDisplayBuf), "SIM %s", baseState);
    } else {
        strlcpy(stateDisplayBuf, baseState, sizeof(stateDisplayBuf));
    }
    lv_label_set_text(lbl_state, stateDisplayBuf);

    if (hasFault || strcmp(data.state, "ABORT") == 0 || strcmp(data.state, "STOPPED") == 0) {
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0xf43f5e), 0);
        lv_obj_set_style_border_color(badge_state, lv_color_hex(0xf43f5e), 0);
        lv_obj_set_style_bg_color(dot_state, lv_color_hex(0xf43f5e), 0);
    } else if (strcmp(data.state, "STOPPING") == 0) {
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0xf59e0b), 0);
        lv_obj_set_style_border_color(badge_state, lv_color_hex(0xf59e0b), 0);
        lv_obj_set_style_bg_color(dot_state, lv_color_hex(0xf59e0b), 0);
    } else if (data.brew_active) {
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x00f0ff), 0);
        lv_obj_set_style_border_color(badge_state, lv_color_hex(0x00f0ff), 0);
        lv_obj_set_style_bg_color(dot_state, lv_color_hex(0x00f0ff), 0);
    } else if (is_flushing_active || mcuFlushing) {
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x38bdf8), 0);
        lv_obj_set_style_border_color(badge_state, lv_color_hex(0x38bdf8), 0);
        lv_obj_set_style_bg_color(dot_state, lv_color_hex(0x38bdf8), 0);
    } else if (strcmp(data.state, "DONE") == 0) {
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0xc084fc), 0);
        lv_obj_set_style_border_color(badge_state, lv_color_hex(0xc084fc), 0);
        lv_obj_set_style_bg_color(dot_state, lv_color_hex(0xc084fc), 0);
    } else {
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x00f0ff), 0);
        lv_obj_set_style_border_color(badge_state, lv_color_hex(0x00f0ff), 0);
        lv_obj_set_style_bg_color(dot_state, lv_color_hex(0x00f0ff), 0);
    }

    // 11. Dynamic Extraction Chart Plotting
    //
    // The tail is plotted as well: the pump has stopped, but the last of the coffee is still
    // arriving in the cup, and watching the weight climb on the graph is exactly what the operator
    // wants to see. The window closes when the weight settles, and the curve stops there.
    if ((effectiveBrewActive || data.weight_settling) && !is_flushing_active && !mcuFlushing) {
        uint32_t nowMs = millis();
        if (nowMs - lastChartRefreshMs >= 50) {
            lastChartRefreshMs = nowMs;

            float tw = data.target_weight > 0.0f ? data.target_weight : s_ui_target_weight;

            // The shot clock is frozen once the pump stops, so the tail is placed with the time
            // that has passed since the stop. The anchor is taken the first time the controller
            // reports the settling flag, which is the moment the pump stopped.
            float sampleT = data.shot_time;
            if (data.weight_settling) {
                if (!s_tail_active) {
                    s_tail_active = true;
                    s_tail_startMs = nowMs;
                    // The tail continues from where the curve ended, so the two join up seamlessly.
                    s_tail_baseT = (s_shot_sample_count > 0)
                                   ? s_shot_samples[s_shot_sample_count - 1].t
                                   : data.shot_time;
                }
                sampleT = s_tail_baseT + (float)(nowMs - s_tail_startMs) * 0.001f;
            } else {
                s_tail_active = false;
            }

            // The shot clock must be monotonic within a chart. A sample that steps backwards means a
            // new shot has begun while the previous curve is still on screen - drop the old curve
            // instead of drawing a segment that runs backwards across the whole chart.
            if (s_shot_sample_count > 0 && sampleT < s_shot_samples[s_shot_sample_count - 1].t) {
                resetShotChart();
            }

            // The tail carries the weight alone. Once the pump has stopped the pressure falls away
            // and the flow reads zero, and neither of them says anything about the shot any more -
            // so the gauges are simply not recorded past the pump stop, and their curves end there.
            const bool gauges = !data.weight_settling;

            // The cup flow the scale measures, ml/s; -1 when no scale is reporting (curve gap).
            const float cupFlow = data.scale_conn
                                ? std::max(0.0f, data.weight_flow) / kEspressoDensityGPerMl
                                : -1.0f;
            if (s_shot_sample_count < (int)(sizeof(s_shot_samples) / sizeof(s_shot_samples[0]))) {
                s_shot_samples[s_shot_sample_count++] = { sampleT, data.pressure, data.flow_rate,
                                                         data.weight, gauges, cupFlow };
            }

            bool maxTimeChanged = false;
            if (sampleT > currentMaxTime - 5.0f) {
                currentMaxTime = sampleT + 10.0f;
                maxTimeChanged = true;
            }

            if (maxTimeChanged || std::abs(currentMaxTime - s_last_drawn_max_time) > 0.5f) {
                s_last_drawn_max_time = currentMaxTime;
                lv_coord_t twCoord = mapWeightCoord(tw, tw);
                for (int i = 0; i < kChartPoints; i++) {
                    float t = ((float)i / (float)(kChartPoints - 1)) * currentMaxTime;
                    float plannedP = calculateProfilePressure(t);
                    lv_chart_set_value_by_id(chart_brew, ser_target_profile, i, mapPressureCoord(plannedP));
                    lv_chart_set_value_by_id(chart_brew, ser_target_weight, i, twCoord);
                }

                // Re-map all buffered shot samples onto the updated time scale so live curves stay aligned
                lv_chart_set_all_value(chart_brew, ser_pressure, LV_CHART_POINT_NONE);
                lv_chart_set_all_value(chart_brew, ser_flow, LV_CHART_POINT_NONE);
                lv_chart_set_all_value(chart_brew, ser_cupflow, LV_CHART_POINT_NONE);
                lv_chart_set_all_value(chart_brew, ser_weight, LV_CHART_POINT_NONE);
                s_last_point_idx = -1;
                s_last_gauge_idx = -1;
                s_last_cup_idx = -1;
                for (int s = 0; s < s_shot_sample_count; s++) {
                    int idx = (int)((s_shot_samples[s].t / currentMaxTime) * (float)(kChartPoints - 1) + 0.5f);
                    if (idx < 0 || idx >= kChartPoints) continue;

                    // The weight curve runs over the whole shot, tail included.
                    lv_coord_t wC = mapWeightCoord(s_shot_samples[s].w, tw);
                    if (s_last_point_idx >= 0 && idx > s_last_point_idx + 1) {
                        for (int k = s_last_point_idx + 1; k < idx; k++) {
                            float interp = (float)(k - s_last_point_idx) / (float)(idx - s_last_point_idx);
                            lv_chart_set_value_by_id(chart_brew, ser_weight, k, (lv_coord_t)(s_last_val_w + (wC - s_last_val_w) * interp));
                        }
                    }
                    lv_chart_set_value_by_id(chart_brew, ser_weight, idx, wC);
                    s_last_point_idx = idx;
                    s_last_val_w = wC;

                    // The gauge curves stop at the pump stop: a tail sample does not touch them, so
                    // they stay where the shot ended.
                    if (!s_shot_samples[s].gauges) continue;
                    lv_coord_t pC = mapPressureCoord(s_shot_samples[s].p);
                    lv_coord_t fC = mapFlowCoord(s_shot_samples[s].f);
                    if (s_last_gauge_idx >= 0 && idx > s_last_gauge_idx + 1) {
                        for (int k = s_last_gauge_idx + 1; k < idx; k++) {
                            float interp = (float)(k - s_last_gauge_idx) / (float)(idx - s_last_gauge_idx);
                            lv_chart_set_value_by_id(chart_brew, ser_pressure, k, (lv_coord_t)(s_last_val_p + (pC - s_last_val_p) * interp));
                            lv_chart_set_value_by_id(chart_brew, ser_flow, k, (lv_coord_t)(s_last_val_f + (fC - s_last_val_f) * interp));
                        }
                    }
                    lv_chart_set_value_by_id(chart_brew, ser_pressure, idx, pC);
                    lv_chart_set_value_by_id(chart_brew, ser_flow, idx, fC);
                    s_last_gauge_idx = idx;
                    s_last_val_p = pC;
                    s_last_val_f = fC;
                    plotCupFlow(idx, s_shot_samples[s].cf);
                }
            } else {
                int currentPointIdx = (int)((sampleT / currentMaxTime) * (float)(kChartPoints - 1) + 0.5f);
                if (currentPointIdx >= 0 && currentPointIdx < kChartPoints) {
                    lv_coord_t currW = mapWeightCoord(data.weight, tw);

                    // If any points were skipped due to telemetry timing, interpolate them to avoid line gaps
                    if (s_last_point_idx >= 0 && currentPointIdx > s_last_point_idx + 1) {
                        for (int k = s_last_point_idx + 1; k < currentPointIdx; k++) {
                            float interp = (float)(k - s_last_point_idx) / (float)(currentPointIdx - s_last_point_idx);
                            lv_chart_set_value_by_id(chart_brew, ser_weight, k, (lv_coord_t)(s_last_val_w + (currW - s_last_val_w) * interp));
                        }
                    }
                    lv_chart_set_value_by_id(chart_brew, ser_weight, currentPointIdx, currW);
                    s_last_point_idx = currentPointIdx;
                    s_last_val_w = currW;

                    // The gauge curves stop at the pump stop. A tail sample does not touch them, so
                    // the pressure and the flow stay where the shot ended.
                    if (gauges) {
                        lv_coord_t currP = mapPressureCoord(data.pressure);
                        lv_coord_t currF = mapFlowCoord(data.flow_rate);
                        if (s_last_gauge_idx >= 0 && currentPointIdx > s_last_gauge_idx + 1) {
                            for (int k = s_last_gauge_idx + 1; k < currentPointIdx; k++) {
                                float interp = (float)(k - s_last_gauge_idx) / (float)(currentPointIdx - s_last_gauge_idx);
                                lv_chart_set_value_by_id(chart_brew, ser_pressure, k, (lv_coord_t)(s_last_val_p + (currP - s_last_val_p) * interp));
                                lv_chart_set_value_by_id(chart_brew, ser_flow, k, (lv_coord_t)(s_last_val_f + (currF - s_last_val_f) * interp));
                            }
                        }
                        lv_chart_set_value_by_id(chart_brew, ser_pressure, currentPointIdx, currP);
                        lv_chart_set_value_by_id(chart_brew, ser_flow, currentPointIdx, currF);
                        s_last_gauge_idx = currentPointIdx;
                        s_last_val_p = currP;
                        s_last_val_f = currF;
                        plotCupFlow(currentPointIdx, cupFlow);
                    }
                }
            }
            lv_chart_refresh(chart_brew);
        }
    }
}
