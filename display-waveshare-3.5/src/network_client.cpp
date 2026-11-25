#include "network_client.h"

// Shared with the controller firmware - the binary telemetry frame layout.
#include "telemetry_wire.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_mac.h>
#include <vector>
#include <memory>

TelemetryData g_telemetry;
SemaphoreHandle_t g_telemetryMutex = nullptr;

static WebSocketsClient webSocket;
static SemaphoreHandle_t s_wsTxMutex = nullptr;

static bool safeWsSendTXT(const char* text) {
    if (s_wsTxMutex == nullptr) {
        s_wsTxMutex = xSemaphoreCreateMutex();
    }
    if (s_wsTxMutex != nullptr && xSemaphoreTake(s_wsTxMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool res = webSocket.sendTXT(text);
        xSemaphoreGive(s_wsTxMutex);
        return res;
    }
    return false;
}

static Preferences prefs;
static DNSServer dnsServer;
static WebServer webServer(80);

static bool in_portal_mode = false;
static char portal_ssid[33] = "SmartBrew-Display-Setup";
static uint32_t portal_reboot_at = 0;

static char wifi_ssid[33] = "";
static char wifi_pass[65] = "";
static char mcu_host[64]  = "smartbrew.local";
static IPAddress mcu_ip;                 // 0.0.0.0 until mDNS or a direct IP gives one
static bool mcu_ip_resolved = false;
// The controller address the WebSocket last connected to, for the host it was connected under.
// It is the fallback when mDNS cannot find the controller (mDNS is often unreliable on home
// networks). There is no built-in default address: one network's controller IP is meaningless on
// another, and pointing a display at a stranger's device is worse than saying it is not found.
static IPAddress mcu_last_ip;
static char      mcu_last_host[64] = "";
static constexpr uint32_t kMcuResolveRetryMs = 5000;   // retry while no address is known
static void rememberMcuAddress(const IPAddress& ip);
static uint32_t last_mdns_attempt = 0;
static uint32_t last_profile_sync = 0;
static uint32_t last_wifi_check = 0;
static uint32_t sta_connect_start = 0;

// Asynchronous profile selection & simulation mode state (dispatched on Core 0)
static char s_pendingProfileId[32] = {0};
static bool s_pendingProfileRequest = false;
static bool s_pendingSimRequest = false;
static bool s_pendingSimValue = false;
static uint8_t s_onConnectSyncStep = 0;

// -----------------------------------------------------------------------------
// Profile Cache Storage & Defaults
// -----------------------------------------------------------------------------
struct CachedProfile {
    char id[32];
    char name[48];
    float target_weight;
    float total_duration;
    int step_count;
    ProfileStepData steps[MAX_PROFILE_STEPS];
};

static CachedProfile s_cachedProfiles[MAX_DISPLAY_PROFILES];
static int s_cachedProfileCount = 0;

static uint32_t s_last_local_sim_change = 0;
static uint32_t s_last_local_profile_change = 0;
static uint32_t s_last_local_flush_ms = 0;

static void ensureTelemetryMutex() {
    if (g_telemetryMutex == nullptr) {
        g_telemetryMutex = xSemaphoreCreateMutex();
    }
}

bool getTelemetrySnapshot(TelemetryData& out) {
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = g_telemetry;
        xSemaphoreGive(g_telemetryMutex);
        return true;
    }
    return false;
}

static bool isMcuConnectedSafe() {
    bool conn = false;
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        conn = g_telemetry.mcu_connected;
        xSemaphoreGive(g_telemetryMutex);
    }
    return conn;
}

static bool isBrewActiveSafe() {
    bool active = false;
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        const bool flushing = (strcmp(g_telemetry.state, "FLUSHING") == 0 ||
                               strcmp(g_telemetry.state, "FLUSH") == 0 ||
                               (s_last_local_flush_ms != 0 && (millis() - s_last_local_flush_ms < 1500)));
        active = g_telemetry.brew_active || flushing;
        xSemaphoreGive(g_telemetryMutex);
    }
    return active;
}

static const char* const kPresetProfileIds[] = {
    "classic_9bar",
    "turbo_shot",
    "blooming_allonge",
    "gentle_lever",
    "italian_dark"
};
static const size_t kPresetProfileCount = sizeof(kPresetProfileIds) / sizeof(kPresetProfileIds[0]);

static const char* getPresetDisplayName(const char* id) {
    if (!id || strlen(id) == 0) return nullptr;
    if (strcasecmp(id, "classic_9bar") == 0) return "Classic 9-Bar";
    if (strcasecmp(id, "turbo_shot") == 0) return "Turbo Shot";
    if (strcasecmp(id, "blooming_allonge") == 0) return "Blooming Espresso";
    if (strcasecmp(id, "gentle_lever") == 0) return "Gentle Lever Profile";
    if (strcasecmp(id, "italian_dark") == 0) return "Italian Dark Roast";
    return nullptr;
}

static void loadDefaultProfiles() {
    s_cachedProfileCount = 0;

    // 0: Classic 9-Bar (Index 0 in Web & Android)
    CachedProfile& p0 = s_cachedProfiles[s_cachedProfileCount++];
    strlcpy(p0.id, "classic_9bar", sizeof(p0.id));
    strlcpy(p0.name, "Classic 9-Bar", sizeof(p0.name));
    p0.target_weight = 36.0f;
    p0.total_duration = 37.0f;
    p0.step_count = 4;
    strlcpy(p0.steps[0].name, "Pre-Infusion", sizeof(p0.steps[0].name));
    strlcpy(p0.steps[0].type, "pressure", sizeof(p0.steps[0].type));
    strlcpy(p0.steps[0].curve, "linear", sizeof(p0.steps[0].curve));
    p0.steps[0].start_val = 3.0f; p0.steps[0].end_val = 3.0f; p0.steps[0].duration = 4.0f;
    strlcpy(p0.steps[1].name, "Ramp to 9 Bar", sizeof(p0.steps[1].name));
    strlcpy(p0.steps[1].type, "pressure", sizeof(p0.steps[1].type));
    strlcpy(p0.steps[1].curve, "ease_in_out", sizeof(p0.steps[1].curve));
    p0.steps[1].start_val = 3.0f; p0.steps[1].end_val = 9.0f; p0.steps[1].duration = 3.0f;
    strlcpy(p0.steps[2].name, "Peak Extraction", sizeof(p0.steps[2].name));
    strlcpy(p0.steps[2].type, "pressure", sizeof(p0.steps[2].type));
    strlcpy(p0.steps[2].curve, "linear", sizeof(p0.steps[2].curve));
    p0.steps[2].start_val = 9.0f; p0.steps[2].end_val = 9.0f; p0.steps[2].duration = 20.0f;
    strlcpy(p0.steps[3].name, "Gentle Decline", sizeof(p0.steps[3].name));
    strlcpy(p0.steps[3].type, "pressure", sizeof(p0.steps[3].type));
    strlcpy(p0.steps[3].curve, "ease_out", sizeof(p0.steps[3].curve));
    p0.steps[3].start_val = 9.0f; p0.steps[3].end_val = 6.0f; p0.steps[3].duration = 10.0f;

    // 1: Turbo Shot (Index 1 in Web & Android)
    CachedProfile& p1 = s_cachedProfiles[s_cachedProfileCount++];
    strlcpy(p1.id, "turbo_shot", sizeof(p1.id));
    strlcpy(p1.name, "Turbo Shot", sizeof(p1.name));
    p1.target_weight = 42.0f;
    p1.total_duration = 20.5f;
    p1.step_count = 2;
    strlcpy(p1.steps[0].name, "Fast Ramp", sizeof(p1.steps[0].name));
    strlcpy(p1.steps[0].type, "pressure", sizeof(p1.steps[0].type));
    strlcpy(p1.steps[0].curve, "linear", sizeof(p1.steps[0].curve));
    p1.steps[0].start_val = 2.0f; p1.steps[0].end_val = 6.0f; p1.steps[0].duration = 2.5f;
    strlcpy(p1.steps[1].name, "6-Bar Flat", sizeof(p1.steps[1].name));
    strlcpy(p1.steps[1].type, "pressure", sizeof(p1.steps[1].type));
    strlcpy(p1.steps[1].curve, "constant", sizeof(p1.steps[1].curve));
    p1.steps[1].start_val = 6.0f; p1.steps[1].end_val = 6.0f; p1.steps[1].duration = 18.0f;

    // 2: Blooming Espresso (Index 2 in Web & Android)
    CachedProfile& p2 = s_cachedProfiles[s_cachedProfileCount++];
    strlcpy(p2.id, "blooming_allonge", sizeof(p2.id));
    strlcpy(p2.name, "Blooming Espresso", sizeof(p2.name));
    p2.target_weight = 40.0f;
    p2.total_duration = 51.0f;
    p2.step_count = 4;
    strlcpy(p2.steps[0].name, "Fast Fill", sizeof(p2.steps[0].name));
    strlcpy(p2.steps[0].type, "pressure", sizeof(p2.steps[0].type));
    strlcpy(p2.steps[0].curve, "linear", sizeof(p2.steps[0].curve));
    p2.steps[0].start_val = 3.0f; p2.steps[0].end_val = 3.0f; p2.steps[0].duration = 5.0f;
    strlcpy(p2.steps[1].name, "Zero-Bloom", sizeof(p2.steps[1].name));
    strlcpy(p2.steps[1].type, "power", sizeof(p2.steps[1].type));
    strlcpy(p2.steps[1].curve, "instant", sizeof(p2.steps[1].curve));
    p2.steps[1].start_val = 0.0f; p2.steps[1].end_val = 0.0f; p2.steps[1].duration = 12.0f;
    strlcpy(p2.steps[2].name, "Ramp to Peak", sizeof(p2.steps[2].name));
    strlcpy(p2.steps[2].type, "pressure", sizeof(p2.steps[2].type));
    strlcpy(p2.steps[2].curve, "ease_in_out", sizeof(p2.steps[2].curve));
    p2.steps[2].start_val = 2.0f; p2.steps[2].end_val = 8.5f; p2.steps[2].duration = 4.0f;
    strlcpy(p2.steps[3].name, "Declining Ext", sizeof(p2.steps[3].name));
    strlcpy(p2.steps[3].type, "pressure", sizeof(p2.steps[3].type));
    strlcpy(p2.steps[3].curve, "ease_out", sizeof(p2.steps[3].curve));
    p2.steps[3].start_val = 8.5f; p2.steps[3].end_val = 5.5f; p2.steps[3].duration = 30.0f;

    // 3: Gentle Lever Profile (Index 3 in Web & Android)
    CachedProfile& p3 = s_cachedProfiles[s_cachedProfileCount++];
    strlcpy(p3.id, "gentle_lever", sizeof(p3.id));
    strlcpy(p3.name, "Gentle Lever Profile", sizeof(p3.name));
    p3.target_weight = 36.0f;
    p3.total_duration = 36.0f;
    p3.step_count = 3;
    strlcpy(p3.steps[0].name, "Low Soak", sizeof(p3.steps[0].name));
    strlcpy(p3.steps[0].type, "pressure", sizeof(p3.steps[0].type));
    strlcpy(p3.steps[0].curve, "linear", sizeof(p3.steps[0].curve));
    p3.steps[0].start_val = 1.5f; p3.steps[0].end_val = 1.5f; p3.steps[0].duration = 8.0f;
    strlcpy(p3.steps[1].name, "Spring Ramp", sizeof(p3.steps[1].name));
    strlcpy(p3.steps[1].type, "pressure", sizeof(p3.steps[1].type));
    strlcpy(p3.steps[1].curve, "ease_in_out", sizeof(p3.steps[1].curve));
    p3.steps[1].start_val = 2.0f; p3.steps[1].end_val = 8.5f; p3.steps[1].duration = 3.0f;
    strlcpy(p3.steps[2].name, "Spring Decay", sizeof(p3.steps[2].name));
    strlcpy(p3.steps[2].type, "pressure", sizeof(p3.steps[2].type));
    strlcpy(p3.steps[2].curve, "ease_out", sizeof(p3.steps[2].curve));
    p3.steps[2].start_val = 8.5f; p3.steps[2].end_val = 5.0f; p3.steps[2].duration = 25.0f;

    // 4: Italian Dark Roast (Index 4 in Web & Android)
    CachedProfile& p4 = s_cachedProfiles[s_cachedProfileCount++];
    strlcpy(p4.id, "italian_dark", sizeof(p4.id));
    strlcpy(p4.name, "Italian Dark Roast", sizeof(p4.name));
    p4.target_weight = 30.0f;
    p4.total_duration = 28.0f;
    p4.step_count = 2;
    strlcpy(p4.steps[0].name, "Soft Pre-Wet", sizeof(p4.steps[0].name));
    strlcpy(p4.steps[0].type, "pressure", sizeof(p4.steps[0].type));
    strlcpy(p4.steps[0].curve, "linear", sizeof(p4.steps[0].curve));
    p4.steps[0].start_val = 2.0f; p4.steps[0].end_val = 2.0f; p4.steps[0].duration = 3.0f;
    strlcpy(p4.steps[1].name, "9-Bar Flat Pull", sizeof(p4.steps[1].name));
    strlcpy(p4.steps[1].type, "pressure", sizeof(p4.steps[1].type));
    strlcpy(p4.steps[1].curve, "constant", sizeof(p4.steps[1].curve));
    p4.steps[1].start_val = 9.0f; p4.steps[1].end_val = 9.0f; p4.steps[1].duration = 25.0f;
}

