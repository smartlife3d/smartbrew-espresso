// =================================================================================================
//  main.cpp - Universal Espresso Pressure & Flow Profiling Controller
//
//  DUAL-CORE TASK MAP (docs/Architecture.md)
//  --------------------------------------------------------------------------------------
//   CORE 1
//     task "ctrl"      prio 5  100 Hz pressure sampling, profile engine, PID + feed-forward, all
//                              safety interlocks, virtual button service, zero-cross watchdog.
//     loopTask         prio 1  Arduino loop(): Wi-Fi reconnect/liveness, captive-portal DNS, serial
//                              console, status LED.
//
//     loop() is on Core 1 because the Arduino-ESP32 core starts it on ARDUINO_RUNNING_CORE, which
//     the precompiled sdkconfig sets to 1. "ctrl" outranks it (5 vs 1) and always preempts it, so
//     the control loop is not delayed by it - but loop() is NOT a Core 0 task: never put slow or
//     busy-waiting work there on the assumption that it runs beside the radio. Console commands
//     such as `scan` (blocking Wi-Fi scan) and `use` (LittleFS write) run here.
//
//   CORE 0
//     task "tele"      prio 3  20 Hz brewing / 10 Hz settling / 2 Hz idle: WebSocket binary frames,
//                              UART1 NDJSON, shot-ledger and config saves.
//     task "ble"       prio 2  20 Hz BLE scale state machine: scan, connect, GATT discovery, tare.
//                              NOT on the task watchdog - a NimBLE connect or service discovery
//                              blocks for seconds by design (bounded by NimBLE's own timeouts).
//     NimBLE host task         scale radio stack (CONFIG_BT_NIMBLE_PINNED_TO_CORE 0).
//     task "log_drain" prio 2  ring buffer drain to USB CDC / UART0.
//     task "cal_zero"  prio 1  ephemeral atmospheric zero calibration.
//     task "ota_pull"  prio 1  ephemeral firmware download.
//
//     async_tcp        prio 10 AsyncWebServer / WebSocket handlers, pinned to Core 0 by
//                              -DCONFIG_ASYNC_TCP_RUNNING_CORE=0. It outranks "ctrl", so on Core 1 a
//                              slow handler froze the control loop for up to 0.9 s.
//
//  The zero-cross ISR is always attached on Core 1 (PsmDimmer::arm()), and it runs from IRAM at
//  interrupt level, above every task above.
//
//  SERIAL CONSOLE (docs/Testing.md, Stage 3)
//  --------------------------------------------------------------------------------------
//  Type `help` for the command list. The console can inject simulated pressure and weight so the
//  entire profile engine, PID and cutoff chain can be exercised with no hardware attached.
// =================================================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <driver/gpio.h>

#include "pins.h"
#include "logger.h"
#include "config_schema.h"
#include "pump_presets.h"
#include "profile_types.h"

#include "hal/hal_psm.h"
#include "hal/hal_pressure.h"
#include "hal/hal_button.h"

#include "control/pid_controller.h"
#include "control/profile_engine.h"
#include "control/shot_history.h"

#include "scale/scale_manager.h"

#include "network/wifi_portal.h"
#include "network/web_api.h"
#include "network/aux_bridge.h"
#include "network/ota_service.h"

#include "brew_controller.h"

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>

// -------------------------------------------------------------------------------------------------
// Globals
// -------------------------------------------------------------------------------------------------
static AsyncWebServer g_http(80);

static TaskHandle_t hCtrl = nullptr;
static TaskHandle_t hTele = nullptr;
static TaskHandle_t hBle  = nullptr;

// Serial console state
static char     conBuf[192];
static uint8_t  conLen  = 0;
static uint32_t conLast = 0;

// -------------------------------------------------------------------------------------------------
// Forward declarations
// -------------------------------------------------------------------------------------------------
static void seedFilesystem();

// =================================================================================================
// CORE 1 - CONTROL TASK  (100 Hz)
// =================================================================================================
static void taskControl(void* arg) {
    esp_task_wdt_add(nullptr); // Subscribe Core 1 control task to hardware task watchdog
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / CONTROL_HZ);

    // OTA rollback: the new image is flashed with Update.end(false), so it boots in PENDING_VERIFY.
    // If the firmware does not mark itself valid within the bootloader's window, the bootloader
    // reverts to the previous partition by itself. Mark it valid only after the control loop has
    // run cleanly for a while.
    const uint32_t otaConfirmAfterMs = 30000UL;
    bool otaConfirmed = false;
    uint32_t prevStartUs = 0, lastStallLogMs = 0;

    for (;;) {
        esp_task_wdt_reset();
        const uint32_t t0 = micros();
        g_brew.tick((uint32_t)millis());
        const uint32_t t1 = micros();
        // Stall report: a tick should take well under 1 ms and start every 10 ms. Name the stage
        // that took the time, or show that the task was simply not scheduled (gap long, tick
        // short: flash cache off, or an interrupt-disabled section elsewhere on this core).
        const uint32_t gapUs = prevStartUs ? (t0 - prevStartUs) : 0;
        prevStartUs = t0;
        if ((t1 - t0 > 30000UL || gapUs > 60000UL) && millis() - lastStallLogMs > 1000UL) {
            lastStallLogMs = millis();
            static const char* kStage[] = {"commands", "psm/button", "pressure", "scale",
                                           "interlocks", "state machine", "telemetry"};
            uint32_t worst = 0; int wi = 0;
            for (int i = 0; i < 7; ++i) {
                const uint32_t a = g_brew.stageUs(i);
                const uint32_t b = (i < 6) ? g_brew.stageUs(i + 1) : t1;
                if (b - a > worst) { worst = b - a; wi = i; }
            }
            SB_LOGW("Control loop stall: %lu ms since the last tick, tick took %lu ms "
                    "(slowest stage '%s' %lu ms)", (unsigned long)(gapUs / 1000),
                    (unsigned long)((t1 - t0) / 1000), kStage[wi], (unsigned long)(worst / 1000));
        }

        if (!otaConfirmed && (uint32_t)millis() > otaConfirmAfterMs) {
            otaConfirmed = true;   // only try once per boot
            const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                SB_LOGI("Firmware image marked valid - OTA rollback cancelled.");
            } else if (err == ESP_ERR_NOT_FOUND) {
                SB_LOGI("No pending OTA rollback to cancel (image was already valid).");
            } else {
                SB_LOGE("Could not mark the firmware image valid (%d) - the bootloader may roll "
                        "back to the previous image on the next reset.", (int)err);
            }
        }

        vTaskDelayUntil(&last, period);
    }
}

