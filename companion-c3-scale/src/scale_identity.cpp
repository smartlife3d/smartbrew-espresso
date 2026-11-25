#include "scale_identity.h"

#include <Preferences.h>
#include <esp_mac.h>
#include <string.h>

#include "scale_config.h"

static const char* kNvsNamespace = "scaleid";
static const char* kNvsKeyName   = "btname";

static char       s_name[SCALE_NAME_MAX + 1] = {0};
static char       s_shortId[5]                = {0};
static Preferences s_prefs;

// Printable ASCII, no leading/trailing spaces, non-empty, <= SCALE_NAME_MAX.
static bool nameIsValid(const char* in) {
    if (in == nullptr) return false;

    size_t len = 0;
    while (in[len] != '\0' && len <= SCALE_NAME_MAX) len++;
    if (len == 0 || len > SCALE_NAME_MAX) return false;

    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    if (in[0] == ' ' || in[len - 1] == ' ') return false;
    return true;
}

static void copyName(char* dst, size_t dstSize, const char* src) {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

void scaleIdentityInit(void) {
    // Per-unit id from the factory MAC: last 2 bytes, upper nibble of the last.
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        strncpy(s_shortId, "0000", sizeof(s_shortId));
    } else {
        snprintf(s_shortId, sizeof(s_shortId), "%02X%02X", mac[4], mac[5]);
    }

    copyName(s_name, sizeof(s_name), SCALE_DEVICE_NAME);

    s_prefs.begin(kNvsNamespace, false);
    const String stored = s_prefs.getString(kNvsKeyName, "");
    if (stored.length() > 0 && nameIsValid(stored.c_str())) {
        copyName(s_name, sizeof(s_name), stored.c_str());
        Serial.printf("[id] stored BLE name \"%s\"\n", s_name);
    } else {
        Serial.printf("[id] compiled BLE name \"%s\" (no override stored)\n", s_name);
    }
    Serial.printf("[id] unit %s  (service 0x%04X / notify 0x%04X / write 0x%04X)\n",
                  s_shortId, SCALE_SERVICE_UUID, BOOKOO_NOTIFY_UUID, BOOKOO_WRITE_UUID);
}

const char* scaleDeviceName(void) { return s_name; }
const char* scaleIdentityShortId(void) { return s_shortId; }

bool scaleIdentitySetName(const char* name) {
    if (!nameIsValid(name)) return false;
    s_prefs.putString(kNvsKeyName, name);
    Serial.printf("[id] stored \"%s\" - reboot to advertise it\n", name);
    return true;
}

void scaleIdentityClearName(void) {
    s_prefs.remove(kNvsKeyName);
    Serial.printf("[id] cleared stored name - reboot to advertise \"%s\"\n", SCALE_DEVICE_NAME);
}