static void sanitizeProfileDisplayName(const char* rawName, const char* fallbackId, char* outBuf, size_t outSize) {
    if (!outBuf || outSize == 0) return;
    outBuf[0] = '\0';

    // 1. Check if fallbackId is a known standard preset
    const char* presetName = getPresetDisplayName(fallbackId);
    if (!presetName && rawName) {
        presetName = getPresetDisplayName(rawName);
    }

    // 2. Evaluate if rawName is valid (not empty, not "Profile", "Unnamed", "Default")
    bool rawValid = false;
    if (rawName && strlen(rawName) > 0) {
        if (strcasecmp(rawName, "Profile") != 0 &&
            strcasecmp(rawName, "Unnamed") != 0 &&
            strcasecmp(rawName, "Default") != 0) {
            rawValid = true;
        }
    }

    // If rawName is valid, use it; otherwise prefer canonical presetName or fallbackId
    const char* src = rawValid ? rawName : (presetName ? presetName : fallbackId);
    if (!src || strlen(src) == 0 || strcasecmp(src, "Profile") == 0) {
        if (presetName) {
            strlcpy(outBuf, presetName, outSize);
            return;
        }
        if (fallbackId && strlen(fallbackId) > 0 && strcasecmp(fallbackId, "Profile") != 0) {
            src = fallbackId;
        } else {
            snprintf(outBuf, outSize, "Classic 9-Bar");
            return;
        }
    }

    // Check if src itself is a preset ID (e.g. "classic_9bar")
    const char* pMatch = getPresetDisplayName(src);
    if (pMatch) {
        strlcpy(outBuf, pMatch, outSize);
        return;
    }

    // Strip unprintable / control characters
    char temp[64] = {0};
    size_t tIdx = 0;
    for (size_t i = 0; src[i] != '\0' && tIdx < sizeof(temp) - 1; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c <= 126 && c != '\n' && c != '\r' && c != '\t') {
            temp[tIdx++] = c;
        }
    }
    temp[tIdx] = '\0';

    char* p = temp;
    while (*p == ' ') p++;

    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == ' ') {
        p[--len] = '\0';
    }

    if (len == 0 || strcasecmp(p, "Profile") == 0) {
        if (presetName) {
            strlcpy(outBuf, presetName, outSize);
            return;
        }
        if (fallbackId && strlen(fallbackId) > 0 && strcasecmp(fallbackId, "Profile") != 0) {
            p = (char*)fallbackId;
            len = strlen(p);
        } else {
            snprintf(outBuf, outSize, "Classic 9-Bar");
            return;
        }
    }

    // If it looks like an identifier with underscores/hyphens, format into Title Case
    if (strchr(p, '_') != nullptr || strchr(p, '-') != nullptr) {
        char titleBuf[64] = {0};
        size_t bIdx = 0;
        bool capNext = true;
        for (size_t i = 0; p[i] != '\0' && bIdx < sizeof(titleBuf) - 1; ++i) {
            char c = p[i];
            if (c == '_' || c == '-') {
                if (bIdx > 0 && titleBuf[bIdx - 1] != ' ') {
                    titleBuf[bIdx++] = ' ';
                }
                capNext = true;
            } else if (capNext) {
                titleBuf[bIdx++] = (char)toupper((unsigned char)c);
                capNext = false;
            } else {
                titleBuf[bIdx++] = c;
            }
        }
        titleBuf[bIdx] = '\0';
        if (bIdx > 0) {
            strlcpy(outBuf, titleBuf, outSize);
            return;
        }
    }

    strlcpy(outBuf, p, outSize);
}

// The profile cache is a ~640 B record per profile (10 profiles ~6.4 KB) in a 20 KB NVS partition
// it shares with Wi-Fi, PHY calibration and the display settings. NVS writes the new copy of a
// blob before it releases the old one, so rewriting it needs room for both, and that does not fit:
// "nvs_set_blob fail: prof_bin_v2 NOT_ENOUGH_SPACE". The controller is polled every 90 s and the
// profiles rarely change, so:
//   1. skip the write when the stored cache already matches (also saves flash wear);
//   2. otherwise remove the old blob first, so the new one only needs room for itself;
//   3. write the count only after the blob landed, so a failed write leaves a consistent "no cache"
//      state and the next boot falls back to the built-in presets instead of a half-written cache.
// The compare is byte-wise. Profiles parsed from the controller are value-initialised, so their
// padding is zero; where stale bytes differ anyway, the cost is one unnecessary write.
static void saveProfileCacheToPrefs() {
    const size_t blobLen = sizeof(CachedProfile) * (size_t)s_cachedProfileCount;
    prefs.begin("sb_display", false);

    if (prefs.getInt("prof_cnt_v2", -1) == s_cachedProfileCount) {
        if (s_cachedProfileCount == 0) { prefs.end(); return; }
        if (prefs.getBytesLength("prof_bin_v2") == blobLen) {
            std::unique_ptr<uint8_t[]> stored(new (std::nothrow) uint8_t[blobLen]);
            if (stored && prefs.getBytes("prof_bin_v2", stored.get(), blobLen) == blobLen &&
                memcmp(stored.get(), s_cachedProfiles, blobLen) == 0) {
                prefs.end();
                return;   // unchanged - nothing to write
            }
        }
    }

    prefs.remove("prof_cnt_v2");
    prefs.remove("prof_bin_v2");
    bool ok = true;
    if (s_cachedProfileCount > 0) {
        ok = prefs.putBytes("prof_bin_v2", s_cachedProfiles, blobLen) == blobLen;
    }
    if (ok) {
        prefs.putInt("prof_cnt_v2", s_cachedProfileCount);
    } else {
        nvs_stats_t st{};
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            Serial.printf("[Profiles] Could not cache %u B of profiles in NVS (%u of %u entries free) "
                          "- the presets are used after a reboot until the controller is reached.\n",
                          (unsigned)blobLen, (unsigned)st.free_entries, (unsigned)st.total_entries);
        }
    }
    prefs.end();
}