// =================================================================================================
// CORE 0 - TELEMETRY & EVENT TASK  (20 Hz brewing / 10 Hz settling / 2 Hz idle)
// =================================================================================================
static void taskTelemetry(void* arg) {
    esp_task_wdt_add(nullptr);
    TickType_t last = xTaskGetTickCount();
    uint32_t lastCycleUs = micros();
    bool lastActive = false;

    for (;;) {
        esp_task_wdt_reset();
        const uint32_t cycleStartUs = micros();
        bool active = false;
        // Drain events from Core 1 BrewController
        BrewEvent evt;
        while (g_brew.pollEvent(evt)) {
            switch (evt.type) {
                case EventType::SHOT_COMPLETED:
                    // A drip lag learned on the control core is applied and saved here, on Core 0,
                    // because saving writes flash. The control core reads the float on its next
                    // shot; a single aligned 32-bit store cannot tear.
                    {
                        bool changed = false;
                        if (evt.learnedLagSec >= 0.0f && g_config.cfg().scaleLagAutoLearn) {
                            g_config.cfg().scaleLagSec = evt.learnedLagSec;
                            changed = true;
                        }
                        if (evt.learnedFlowScale > 0.0f && g_config.cfg().pumpFlowAutoCal) {
                            g_config.cfg().pumpFlowScale = flowScaleClamp(evt.learnedFlowScale);
                            g_pid.setFlowScale(g_config.cfg().pumpFlowScale);
                            changed = true;
                        }
                        if (changed) g_config.save();
                    }
                    g_history.saveShot();
                    g_web.broadcastShotSummary(evt.shotDurationMs, evt.doseG);
                    g_aux.onShotEnd((uint16_t)evt.shotDurationMs, evt.peakBar, evt.doseG);
                    break;
                case EventType::STATE_CHANGED:
                case EventType::FAULT_SET:
                default:
                    break;
            }
        }

        const BrewState st = g_brew.state();
        active = (st == BrewState::PREPARING ||
                  st == BrewState::BREWING   ||
                  st == BrewState::STARTING  ||
                  st == BrewState::FLUSHING  ||
                  st == BrewState::STOPPING);
        // While the post-shot weight is settling the last of the coffee is still arriving in the
        // cup, so the stream stays fast enough for the operator to follow the yield ticking up.
        const bool settling = g_brew.weightSettling() || g_scale.hasPendingBle() || g_scale.scanning();
        const uint16_t hz  = active ? WS_HZ_BREW : (settling ? WS_HZ_SETTLE : WS_HZ_IDLE);
        const TickType_t period = pdMS_TO_TICKS(1000 / hz);

        // Measure the send path on its own. The event drain above can block for tens of
        // milliseconds on the LittleFS write that archives a finished shot, and folding that
        // into the body time would hide a stalled send path behind a slow disk write.
        const uint32_t bodyStartUs = micros();

        // One binary frame feeds both transports: the WebSocket clients get it verbatim, and the
        // UART1 bridge renders the NDJSON line from the very same bytes.
        uint8_t tFrame[SB_TELEMETRY_MAX_LEN];
        size_t  tLen = 0;
        if (g_brew.telemetryBin(tFrame, sizeof(tFrame), &tLen)) {
            g_web.broadcast(tFrame, tLen);
            g_aux.onTelemetryFrame(tFrame, tLen);
        }
        g_aux.loop((uint32_t)millis());
        g_web.loop();
        g_ota.checkWatchdog();

        // Report how long the send path took and how long the gap since the previous cycle was.
        // A long body means the send path blocks; a long gap means Core 0 itself is starved.
        // Only the running phases feed the gap counter, and only when the previous cycle was
        // running as well: at idle this task is meant to wake once every 500 ms, and the first
        // cycle after a brew starts is still measured against that idle sleep.
        const uint32_t bodyEndUs = micros();
        g_web.noteTelemetryCycle(bodyEndUs - bodyStartUs, cycleStartUs - lastCycleUs,
                                 active && lastActive);
        lastCycleUs = cycleStartUs;
        lastActive = active;

        vTaskDelayUntil(&last, period);
    }
}

