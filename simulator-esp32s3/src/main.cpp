// =================================================================================================
//  main.cpp - ESP32-S3 Hardware-In-The-Loop (HIL) Simulator for Espresso Controller
// =================================================================================================
#include <Arduino.h>
#include "sim_pins.h"
#include "sim_dimmer.h"
#include "sim_button.h"
#include "sim_pressure.h"
#include "sim_ble_scale.h"
#include "sim_physics.h"

namespace {
uint32_t s_lastDashboardMs = 0;
uint32_t s_lastLedMs = 0;
String   s_inputBuffer = "";

// -------------------------------------------------------------------------------------------------
// Onboard status LED: blue = booting, green = idle, cyan = brewing.
// -------------------------------------------------------------------------------------------------
void updateStatusLed(bool booting) {
#if defined(PIN_SIM_STATUS_LED) && (PIN_SIM_STATUS_LED >= 0)
    const uint32_t now = millis();
    if (!booting && (now - s_lastLedMs < 250)) return;
    s_lastLedMs = now;
    if (booting) {
        neopixelWrite(PIN_SIM_STATUS_LED, 0, 0, 40);
    } else if (g_simButton.isBrewing()) {
        neopixelWrite(PIN_SIM_STATUS_LED, 0, 60, 60);   // cyan
    } else {
        neopixelWrite(PIN_SIM_STATUS_LED, 0, 40, 0);    // green
    }
#endif
}

void printHelp() {
    Serial.println(F("\n--- ESP32-S3 HIL SIMULATOR COMMANDS ---"));
    Serial.println(F("  start / brew     : Simulate barista pressing the machine brew button (starts ZC)"));
    Serial.println(F("  stop             : Simulate barista pressing the machine brew button (stops ZC)"));
    Serial.println(F("  toggle           : Toggle simulated machine brew state"));
    Serial.println(F("  tare             : Reset simulated scale weight to 0.00 g"));
    Serial.println(F("  tail <ms>        : Post-shot weight tail in ms (default 1000, 0 disables it)"));
    Serial.println(F("  weight <g>       : Manually inject scale weight in grams (e.g. 'weight 36.0')"));
    Serial.println(F("  press <bar>      : Lock pressure to a fixed bar (e.g. 'press 9.0', 'press auto')"));
    Serial.println(F("  raw <mv>         : Force the ADC node voltage (e.g. 'raw 50' open wire, 'raw 3200' over range)"));
    Serial.println(F("  raw off          : Clear the raw node voltage injection"));
    Serial.println(F("  puck <type>      : Puck profile: coarse | medium | fine | choked | <ohms>"));
    Serial.println(F("  pump <key>       : Pump model: ulka_ex5 | ulka_ex4 | olab_silent"));
    Serial.println(F("  range <bar>      : Transducer full scale: 12 (controller 1.2 MPa) | 16 (1.6 MPa)"));
    Serial.println(F("  50hz / 60hz      : Switch AC mains frequency simulation"));
    Serial.println(F("  status           : Print current simulator status"));
    Serial.println(F("  help             : Show this help menu\n"));
}

void printBanner() {
    Serial.println(F("==================================================================="));
    Serial.println(F("  ESP32-S3 HARDWARE-IN-THE-LOOP (HIL) TEST RIG & SIMULATOR         "));
    Serial.println(F("  Simulating: AC Dimmer, Transducer, Bookoo Scale & Brew Switch    "));
    Serial.println(F("==================================================================="));
    Serial.println(F("  WIRING CONNECTION TO CONTROLLER ESP32-S3:                       "));
    Serial.println(F("    Controller GPIO 4 (PIN_ZC_IN)      <--- Simulator GPIO 4 (ZC)  "));
    Serial.println(F("    Controller GPIO 5 (PIN_PSM_GATE)   ---> Simulator GPIO 5 (GATE)"));
    Serial.println(F("    Controller GPIO 6 (PIN_BTN_TRIG)   ---> Simulator GPIO 6 (BTN) "));
    Serial.println(F("    Controller GPIO 1 (ADC_FALLBACK)   <--- Simulator GPIO 7 (PWM) "));
    Serial.println(F("    Controller GND                     <=== Simulator GND          "));
    Serial.println(F("  !! The 10k/1uF RC filter between GPIO 7 and GPIO 1 is MANDATORY   "));
    Serial.println(F("===================================================================\n"));
}

void printStatus() {
    const float duty   = g_simDimmer.measuredDutyPct();
    const float bar    = g_simPressure.currentBar();
    const float mv     = g_simPressure.currentMv();
    const float flow   = g_simPhysics.flowMlPerSec();
    const float weight = g_simPhysics.weightGrams();
    const float vol    = g_simPhysics.volumePumpedMl();
    const float resist = g_simPhysics.puckResistance();
    const SimPumpPreset& pump = g_simPhysics.pump();

    Serial.println(F("---------------- SIMULATOR STATUS ----------------"));
    Serial.printf("  Machine state   : %s\n", g_simButton.isBrewing() ? "BREWING" : "IDLE");
    Serial.printf("  Mains           : %u Hz  (ZC %lu pulses, %s)\n",
                  (unsigned)g_simDimmer.mainsHz(), (unsigned long)g_simDimmer.zcPulseCount(),
                  g_simDimmer.isMainsActive() ? "ACTIVE" : "idle");
    Serial.printf("  Measured duty   : %.1f %%  (gate pulses: %lu)\n",
                  duty, (unsigned long)g_simDimmer.gatePulseCount());
    Serial.printf("  Pressure        : %.2f bar  (%.0f mV at the ADC node)\n", bar, mv);
    Serial.printf("  Flow            : %.2f ml/s   (%.1f ml pumped)\n", flow, vol);
    Serial.printf("  Cup weight      : %.2f g%s\n", weight,
                  g_simPhysics.tailActive() ? "  (post-shot tail running)" : "");
    Serial.printf("  Post-shot tail  : %lu ms\n", (unsigned long)g_simPhysics.tailMs());
    Serial.printf("  Puck resistance : %.3f  (grind: %s)\n", resist,
                  g_simPhysics.grind() == PuckGrind::COARSE ? "coarse" :
                  g_simPhysics.grind() == PuckGrind::MEDIUM ? "medium" :
                  g_simPhysics.grind() == PuckGrind::FINE   ? "fine" : "choked");
    Serial.printf("  Pump model      : %s (minAct %.0f%%, 9bar @ %.0f%%, %.1f ml/s, stall %.1f bar)\n",
                  pump.name, pump.minActivationPct, pump.holdNineBarPct, pump.maxFlowMlS, pump.stallBar);
    Serial.printf("  Transducer range: %.1f bar full scale  -> controller MUST use %s\n",
                  g_simPressure.fullScaleBar(),
                  (g_simPressure.fullScaleBar() > 14.0f) ? "1.6 MPa" : "1.2 MPa");
    Serial.printf("  Pressure source : %s\n", g_simPressure.isManualOverride() ? "MANUAL OVERRIDE" : "hydraulic model");
    Serial.printf("  BLE scale       : %s%s  (notifies: %lu, tares: %lu)\n",
                  g_simBleScale.isConnected() ? "CONNECTED" : "disconnected",
                  g_simBleScale.isSubscribed() ? " (subscribed)" : "",
                  (unsigned long)g_simBleScale.notifyCount(), (unsigned long)g_simBleScale.tareCount());
    Serial.printf("  Controller btn  : %lu pulses, last %u ms\n",
                  (unsigned long)g_simButton.controllerPulseCount(),
                  (unsigned)g_simButton.lastPulseDurationMs());
    Serial.println(F("---------------------------------------------------"));
}

void handleCommand(const String& cmdLine) {
    String cmd = cmdLine;
    cmd.trim();
    if (cmd.length() == 0) return;

    // Split into a verb and an argument so both can be handled case-insensitively.
    int sp = cmd.indexOf(' ');
    if (sp < 0) sp = cmd.indexOf('\t');
    String verb = (sp < 0) ? cmd : cmd.substring(0, sp);
    String arg = (sp < 0) ? String("") : cmd.substring(sp + 1);
    verb.toLowerCase();
    arg.toLowerCase();
    arg.trim();

    if (verb == "start" || verb == "brew") {
        g_simButton.setBrewing(true, "CLI Command");
    } else if (verb == "stop") {
        g_simButton.setBrewing(false, "CLI Command");
    } else if (verb == "toggle") {
        g_simButton.toggleMachine("CLI Command");
    } else if (verb == "tare") {
        g_simPhysics.requestTare();
        g_simPhysics.setWeight(0.0f);
        Serial.println(F("[CLI] Scale tared to 0.00 g"));
    } else if (verb == "tail") {
        if (arg.length() == 0) {
            Serial.printf("[CLI] Post-shot weight tail: %lu ms%s\n",
                          (unsigned long)g_simPhysics.tailMs(),
                          g_simPhysics.tailActive() ? " (running)" : "");
        } else {
            long ms = arg.toInt();
            if (ms < 0) ms = 0;
            if (ms > 60000) ms = 60000;
            g_simPhysics.setTailMs((uint32_t)ms);
            Serial.printf("[CLI] Post-shot weight tail set to %ld ms\n", ms);
        }
    } else if (verb == "weight") {
        if (arg.length() == 0) {
            Serial.println(F("[CLI] Usage: weight <grams>"));
        } else {
            const float w = arg.toFloat();
            g_simPhysics.setWeight(w);
            Serial.printf("[CLI] Scale weight manually set to %.2f g\n", w);
        }
    } else if (verb == "press") {
        if (arg.length() == 0) {
            Serial.println(F("[CLI] Usage: press <bar> | press auto"));
        } else if (arg == "auto") {
            g_simPressure.setManualOverride(false);
        } else {
            const float b = arg.toFloat();
            g_simPressure.setManualOverride(true, b);
        }
    } else if (verb == "raw") {
        if (arg.length() == 0) {
            Serial.printf("[CLI] Node voltage is %.0f mV\n", g_simPressure.currentMv());
        } else if (arg == "off" || arg == "auto") {
            g_simPressure.setRawMvOverride(false);
        } else {
            g_simPressure.setRawMvOverride(true, arg.toFloat());
        }
    } else if (verb == "puck") {
        if (arg == "coarse") {
            g_simPhysics.setGrind(PuckGrind::COARSE);
            Serial.println(F("[CLI] Puck grind set to COARSE (fast flow, low backpressure)"));
        } else if (arg == "medium") {
            g_simPhysics.setGrind(PuckGrind::MEDIUM);
            Serial.println(F("[CLI] Puck grind set to MEDIUM (standard 9-bar shot)"));
        } else if (arg == "fine") {
            g_simPhysics.setGrind(PuckGrind::FINE);
            Serial.println(F("[CLI] Puck grind set to FINE (restricted flow, high backpressure)"));
        } else if (arg == "choked") {
            g_simPhysics.setGrind(PuckGrind::CHOKED);
            Serial.println(F("[CLI] Puck grind set to CHOKED (near-zero flow, max pump pressure)"));
        } else {
            const float r = arg.toFloat();
            if (r > 0.0f) {
                g_simPhysics.setResistance(r);
                Serial.printf("[CLI] Puck resistance manually set to %.3f\n", r);
            } else {
                Serial.println(F("[CLI] Usage: puck <coarse|medium|fine|choked|ohms>"));
            }
        }
    } else if (verb == "pump") {
        bool found = false;
        for (uint8_t i = 0; i < SIM_PUMP_COUNT; ++i) {
            String key = String(SIM_PUMPS[i].key);
            key.toLowerCase();
            if (arg == key) {
                g_simPhysics.setPumpIndex(i);
                Serial.printf("[CLI] Pump model -> %s\n", SIM_PUMPS[i].name);
                found = true;
                break;
            }
        }
        if (!found) {
            Serial.printf("[CLI] Unknown pump '%s'. Known: ulka_ex5, ulka_ex4, olab_silent\n", arg.c_str());
        }
    } else if (verb == "range") {
        if (arg.length() == 0) {
            Serial.printf("[CLI] Transducer full scale is %.1f bar\n", g_simPressure.fullScaleBar());
        } else {
            g_simPressure.setFullScaleBar(arg.toFloat());
        }
    } else if (verb == "50hz") {
        g_simDimmer.setMainsHz(50);
        Serial.println(F("[CLI] Mains simulation set to 50 Hz (100 Hz ZC pulses)"));
    } else if (verb == "60hz") {
        g_simDimmer.setMainsHz(60);
        Serial.println(F("[CLI] Mains simulation set to 60 Hz (120 Hz ZC pulses)"));
    } else if (verb == "status") {
        printStatus();
    } else if (verb == "help" || verb == "?") {
        printHelp();
    } else {
        Serial.printf("[CLI] Unknown command: '%s'. Type 'help' for commands.\n", verb.c_str());
    }
}
} // namespace