static void loadProfileCacheFromPrefs() {
    prefs.begin("sb_display", true);
    int cnt = prefs.getInt("prof_cnt_v2", 0);
    std::unique_ptr<CachedProfile[]> loaded(new (std::nothrow) CachedProfile[MAX_DISPLAY_PROFILES]);
    bool loadedOk = false;

    if (loaded && cnt > 0 && cnt <= MAX_DISPLAY_PROFILES) {
        size_t expectedSize = sizeof(CachedProfile) * cnt;
        size_t readLen = prefs.getBytes("prof_bin_v2", loaded.get(), expectedSize);
        if (readLen == expectedSize) {
            loadedOk = true;
        }
    }
    prefs.end();

    if (!loadedOk || cnt <= 0 || !loaded) {
        loadDefaultProfiles();
        saveProfileCacheToPrefs();
        return;
    }

    // Initialize defaults as baseline
    std::unique_ptr<CachedProfile[]> defaultProfiles(new (std::nothrow) CachedProfile[kPresetProfileCount]);
    std::unique_ptr<CachedProfile[]> merged(new (std::nothrow) CachedProfile[MAX_DISPLAY_PROFILES]);
    if (!defaultProfiles || !merged) {
        loadDefaultProfiles();
        saveProfileCacheToPrefs();
        return;
    }

    loadDefaultProfiles(); // Populates s_cachedProfiles with the 5 presets in canonical order
    for (size_t k = 0; k < kPresetProfileCount; ++k) {
        defaultProfiles[k] = s_cachedProfiles[k];
    }

    // Reconstruct s_cachedProfiles with canonical Presets FIRST, then Custom profiles
    int mergedCount = 0;

    // 1. For each preset in canonical order (classic_9bar, turbo_shot, blooming_allonge, gentle_lever, italian_dark)
    for (size_t k = 0; k < kPresetProfileCount; ++k) {
        const char* pId = kPresetProfileIds[k];
        bool found = false;
        for (int i = 0; i < cnt; ++i) {
            if (strcasecmp(loaded[i].id, pId) == 0) {
                merged[mergedCount] = loaded[i];
                char cleanName[48];
                sanitizeProfileDisplayName(loaded[i].name, pId, cleanName, sizeof(cleanName));
                strlcpy(merged[mergedCount].name, cleanName, sizeof(merged[mergedCount].name));
                strlcpy(merged[mergedCount].id, pId, sizeof(merged[mergedCount].id));
                mergedCount++;
                found = true;
                break;
            }
        }
        if (!found) {
            // Restore clean default preset
            merged[mergedCount++] = defaultProfiles[k];
        }
    }

    // 2. Append any valid custom profiles from NVS
    for (int i = 0; i < cnt && mergedCount < MAX_DISPLAY_PROFILES; ++i) {
        if (loaded[i].id[0] == '\0' || strcasecmp(loaded[i].id, "Profile") == 0) continue;
        bool isPreset = false;
        for (size_t k = 0; k < kPresetProfileCount; ++k) {
            if (strcasecmp(loaded[i].id, kPresetProfileIds[k]) == 0) {
                isPreset = true;
                break;
            }
        }
        if (!isPreset) {
            merged[mergedCount] = loaded[i];
            char cleanName[48];
            sanitizeProfileDisplayName(loaded[i].name, loaded[i].id, cleanName, sizeof(cleanName));
            strlcpy(merged[mergedCount].name, cleanName, sizeof(merged[mergedCount].name));
            mergedCount++;
        }
    }

    s_cachedProfileCount = mergedCount;
    memcpy(s_cachedProfiles, merged.get(), sizeof(CachedProfile) * mergedCount);
    saveProfileCacheToPrefs();
    Serial.printf("[Profiles] Restored %d cached profiles from local NVS (canonical preset order)\n", mergedCount);
}

static void syncCachedProfilesToTelemetryLocked(const char* preferredActiveId = nullptr) {
    g_telemetry.total_profiles = s_cachedProfileCount;
    int activeIdx = -1;

    for (int i = 0; i < s_cachedProfileCount; i++) {
        char cleanName[48];
        sanitizeProfileDisplayName(s_cachedProfiles[i].name, s_cachedProfiles[i].id, cleanName, sizeof(cleanName));
        strlcpy(g_telemetry.profile_ids[i], s_cachedProfiles[i].id, sizeof(g_telemetry.profile_ids[i]));
        strlcpy(g_telemetry.profile_names[i], cleanName, sizeof(g_telemetry.profile_names[i]));

        if (activeIdx < 0 && preferredActiveId && strlen(preferredActiveId) > 0) {
            if (strcasecmp(preferredActiveId, s_cachedProfiles[i].id) == 0 ||
                strcasecmp(preferredActiveId, cleanName) == 0 ||
                strcasecmp(preferredActiveId, s_cachedProfiles[i].name) == 0) {
                activeIdx = i;
            }
        }
    }

    if (activeIdx < 0) {
        for (int i = 0; i < s_cachedProfileCount; i++) {
            if (strcasecmp(g_telemetry.active_profile_id, s_cachedProfiles[i].id) == 0 ||
                strcasecmp(g_telemetry.active_profile, s_cachedProfiles[i].name) == 0) {
                activeIdx = i;
                break;
            }
        }
    }

    if (activeIdx < 0 && s_cachedProfileCount > 0) {
        activeIdx = 0;
    }

    if (activeIdx >= 0 && activeIdx < s_cachedProfileCount) {
        g_telemetry.active_profile_idx = activeIdx;
        const CachedProfile& ap = s_cachedProfiles[activeIdx];
        strlcpy(g_telemetry.active_profile, ap.name, sizeof(g_telemetry.active_profile));
        strlcpy(g_telemetry.active_profile_id, ap.id, sizeof(g_telemetry.active_profile_id));
        g_telemetry.target_weight = ap.target_weight;
        g_telemetry.step_count = ap.step_count;
        for (int j = 0; j < ap.step_count; j++) {
            g_telemetry.steps[j] = ap.steps[j];
        }
        g_telemetry.total_profile_duration = ap.total_duration;
        g_telemetry.profiles_updated = true;
    }
}

static void syncCachedProfilesToTelemetry(const char* preferredActiveId = nullptr) {
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        syncCachedProfilesToTelemetryLocked(preferredActiveId);
        xSemaphoreGive(g_telemetryMutex);
    }
}

// -----------------------------------------------------------------------------
// Captive Portal Routes & HTML
// -----------------------------------------------------------------------------
static void handlePortalRoot() {
    String html = "<!doctype html><html lang='en'><head>"
                  "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>SmartBrew Display — Wi-Fi Setup</title><style>"
                  ":root{--bg:#07090e;--panel:#111827;--line:#1e293b;--txt:#f8fafc;--dim:#94a3b8;--acc:#00f0ff;--acc-sub:#10b981}"
                  "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--bg);color:var(--txt);"
                  "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;flex-direction:column;align-items:center;padding:24px 16px}"
                  ".card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:24px;width:100%;max-width:390px;box-shadow:0 8px 32px rgba(0,0,0,0.5)}"
                  "h2{margin:0 0 6px;color:var(--acc);font-size:22px;font-weight:700;display:flex;align-items:center;gap:8px}"
                  "p{color:var(--dim);font-size:13px;margin:0 0 16px;line-height:1.45}"
                  "label{display:block;font-size:11px;color:var(--dim);margin-bottom:6px;text-transform:uppercase;letter-spacing:.05em;font-weight:700}"
                  "input,select{width:100%;padding:11px 12px;border-radius:8px;border:1px solid var(--line);background:#0b1120;color:var(--txt);font-size:15px;margin-bottom:16px}"
                  "input:focus,select:focus{outline:none;border-color:var(--acc)}"
                  "button{width:100%;padding:13px;border-radius:8px;border:none;background:var(--acc);color:#06231d;font-size:15px;font-weight:700;cursor:pointer;margin-top:8px}"
                  "button:active{transform:translateY(1px);opacity:.9}"
                  ".note{font-size:11px;color:var(--dim);text-align:center;margin-top:14px}"
                  "</style></head><body><div class='card'>"
                  "<h2><span>☕</span> SmartBrew Display</h2>"
                  "<p>Configure Wi-Fi to connect this 3.5\" Touch Display to your SmartBrew espresso controller.</p>"
                  "<form action='/save' method='POST'>"
                  "<label for='s'>Wi-Fi Network (SSID)</label>";

    int n = WiFi.scanComplete();
    if (n > 0) {
        html += "<select onchange='if(this.value){document.getElementById(\"s\").value=this.value;}'>";
        html += "<option value=''>-- Select nearby network --</option>";
        std::vector<String> seen;
        for (int i = 0; i < n; ++i) {
            String s = WiFi.SSID(i);
            if (s.length() > 0) {
                bool exists = false;
                for (const auto& item : seen) {
                    if (item == s) { exists = true; break; }
                }
                if (!exists) {
                    seen.push_back(s);
                    html += "<option value='" + s + "'>" + s + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
                }
            }
        }
        html += "</select>";
        html += "<input type='text' id='s' name='ssid' required value='" + String(wifi_ssid) + "' placeholder='Or enter SSID manually'>";
    } else {
        html += "<input type='text' id='s' name='ssid' required value='" + String(wifi_ssid) + "' placeholder='Enter Wi-Fi network name' autofocus>";
    }

    html += "<label for='p'>Wi-Fi Password</label>"
            "<input type='password' id='p' name='pass' value='" + String(wifi_pass) + "' placeholder='Password (blank if open)'>"
            "<label for='h'>Controller Host / IP</label>"
            "<input type='text' id='h' name='host' value='" + String(mcu_host) + "' placeholder='smartbrew.local or 192.168.x.x'>"
            "<button type='submit'>Save & Connect</button>"
            "</form>"
            "<div class='note'>Device will reboot and associate automatically.</div>"
            "</div></body></html>";

    webServer.send(200, "text/html; charset=utf-8", html);
}

