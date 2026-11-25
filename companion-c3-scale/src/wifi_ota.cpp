#include "wifi_ota.h"

#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "scale_config.h"
#include "scale_identity.h"

static char       s_ssid[33] = {0};
static char       s_pass[65] = {0};
static OtaState   s_state      = OtaState::Idle;
static char       s_detail[48] = "not in update mode";
static bool       s_associated  = false;
static uint32_t   s_lastActivity = 0;
static Preferences s_prefs;

// ---------------------------------------------------------------------------
// Credential storage (NVS)
// ---------------------------------------------------------------------------
void wifiOtaInit(void) {
    s_prefs.begin(OTA_NVS_NAMESPACE, false);
    const String ssid = s_prefs.getString(OTA_NVS_KEY_SSID, "");
    const String pass = s_prefs.getString(OTA_NVS_KEY_PASS, "");
    if (ssid.length() > 0) {
        strncpy(s_ssid, ssid.c_str(), sizeof(s_ssid) - 1);
        strncpy(s_pass, pass.c_str(), sizeof(s_pass) - 1);
    }
}

bool wifiOtaHasCredentials(void) { return s_ssid[0] != '\0'; }
const char* wifiOtaSsid(void) { return s_ssid; }

bool wifiOtaSetCredentials(const char* ssid, const char* pass) {
    if (ssid == nullptr || ssid[0] == '\0' || strlen(ssid) > 32) return false;
    const size_t passLen = (pass == nullptr) ? 0 : strlen(pass);
    if (passLen > 64) return false;

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, (pass == nullptr) ? "" : pass, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';

    s_prefs.putString(OTA_NVS_KEY_SSID, s_ssid);
    s_prefs.putString(OTA_NVS_KEY_PASS, s_pass);
    Serial.printf("[ota] AP stored: ssid=\"%s\" pass=%s\n", s_ssid,
                  s_pass[0] ? "set" : "open");
    return true;
}

void wifiOtaClearCredentials(void) {
    s_prefs.remove(OTA_NVS_KEY_SSID);
    s_prefs.remove(OTA_NVS_KEY_PASS);
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    Serial.println(F("[ota] AP credentials cleared"));
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------
OtaState wifiOtaState(void) { return s_state; }

const char* wifiOtaStateName(void) {
    switch (s_state) {
        case OtaState::Idle:       return "IDLE";
        case OtaState::Connecting: return "CONNECTING";
        case OtaState::Ready:      return "READY";
        case OtaState::Transfer:   return "TRANSFER";
        case OtaState::Failed:     return "FAILED";
        default:                   return "?";
    }
}

const char* wifiOtaStatus(void) { return s_detail; }

// ---------------------------------------------------------------------------
// Leave update mode: drop the AP and go back to advertising
// ---------------------------------------------------------------------------
static void leaveUpdateMode(const char* reason) {
    ArduinoOTA.end();

    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(WiFi.channel(), SCALE_WIFI_SECOND_CHAN);
    s_associated = false;
    s_state = OtaState::Idle;
    strncpy(s_detail, reason, sizeof(s_detail) - 1);
    s_detail[sizeof(s_detail) - 1] = '\0';

    Serial.printf("[ota] left update mode (%s) - BLE advertising resumed\n", reason);
}

// ---------------------------------------------------------------------------
// OTA task: drives ArduinoOTA and the idle timeout
// ---------------------------------------------------------------------------
static void otaTask(void* /*arg*/) {
    char hostname[40];
    snprintf(hostname, sizeof(hostname), "%s%s", OTA_HOSTNAME_PREFIX,
             scaleIdentityShortId());

    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(OTA_PASSWORD);      // change OTA_PASSWORD before selling
    ArduinoOTA.setTimeout(OTA_TIMEOUT_MS);
    ArduinoOTA.setMdnsEnabled(true);

    ArduinoOTA.onStart([]() {
        s_state = OtaState::Transfer;
        strncpy(s_detail, "receiving image", sizeof(s_detail) - 1);
        Serial.printf("[ota] update started from %s, switching slot\n",
                      ArduinoOTA.getHostname().c_str());
    });
    ArduinoOTA.onProgress([](unsigned int current, unsigned int total) {
        static int lastPct = -1;
        const int pct = (total > 0) ? (int)((current * 100UL) / total) : 0;
        if (pct != lastPct && (pct % 10) == 0) {
            lastPct = pct;
            Serial.printf("[ota] %d%%\n", pct);
        }
    });
    ArduinoOTA.onEnd([]() {
        Serial.println(F("[ota] update OK - rebooting into the new image"));
    });
    ArduinoOTA.onError([](ota_error_t err) {
        char buf[48];
        snprintf(buf, sizeof(buf), "error %d", (int)err);
        strncpy(s_detail, buf, sizeof(s_detail) - 1);
        s_state = OtaState::Failed;
        Serial.printf("[ota] update failed: %d\n", (int)err);
    });

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(s_ssid, s_pass[0] ? s_pass : nullptr);

    s_state = OtaState::Connecting;
    strncpy(s_detail, "joining AP", sizeof(s_detail) - 1);
    Serial.printf("[ota] joining \"%s\" as %s ...\n", s_ssid, hostname);

    const uint32_t deadline = millis() + OTA_CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - deadline) < OTA_CONNECT_TIMEOUT_MS) {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        strncpy(s_detail, "AP join failed", sizeof(s_detail) - 1);
        s_state = OtaState::Failed;
        Serial.printf("[ota] could not join \"%s\" - returning to broadcast mode\n", s_ssid);
        leaveUpdateMode("AP join failed");
        vTaskDelete(NULL);
        return;
    }

    s_associated = true;
    s_state = OtaState::Ready;
    snprintf(s_detail, sizeof(s_detail), "%s:%d", WiFi.localIP().toString().c_str(),
             (int)OTA_PORT);
    Serial.printf("[ota] READY at %s:%d - run: pio run -t upload --upload-port %s\n",
                  WiFi.localIP().toString().c_str(), (int)OTA_PORT,
                  WiFi.localIP().toString().c_str());
    ArduinoOTA.begin();
    s_lastActivity = millis();

    for (;;) {
        ArduinoOTA.handle();
        if (s_state == OtaState::Transfer) {
            s_lastActivity = millis();
        }
        if ((uint32_t)(millis() - s_lastActivity) > OTA_TIMEOUT_MS) {
            Serial.println(F("[ota] idle timeout"));
            leaveUpdateMode("idle timeout");
            vTaskDelete(NULL);
            return;
        }
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool wifiOtaRequest(void) {
    if (s_state != OtaState::Idle) {
        Serial.println(F("[ota] already in update mode"));
        return false;
    }
    if (!wifiOtaHasCredentials()) {
        Serial.println(F("[ota] no AP stored - use: wifi <ssid> <password>"));
        return false;
    }
    if (xTaskCreate(otaTask, "ota", OTA_TASK_STACK, nullptr, 3, nullptr) != pdPASS) {
        Serial.println(F("[ota] cannot start OTA task"));
        return false;
    }
    return true;
}