void setup() {
    // Immediately hold output pins LOW before USB CDC enumeration delay
    pinMode(PIN_SIM_ZC_OUT, OUTPUT);
    digitalWrite(PIN_SIM_ZC_OUT, LOW);
    pinMode(PIN_SIM_PRESSURE_PWM, OUTPUT);
    digitalWrite(PIN_SIM_PRESSURE_PWM, LOW);

    Serial.begin(115200);
    // Allow USB CDC time to attach
    delay(1000);

    updateStatusLed(true);   // blue = booting
    printBanner();

    Serial.println(F("[INIT] Initializing Simulator Dimmer Subsystem (GPIO 4 ZC, GPIO 5 Gate)..."));
    g_simDimmer.begin(50);

    Serial.println(F("[INIT] Initializing Machine State & Brew Button (GPIO 6 BTN, GPIO 0 Barista)..."));
    g_simButton.begin();

    Serial.println(F("[INIT] Initializing Simulated Pressure Transducer (GPIO 7 PWM)..."));
    g_simPressure.begin();

    Serial.println(F("[INIT] Initializing Bookoo BLE Scale Peripheral (Service 0x0FFE)..."));
    g_simBleScale.begin("Bookoo_SIM");

    Serial.println(F("[INIT] Initializing Real-Time Hydraulic & Extraction Physics Model..."));
    g_simPhysics.begin();

    Serial.printf("[INIT] Transducer full scale: %.1f bar (set 'range 16' if the controller is "
                  "configured for 1.6 MPa)\n", g_simPressure.fullScaleBar());

    Serial.println(F("[INIT] HIL Simulator Ready! Type 'help' for interactive console.\n"));
}