static void handlePortalSave() {
    String sSsid = webServer.arg("ssid");
    String sPass = webServer.arg("pass");
    String sHost = webServer.arg("host");
    sSsid.trim();
    sPass.trim();
    sHost.trim();

    if (sSsid.length() > 0) {
        saveNetworkConfig(sSsid.c_str(), sPass.c_str(), sHost.length() > 0 ? sHost.c_str() : "smartbrew.local");

        String html = "<!doctype html><html lang='en'><head>"
                      "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<title>SmartBrew Display — Saved</title></head>"
                      "<body style='font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#07090e;color:#f8fafc;"
                      "display:grid;place-items:center;min-height:100vh;margin:0;text-align:center;padding:16px'>"
                      "<div style='background:#111827;border:1px solid #1e293b;border-radius:14px;padding:24px;max-width:360px;box-shadow:0 8px 32px rgba(0,0,0,0.5)'>"
                      "<h2 style='color:#10b981;margin-top:0'>&#10003; Saved!</h2>"
                      "<p style='font-size:15px;line-height:1.4'>Connecting to <b>" + sSsid + "</b>...</p>"
                      "<p style='color:#94a3b8;font-size:13px'>Display will connect to SmartBrew Controller at <b>" +
                      (sHost.length() > 0 ? sHost : "smartbrew.local") + "</b>.</p>"
                      "</div></body></html>";
        webServer.send(200, "text/html; charset=utf-8", html);
        portal_reboot_at = millis() + 1200;
    } else {
        webServer.send(400, "text/plain; charset=utf-8", "Missing SSID");
    }
}

static bool s_portal_routes_registered = false;
static void registerPortalRoutes() {
    if (s_portal_routes_registered) return;
    s_portal_routes_registered = true;

    webServer.on("/", HTTP_GET, handlePortalRoot);
    webServer.on("/wifi", HTTP_GET, handlePortalRoot);
    webServer.on("/save", HTTP_POST, handlePortalSave);

    static const char* const probeUris[] = {
        "/generate_204", "/gen_204", "/ncsi.txt", "/hotspot-detect.html", "/canonical.html", "/connecttest.txt"
    };
    for (const char* u : probeUris) {
        webServer.on(u, HTTP_GET, []() {
            webServer.sendHeader("Location", "http://192.168.4.1/wifi", true);
            webServer.send(302, "text/plain", "");
        });
    }

    webServer.onNotFound([]() {
        webServer.sendHeader("Location", "http://192.168.4.1/wifi", true);
        webServer.send(302, "text/plain", "");
    });
}

void resetWifiConfig() {
    Serial.println("[Wi-Fi] Resetting stored Wi-Fi credentials from NVS flash...");
    Preferences p;
    p.begin("sb_display", false);
    p.remove("ssid");
    p.remove("pass");
    p.end();

    Preferences p2;
    p2.begin("sbwifi", false);
    p2.remove("ssid");
    p2.remove("pass");
    p2.clear();
    p2.end();

    wifi_ssid[0] = '\0';
    wifi_pass[0] = '\0';

    WiFi.disconnect(true, true); // Erase credentials from ESP-IDF radio NVS
    delay(100);
}

static volatile bool s_request_portal_start = false;
static volatile bool s_request_portal_stop = false;

void requestStartSetupPortal() {
    s_request_portal_start = true;
}

void requestStopSetupPortal() {
    s_request_portal_stop = true;
}

void startSetupPortal() {
    in_portal_mode = true;
    webSocket.disconnect();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(portal_ssid, sizeof(portal_ssid), "SmartBrew-Display-%02X%02X", mac[4], mac[5]);

    WiFi.disconnect(false, false);
    delay(50);
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_17dBm);

    IPAddress apIP(192, 168, 4, 1);
    IPAddress netmask(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apIP, netmask);
    WiFi.softAP(portal_ssid, nullptr, 1, 0, 4);

    delay(50);
    dnsServer.stop();
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);

    registerPortalRoutes();
    webServer.begin();

    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_telemetry.in_setup_portal = true;
        strlcpy(g_telemetry.portal_ssid, portal_ssid, sizeof(g_telemetry.portal_ssid));
        xSemaphoreGive(g_telemetryMutex);
    }

    Serial.printf("[Portal] Hotspot active: '%s' (http://192.168.4.1/wifi)\n", portal_ssid);
}

void stopSetupPortal() {
    in_portal_mode = false;
    dnsServer.stop();
    webServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_telemetry.in_setup_portal = false;
        xSemaphoreGive(g_telemetryMutex);
    }

    if (strlen(wifi_ssid) > 0) {
        WiFi.begin(wifi_ssid, wifi_pass);
    }
}

bool isInSetupPortal() {
    return in_portal_mode;
}

const char* getPortalSsid() {
    return portal_ssid;
}

// -----------------------------------------------------------------------------
// Controller Telemetry & Profile Sync (Instant Caching & Non-blocking)
// -----------------------------------------------------------------------------
static int parseProfilePhasesFromJson(JsonArray phases, CachedProfile& cp, float& outTotalDur) {
    int stepCnt = 0;
    float totalDur = 0.0f;
    float lastSetVal = 0.0f;

    for (JsonObject ph : phases) {
        if (stepCnt >= MAX_PROFILE_STEPS) break;
        const char* stName = ph["name"] | "PHASE";
        strlcpy(cp.steps[stepCnt].name, stName, sizeof(cp.steps[stepCnt].name));

        const char* m = ph["mode"] | (ph["type"] | "pressure");
        if (ph["pump"].is<JsonObject>()) {
            m = ph["pump"]["target"] | "pressure";
        }
        strlcpy(cp.steps[stepCnt].type, m, sizeof(cp.steps[stepCnt].type));

        const char* cv = ph["curve"] | "linear";
        if (ph["target"].is<JsonObject>() && !ph["target"]["curve"].isNull()) {
            cv = ph["target"]["curve"] | "linear";
        }
        strlcpy(cp.steps[stepCnt].curve, cv, sizeof(cp.steps[stepCnt].curve));

        float sVal = 0.0f;
        float eVal = 0.0f;
        if (strcasecmp(m, "pause") == 0) {
            sVal = 0.0f;
            eVal = 0.0f;
        } else if (!ph["startVal"].isNull()) {
            sVal = ph["startVal"].as<float>();
            eVal = ph["endVal"].isNull() ? sVal : ph["endVal"].as<float>();
        } else if (!ph["value"].isNull()) {
            sVal = ph["value"].as<float>();
            eVal = sVal;
        } else if (!ph["target"].isNull()) {
            if (ph["target"].is<JsonObject>()) {
                sVal = ph["target"]["start"] | 0.0f;
                eVal = ph["target"]["end"] | sVal;
            } else {
                sVal = ph["target"].as<float>();
                eVal = sVal;
            }
        } else if (ph["pump"].is<JsonObject>()) {
            JsonObject gp = ph["pump"].as<JsonObject>();
            float v = (strcasecmp(m, "flow") == 0 || strcasecmp(m, "power") == 0)
                      ? (gp["flow"] | 0.0f)
                      : (gp["pressure"] | 0.0f);
            sVal = v;
            eVal = v;
            if (ph["transition"].is<JsonObject>()) {
                const char* tt = ph["transition"]["type"] | "instant";
                if (strcasecmp(tt, "linear") == 0 && stepCnt > 0) {
                    sVal = lastSetVal;
                    eVal = v;
                    strlcpy(cp.steps[stepCnt].curve, "linear", sizeof(cp.steps[stepCnt].curve));
                } else if (strcasecmp(tt, "ease_in_out") == 0 || strcasecmp(tt, "ease_out") == 0 || strcasecmp(tt, "ease_in") == 0) {
                    if (stepCnt > 0) sVal = lastSetVal;
                    eVal = v;
                    strlcpy(cp.steps[stepCnt].curve, tt, sizeof(cp.steps[stepCnt].curve));
                }
            }
        } else {
            sVal = 9.0f;
            eVal = 9.0f;
        }
        lastSetVal = eVal;
        cp.steps[stepCnt].start_val = sVal;
        cp.steps[stepCnt].end_val = eVal;

        float dur = 5.0f;
        if (!ph["durationSec"].isNull()) dur = ph["durationSec"].as<float>();
        else if (!ph["duration"].isNull()) dur = ph["duration"].as<float>();
        else if (!ph["seconds"].isNull()) dur = ph["seconds"].as<float>();
        else if (ph["transition"].is<JsonObject>() && !ph["transition"]["durationSec"].isNull()) dur = ph["transition"]["durationSec"].as<float>();
        else if (ph["stopConditions"].is<JsonObject>() && !ph["stopConditions"]["time"].isNull()) dur = ph["stopConditions"]["time"].as<float>() / 1000.0f;
        else if (ph["target"].is<JsonObject>() && !ph["target"]["time"].isNull()) dur = ph["target"]["time"].as<float>() / 1000.0f;
        if (dur <= 0.1f) dur = 1.0f;

        cp.steps[stepCnt].duration = dur;
        totalDur += dur;
        stepCnt++;
    }
    outTotalDur = totalDur;
    return stepCnt;
}