// =================================================================================================
// CORE 0 - BLE SCALE TASK  (20 Hz)
// =================================================================================================
// The scale state machine runs in its own task because NimBLE client calls block the caller:
// connect() waits up to the connect timeout + 1 s, and service discovery / subscribe wait without
// a deadline until the peer answers or the link drops (NimBLE's 30 s GATT timeout). Run inside
// "tele", one unresponsive scale stalled the WebSocket stream and tripped the 5 s task watchdog.
// This task is deliberately not subscribed to the watchdog; it holds no lock the control or
// telemetry tasks wait on, so a slow BLE call here only delays the scale.
static void taskBle(void* arg) {
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);
    for (;;) {
        g_scale.serviceBle((uint32_t)millis());
        // A blocking connect leaves `last` far behind; resync instead of bursting to catch up.
        if (xTaskGetTickCount() - last > period) last = xTaskGetTickCount();
        vTaskDelayUntil(&last, period);
    }
}

// =================================================================================================
// SERIAL CONSOLE  (docs/Testing.md)
// =================================================================================================
static void conPrint(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    sb_log::putRaw(b);
    sb_log::putRaw("\n");
}

static void printHelp() {
    SB_BANNER("");
#if defined(SB_BENCH) && (SB_BENCH != 0)
    SB_LOGI("SMART BREW CONTROLLER - bench console (docs/Testing.md)");
#else
    SB_LOGI("SMART BREW CONTROLLER - console (docs/Testing.md)");
#endif
    SB_LOGI("--- power stage -------------------------------------------------------------");
    SB_LOGI("  zc                   print zero-cross and mains diagnostics");
#if defined(SB_BENCH) && (SB_BENCH != 0)
    SB_LOGI("  duty <0-100>         force the PSM duty (BYPASSES the PID - use on a jig only)");
    SB_LOGI("  btn [<ms>]           press the virtual button (configured length, or hold <ms> up to 30000)");
#endif
    SB_LOGI("  disarm               full power-stage disarm (OTA equivalent)");
    SB_LOGI("  arm                  re-arm the power stage");
    SB_LOGI("--- pressure ----------------------------------------------------------------");
#if defined(SB_BENCH) && (SB_BENCH != 0)
    SB_LOGI("  press <bar>          simulate a pressure reading (enables the simulator)");
    SB_LOGI("  zero                 run the atmospheric zero calibration");
    SB_LOGI("  sim on|off           engage / release the pressure simulator");
#endif
    SB_LOGI("--- shot -------------------------------------------------------------------");
    SB_LOGI("  start | stop | tare | next");
#if defined(SB_BENCH) && (SB_BENCH != 0)
    SB_LOGI("  weight <g>           inject a simulated scale reading");
    SB_LOGI("  set <bar>            override the pressure setpoint for the current phase");
    SB_LOGI("  manual <0-100>       take manual control of the duty cycle");
#endif
    SB_LOGI("--- profiles ----------------------------------------------------------------");
    SB_LOGI("  profiles             list the profiles in LittleFS");
    SB_LOGI("  use <id>             load a profile by id");
    SB_LOGI("  phase                print the phase table of the active profile");
    SB_LOGI("--- network --------------------------------------------------------------");
    SB_LOGI("  wifi [<ssid> <pass>] show status, or save credentials and connect");
    SB_LOGI("  portal               start the setup hotspot (captive portal)");
    SB_LOGI("  scan                 list 2.4 GHz Wi-Fi networks (blocks the console briefly)");
    SB_LOGI("--- diagnostics --------------------------------------------------------------");
    SB_LOGI("  status | heap | scale | clear | help");
    SB_LOGI("  i2c                  check the ADS1115 bus: line levels and every device that answers");
    SB_LOGI("  led [<r> <g> <b> | bright <0-100> | auto] control status LED brightness or color");
    SB_BANNER("");
}

static void printStatus() {
    Telemetry t;
    g_brew.snapshot(t);
    conPrint("");
    conPrint("  state        %-9s faults 0x%04X  shots %u",
             brewStateName(g_brew.state()), (unsigned)g_brew.faults(), (unsigned)g_brew.shotCount());
    conPrint("  pressure     %.2f bar (target %.2f, node %.0f mV, src %s)",
             t.pBar, t.pTarget, g_pressure.lastMv(), g_pressure.sourceName());
    conPrint("  duty         %.1f %% (feed-forward %.1f %%, Kp %.2f Ki %.2f Kd %.2f%s)",
             g_psm.dutyPct(), g_pid.feedForward(t.pTarget),
             g_pid.proportionalTerm() + 1.0f * g_pid.lastError(),
             g_pid.integralTerm() / (g_pid.lastError() != 0 ? g_pid.lastError() : 1),
             g_pid.derivativeTerm(),
             g_pid.integralFrozen() ? ", integral FROZEN" : "");
    conPrint("  zero-cross   %u Hz (detected %u Hz), %u pulses, %u rejected, %u timeouts",
             (unsigned)g_psm.mainsHz(), (unsigned)g_psm.detectedHz(),
             (unsigned)g_psm.pulsesAccepted(), (unsigned)g_psm.pulsesRejected(),
             (unsigned)g_psm.missedTimeoutCount());
    conPrint("  ac_present   %s   period %.2f ms   gate hold %u us",
             g_psm.acPowerPresent() ? "YES" : "no",
             g_psm.lastZcPeriodUs() / 1000.0f, (unsigned)g_psm.gateHoldUs());
    conPrint("  phase        %u '%s'  %u %%   psm ceiling %u %%",
             t.phaseIdx, t.phaseName, t.phasePct,
             g_profile.phaseIndex() < g_profile.phaseCount()
                 ? g_profile.profile().phases[g_profile.phaseIndex()].maxPsmPct : 0);
    conPrint("  scale        %s %s [%s] mac %s, %u updates, slope %.2f g/s, bat %d %%",
             g_scale.stateName(), g_scale.deviceName(), scaleLinkName(g_scale.link()),
             g_scale.mac(), (unsigned)g_scale.updates(), g_scale.slopeGs(), g_scale.battery());
    conPrint("  button       %u pulses, %u blocked, width %u ms",
             (unsigned)g_button.pulses(), (unsigned)g_button.blocked(),
             (unsigned)g_button.pulseWidthMs());
    conPrint("  flow         virtual %.2f ml/s   weight-derived %.2f g/s",
             t.flowVirt, t.flowWeight);
    conPrint("  memory       heap %u free / %u min, psram %u free",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             (unsigned)ESP.getFreePsram());
    conPrint("");
}