void loop() {
    // 1. Process Virtual Brew Button (GPIO 6) and Barista Button (GPIO 0)
    g_simButton.update();

    // 2. Measure PSM Gate Conduction & Duty Cycle from Controller (GPIO 5)
    g_simDimmer.update();

    // 3. Step Real-Time Hydraulic & Puck Physics Model (100 Hz)
    g_simPhysics.update();

    // 4. Update Pressure Hardware Voltage (GPIO 7)
    g_simPressure.update();

    // 5. Stream BLE Scale Weight Frames (33 Hz)
    g_simBleScale.update();

    // 6. Handle Serial CLI Input
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (s_inputBuffer.length() > 0) {
                handleCommand(s_inputBuffer);
                s_inputBuffer = "";
            }
        } else if (s_inputBuffer.length() < 64) {
            s_inputBuffer += c;
        }
    }

    // 7. Periodic Live Status Dashboard
    // The post-shot weight tail lasts about a second, so the idle refresh rate of 1.5 s would
    // skip it entirely. Refresh fast while the tail is running so the yield climbing into the cup
    // is actually visible on the console.
    const uint32_t now = millis();
    const bool tailRunning = g_simPhysics.tailActive();
    const uint32_t refreshRateMs = g_simButton.isBrewing() ? 500 : (tailRunning ? 150 : 1500);
    if (now - s_lastDashboardMs >= refreshRateMs) {
        s_lastDashboardMs = now;

        const char* stateStr = g_simButton.isBrewing() ? "BREWING" : "IDLE";
        const float duty     = g_simDimmer.measuredDutyPct();
        const float bar      = g_simPressure.currentBar();
        const float mv       = g_simPressure.currentMv();
        const float flow     = g_simPhysics.flowMlPerSec();
        const float weight   = g_simBleScale.weightGrams();
        const bool  bleConn  = g_simBleScale.isConnected();
        const bool  bleSub   = g_simBleScale.isSubscribed();
        const uint32_t zcCnt = g_simDimmer.zcPulseCount();

        Serial.printf("[%s] Dimmer: %5.1f%% | P: %4.1f bar (%4.0f mV) | Q: %4.1f ml/s | W: %5.2f g%s | BLE: %s%s | ZC: %lu\n",
                      stateStr,
                      duty,
                      bar,
                      mv,
                      flow,
                      weight,
                      tailRunning ? "*" : " ",
                      bleConn ? "CONN" : "DISC",
                      bleSub ? "(SUB)" : "",
                      (unsigned long)zcCnt);
    }

    // 8. Status LED
    updateStatusLed(false);
}