static void fetchProfilesList() {
    if (WiFi.status() != WL_CONNECTED || !mcu_ip_resolved) return;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(1500);
    String url = "http://" + mcu_ip.toString() + "/api/v1/profiles";
    if (http.begin(client, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);
            if (!err) {
                String activeId = doc["active"] | "";
                JsonArray profArr = doc["items"].is<JsonArray>() ? doc["items"].as<JsonArray>() : doc["profiles"].as<JsonArray>();

                if (!profArr.isNull() && profArr.size() > 0) {
                    std::unique_ptr<CachedProfile[]> newProfiles(new (std::nothrow) CachedProfile[MAX_DISPLAY_PROFILES]);
                    std::unique_ptr<CachedProfile[]> defaultPresets(new (std::nothrow) CachedProfile[kPresetProfileCount]);
                    if (!newProfiles || !defaultPresets) {
                        http.end();
                        return;
                    }
                    int newCount = 0;

                    // Baseline default presets in case the Controller doesn't provide all phases
                    loadDefaultProfiles();
                    for (size_t k = 0; k < kPresetProfileCount; ++k) {
                        defaultPresets[k] = s_cachedProfiles[k];
                    }

                    // 1. Process standard presets in canonical sequence:
                    // classic_9bar, turbo_shot, blooming_allonge, gentle_lever, italian_dark
                    for (size_t k = 0; k < kPresetProfileCount; ++k) {
                        const char* targetId = kPresetProfileIds[k];
                        bool found = false;

                        for (JsonObject p : profArr) {
                            const char* pid = p["id"] | "";
                            if (strcasecmp(pid, targetId) == 0) {
                                CachedProfile cp{};
                                strlcpy(cp.id, targetId, sizeof(cp.id));
                                const char* pnm = p["name"] | "";
                                char cleanName[48];
                                sanitizeProfileDisplayName(pnm, targetId, cleanName, sizeof(cleanName));
                                strlcpy(cp.name, cleanName, sizeof(cp.name));

                                float tw = defaultPresets[k].target_weight;
                                if (!p["targetWeightG"].isNull()) {
                                    tw = p["targetWeightG"].as<float>();
                                } else if (!p["target_weight"].isNull()) {
                                    tw = p["target_weight"].as<float>();
                                } else if (p["globalStopConditions"].is<JsonObject>() && !p["globalStopConditions"]["weight"].isNull()) {
                                    tw = p["globalStopConditions"]["weight"].as<float>();
                                }
                                cp.target_weight = tw;

                                JsonArray phases = p["phases"].is<JsonArray>() ? p["phases"].as<JsonArray>() : p["steps"].as<JsonArray>();
                                float totalDur = 0.0f;
                                int stepCnt = parseProfilePhasesFromJson(phases, cp, totalDur);

                                if (stepCnt > 0) {
                                    cp.step_count = stepCnt;
                                    cp.total_duration = (totalDur > 5.0f) ? totalDur : defaultPresets[k].total_duration;
                                } else {
                                    // Fallback to default preset steps
                                    cp.step_count = defaultPresets[k].step_count;
                                    cp.total_duration = defaultPresets[k].total_duration;
                                    for (int s = 0; s < defaultPresets[k].step_count; ++s) {
                                        cp.steps[s] = defaultPresets[k].steps[s];
                                    }
                                }

                                newProfiles[newCount++] = cp;
                                found = true;
                                break;
                            }
                        }

                        if (!found) {
                            newProfiles[newCount++] = defaultPresets[k];
                        }
                    }

                    // 2. Append custom user profiles (non-presets)
                    for (JsonObject p : profArr) {
                        if (newCount >= MAX_DISPLAY_PROFILES) break;
                        const char* pid = p["id"] | "";
                        if (!pid || pid[0] == '\0') continue;

                        bool isPreset = false;
                        for (size_t k = 0; k < kPresetProfileCount; ++k) {
                            if (strcasecmp(pid, kPresetProfileIds[k]) == 0) {
                                isPreset = true;
                                break;
                            }
                        }
                        if (isPreset) continue;

                        CachedProfile cp{};
                        strlcpy(cp.id, pid, sizeof(cp.id));
                        const char* pnm = p["name"] | "";
                        char cleanName[48];
                        sanitizeProfileDisplayName(pnm, pid, cleanName, sizeof(cleanName));
                        strlcpy(cp.name, cleanName, sizeof(cp.name));

                        float tw = 36.0f;
                        if (!p["targetWeightG"].isNull()) tw = p["targetWeightG"].as<float>();
                        else if (!p["target_weight"].isNull()) tw = p["target_weight"].as<float>();
                        else if (p["globalStopConditions"].is<JsonObject>() && !p["globalStopConditions"]["weight"].isNull()) tw = p["globalStopConditions"]["weight"].as<float>();
                        cp.target_weight = tw;

                        JsonArray phases = p["phases"].is<JsonArray>() ? p["phases"].as<JsonArray>() : p["steps"].as<JsonArray>();
                        float totalDur = 0.0f;
                        int stepCnt = parseProfilePhasesFromJson(phases, cp, totalDur);
                        cp.step_count = stepCnt;
                        cp.total_duration = (totalDur > 5.0f) ? totalDur : 35.0f;

                        newProfiles[newCount++] = cp;
                    }

                    s_cachedProfileCount = newCount;
                    memcpy(s_cachedProfiles, newProfiles.get(), sizeof(CachedProfile) * newCount);
                    saveProfileCacheToPrefs();
                    syncCachedProfilesToTelemetry(activeId.c_str());
                    Serial.printf("[Profiles] Synced %d profiles from the Controller (active: %s). Canonical preset order maintained.\n", newCount, activeId.c_str());
                }
            }
        }
        http.end();
    }
}

// =================================================================================================
// BREW LEDGER
//
// The Controller keeps the recorded shots in its LittleFS ledger and serves the summary list at
// GET /api/v1/history. The display fetches that list here - it is the only place with the
// Controller's address and the only task allowed to do HTTP, so the UI thread never blocks on it.
// =================================================================================================
HistoryData g_history;
SemaphoreHandle_t g_historyMutex = nullptr;
static volatile bool s_historyRefreshRequested = false;

static void ensureHistoryMutex() {
    if (g_historyMutex == nullptr) g_historyMutex = xSemaphoreCreateMutex();
}

void requestHistoryRefresh() {
    s_historyRefreshRequested = true;
}

bool getHistorySnapshot(HistoryData& out) {
    ensureHistoryMutex();
    if (g_historyMutex == nullptr) return false;
    if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    out = g_history;
    xSemaphoreGive(g_historyMutex);
    return true;
}

// Store the fetched list. The shots arrive in slot order, which is the order they were recorded
// in until the ledger wraps, so they are sorted by their timestamp: the newest shot first.
static void storeHistory(HistoryData& fresh) {
    for (int i = 1; i < fresh.count; ++i) {
        HistoryShotData key = fresh.shots[i];
        int j = i - 1;
        while (j >= 0 && fresh.shots[j].epoch_ms < key.epoch_ms) {
            fresh.shots[j + 1] = fresh.shots[j];
            --j;
        }
        fresh.shots[j + 1] = key;
    }

    ensureHistoryMutex();
    if (g_historyMutex == nullptr) return;
    if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    g_history = fresh;
    xSemaphoreGive(g_historyMutex);
}

static void markHistoryFailed() {
    ensureHistoryMutex();
    if (g_historyMutex == nullptr) return;
    if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    g_history.failed = true;
    xSemaphoreGive(g_historyMutex);
}

// Read a field, falling back to the alternative name the Controller may have used for it.
static float historyFloat(JsonObject o, const char* key, const char* alt, float fallback) {
    if (!o[key].isNull()) return o[key].as<float>();
    if (alt && !o[alt].isNull()) return o[alt].as<float>();
    return fallback;
}

static const char* historyStr(JsonObject o, const char* key, const char* alt, const char* fallback) {
    if (!o[key].isNull()) return o[key].as<const char*>();
    if (alt && !o[alt].isNull()) return o[alt].as<const char*>();
    return fallback;
}