static void printZc() {
    SB_BANNER("");
    SB_LOGI("ZERO-CROSS / PSM DIAGNOSTIC (updated every tick)");
    SB_LOGI("  armed         %s", g_psm.isArmed() ? "yes" : "NO - power stage disabled");
    SB_LOGI("  duty          %.2f %%", g_psm.dutyPct());
    SB_LOGI("  mains         %u Hz (locked %s, detected %s)",
             (unsigned)g_psm.mainsHz(),
             g_config.cfg().mainsMode == MAINS_50HZ ? "50" :
             g_config.cfg().mainsMode == MAINS_60HZ ? "60" : "auto",
             g_psm.detectedHz() ? (g_psm.detectedHz() == 60 ? "60 Hz" : "50 Hz") : "not yet");
    SB_LOGI("  gate hold     %u us   (hard limit %u us)", (unsigned)g_psm.gateHoldUs(),
             (unsigned)(g_psm.gateHoldUs() * 1.6f));
    SB_LOGI("  ac_present    %s", g_psm.acPowerPresent() ? "YES" : "no");
    SB_LOGI("  brew_confirmed %s", g_psm.brewConfirmed() ? "YES" : "no");
    SB_LOGI("  pulses        %u accepted / %u rejected as glitch / %u missing-timeouts",
             (unsigned)g_psm.pulsesAccepted(), (unsigned)g_psm.pulsesRejected(),
             (unsigned)g_psm.missedTimeoutCount());
    SB_LOGI("  last period   %.3f ms   full AC cycles fired %u",
             g_psm.lastZcPeriodUs() / 1000.0f, (unsigned)g_psm.fullCycles());
    SB_LOGI("  ZC detection  %u / 8 samples", (unsigned)g_psm.detectionProgress());
    SB_LOGI("  explanation: accepted edges %u - rejected %u = %u usable",
             (unsigned)g_psm.pulsesAccepted(), (unsigned)g_psm.pulsesRejected(),
             (unsigned)(g_psm.pulsesAccepted() - g_psm.pulsesRejected()));
    SB_BANNER("");
}

static void listProfiles() {
    File dir = LittleFS.open("/profiles");
    if (!dir) { SB_LOGW("No /profiles directory"); return; }
    SB_BANNER("");
    SB_LOGI("PROFILES in LittleFS:");
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String n = f.name();
            SB_LOGI("  %-28s %u bytes%s", n.c_str(), (unsigned)f.size(),
                    (n == String(g_profile.profile().id) ? "   <== ACTIVE" : ""));
        }
        f = dir.openNextFile();
    }
    dir.close();
    SB_BANNER("");
}

static void printPhaseTable() {
    const Profile& p = g_profile.profile();
    SB_BANNER("");
    SB_LOGI("PROFILE '%s' (id=%s) target %.1f g, max %.1f s, %u phases",
             p.name, p.id, p.targetWeightG, p.maxTotalTimeSec, p.phaseCount);
    for (uint8_t i = 0; i < p.phaseCount; ++i) {
        const Phase& ph = p.phases[i];
        SB_LOGI("  [%u] %-22s %-8s %.2f -> %.2f bar (%s) ceil %u%%  dur %.1fs  %s  %u trigger(s)",
                 i, ph.name, modeName(ph.mode), ph.startVal, ph.endVal, curveName(ph.curve),
                 ph.maxPsmPct, ph.transition.durationSec,
                 ph.transition.logic == TriggerLogic::AND ? "AND" : "OR ",
                 ph.transition.triggerCount);
        for (uint8_t k = 0; k < ph.transition.triggerCount; ++k) {
            SB_LOGI("        %-14s %.2f", triggerName(ph.transition.triggers[k].type),
                     ph.transition.triggers[k].value);
        }
    }
    SB_BANNER("");
}

