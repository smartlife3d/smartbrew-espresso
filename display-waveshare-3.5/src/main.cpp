#include <Arduino.h>
#include <string.h>
#include "display_driver.h"
#include "ui_screen.h"
#include "network_client.h"
#include "screensaver.h"

// Background Network & Telemetry Task (Pinned to ESP32 Core 0)
// Pure network ingestion: NEVER touches LVGL or waits for display flushes
void networkTask(void* pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(10); // 100 Hz loop for responsive WS reception

    for (;;) {
        updateNetworkLoop();
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=======================================================");
    Serial.println(" SmartBrew Client Display - Waveshare 3.5\" Touch-LCD-B ");
    Serial.println(" ESP32-S3R8 (16MB Flash, 8MB PSRAM, AXS15231B QSPI)    ");
    Serial.println("=======================================================");

    // 1. Initialize Network Client & Telemetry Mutex before UI constructs
    initNetworkClient();

    // 2. Initialize Waveshare 3.5B QSPI Display, AXP2101 PMIC, TCA9554 & Touch
    initDisplayAndTouch();

    // 3. Construct LVGL 8.3 Cockpit Interface (matching Web & Android displays)
    createEspressoUi();

    // 3b. Screen saver: a second LVGL screen that covers the cockpit after a
    //     period without any touch.  A touch brings the cockpit back as it was.
    screensaverInit(lv_scr_act());

    // 4. Spawn Background Network & Telemetry Task on Core 0
    xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        16384,
        nullptr,
        1,
        nullptr,
        0 // Pinned to Core 0
    );

    Serial.println("[System] Client Display Ready. Dual-core pipeline active.");
}

void loop() {
    static uint32_t lastUiTick = 0;
    uint32_t now = millis();

    // Independent steady UI update decoupled from Wi-Fi packet jitter
    if (now - lastUiTick >= 30) { // Steady ~33 Hz / 30 ms tick
        lastUiTick = now;

        static TelemetryData snap;
        bool haveSnap = false;

        if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            snap = g_telemetry;
            haveSnap = true;
            g_telemetry.profiles_updated = false;
            xSemaphoreGive(g_telemetryMutex);
        }

        if (haveSnap) {
            updateEspressoUiTelemetry(snap);
        }

        // Screen saver: idle detection plus the "the machine is working, keep
        // the cockpit on screen" rule.  A controller that is not connected is
        // not treated as busy - otherwise a connection lost during a shot
        // would hold the screen saver off for good.
        {
            const bool machineBusy =
                snap.mcu_connected &&
                (snap.brew_active ||
                 strcmp(snap.state, "STARTING") == 0 ||
                 strcmp(snap.state, "BREWING") == 0 ||
                 strcmp(snap.state, "FLUSHING") == 0 ||
                 strcmp(snap.state, "STOPPING") == 0);
            screensaverTick(now, machineBusy);
        }

        // The brew ledger the Controller holds. The copy is taken under the mutex and drawn only
        // while the ledger is open - there is no reason to copy it while nobody is looking.
        if (historyLedgerVisible()) {
            HistoryData hist;
            if (getHistorySnapshot(hist)) {
                updateEspressoUiHistory(hist);
            }
        }
    }

    // Core 1 is the sole owner of LVGL: no cross-core lock contention or flush stalls
    lv_timer_handler();
    delay(5);
}