static void fetchHistory() {
    if (WiFi.status() != WL_CONNECTED || !mcu_ip_resolved) {
        markHistoryFailed();
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(2000);
    String url = "http://" + mcu_ip.toString() + "/api/v1/history";
    if (!http.begin(client, url)) {
        markHistoryFailed();
        return;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        Serial.printf("[History] The Controller did not answer the ledger request (%d).\n", httpCode);
        markHistoryFailed();
        return;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
        Serial.println("[History] The ledger response could not be read.");
        markHistoryFailed();
        return;
    }

    HistoryData fresh;
    fresh.valid = true;
    fresh.failed = false;
    fresh.updated_ms = millis();

    JsonArray shots = doc["shots"].is<JsonArray>() ? doc["shots"].as<JsonArray>() : JsonArray();
    if (!shots.isNull()) {
        for (JsonObject s : shots) {
            if (fresh.count >= MAX_DISPLAY_SHOTS) break;
            HistoryShotData& shot = fresh.shots[fresh.count];
            const char* name = historyStr(s, "profileName", "profile_name", "Custom Shot");
            strlcpy(shot.profile_name, name, sizeof(shot.profile_name));
            shot.dose_g      = historyFloat(s, "doseG", "dose_g", 0.0f);
            shot.peak_bar    = historyFloat(s, "peakBar", "peak_bar", 0.0f);
            shot.duration_ms = (uint32_t)historyFloat(s, "durationMs", "duration_s", 0.0f);
            if (shot.duration_ms == 0) {
                // The Controller also reports the duration in seconds under another name.
                shot.duration_ms = (uint32_t)(historyFloat(s, "duration_s", nullptr, 0.0f) * 1000.0f);
            }
            shot.epoch_ms    = s["epochMs"].isNull() ? 0ULL : s["epochMs"].as<uint64_t>();
            fresh.count++;
        }
    }

    storeHistory(fresh);
    Serial.printf("[History] Ledger updated: %d shot(s) from the Controller.\n", fresh.count);
}

static void fetchMcuStatus() {
    if (WiFi.status() != WL_CONNECTED || !mcu_ip_resolved) return;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(1000);
    String url = "http://" + mcu_ip.toString() + "/api/v1/status";
    if (http.begin(client, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, payload)) {
                ensureTelemetryMutex();
                if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    // 1. Simulation mode sync
                    if (!doc["sim"].isNull()) {
                        bool mcuSim = doc["sim"].as<bool>();
                        if (millis() - s_last_local_sim_change > 3000) {
                            if (g_telemetry.simulation_mode != mcuSim) {
                                g_telemetry.simulation_mode = mcuSim;
                                prefs.begin("sb_display", false);
                                prefs.putBool("sim_mode", mcuSim);
                                prefs.end();
                                Serial.printf("[Status] Synced Simulation mode from the Controller -> %s\n", mcuSim ? "ON" : "OFF");
                            }
                        }
                    }

                    // 2. Active profile sync from the Controller / Desktop browser
                    const char* mcuProf = doc["profile_id"] | (doc["profile"] | "");
                    if (mcuProf[0] && strcasecmp(mcuProf, g_telemetry.active_profile) != 0 &&
                        strcasecmp(mcuProf, g_telemetry.active_profile_id) != 0) {
                        if (millis() - s_last_local_profile_change > 3000) {
                            syncCachedProfilesToTelemetryLocked(mcuProf);
                            Serial.printf("[Profiles] Synced active profile from the Controller -> %s\n", mcuProf);
                        }
                    }
                    xSemaphoreGive(g_telemetryMutex);
                }
            }
        }
        http.end();
    }
}

// State name for a binary telemetry frame (SBState).
static const char* sbStateName(uint8_t state) {
    switch (state) {
        case SB_STATE_IDLE:     return "IDLE";
        case SB_STATE_STARTING: return "STARTING";
        case SB_STATE_BREWING:  return "BREWING";
        case SB_STATE_FLUSHING: return "FLUSHING";
        case SB_STATE_STOPPING: return "STOPPING";
        case SB_STATE_FAULT:    return "FAULT";
        default:                return "IDLE";
    }
}

static void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    ensureTelemetryMutex();
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected from SmartBrew Controller");
            if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_telemetry.mcu_connected = false;
                xSemaphoreGive(g_telemetryMutex);
            }
            break;
            
        case WStype_CONNECTED:
            Serial.printf("[WS] Connected to SmartBrew Controller at %s\n", payload);
            rememberMcuAddress(mcu_ip);
            if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_telemetry.mcu_connected = true;
                xSemaphoreGive(g_telemetryMutex);
            }
            s_onConnectSyncStep = 1;
            break;
            
        case WStype_BIN: {
            // Binary telemetry frame. The layout is shared with the controller firmware through
            // include/telemetry_wire.h - the same header the controller compiles against, so the two can
            // never drift apart.
            if (!payload || length < SB_TELEMETRY_MIN_LEN || length > SB_TELEMETRY_MAX_LEN) break;

            SbTelemetryFrame f;
            if (!sbTelemetryDecode(payload, length, f)) break;

            if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (millis() - s_last_local_sim_change > 3000) {
                    bool mcuSim = (f.flags & SB_FLAG_SIMULATION) != 0;
                    if (g_telemetry.simulation_mode != mcuSim) {
                        g_telemetry.simulation_mode = mcuSim;
                        Serial.printf("[Status] Synced Simulation mode from the Controller -> %s\n", mcuSim ? "ON" : "OFF");
                    }
                }

                if (f.profile_id[0] &&
                    strcasecmp(f.profile_id, g_telemetry.active_profile) != 0 &&
                    strcasecmp(f.profile_id, g_telemetry.active_profile_id) != 0) {
                    if (millis() - s_last_local_profile_change > 3000) {
                        syncCachedProfilesToTelemetryLocked(f.profile_id);
                        Serial.printf("[WS] Synced active profile from the Controller -> %s\n", f.profile_id);
                    }
                }

                g_telemetry.t_ms = f.t_ms;
                g_telemetry.pressure = f.p_bar / 100.0f;
                g_telemetry.target_pressure = f.p_target / 100.0f;
                g_telemetry.flow_rate = f.flow_virt / 100.0f;
                g_telemetry.psm_pct = f.psm_pct;
                g_telemetry.weight = f.weight_g / 10.0f;
                g_telemetry.weight_flow = f.flow_weight / 100.0f;
                g_telemetry.scale_conn = (f.flags & SB_FLAG_SCALE_CONNECTED) != 0;
                g_telemetry.weight_settling = (f.flags & SB_FLAG_WEIGHT_SETTLING) != 0;
                strlcpy(g_telemetry.scale_name, f.scale_name, sizeof(g_telemetry.scale_name));
                g_telemetry.scale_bat = f.scale_bat;
                g_telemetry.ac_hz = f.ac_hz;
                g_telemetry.phase_pct = f.phase_pct;
                g_telemetry.faults = f.faults;

                const char* st = sbStateName(f.state);
                strlcpy(g_telemetry.state, st, sizeof(g_telemetry.state));
                strlcpy(g_telemetry.phase_name, f.phase_name, sizeof(g_telemetry.phase_name));

                bool active = (strcmp(st, "BREWING") == 0 || strcmp(st, "STARTING") == 0 ||
                               strcmp(st, "STOPPING") == 0);
                g_telemetry.brew_active = active;
                g_telemetry.shot_time = active ? ((float)g_telemetry.t_ms / 1000.0f) : 0.0f;

                g_telemetry.rx_millis = millis();
                g_telemetry.mcu_connected = true;
                xSemaphoreGive(g_telemetryMutex);
            }
            break;
        }

        case WStype_TEXT: {
            if (!payload || length == 0) break;
            
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload, length);
            if (error) break;

            if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                // Check if the Controller is in simulation mode
                if (!doc["sim"].isNull()) {
                    bool mcuSim = doc["sim"].as<bool>();
                    if (millis() - s_last_local_sim_change > 3000) {
                        if (g_telemetry.simulation_mode != mcuSim) {
                            g_telemetry.simulation_mode = mcuSim;
                            Serial.printf("[Status] Synced Simulation mode from the Controller -> %s\n", mcuSim ? "ON" : "OFF");
                        }
                    }
                }

                // Check if active profile changed on the Controller
                const char* prof = doc["profile_id"] | (doc["profile"] | "");
                if (prof[0] && strcasecmp(prof, g_telemetry.active_profile) != 0 &&
                    strcasecmp(prof, g_telemetry.active_profile_id) != 0) {
                    if (millis() - s_last_local_profile_change > 3000) {
                        syncCachedProfilesToTelemetryLocked(prof);
                        Serial.printf("[WS] Synced active profile from the Controller -> %s\n", prof);
                    }
                }

                if (strcmp(doc["evt"] | "", "shot_end") == 0) {
                    g_telemetry.brew_active = false;
                    if (!doc["duration_ms"].isNull()) {
                        g_telemetry.t_ms = doc["duration_ms"] | g_telemetry.t_ms;
                        g_telemetry.shot_time = (float)g_telemetry.t_ms / 1000.0f;
                    }
                    if (!doc["dose_g"].isNull()) {
                        float dose = doc["dose_g"] | 0.0f;
                        if (dose > 0.1f) g_telemetry.weight = dose;
                    }
                }

                if (!doc["p_bar"].isNull()) {
                    g_telemetry.t_ms = doc["t_ms"] | 0;
                    g_telemetry.pressure = doc["p_bar"] | 0.0f;
                    g_telemetry.target_pressure = doc["p_target"] | 0.0f;
                    g_telemetry.flow_rate = doc["flow_virt"] | 0.0f;
                    g_telemetry.psm_pct = doc["psm_pct"] | 0;
                    g_telemetry.weight = doc["weight_g"] | 0.0f;
                    g_telemetry.weight_flow = doc["flow_weight"] | 0.0f;
                    g_telemetry.scale_conn = doc["scale_conn"] | false;
                    g_telemetry.weight_settling = doc["weight_settling"] | false;
                    strlcpy(g_telemetry.scale_name, doc["scale_name"] | "", sizeof(g_telemetry.scale_name));
                    g_telemetry.scale_bat = doc["scale_bat"] | -1;
                    g_telemetry.ac_hz = doc["ac_hz"] | 0;
                    g_telemetry.phase_pct = doc["phase_pct"] | 0;
                    g_telemetry.faults = doc["faults"] | 0;

                    const char* st = doc["state"] | "IDLE";
                    strlcpy(g_telemetry.state, st, sizeof(g_telemetry.state));
                    strlcpy(g_telemetry.phase_name, doc["phase_name"] | "", sizeof(g_telemetry.phase_name));

                    bool active = (strcmp(st, "BREWING") == 0 || strcmp(st, "STARTING") == 0 ||
                                   strcmp(st, "STOPPING") == 0);
                    g_telemetry.brew_active = active;
                    g_telemetry.shot_time = active ? ((float)g_telemetry.t_ms / 1000.0f) : 0.0f;
                }
                g_telemetry.mcu_connected = true;
                xSemaphoreGive(g_telemetryMutex);
            }
            break;
        }
        default:
            break;
    }
}