static void handleConsoleLine(char* line) {
    while (*line == ' ') ++line;
    if (!*line) return;

    char* tok[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    uint8_t n = 0;
    char* save = nullptr;
    for (char* t = strtok_r(line, " \t", &save); t && n < 6; t = strtok_r(nullptr, " \t", &save)) {
        tok[n++] = t;
    }
    if (!n) return;

    const char* c0 = tok[0];

    // ---- help / status -------------------------------------------------------------------------
    if (strcasecmp(c0, "help") == 0 || strcasecmp(c0, "?") == 0) { printHelp(); return; }
    if (strcasecmp(c0, "status") == 0)  { printStatus(); return; }
    if (strcasecmp(c0, "zc") == 0)      { printZc(); return; }
    if (strcasecmp(c0, "clear") == 0)  { g_brew.clearFaults(); return; }
    if (strcasecmp(c0, "heap") == 0) {
        SB_LOGI("heap %u free / %u min-ever, psram %u free, largest block %u",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getMaxAllocHeap());
        return;
    }
    if (strcasecmp(c0, "led") == 0) {
        if (n > 2 && strcasecmp(tok[1], "bright") == 0) {
            uint8_t b = (uint8_t)constrain(atoi(tok[2]), 0, 100);
            g_statusLed.setBrightness(b);
            g_config.cfg().ledBrightness = b;
            g_config.save();
            SB_LOGI("LED brightness set to %u%% (saved to config)", b);
            return;
        }
        if (n > 1 && strcasecmp(tok[1], "auto") == 0) {
            g_statusLed.clearManual();
            SB_LOGI("LED returned to automatic mode.");
            return;
        }
        uint8_t r = n > 1 ? atoi(tok[1]) : 255;
        uint8_t g = n > 2 ? atoi(tok[2]) : 0;
        uint8_t b = n > 3 ? atoi(tok[3]) : 0;
        g_statusLed.setManual(true);
        g_statusLed.setColor(r, g, b);
        SB_LOGI("LED forced to %u,%u,%u at %u%% brightness. (Type 'led auto' to return to automatic, or 'led bright <0-100>')",
                r, g, b, g_statusLed.brightness());
        return;
    }
    if (strcasecmp(c0, "scale") == 0) {
        if (n > 1 && strcasecmp(tok[1], "scan") == 0) {
            SB_LOGI("Starting BLE scale scan (8s)...");
            g_scale.startScan(8);
        } else if (n > 2 && strcasecmp(tok[1], "pair") == 0) {
            SB_LOGI("Pairing BLE scale to '%s'...", tok[2]);
            g_scale.pairTo(tok[2]);
        } else if (n > 1 && strcasecmp(tok[1], "disc") == 0) {
            SB_LOGI("Disconnecting scale...");
            g_scale.disconnect();
        } else {
            SB_LOGI("scale: %s %s [%s] mac=%s batt=%d%% updates=%u cutoff_events=%u slope=%.2f g/s ble_init=%s",
                    g_scale.stateName(), g_scale.deviceName(), scaleLinkName(g_scale.link()),
                    g_scale.mac(), g_scale.battery(), (unsigned)g_scale.updates(),
                    (unsigned)g_scale.cutoffEvents(), g_scale.slopeGs(),
                    g_scale.bleInitialized() ? "yes" : "deferred");
            SB_LOGI("Usage: scale | scale scan | scale pair <mac|AUTO> | scale disc");
        }
        return;
    }

    // ---- power stage ----------------------------------------------------------------------------
#if !defined(SB_BENCH) || (SB_BENCH == 0)
    if (strcasecmp(c0, "duty") == 0 || strcasecmp(c0, "sim") == 0 || strcasecmp(c0, "press") == 0 ||
        strcasecmp(c0, "weight") == 0 || strcasecmp(c0, "set") == 0 || strcasecmp(c0, "manual") == 0 ||
        strcasecmp(c0, "btn") == 0 || strcasecmp(c0, "zero") == 0) {
        conPrint("ERR: bench feature disabled in production build (-DSB_BENCH=1 required)");
        return;
    }
#endif

#if defined(SB_BENCH) && (SB_BENCH != 0)
    if (strcasecmp(c0, "duty") == 0) {
        const float pct = n > 1 ? atof(tok[1]) : 0.0f;
        g_brew.setManualPower(pct);
        SB_LOGI("Duty forced to %.1f %% (BYPASSES the PID - this is a jig command, never use it "
                "on a live machine with the boiler hot)", pct);
        return;
    }
    if (strcasecmp(c0, "btn") == 0) {
        // `btn` presses for the configured length; `btn <ms>` holds the button for up to 30 s, so
        // the opto wiring can be measured with a slow multimeter. Never use the long form on a
        // machine whose logic reads a long press as a different command.
        const uint32_t holdMs = (n > 1) ? (uint32_t)strtoul(tok[1], nullptr, 10) : 0;
        if (holdMs > 30000) conPrint("btn: hold capped at 30000 ms");
        const ButtonResult r = holdMs ? g_button.pulseFor(holdMs) : g_button.pulse();
        SB_LOGI("Virtual button %s (pulse #%u, %u ms)",
                r == ButtonResult::PULSED ? "PULSED" :
                r == ButtonResult::BLOCKED_RETRIGGER ? "BLOCKED (retrigger dead time)" : "BLOCKED",
                (unsigned)g_button.pulses(),
                (unsigned)(holdMs ? (holdMs > 30000 ? 30000 : (holdMs < 20 ? 20 : holdMs))
                                  : g_button.pulseWidthMs()));
        return;
    }
#endif
    if (strcasecmp(c0, "disarm") == 0) {
        g_brew.requestStop("console disarm");
        g_psm.disarm();
        g_button.abort();
        SB_LOGE("Power stage DISARMED. Type 'arm' to re-enable.");
        return;
    }
    if (strcasecmp(c0, "arm") == 0) {
        g_psm.arm();
        g_button.begin();
        SB_LOGI("Power stage ARMED.");
        return;
    }

    // ---- pressure -------------------------------------------------------------------------------
#if defined(SB_BENCH) && (SB_BENCH != 0)
    if (strcasecmp(c0, "sim") == 0) {
        const bool on = n > 1 && (strcasecmp(tok[1], "on") == 0 || strcmp(tok[1], "1") == 0);
        g_brew.setSimulationMode(on);
        SB_LOGI("Simulation mode %s.", on ? "ENGAGED - virtual extraction flow active" : "released");
        return;
    }
    if (strcasecmp(c0, "press") == 0) {
        const float bar = n > 1 ? atof(tok[1]) : 0.0f;
        if (!g_pressure.simulator()) g_pressure.setSimulator(true);
        g_pressure.setSimBar(bar);
        SB_LOGI("Simulated pressure now %.2f bar", bar);
        return;
    }
    if (strcasecmp(c0, "zero") == 0) {
        if (g_brew.state() != BrewState::IDLE) {
            conPrint("ERR: cannot calibrate zero while active (state != IDLE)");
            return;
        }
        if (g_pressure.isCalibrating()) {
            conPrint("ERR: zero calibration already in progress");
            return;
        }
        if (g_pressure.startZeroCalibration(60)) {
            conPrint("Zero calibration started in background (60 samples)...");
        } else {
            conPrint("ERR: failed to start zero calibration");
        }
        return;
    }
#endif

    // ---- shot ------------------------------------------------------------------------------------
    if (strcasecmp(c0, "start") == 0) { g_brew.requestStart(); return; }
    if (strcasecmp(c0, "stop") == 0)  { g_brew.requestStop("console");   return; }
    if (strcasecmp(c0, "tare") == 0)  { g_brew.requestTare(); SB_LOGI("Scale tare requested."); return; }
    if (strcasecmp(c0, "next") == 0)  { g_brew.requestNextPhase(); return; }
#if defined(SB_BENCH) && (SB_BENCH != 0)
    if (strcasecmp(c0, "weight") == 0) {
        const float g = n > 1 ? atof(tok[1]) : 0.0f;
        g_brew.setSimWeight(g);
        SB_LOGI("Injected scale reading %.1f g", g);
        return;
    }
    if (strcasecmp(c0, "set") == 0) {
        const float bar = n > 1 ? atof(tok[1]) : 0.0f;
        g_brew.setPressureOverride(bar);
        return;
    }
    if (strcasecmp(c0, "manual") == 0) {
        const float pct = n > 1 ? atof(tok[1]) : 0.0f;
        g_brew.setManualPower(pct);
        return;
    }
#endif

    // ---- profiles ---------------------------------------------------------------------------------
    if (strcasecmp(c0, "profiles") == 0) { listProfiles(); return; }
    if (strcasecmp(c0, "use") == 0) {
        if (n > 1 && g_brew.loadProfile(tok[1])) {
            File idx = LittleFS.open("/profiles/index.json.tmp", "w");
            if (idx) {
                idx.printf("{\"active\":\"%s\"}", tok[1]);
                idx.close();
                LittleFS.rename("/profiles/index.json.tmp", "/profiles/index.json");
            }
            printPhaseTable();
        } else {
            SB_LOGE("Usage: use <id>");
        }
        return;
    }
    if (strcasecmp(c0, "phase") == 0) { printPhaseTable(); return; }

    // ---- wifi -------------------------------------------------------------------------------------
    if (strcasecmp(c0, "wifi") == 0) {
        if (n >= 3) {
            SB_LOGI("Setting Wi-Fi credentials for SSID '%s'...", tok[1]);
            g_wifi.saveCredentials(tok[1], tok[2]);
        } else {
            SB_LOGI("Wi-Fi status: %s (IP: %s, RSSI: %d dBm)", g_wifi.modeName(), g_wifi.ip(), g_wifi.rssi());
            SB_LOGI("Usage: wifi <ssid> <password>");
        }
        return;
    }
    if (strcasecmp(c0, "i2c") == 0) {
        // While the ADS1115 is in use the control task reads it at 100 Hz on Core 1; a scan from
        // here would share the bus with it. Only scan when the internal ADC is the source.
        if (g_pressure.source() == PressureSource::ADS1115) {
            SB_LOGI("I2C: ADS1115 is in use (%s) - the bus works, nothing to diagnose.",
                    g_pressure.sourceName());
            return;
        }
        // 1. Idle line levels with the ESP32's own pull-ups off. Both lines must read HIGH: the
        //    ADS1115 breakout's pull-up resistors hold them at 3.3 V. LOW means no power on the
        //    breakout, no pull-ups, a short to GND, or the wire is on another pin.
        Wire.end();
        pinMode(PIN_I2C_SDA, INPUT);
        pinMode(PIN_I2C_SCL, INPUT);
        delay(2);
        const int sda = digitalRead(PIN_I2C_SDA);
        const int scl = digitalRead(PIN_I2C_SCL);
        SB_LOGI("I2C idle levels: SDA (GPIO %d) = %s, SCL (GPIO %d) = %s", PIN_I2C_SDA,
                sda ? "HIGH ok" : "LOW", PIN_I2C_SCL, scl ? "HIGH ok" : "LOW");
        if (!sda || !scl) {
            SB_LOGW("A LOW line: check the breakout has VDD on 3.3 V and GND, that the wire goes to "
                    "the GPIO above, and that nothing shorts it to GND.");
        }
        // 2. Scan every 7-bit address at 100 kHz, which tolerates long or weak wiring.
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
        Wire.setTimeOut(50);
        int found = 0;
        for (uint8_t a = 0x08; a <= 0x77; ++a) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) {
                const bool ads = (a >= 0x48 && a <= 0x4B);
                SB_LOGI("I2C: device answers at 0x%02X%s", a,
                        ads ? " (ADS1115 address range)" : "");
                found++;
            }
        }
        if (found == 0) {
            SB_LOGW("I2C: no device answered. If both lines read HIGH, SDA and SCL may be swapped "
                    "(SDA must be GPIO %d, SCL GPIO %d).", PIN_I2C_SDA, PIN_I2C_SCL);
        } else {
            SB_LOGI("I2C: %d device(s). An ADS1115 at 0x48-0x4B is used after a reboot.", found);
        }
        Wire.setClock(400000);
        return;
    }
    if (strcasecmp(c0, "portal") == 0) {
        SB_LOGI("Starting SoftAP captive setup portal...");
        g_wifi.startPortal();
        return;
    }
    if (strcasecmp(c0, "scan") == 0) {
        SB_LOGI("Scanning 2.4 GHz Wi-Fi networks...");
        const int16_t nNet = WiFi.scanNetworks(false, true);
        if (nNet <= 0) {
            SB_LOGW("No 2.4 GHz networks found");
        } else {
            SB_LOGI("Found %d network(s):", (int)nNet);
            for (int16_t i = 0; i < nNet; i++) {
                SB_LOGI("  [%2d] %-30s RSSI:%4d dBm  CH:%2d  %s",
                        (int)(i + 1), WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SECURE");
            }
        }
        WiFi.scanDelete();
        return;
    }

    SB_LOGW("Unknown command '%s'. Type 'help'.", c0);
}