static void resolveMcuAddress() {
    IPAddress parsedIp;
    if (parsedIp.fromString(mcu_host)) {
        mcu_ip = parsedIp;
        mcu_ip_resolved = true;
        Serial.printf("[mDNS] Direct Controller IP specified: %s\n", mcu_ip.toString().c_str());
        return;
    }

    String hostname = mcu_host;
    if (hostname.endsWith(".local")) {
        hostname = hostname.substring(0, hostname.length() - 6);
    }

    Serial.printf("[mDNS] Resolving '%s' via mDNS...\n", hostname.c_str());
    IPAddress resolvedIp = MDNS.queryHost(hostname.c_str(), 1500);
    if (resolvedIp == IPAddress(0, 0, 0, 0) && hostname.equalsIgnoreCase("smartbrew")) {
        resolvedIp = MDNS.queryHost("brew-mcu", 1000);
    }
    if (resolvedIp != IPAddress(0, 0, 0, 0)) {
        mcu_ip = resolvedIp;
        mcu_ip_resolved = true;
        Serial.printf("[mDNS] Successfully resolved Controller host '%s' -> %s\n", hostname.c_str(), mcu_ip.toString().c_str());
    } else if (mcu_last_ip != IPAddress(0, 0, 0, 0) && strcasecmp(mcu_last_host, mcu_host) == 0) {
        mcu_ip = mcu_last_ip;
        mcu_ip_resolved = true;
        Serial.printf("[mDNS] Controller host '%s' not found. Using its last known address: %s\n",
                      hostname.c_str(), mcu_ip.toString().c_str());
    } else {
        mcu_ip_resolved = false;
        Serial.printf("[mDNS] Controller host '%s' not found and no earlier address is known - retrying "
                      "in %u s. Enter the controller's IP in the display setup page if mDNS keeps failing.\n",
                      hostname.c_str(), (unsigned)(kMcuResolveRetryMs / 1000));
    }
}

// Remember the address a WebSocket connection actually succeeded on, together with the host it
// belongs to, so a later mDNS failure can fall back to it. Written only when it changes.
static void rememberMcuAddress(const IPAddress& ip) {
    if (ip == IPAddress(0, 0, 0, 0)) return;
    if (ip == mcu_last_ip && strcasecmp(mcu_last_host, mcu_host) == 0) return;
    mcu_last_ip = ip;
    strlcpy(mcu_last_host, mcu_host, sizeof(mcu_last_host));
    prefs.begin("sb_display", false);
    prefs.putString("mcu_last_ip", ip.toString());
    prefs.putString("mcu_last_host", mcu_last_host);
    prefs.end();
    Serial.printf("[mDNS] Remembered %s as the address of '%s'\n", ip.toString().c_str(), mcu_last_host);
}

void initNetworkClient() {
    ensureTelemetryMutex();

    prefs.begin("sb_display", false);
    String sSsid = prefs.getString("ssid", "");
    String sPass = prefs.getString("pass", "");
    String sHost = prefs.getString("mcu_host", "");
    String sLastIp = prefs.getString("mcu_last_ip", "");
    String sLastHost = prefs.getString("mcu_last_host", "");
    String sActiveProf = prefs.getString("active_prof", "classic_9bar");
    bool simMode = prefs.getBool("sim_mode", false);
    prefs.end();

    if (sSsid.length() == 0) {
        Preferences sbPrefs;
        sbPrefs.begin("sbwifi", true);
        sSsid = sbPrefs.getString("ssid", "");
        sPass = sbPrefs.getString("pass", "");
        sbPrefs.end();
    }

    if (sSsid.length() > 0) {
        strlcpy(wifi_ssid, sSsid.c_str(), sizeof(wifi_ssid));
        strlcpy(wifi_pass, sPass.c_str(), sizeof(wifi_pass));
    }
    if (sHost.length() > 0) {
        if (sHost.equalsIgnoreCase("brew-mcu.local") || sHost.equalsIgnoreCase("brew-mcu")) {
            sHost = "smartbrew.local";
            prefs.begin("sb_display", false);
            prefs.putString("mcu_host", sHost);
            prefs.end();
        }
        strlcpy(mcu_host, sHost.c_str(), sizeof(mcu_host));
    }
    if (sLastIp.length() > 0 && mcu_last_ip.fromString(sLastIp)) {
        strlcpy(mcu_last_host, sLastHost.c_str(), sizeof(mcu_last_host));
    }

    // Set simulation mode initial state
    g_telemetry.simulation_mode = simMode;

    // Load cached profiles from flash NVS immediately (0 ms delay on boot)
    loadProfileCacheFromPrefs();
    syncCachedProfilesToTelemetry(sActiveProf.c_str());

    if (strlen(wifi_ssid) == 0) {
        Serial.println("[Wi-Fi] No Wi-Fi credentials found. Launching Phone Setup Portal...");
        startSetupPortal();
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        Serial.printf("[Wi-Fi] Connecting to '%s'...\n", wifi_ssid);
        WiFi.begin(wifi_ssid, wifi_pass);
        sta_connect_start = millis();
        MDNS.begin("smartbrew-display");
    }
}