static void serviceConsole() {
    while (Serial.available() || Serial0.available()) {
        const int c = Serial.available() ? Serial.read() : Serial0.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            conBuf[conLen] = '\0';
            handleConsoleLine(conBuf);
            conLen = 0;
            conLast = millis();
        } else if (c == 8 || c == 127) {
            if (conLen) conLen--;                 // backspace
        } else if (conLen < sizeof(conBuf) - 1) {
            conBuf[conLen++] = (char)c;
            sb_log::putRaw((const char*)&conBuf[conLen - 1]);
        }
    }

    // Nudge the prompt if the operator pressed enter and never typed.
    if (conLen && millis() - conLast > 2500) {
        sb_log::putRaw("\n");
        conLen = 0;
    }
}

// =================================================================================================
// FILESYSTEM SEEDING
// =================================================================================================
static bool mountLittleFS(bool formatOnFail = true) {
    if (LittleFS.begin(formatOnFail, "/littlefs", 10, "spiffs")) return true;
    if (LittleFS.begin(formatOnFail, "/littlefs", 10, "littlefs")) return true;
    return false;
}

static void seedFilesystem() {
    if (!mountLittleFS(true)) {
        SB_LOGE("LittleFS mount failed even after format.");
        return;
    }

    // Seed default profiles on first boot.
    if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");

    static const char* kSeedProfileIds[] = {
        "blooming_allonge", "classic_9bar", "turbo_shot", "gentle_lever", "italian_dark",
        "londinium", "iuiuiu_classic", "adaptive", "blooming_flow", "slow_pi_flat"
    };

    // A built-in profile is (re)written only when
    //   - the file does not exist,
    //   - the file is a seed from an older SB_SEED_PROFILE_VERSION, or
    //   - the file is byte-identical to the built-in preset but has no seed_version yet: an
    //     untouched seed from before seed versions existed. It is tagged so later updates reach it.
    // A file without seed_version that differs from the preset belongs to the operator (the save
    // endpoint strips seed_version) and is never touched. This replaces a content heuristic that
    // rewrote blooming_allonge on every boot whenever it contained "pressure_le" - which silently
    // undid any edit that added such a trigger.
    for (const char* pid : kSeedProfileIds) {
        const char* pJson = ProfileEngine::getPresetJson(pid);
        if (!pJson) continue;
        char pPath[64];
        snprintf(pPath, sizeof(pPath), "/profiles/%s.json", pid);

        const char* why = nullptr;
        if (!LittleFS.exists(pPath)) {
            why = "seeded";
        } else {
            File existing = LittleFS.open(pPath, "r");
            if (existing) {
                const String content = existing.readString();
                existing.close();
                const int fileSeed = ProfileEngine::seedVersionOf(content);
                if (fileSeed >= 0 && fileSeed < ProfileEngine::SB_SEED_PROFILE_VERSION) {
                    why = "updated to the current built-in version";
                } else if (fileSeed < 0 && content == pJson) {
                    why = "tagged as an untouched built-in";
                }
            }
        }
        if (!why) continue;

        const String seed = ProfileEngine::seedJson(pid);
        char tmpPath[72];
        snprintf(tmpPath, sizeof(tmpPath), "/profiles/%s.json.tmp", pid);
        File f = LittleFS.open(tmpPath, "w");
        if (!f) continue;
        f.print(seed);
        f.close();
        if (!LittleFS.rename(tmpPath, pPath)) {
            LittleFS.remove(pPath);
            LittleFS.rename(tmpPath, pPath);
        }
        SB_LOGI("Profile %s %s (seed v%d)", pPath, why, ProfileEngine::SB_SEED_PROFILE_VERSION);
    }

    if (!LittleFS.exists("/profiles/index.json")) {
        File f = LittleFS.open("/profiles/index.json", "w");
        if (f) { f.print("{\"active\":\"classic_9bar\"}"); f.close(); }
    }
    if (!LittleFS.exists("/history")) LittleFS.mkdir("/history");
}

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "usb_phy.h"

// =================================================================================================
void setup() {
    // Keep USB CDC+JTAG clock alive when PHY initializes
    sb_usb_phy_keepalive();

    // ---- console first so we can report everything that follows -------------------------------
    Serial.begin(115200);
    Serial0.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 1500) { delay(10); }
    sb_usb_phy_keepalive();

    // ---- WDT + panic first so background tasks (log_drain) can subscribe immediately ------------
    // The one place the task-watchdog timeout is set. Arduino-ESP32 links a precompiled IDF, so a
    // CONFIG_ESP_TASK_WDT_TIMEOUT_S build flag cannot change it; this runtime call can.
    // Arduino-ESP32 3.x starts the watchdog before setup(), so it is reconfigured here; the idle-task
    // subscriptions are kept as the precompiled sdkconfig set them.
    {
        const esp_task_wdt_config_t wdtCfg = {
            .timeout_ms     = 5000,  // 5-second task watchdog
            .idle_core_mask = 0
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
                              | (1U << 0)
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
                              | (1U << 1)
#endif
            ,
            .trigger_panic  = true,
        };
        if (esp_task_wdt_reconfigure(&wdtCfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_init(&wdtCfg);
    }
    esp_task_wdt_add(nullptr);            // subscribe this (loop) task

    sb_log::initAsync();

    // Mount storage first so banner and config can read partition stats
    seedFilesystem();

    const esp_reset_reason_t rstReason = esp_reset_reason();

    SB_BANNER("");
    SB_BANNER("  UNIVERSAL ESPRESSO PRESSURE & FLOW PROFILING CONTROLLER");
    SB_LOGI("  firmware %s (build %s)   chip %s rev %d   %d cores @ %d MHz",
            SB_FW_VERSION, SB_BUILD_REV, ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
            ESP.getCpuFreqMHz());
    SB_LOGI("  flash %u MB   PSRAM %u MB   littlefs %u kB   reset: %s (%d)",
            (unsigned)(ESP.getFlashChipSize() >> 20),
            (unsigned)(ESP.getPsramSize() >> 20),
            (unsigned)(LittleFS.totalBytes() / 1024),
            resetReasonStr(rstReason), (int)rstReason);
    SB_BANNER("");

    // ---- storage --------------------------------------------------------------------------------
    g_config.begin();
    g_history.begin();
    g_profile.begin();
    g_profile.loadActive(nullptr);

    // ---- hardware --------------------------------------------------------------------------------
    g_button.begin();
    g_psm.begin();
    g_pressure.begin();
    g_pid.begin();
    g_brew.begin();
    g_statusLed.begin();

    // ---- radio + services ---------------------------------------------------------------------------
    SB_LOGI("Init: starting g_wifi.begin...");
    g_wifi.begin(&g_http, "SmartBrew");
    SB_LOGI("Init: starting g_scale.begin...");
    g_scale.begin();
    SB_LOGI("Init: starting g_ota.begin...");
    g_ota.begin();
    SB_LOGI("Init: starting g_web.begin...");
    g_web.begin(&g_http, 80);
    SB_LOGI("Init: starting g_aux.begin...");
    g_aux.begin();
    SB_LOGI("Init: all radio services initialized.");

    // ---- tasks ----------------------------------------------------------------------------------------
    // Core 1: dedicated real-time control loop (PSM + PID + safety)
    xTaskCreatePinnedToCore(taskControl,   "ctrl", 6144, nullptr, 5, &hCtrl, 1);
    // Core 0: telemetry broadcast (WebSocket + UART1)
    xTaskCreatePinnedToCore(taskTelemetry, "tele", 6144, nullptr, 3, &hTele, 0);
    // Core 0: BLE scale state machine (blocking NimBLE client calls live here, not in "tele")
    xTaskCreatePinnedToCore(taskBle,       "ble",  8192, nullptr, 2, &hBle,  0);

    SB_BANNER("");
    SB_LOGI("Tasks: ctrl(Core 1, 100Hz) tele(Core 0, 20/10/2 Hz) ble(Core 0, 20 Hz) loop(Core 1, services) - running.");
    SB_LOGI("Profile '%s' with %u phase(s). Press start to begin a shot.",
            g_profile.profile().name, g_profile.phaseCount());
    if (g_psm.pulsesAccepted() == 0) {
        SB_LOGI("No zero-cross signal yet - awaiting mains signal. On the bench, drive a");
        SB_LOGI("50 Hz / 60 Hz pulse train into the zero-cross input, or run `sim on` for a software-only test.");
    }
    SB_BANNER("");
    printHelp();
}

// =================================================================================================
void loop() {
    esp_task_wdt_reset();
    g_wifi.loop();
    serviceConsole();
    g_statusLed.update(millis(),
                       g_brew.state() == BrewState::BREWING,
                       g_brew.state() == BrewState::IDLE && g_brew.faults() == 0,
                       g_brew.faults() != 0);
    vTaskDelay(pdMS_TO_TICKS(5));
}