void updateNetworkLoop() {
    uint32_t now = millis();

    // Check asynchronous portal requests from UI touch thread
    if (s_request_portal_start) {
        s_request_portal_start = false;
        webSocket.disconnect();
        startSetupPortal();
        return;
    }
    if (s_request_portal_stop) {
        s_request_portal_stop = false;
        stopSetupPortal();
        return;
    }

    if (in_portal_mode) {
        dnsServer.processNextRequest();
        webServer.handleClient();
        if (portal_reboot_at > 0 && now >= portal_reboot_at) {
            portal_reboot_at = 0;
            ESP.restart();
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        // Unresolved: retry every few seconds (each attempt blocks for up to 2.5 s of mDNS queries).
        // Resolved but disconnected: look the name up again every 60 s, in case the address moved.
        const bool retryUnresolved = !mcu_ip_resolved &&
            (last_mdns_attempt == 0 || now - last_mdns_attempt > kMcuResolveRetryMs);
        if (retryUnresolved || (mcu_ip_resolved && now - last_mdns_attempt > 60000 && !isMcuConnectedSafe())) {
            last_mdns_attempt = now;
            resolveMcuAddress();

            if (mcu_ip_resolved) {
                Serial.printf("[WS] Configuring WebSocket to %s:80/ws\n", mcu_ip.toString().c_str());
                webSocket.begin(mcu_ip.toString().c_str(), 80, "/ws");
                webSocket.setExtraHeaders();
                webSocket.onEvent(webSocketEvent);
                webSocket.setReconnectInterval(3000);
            }
        }

        if (s_wsTxMutex != nullptr && xSemaphoreTake(s_wsTxMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            webSocket.loop();
            xSemaphoreGive(s_wsTxMutex);
        } else {
            webSocket.loop();
        }

        // Staggered post-connect HTTP sync: one request per loop iteration so webSocket.loop() is pumped between fetches
        if (s_onConnectSyncStep > 0 && !isBrewActiveSafe()) {
            uint8_t step = s_onConnectSyncStep;
            if (step == 1) {
                s_onConnectSyncStep = 2;
                fetchProfilesList();
            } else if (step == 2) {
                s_onConnectSyncStep = 3;
                fetchMcuStatus();
            } else {
                s_onConnectSyncStep = 0;
                fetchHistory();
            }
            return;
        }

        // Check if there is an asynchronous profile selection to dispatch to the Controller
        if (s_pendingProfileRequest) {
            char profIdToSend[32] = {0};
            ensureTelemetryMutex();
            if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                strlcpy(profIdToSend, s_pendingProfileId, sizeof(profIdToSend));
                s_pendingProfileRequest = false;
                xSemaphoreGive(g_telemetryMutex);
            }
            if (profIdToSend[0]) {
                prefs.begin("sb_display", false);
                prefs.putString("active_prof", profIdToSend);
                prefs.end();
                sendSelectProfile(profIdToSend);
            }
        }

        // Check if there is an asynchronous simulation mode toggle to dispatch to the Controller
        if (s_pendingSimRequest) {
            bool simVal = false;
            ensureTelemetryMutex();
            if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                simVal = s_pendingSimValue;
                s_pendingSimRequest = false;
                xSemaphoreGive(g_telemetryMutex);
            }
            prefs.begin("sb_display", false);
            prefs.putBool("sim_mode", simVal);
            prefs.end();

            if (webSocket.isConnected()) {
                char wsCmd[48];
                snprintf(wsCmd, sizeof(wsCmd), "{\"cmd\":\"SIM\",\"on\":%s}", simVal ? "true" : "false");
                safeWsSendTXT(wsCmd);
            }
            if (mcu_ip_resolved && WiFi.status() == WL_CONNECTED) {
                WiFiClient client;
                HTTPClient http;
                http.setTimeout(1000);
                String url = "http://" + mcu_ip.toString() + "/api/v1/bench/sim?on=" + (simVal ? "1" : "0");
                if (http.begin(client, url)) {
                    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
                    http.POST(simVal ? "on=1" : "on=0");
                    http.end();
                }
            }
        }

        // Periodic background Controller status sync (fallback when WebSocket is disconnected)
        static uint32_t last_mcu_status_sync = 0;
        if (now - last_mcu_status_sync > 5000) {
            last_mcu_status_sync = now;
            if (!isMcuConnectedSafe() && !isBrewActiveSafe()) {
                fetchMcuStatus();
            }
        }

        // Periodic background profile sync: every 90 seconds (never blocks or hammers the Controller)
        if (now - last_profile_sync > 90000) {
            last_profile_sync = now;
            if (!isBrewActiveSafe()) {
                fetchProfilesList();
            }
        }

        // The brew ledger: fetched when the UI asks for it and on a slow timer afterwards. It
        // never runs while a shot is in progress, so it cannot compete with the live telemetry,
        // and a request made during a brew simply waits for the shot to end.
        static uint32_t last_history_sync = 0;
        bool historyDue = s_historyRefreshRequested || (now - last_history_sync > 20000);
        if (historyDue && !isBrewActiveSafe()) {
            s_historyRefreshRequested = false;
            last_history_sync = now;
            fetchHistory();
        }
    } else {
        if (now - last_wifi_check > 10000) {
            last_wifi_check = now;
            if (strlen(wifi_ssid) > 0 && WiFi.status() != WL_CONNECTED) {
                Serial.printf("[Wi-Fi] Reconnecting to '%s'...\n", wifi_ssid);
                WiFi.disconnect();
                WiFi.begin(wifi_ssid, wifi_pass);
            }
        }
        if (sta_connect_start > 0 && (now - sta_connect_start > 75000)) {
            Serial.println("[Wi-Fi] Could not connect to Wi-Fi within 75s. Launching Setup Portal.");
            startSetupPortal();
        }
    }
}

// -----------------------------------------------------------------------------
// Interactive Commands & Machine Control
// -----------------------------------------------------------------------------
bool sendStartBrew() {
    if (!isMcuConnectedSafe()) return false;
    Serial.println("[WS] TX: {\"cmd\":\"START\"}");
    return safeWsSendTXT("{\"cmd\":\"START\"}");
}

bool sendStopBrew() {
    s_last_local_flush_ms = 0;
    if (!isMcuConnectedSafe()) return false;
    Serial.println("[WS] TX: {\"cmd\":\"STOP\",\"reason\":\"touch client stop\"}");
    return safeWsSendTXT("{\"cmd\":\"STOP\",\"reason\":\"touch client stop\"}");
}

bool sendFlush(float seconds) {
    s_last_local_flush_ms = millis();
    if (!isMcuConnectedSafe()) return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"FLUSH\",\"sec\":%.1f}", seconds);
    Serial.printf("[WS] TX: %s\n", buf);
    return safeWsSendTXT(buf);
}

bool sendTare() {
    bool conn = false;
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_telemetry.weight = 0.0f;
        g_telemetry.weight_flow = 0.0f;
        g_telemetry.t_ms = 0;
        g_telemetry.shot_time = 0.0f;
        conn = g_telemetry.mcu_connected;
        xSemaphoreGive(g_telemetryMutex);
    }
    if (!conn) return false;
    Serial.println("[WS] TX: {\"cmd\":\"TARE\"}");
    return safeWsSendTXT("{\"cmd\":\"TARE\"}");
}

bool sendSelectProfile(const char* id) {
    if (!mcu_ip_resolved || WiFi.status() != WL_CONNECTED) return false;

    // 1. Send via WebSocket if connected (instantaneous zero-HTTP-overhead)
    if (webSocket.isConnected()) {
        char wsCmd[64];
        snprintf(wsCmd, sizeof(wsCmd), "{\"cmd\":\"USE\",\"id\":\"%s\"}", id);
        safeWsSendTXT(wsCmd);
    }

    // 2. Also send via HTTP POST with id in query string AND body
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(1000);
    String url = "http://" + mcu_ip.toString() + "/api/v1/profiles/active?id=" + String(id);
    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        String postBody = "id=" + String(id);
        int httpCode = http.POST(postBody);
        http.end();
        return (httpCode == HTTP_CODE_OK || httpCode == 204);
    }
    return false;
}

void applySelectedProfile(int index) {
    s_last_local_profile_change = millis();

    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (index < 0 || index >= s_cachedProfileCount) {
            xSemaphoreGive(g_telemetryMutex);
            return;
        }
        g_telemetry.active_profile_idx = index;
        const CachedProfile& cp = s_cachedProfiles[index];
        strlcpy(g_telemetry.active_profile, cp.name, sizeof(g_telemetry.active_profile));
        strlcpy(g_telemetry.active_profile_id, cp.id, sizeof(g_telemetry.active_profile_id));
        g_telemetry.target_weight = cp.target_weight;
        g_telemetry.step_count = cp.step_count;
        for (int i = 0; i < cp.step_count; i++) {
            g_telemetry.steps[i] = cp.steps[i];
        }
        g_telemetry.total_profile_duration = cp.total_duration;
        g_telemetry.profiles_updated = true;

        strlcpy(s_pendingProfileId, cp.id, sizeof(s_pendingProfileId));
        s_pendingProfileRequest = true;

        Serial.printf("[Profiles] Switched profile to: %s (Target: %.1fg, Steps: %d)\n",
                      cp.name, cp.target_weight, cp.step_count);
        xSemaphoreGive(g_telemetryMutex);
    }
}

void setSimulationMode(bool enable) {
    s_last_local_sim_change = millis();

    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_telemetry.simulation_mode = enable;
        s_pendingSimValue = enable;
        s_pendingSimRequest = true;
        xSemaphoreGive(g_telemetryMutex);
    }
    Serial.printf("[Sim] Simulation mode queued: %s\n", enable ? "ENABLED" : "DISABLED");
}

bool isSimulationMode() {
    bool sim = false;
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sim = g_telemetry.simulation_mode;
        xSemaphoreGive(g_telemetryMutex);
    }
    return sim;
}

float getPlannedProfilePressure(float t) {
    ensureTelemetryMutex();
    if (g_telemetryMutex != nullptr && xSemaphoreTake(g_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int cnt = g_telemetry.step_count;
        if (cnt <= 0) {
            xSemaphoreGive(g_telemetryMutex);
            return 9.0f;
        }
        float cum = 0.0f;
        for (int i = 0; i < cnt; i++) {
            float dur = g_telemetry.steps[i].duration;
            if (dur <= 0.0f) dur = 1.0f;
            if (t <= cum + dur) {
                float prog = (t - cum) / dur;
                if (prog < 0.0f) prog = 0.0f;
                if (prog > 1.0f) prog = 1.0f;
                float s = g_telemetry.steps[i].start_val;
                float e = g_telemetry.steps[i].end_val;
                float val = s + (e - s) * prog;
                xSemaphoreGive(g_telemetryMutex);
                return val;
            }
            cum += dur;
        }
        float lastVal = g_telemetry.steps[cnt - 1].end_val;
        xSemaphoreGive(g_telemetryMutex);
        return lastVal;
    }
    return 9.0f;
}

const char* getWifiSsid() {
    return wifi_ssid;
}

static char mcu_ip_str[32] = {0};

const char* getControllerIp() {
    if (mcu_ip_resolved) {
        snprintf(mcu_ip_str, sizeof(mcu_ip_str), "%u.%u.%u.%u", mcu_ip[0], mcu_ip[1], mcu_ip[2], mcu_ip[3]);
        return mcu_ip_str;
    }
    return mcu_host;
}

bool isWifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void saveNetworkConfig(const char* ssid, const char* pass, const char* host) {
    prefs.begin("sb_display", false);
    if (ssid) prefs.putString("ssid", ssid);
    if (pass) prefs.putString("pass", pass);
    if (host) prefs.putString("mcu_host", host);
    prefs.end();

    if (ssid) strlcpy(wifi_ssid, ssid, sizeof(wifi_ssid));
    if (pass) strlcpy(wifi_pass, pass, sizeof(wifi_pass));
    if (host) strlcpy(mcu_host, host, sizeof(mcu_host));

    mcu_ip_resolved = false;
    WiFi.disconnect();
    WiFi.begin(wifi_ssid, wifi_pass);
    sta_connect_start = millis();
}
