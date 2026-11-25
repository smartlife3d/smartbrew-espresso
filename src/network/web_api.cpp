// =================================================================================================
//  web_api.cpp - see web_api.h
// =================================================================================================

#include "web_api.h"
#include "web_assets.h"
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
#include "wifi_portal.h"
#include "ota_service.h"
#include "brew_controller.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>

namespace {

static const char* const captiveProbePaths[] = {
    "/generate_204",
    "/gen_204",
    "/ncsi.txt",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/canonical.html",
    "/connecttest.txt"
};

// -------------------------------------------------------------------------------------------------
// Cross-origin guard
//
// The device serves its own cockpit, so every legitimate browser request is same-origin and
// carries either no Origin header or one whose authority matches the Host the request was sent
// to. A request originating from any other web page is refused before it can reach the pump,
// the configuration or the firmware.
//
// This matters because a "simple" POST (no custom headers, no JSON content type) is sent by the
// browser without a preflight: the browser would happily deliver it and merely hide the reply.
// A wildcard CORS header plus a simple POST is therefore enough for an arbitrary web page on the
// LAN to start a shot, rewrite the config or flash a firmware image.
// -------------------------------------------------------------------------------------------------
static String originAuthority(const String& origin) {
    String h = origin;
    const int scheme = h.indexOf("://");
    if (scheme >= 0) h = h.substring(scheme + 3);
    const int slash = h.indexOf('/');
    if (slash >= 0) h = h.substring(0, slash);
    return h;
}

static bool isLocalOrAppOrigin(const String& origin) {
    if (origin.equalsIgnoreCase("file://") || origin.startsWith("file://")) return true;
    if (!origin.startsWith("https://") && !origin.startsWith("http://")) return false;
    return originAuthority(origin).equalsIgnoreCase("appassets.androidplatform.net");
}

static String normalizeHost(String h) {
    if (h.endsWith(":80")) {
        h = h.substring(0, h.length() - 3);
    } else if (h.endsWith(":443")) {
        h = h.substring(0, h.length() - 4);
    }
    return h;
}

// Current Wi-Fi power-save mode, for diagnostics. WIFI_PS_NONE is 0, MIN_MODEM 1, MAX_MODEM 2.
// Modem sleep is what costs the telemetry link its latency, so it is worth being able to see
// the mode the driver is actually in.
static int wifiPsMode() {
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) != ESP_OK) return -1;
    return (int)ps;
}

static bool sameOriginRequest(AsyncWebServerRequest* r) {
    if (!r || !r->hasHeader("Origin")) return true;   // native client, curl, same-origin fetch
    const String origin = r->header("Origin");
    if (origin.length() == 0 || origin.equalsIgnoreCase("null") || isLocalOrAppOrigin(origin)) return true;
    const String oHost = normalizeHost(originAuthority(origin));
    if (oHost.length() == 0) return false;
    const String host = normalizeHost(r->header("Host"));
    if (host.length() == 0) return false;             // cannot verify -> refuse
    return oHost.equalsIgnoreCase(host);
}

// Returns true when the request was rejected (and the response has been sent).
static bool originRejected(AsyncWebServerRequest* r) {
    if (sameOriginRequest(r)) return false;
    SB_LOGW("Rejected cross-origin request (origin '%s', host '%s')",
            r && r->hasHeader("Origin") ? r->header("Origin").c_str() : "?",
            r && r->hasHeader("Host")    ? r->header("Host").c_str()    : "?");
    if (r) r->send(403, "application/json", "{\"ok\":false,\"err\":\"cross-origin request rejected\"}");
    return true;
}

// Flat malloc-allocated body accumulator for AsyncWebServerRequest::_tempObject.
// ~AsyncWebServerRequest() calls free(_tempObject) directly, so storing a single POD malloc block
// guarantees zero C++ destructor mismatch or internal buffer leak on any disconnect/abort path.
struct RawBodyBuf {
    size_t len;
    size_t cap;
    bool   overCap;
    char   data[1];
};

static void accumulateBodyChunk(AsyncWebServerRequest* r, const uint8_t* data, size_t len,
                                size_t index, size_t total, size_t maxBytes) {
    if (index == 0) {
        if (r->_tempObject) {
            free(r->_tempObject);
            r->_tempObject = nullptr;
        }
        const bool tooLarge = (total > maxBytes);
        const size_t initCap = tooLarge ? 16 : ((total > 0 && total <= maxBytes) ? total : 1024);
        RawBodyBuf* rb = static_cast<RawBodyBuf*>(malloc(sizeof(RawBodyBuf) + initCap));
        if (!rb) return;
        rb->len = 0;
        rb->cap = initCap;
        rb->overCap = tooLarge;
        rb->data[0] = '\0';
        r->_tempObject = rb;
    }
    RawBodyBuf* rb = static_cast<RawBodyBuf*>(r->_tempObject);
    if (!rb || rb->overCap) return;
    if (rb->len + len > maxBytes) {
        rb->overCap = true;
        return;
    }
    if (rb->len + len > rb->cap) {
        size_t newCap = std::min(maxBytes, std::max(rb->cap * 2, rb->len + len));
        RawBodyBuf* grown = static_cast<RawBodyBuf*>(realloc(rb, sizeof(RawBodyBuf) + newCap));
        if (!grown) {
            rb->overCap = true;
            return;
        }
        rb = grown;
        rb->cap = newCap;
        r->_tempObject = rb;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
}

// CORS for responses: echo the Origin back only when it is this device itself or the Android app's
// local asset origin - never a wildcard, so another web page cannot read the response.
static void addCorsHeaders(AsyncWebServerRequest* req, AsyncWebServerResponse* resp) {
    if (req->hasHeader("Origin")) {
        const String origin = req->header("Origin");
        if (origin.equalsIgnoreCase("null") || isLocalOrAppOrigin(origin)) {
            resp->addHeader("Access-Control-Allow-Origin", origin);
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "*");
            resp->addHeader("Vary", "Origin");
        } else {
            const String oHost = normalizeHost(originAuthority(origin));
            const String host  = normalizeHost(req->header("Host"));
            if (oHost.length() && host.length() && oHost.equalsIgnoreCase(host)) {
                resp->addHeader("Access-Control-Allow-Origin", origin);
                resp->addHeader("Vary", "Origin");
            }
        }
    }
}

static void sendJson(AsyncWebServerRequest* req, const String& json, int code = 200) {
    AsyncWebServerResponse* resp = req->beginResponse(code, "application/json; charset=utf-8", json);
    resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    addCorsHeaders(req, resp);
    req->send(resp);
}

static bool isValidProfileId(const char* id) {
    if (!id || !*id) return false;
    size_t len = strlen(id);
    if (len > 32) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = id[i];
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

static String sanitizeProfileId(const String& raw) {
    String s = raw;
    s.trim();
    int lastSlash = s.lastIndexOf('/');
    if (lastSlash >= 0) s = s.substring(lastSlash + 1);
    if (s.endsWith(".json")) s = s.substring(0, s.length() - 5);

    String clean = "";
    clean.reserve(s.length());
    for (size_t i = 0; i < s.length() && clean.length() < 32; ++i) {
        char c = s[i];
        if (isalnum(c)) {
            clean += (char)tolower(c);
        } else if (c == ' ' || c == '_' || c == '-') {
            if (clean.length() > 0 && clean[clean.length() - 1] != '_') {
                clean += '_';
            }
        }
    }
    while (clean.endsWith("_")) {
        clean = clean.substring(0, clean.length() - 1);
    }
    return clean;
}

static String statusJson() {
    char b[2048];
    const SystemConfig& c = g_config.cfg();

    Telemetry t;
    g_brew.snapshot(t);

    const PumpPreset& p = pumpPreset(c.pumpModel);
    const float kp = c.kp > 0.0f ? c.kp : p.kp;
    const float ki = c.ki > 0.0f ? c.ki : p.ki;
    const float kd = c.kd > 0.0f ? c.kd : p.kd;
    const esp_reset_reason_t rst = esp_reset_reason();

    const char* hname = c.mcuHostname[0] ? c.mcuHostname : "smartbrew";

    snprintf(b, sizeof(b),
        "{\"fw\":\"%s\",\"build\":\"%s\",\"hostname\":\"%s\",\"mcu_name\":\"%s\",\"chip\":\"%s\",\"cores\":%d,"
        "\"epochMs\":%llu,"
        "\"heap_free\":%u,\"heap_min\":%u,\"psram_free\":%u,"
        "\"state\":\"%s\",\"mode\":\"%s\",\"pump_owner\":\"%s\",\"faults\":%lu,\"log_drops\":%u,"
        "\"ctrl_us_min\":%u,\"ctrl_us_max\":%u,\"ctrl_us_avg\":%.0f,"
        "\"tele_body_us_max\":%u,\"tele_body_us_avg\":%u,\"tele_period_us_max\":%u,"
        "\"wifi_ps\":%d,\"ws_clients\":%u,\"ws_skipped\":%u,"
        "\"reset_reason\":%d,\"reset_reason_str\":\"%s\","
        "\"adc_source\":\"%s\",\"adc_mv\":%.1f,\"p_bar\":%.2f,\"p_target\":%.2f,"
        "\"ac_hz\":%u,\"ac_detected\":%u,\"ac_present\":%s,"
        "\"zc_pulses\":%u,\"zc_rejected\":%u,\"zc_missed\":%u,\"zc_period_us\":%.0f,"
        "\"duty_pct\":%.1f,\"ff_pct\":%.1f,\"kp\":%.2f,\"ki\":%.2f,\"kd\":%.2f,"
        "\"integral_frozen\":%s,\"sat\":%u,"
        "\"scale_state\":\"%s\",\"scale_name\":\"%s\",\"scale_mac\":\"%s\",\"scale_link\":\"%s\","
        "\"scale_batt\":%d,\"scale_updates\":%u,\"scale_stale\":%s,\"cutoff_events\":%u,"
        "\"button_pulses\":%u,\"button_blocked\":%u,\"btn_pending\":%s,\"btn_pending_ms\":%u,"
        "\"profile\":\"%s\",\"profile_id\":\"%s\",\"phase\":\"%s\",\"phase_idx\":%u,\"phase_pct\":%u,"
        "\"shot_count\":%u,\"pump\":\"%s\",\"sensor_mpa\":%s,\"mains_mode\":%d,"
        "\"sim\":%s,\"override\":%s,\"manual\":%s,\"weight_sim\":%s,\"bench\":%s,"
        "\"flush_rem_ms\":%u,\"sensor_open_warn\":%s}",
        SB_FW_VERSION, SB_BUILD_REV, hname, hname, ESP.getChipModel(), ESP.getChipCores(),
        (unsigned long long)g_history.wallTimeNow(),
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
        (unsigned)ESP.getFreePsram(),
        brewStateName(t.state),
        t.psmMode == PsmMode::PROFILE ? "PROFILE" : "PASSIVE",
        pumpOwnerName(t.pumpOwner),
        (unsigned long)g_brew.faults(),
        (unsigned)sb_log::logDrops(),
        (unsigned)g_brew.loopUsMin(), (unsigned)g_brew.loopUsMax(), g_brew.loopUsAvg(),
        (unsigned)g_web.teleBodyUsMax(), (unsigned)g_web.teleBodyUsAvg(),
        (unsigned)g_web.telePeriodUsMax(),
        wifiPsMode(), (unsigned)g_web.clients(), (unsigned)g_web.skippedFrames(),
        (int)rst, resetReasonStr(rst),
        g_pressure.sourceName(), g_pressure.lastMv(), t.pBar, t.pTarget,
        (unsigned)g_psm.mainsHz(), (unsigned)g_psm.detectedHz(), g_psm.acPowerPresent() ? "true" : "false",
        (unsigned)g_psm.pulsesAccepted(), (unsigned)g_psm.pulsesRejected(),
        (unsigned)g_psm.missedTimeoutCount(), g_psm.lastZcPeriodUs(),
        g_psm.dutyPct(), g_pid.feedForward(t.pTarget), kp, ki, kd,
        g_pid.integralFrozen() ? "true" : "false", g_pid.saturationCount(),
        g_scale.stateName(), g_scale.deviceName(), g_scale.mac(), scaleLinkName(g_scale.link()),
        g_scale.battery(), (unsigned)g_scale.updates(), g_scale.stale() ? "true" : "false",
        (unsigned)g_scale.cutoffEvents(),
        (unsigned)g_button.pulses(), (unsigned)g_button.blocked(),
        g_button.pending() ? "true" : "false", (unsigned)g_button.pendingAgeMs(),
        t.profileName, t.profileId, t.phaseName, (unsigned)t.phaseIdx,
        (unsigned)t.phasePct,
        (unsigned)g_brew.shotCount(),
        p.name,
        c.sensorRange == SENSOR_1_2MPA ? "\"1.2\"" : "\"1.6\"",
        c.mainsMode,
        g_brew.simulationMode() ? "true" : "false",
        g_brew.overrideActive() ? "true" : "false",
        g_pid.isManual() ? "true" : "false",
        g_brew.simWeightOverride() ? "true" : "false",
#if defined(SB_BENCH) && (SB_BENCH != 0)
        "true"
#else
        "false"
#endif
        ,
        (unsigned)t.flushRemMs,
        g_brew.sensorOpenWarnActive() ? "true" : "false"
    );
    return String(b);
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// WebSocket event handler
// -------------------------------------------------------------------------------------------------
void WebApi::onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                       AwsEventType type, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            SB_LOGI("WS client %u connected (%s)", (unsigned)client->id(),
                    client->remoteIP().toString().c_str());
            if (srv->count() > kMaxWsClients) {
                srv->cleanupClients(kMaxWsClients);
            }

            // Nagle must be OFF for the telemetry socket.
            //
            // ESPAsyncWebServer's AsyncWebSocket never disables Nagle (only AsyncEventSource and
            // WebServer do it), so the socket keeps it on by default. Every telemetry frame is one
            // small TCP segment, and Nagle holds segments 2..N until the peer acknowledges the
            // first one. At the brew rate of 20 Hz the dashboard therefore stopped receiving a
            // smooth 50 ms stream and instead got the frames in delayed-ACK bursts - and when a
            // segment was lost on the Wi-Fi link the stall grew into a full TCP retransmission
            // timeout (>2 s) instead of a single dropped frame.
            //
            // Telemetry is a live feed, so latency matters far more than the handful of bytes
            // that TCP_NODELAY costs us.
            if (client->client()) {
                client->client()->setNoDelay(true);
            }

            // Remember the client so the broadcast path can address it individually.
            // Prune any stale IDs whose sockets are no longer connected so rapid reconnects
            // never exhaust kMaxWsTracked slots before cleanupClients() reaps them.
            uint32_t staleIds[kMaxWsTracked];
            uint8_t currentIds[kMaxWsTracked];
            uint8_t currentCount = 0;
            portENTER_CRITICAL(&_wsMux);
            for (uint8_t i = 0; i < _wsIdCount; ++i) {
                staleIds[i] = _wsIds[i];
            }
            currentCount = _wsIdCount;
            portEXIT_CRITICAL(&_wsMux);

            bool keepMask[kMaxWsTracked] = {false};
            for (uint8_t i = 0; i < currentCount; ++i) {
                AsyncWebSocketClient* existing = srv->client(staleIds[i]);
                keepMask[i] = (existing != nullptr && existing->status() == WS_CONNECTED &&
                               staleIds[i] != client->id());
            }

            portENTER_CRITICAL(&_wsMux);
            uint8_t newCount = 0;
            for (uint8_t i = 0; i < _wsIdCount; ++i) {
                bool keep = true;
                for (uint8_t j = 0; j < currentCount; ++j) {
                    if (_wsIds[i] == staleIds[j] && !keepMask[j]) {
                        keep = false;
                        break;
                    }
                }
                if (keep && _wsIds[i] != client->id() && newCount < kMaxWsTracked - 1) {
                    _wsIds[newCount++] = _wsIds[i];
                }
            }
            if (newCount < kMaxWsTracked) {
                _wsIds[newCount++] = client->id();
            }
            _wsIdCount = newCount;
            portEXIT_CRITICAL(&_wsMux);
            (void)currentIds;

            sendTelemetryTo(client->id());
            break;
        }

        case WS_EVT_DISCONNECT: {
            SB_LOGI("WS client %u disconnected", (unsigned)client->id());
            portENTER_CRITICAL(&_wsMux);
            for (uint8_t i = 0; i < _wsIdCount; ++i) {
                if (_wsIds[i] == client->id()) {
                    _wsIds[i] = _wsIds[--_wsIdCount];
                    break;
                }
            }
            portEXIT_CRITICAL(&_wsMux);
            break;
        }

        case WS_EVT_DATA: {
            if (!data || len == 0) break;
            char buf[256];
            const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
            memcpy(buf, data, n);
            buf[n] = '\0';
            SB_LOGV("WS RX: %s", buf);

            String resp;
            if (!g_brew.handleJsonCommand(buf, resp)) {
                SB_LOGW("WS command rejected: %s", resp.c_str());
            }
            client->text(resp);
            break;
        }

        default:
            break;
    }
}

// -------------------------------------------------------------------------------------------------
void WebApi::begin(AsyncWebServer* http, uint16_t wsPort) {
    _http = http;

    // ------------------------------------------------------------------------------------------
    // Embedded gzipped single-page application (zero LittleFS dependency for web UI)
    // ------------------------------------------------------------------------------------------
    auto serveSpa = [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse(200, "text/html; charset=utf-8", index_html_gz, index_html_gz_len);
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        req->send(resp);
    };

    http->on("/", HTTP_GET, serveSpa);
    http->on("/index.html", HTTP_GET, serveSpa);
    http->on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest* req) {
        static const char kManifest[] =
            "{\"name\":\"Smart Brew Cockpit\",\"short_name\":\"SmartBrew\","
            "\"display\":\"standalone\",\"orientation\":\"any\","
            "\"background_color\":\"#07090e\",\"theme_color\":\"#07090e\",\"start_url\":\"/\"}";
        req->send(200, "application/manifest+json", kManifest);
    });

    // ------------------------------------------------------------------------------------------
    // WebSocket
    // ------------------------------------------------------------------------------------------
    _ws = new AsyncWebSocket("/ws", [this](AsyncWebSocket* srv, AsyncWebSocketClient* client,
                                          AwsEventType type, void* arg, uint8_t* data, size_t len) {
        this->onWsEvent(srv, client, type, data, len);
        (void)arg;
    });

    // Refuse a cross-origin WebSocket handshake. The cockpit is served by the device itself, so a
    // legitimate client always has an Origin that matches the Host it dialled. Without this, any
    // web page on the LAN could open a socket and drive the machine through the command channel.
    _ws->handleHandshake([](AsyncWebServerRequest* request) -> bool {
        if (sameOriginRequest(request)) return true;
        SB_LOGW("Rejected cross-origin WebSocket handshake (origin '%s', host '%s')",
                request && request->hasHeader("Origin") ? request->header("Origin").c_str() : "?",
                request && request->hasHeader("Host")    ? request->header("Host").c_str()    : "?");
        return false;
    });

    http->addHandler(_ws);

    // ------------------------------------------------------------------------------------------
    // REST API
    // ------------------------------------------------------------------------------------------
    http->on("/api/v1/status", HTTP_GET, [](AsyncWebServerRequest* r) {
        sendJson(r, statusJson());
    });

    // ---- config -------------------------------------------------------------------------------
    http->on("/api/v1/config", HTTP_GET, [](AsyncWebServerRequest* r) {
        SystemConfig& c = g_config.cfg();
        char b[1600];
        snprintf(b, sizeof(b),
            "{\"pump\":\"%s\",\"pumpModel\":%d,\"sensor_mpa\":\"%s\",\"mains_mode\":%d,"
            "\"mains_hz\":%u,\"force_ads\":%d,"
            "\"sensorZeroMv\":%.2f,\"sensorFsMv\":%.2f,\"emaAlpha\":%.3f,"
            "\"maxSafetyPressureBar\":%.2f,\"minSensorMv\":%.2f,"
            "\"kp\":%.3f,\"ki\":%.3f,\"kd\":%.3f,\"loopHz\":%.1f,\"headspaceBar\":%.1f,"
            "\"buttonPulseMs\":%u,\"buttonAckTimeoutMs\":%u,\"buttonMinRetriggerMs\":%u,"
            "\"scaleLagSec\":%.2f,\"scaleMac\":\"%s\",\"scaleLockoutSec\":%d,"
            "\"preInfusionProtectSec\":%d,\"otaGitHubUrl\":\"%s\",\"mcuHostname\":\"%s\",\"verboseLogging\":%d,"
            "\"topologyTimeoutSec\":%u,\"pressureFsBar\":%.1f,\"safetyBar\":%.2f,"
            "\"complianceMlPerBar\":%.2f,\"pumpPqExponent\":%.3f,\"pumpPqExponentActive\":%.3f,"
            "\"scaleSlopeWindowMs\":%u,\"scaleLagAutoLearn\":%u,"
            "\"pumpFlowScale\":%.3f,\"pumpFlowAutoCal\":%u,\"pressureCtrlMode\":%u}",
            pumpPreset(c.pumpModel).key, c.pumpModel,
            c.sensorRange == SENSOR_1_2MPA ? "1.2" : "1.6", c.mainsMode,
            (unsigned)g_psm.mainsHz(), c.useAds1115,
            c.sensorZeroMv, c.sensorFsMv, c.emaAlpha,
            c.maxSafetyPressureBar, c.minSensorMv,
            c.kp, c.ki, c.kd, 1.0f / c.loopDt, c.headspaceAntiWindupBar * 0.1f,
            c.buttonPulseMs, c.buttonAckTimeoutMs, c.buttonMinRetriggerMs,
            c.scaleLagSec, c.scaleMac, c.scaleLockoutSec,
            c.preInfusionProtectSec, c.otaGitHubUrl,
            c.mcuHostname[0] ? c.mcuHostname : "smartbrew",
            c.verboseLogging,
            (unsigned)c.topologyTimeoutSec,
            g_config.pressureFsBar(), g_config.safetyPressureBar(),
            c.complianceMlPerBar, c.pumpPqExponent, g_pid.pqExponent(),
            (unsigned)c.scaleSlopeWindowMs, (unsigned)c.scaleLagAutoLearn,
            c.pumpFlowScale, (unsigned)c.pumpFlowAutoCal, (unsigned)c.pressureCtrlMode);
        sendJson(r, String(b));
    });

    http->on("/api/v1/config", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (g_brew.state() != BrewState::IDLE) {
            sendJson(r, "{\"ok\":false,\"err\":\"cannot modify config while machine is not idle\"}", 409);
            return;
        }
        if (!r->hasArg("json")) { sendJson(r, "{\"ok\":false,\"err\":\"no json body\"}", 400); return; }

        DynamicJsonDocument doc(1024);
        if (deserializeJson(doc, r->arg("json"))) {
            sendJson(r, "{\"ok\":false,\"err\":\"malformed json\"}", 400);
            return;
        }
        SystemConfig& c = g_config.cfg();

        if (doc.containsKey("pump")) {
            const PumpPreset& p = pumpPresetFromKey(doc["pump"] | "");
            c.pumpModel = p.id;
        }
        if (doc["sensor_mpa"] == "1.2")      c.sensorRange = SENSOR_1_2MPA;
        if (doc["sensor_mpa"] == "1.6")      c.sensorRange = SENSOR_1_6MPA;
        if (doc.containsKey("mains_mode"))   c.mainsMode = (uint8_t)constrain(doc["mains_mode"] | 0, 0, 2);
        if (doc.containsKey("force_ads"))    c.useAds1115 = (uint8_t)constrain(doc["force_ads"] | 0, 0, 2);
        if (doc.containsKey("sensorZeroMv")) c.sensorZeroMv = doc["sensorZeroMv"].as<float>();
        if (doc.containsKey("sensorFsMv"))   c.sensorFsMv   = doc["sensorFsMv"].as<float>();
        if (doc.containsKey("emaAlpha"))     c.emaAlpha     = constrain(doc["emaAlpha"].as<float>(), 0.01f, 1.0f);
        if (doc.containsKey("maxSafetyPressureBar"))
            c.maxSafetyPressureBar = constrain(doc["maxSafetyPressureBar"].as<float>(), 0.0f, 25.0f);
        if (doc.containsKey("minSensorMv"))  c.minSensorMv  = constrain(doc["minSensorMv"].as<float>(), 50.0f, 1000.0f);
        if (doc.containsKey("kp")) c.kp = constrain(doc["kp"].as<float>(), 0.0f, 50.0f);
        if (doc.containsKey("ki")) c.ki = constrain(doc["ki"].as<float>(), 0.0f, 50.0f);
        if (doc.containsKey("kd")) c.kd = constrain(doc["kd"].as<float>(), 0.0f, 50.0f);
        if (doc.containsKey("headspaceBar"))
            c.headspaceAntiWindupBar = (uint8_t)constrain((int)(doc["headspaceBar"].as<float>() * 10.0f + 0.5f), 0, 99);
        if (doc.containsKey("buttonPulseMs"))
            c.buttonPulseMs = (uint16_t)constrain(doc["buttonPulseMs"].as<int>(), 20, 2000);
        if (doc.containsKey("scaleLagSec"))
            c.scaleLagSec = constrain(doc["scaleLagSec"].as<float>(), 0.0f, 3.0f);
        if (doc.containsKey("scaleLockoutSec"))
            c.scaleLockoutSec = (uint8_t)constrain(doc["scaleLockoutSec"].as<int>(), 0, 30);
        if (doc.containsKey("preInfusionProtectSec"))
            c.preInfusionProtectSec = (uint16_t)constrain(doc["preInfusionProtectSec"].as<int>(), 0, 30);
        if (doc.containsKey("topologyTimeoutSec"))
            c.topologyTimeoutSec = (uint16_t)constrain(doc["topologyTimeoutSec"].as<int>(), 10, 3600);
        if (doc.containsKey("complianceMlPerBar"))
            c.complianceMlPerBar = constrain(doc["complianceMlPerBar"].as<float>(), 0.0f, 10.0f);
        if (doc.containsKey("pumpPqExponent")) {
            // 0 = use the pump preset's exponent; otherwise a believable curve shape.
            const float e = doc["pumpPqExponent"].as<float>();
            c.pumpPqExponent = (e <= 0.0f) ? 0.0f : constrain(e, 0.5f, 4.0f);
        }
        if (doc.containsKey("scaleSlopeWindowMs"))
            c.scaleSlopeWindowMs = (uint16_t)constrain(doc["scaleSlopeWindowMs"].as<int>(),
                                                       (int)SB_SLOPE_WINDOW_MIN_MS,
                                                       (int)SB_SLOPE_WINDOW_MAX_MS);
        if (doc.containsKey("scaleLagAutoLearn"))
            c.scaleLagAutoLearn = doc["scaleLagAutoLearn"].as<bool>() ? 1 : 0;
        if (doc.containsKey("pumpFlowScale"))
            c.pumpFlowScale = flowScaleClamp(doc["pumpFlowScale"].as<float>());
        if (doc.containsKey("pumpFlowAutoCal"))
            c.pumpFlowAutoCal = doc["pumpFlowAutoCal"].as<bool>() ? 1 : 0;
        if (doc.containsKey("pressureCtrlMode"))
            c.pressureCtrlMode = doc["pressureCtrlMode"].as<int>() ? 1 : 0;
        if (doc.containsKey("otaGitHubUrl"))  strlcpy(c.otaGitHubUrl, doc["otaGitHubUrl"] | "", sizeof(c.otaGitHubUrl));
        if (doc.containsKey("scaleMac"))      strlcpy(c.scaleMac, doc["scaleMac"] | "AUTO", sizeof(c.scaleMac));
        if (doc.containsKey("mcuHostname")) {
            const char* hn = doc["mcuHostname"] | "smartbrew";
            if (hn && hn[0]) {
                char clean[sizeof(c.mcuHostname)];
                size_t j = 0;
                for (size_t i = 0; hn[i] && j < sizeof(clean) - 1; ++i) {
                    char ch = (char)tolower((unsigned char)hn[i]);
                    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
                        clean[j++] = ch;
                    }
                }
                clean[j] = '\0';
                if (clean[0] != '\0' && strcmp(c.mcuHostname, clean) != 0) {
                    strlcpy(c.mcuHostname, clean, sizeof(c.mcuHostname));
                    if (WiFi.status() == WL_CONNECTED) {
                        // mDNS is already running under the old name and MDNS.begin() cannot
                        // start it twice (mdns_init fails), so the new name only took effect after
                        // a reboot. Stop it and start it again under the new name.
                        MDNS.end();
                        if (MDNS.begin(c.mcuHostname)) {
                            SB_LOGI("mDNS name changed - now http://%s.local", c.mcuHostname);
                        } else {
                            SB_LOGW("mDNS restart under '%s' failed - the new name works after a reboot",
                                    c.mcuHostname);
                        }
                        MDNS.setInstanceName(c.mcuHostname);
                        MDNS.addService("http", "tcp", 80);
                        MDNS.addService("ws", "tcp", 80);
                        MDNS.addServiceTxt("http", "tcp", "name", (const char*)c.mcuHostname);
                    }
                }
            }
        }

        const bool saved = g_config.save();
        g_pid.applyConfig(c);
        g_button.setPulseWidthMs(c.buttonPulseMs);

        sendJson(r, String("{\"ok\":") + (saved ? "true" : "false") + "}");
    });

    // ---- profiles -----------------------------------------------------------------------------
    // NOTE: the profile routes use EXACT matchers on purpose.
    // AsyncWebServer's default matcher is "BackwardCompatible" (regex
    // ^{uri}(/.*)?$), so http->on("/api/v1/profiles", ...) also answers
    // "/api/v1/profiles/detail" — and because it is registered first it
    // shadows the detail route. The UI then received the profile LIST where
    // it expected one profile, rebuilt it through normalizeProfile() (whose
    // name fallback is the id) and overwrote the operator's display name with
    // the profile id. Exact matchers keep the five profile routes disjoint.
    http->on(AsyncURIMatcher::exact("/api/v1/profiles"), HTTP_GET, [](AsyncWebServerRequest* r) {
        Telemetry snap;
        g_brew.snapshot(snap);
        String activeId = snap.profileId;
        if (LittleFS.exists("/profiles/index.json")) {
            File idx = LittleFS.open("/profiles/index.json", "r");
            if (idx) {
                DynamicJsonDocument idoc(128);
                if (!deserializeJson(idoc, idx)) {
                    String stored = idoc["active"] | "";
                    if (!stored.isEmpty()) activeId = stored;
                }
                idx.close();
            }
        }
        if (activeId.isEmpty()) activeId = "classic_9bar";

        static const char* const kStandardPresets[] = {
            "classic_9bar", "turbo_shot", "blooming_allonge", "gentle_lever", "italian_dark",
            "londinium", "iuiuiu_classic", "adaptive", "blooming_flow", "slow_pi_flat"
        };
        const size_t kPresetCount = sizeof(kStandardPresets) / sizeof(kStandardPresets[0]);

        String out = "{\"active\":\"" + activeId + "\",\"items\":[";
        bool first = true;

        // 1. Output standard presets first in canonical sequence
        for (size_t i = 0; i < kPresetCount; ++i) {
            String path = String("/profiles/") + kStandardPresets[i] + ".json";
            if (LittleFS.exists(path)) {
                File pf = LittleFS.open(path, "r");
                if (pf) {
                    DynamicJsonDocument d(8192);
                    if (!deserializeJson(d, pf)) {
                        d["id"] = kStandardPresets[i];
                        d["active"] = (String(kStandardPresets[i]) == activeId);
                        if (!first) out += ",";
                        first = false;
                        serializeJson(d, out);
                    }
                    pf.close();
                }
            }
        }

        // 2. Output custom profiles
        File dir = LittleFS.open("/profiles");
        if (dir) {
            File f = dir.openNextFile();
            while (f) {
                String fullPath = f.name();
                String base = sanitizeProfileId(fullPath);
                if (f.isDirectory() || !fullPath.endsWith(".json") || base == "index") {
                    f = dir.openNextFile();
                    continue;
                }

                bool isPreset = false;
                for (size_t i = 0; i < kPresetCount; ++i) {
                    if (base == kStandardPresets[i]) {
                        isPreset = true;
                        break;
                    }
                }

                if (!isPreset) {
                    File pf = LittleFS.open(String("/profiles/") + base + ".json", "r");
                    if (pf) {
                        DynamicJsonDocument d(8192);
                        if (!deserializeJson(d, pf)) {
                            d["id"] = base;
                            d["active"] = (base == activeId);
                            if (!first) out += ",";
                            first = false;
                            serializeJson(d, out);
                        }
                        pf.close();
                    }
                }
                f = dir.openNextFile();
            }
            dir.close();
        }
        out += "]}";
        sendJson(r, out);
    });

    http->on(AsyncURIMatcher::exact("/api/v1/profiles"), HTTP_POST,
        [](AsyncWebServerRequest* r) {
            RawBodyBuf* rb = static_cast<RawBodyBuf*>(r->_tempObject);
            const bool overCap = (rb && rb->overCap);
            String body;
            if (rb && !rb->overCap) {
                body = String(rb->data, rb->len);
            }
            if (rb) {
                free(rb);
                r->_tempObject = nullptr;
            }
            if (originRejected(r)) return;
            if (overCap) {
                sendJson(r, "{\"ok\":false,\"err\":\"profile payload exceeds 16 KB limit\"}", 413);
                return;
            }
            if (body.isEmpty()) {
                if (r->hasArg("json")) {
                    body = r->arg("json");
                } else if (r->hasParam("json", true)) {
                    body = r->getParam("json", true)->value();
                } else if (r->hasArg("plain")) {
                    body = r->arg("plain");
                }
            }

            // In case of multipart envelope inside _tempObject
            if (body.indexOf("name=\"json\"") >= 0) {
                int start = body.indexOf("\r\n\r\n", body.indexOf("name=\"json\""));
                if (start >= 0) {
                    start += 4;
                    int end = body.indexOf("\r\n---", start);
                    if (end > start) {
                        body = body.substring(start, end);
                    } else {
                        body = body.substring(start);
                    }
                }
            } else if (body.startsWith("json=")) {
                body = body.substring(5);
            }
            body.trim();

            if (body.isEmpty()) {
                sendJson(r, "{\"ok\":false,\"err\":\"no json body\"}", 400);
                return;
            }

            Profile p{};
            char err[64];
            if (!g_profile.parseJson(body.c_str(), p, err, sizeof(err))) {
                sendJson(r, String("{\"ok\":false,\"err\":\"") + err + "\"}", 400);
                return;
            }

            String cleanId = sanitizeProfileId(p.id);
            if (cleanId.isEmpty()) cleanId = sanitizeProfileId(p.name);
            if (cleanId.isEmpty()) cleanId = "custom";
            strlcpy(p.id, cleanId.c_str(), sizeof(p.id));

            if (!isValidProfileId(p.id)) {
                sendJson(r, "{\"ok\":false,\"err\":\"invalid profile id\"}", 400);
                return;
            }

            if (!LittleFS.exists("/profiles")) {
                LittleFS.mkdir("/profiles");
            }

            char path[96];
            snprintf(path, sizeof(path), "/profiles/%s.json", p.id);
            char tmpPath[96];
            snprintf(tmpPath, sizeof(tmpPath), "/profiles/%s.tmp", p.id);
            File f = LittleFS.open(tmpPath, "w");
            if (!f) {
                sendJson(r, "{\"ok\":false,\"err\":\"cannot write LittleFS\"}", 500);
                return;
            }
            DynamicJsonDocument savedDoc(8192);
            if (!deserializeJson(savedDoc, body)) {
                savedDoc["id"] = p.id;
                // A profile the operator saves is theirs: without seed_version, the boot-time
                // seeding never overwrites it (see seedFilesystem()).
                savedDoc.remove("seed_version");
                String updatedBody;
                serializeJson(savedDoc, updatedBody);
                f.print(updatedBody);
            } else {
                f.print(body);
            }
            f.close();
            if (!LittleFS.rename(tmpPath, path)) {
                LittleFS.remove(path);
                if (!LittleFS.rename(tmpPath, path)) {
                    sendJson(r, "{\"ok\":false,\"err\":\"cannot rename profile file\"}", 500);
                    return;
                }
            }

            File idx = LittleFS.open("/profiles/index.tmp", "w");
            if (idx) {
                idx.printf("{\"active\":\"%s\"}", p.id);
                idx.close();
                if (!LittleFS.rename("/profiles/index.tmp", "/profiles/index.json")) {
                    LittleFS.remove("/profiles/index.json");
                    LittleFS.rename("/profiles/index.tmp", "/profiles/index.json");
                }
            }

            // The loaded profile belongs to the control task - it owns the phase table. Queue the
            // load instead of writing it here; the control task applies it when the machine is
            // idle. See the note in the /profiles/active route.
            g_brew.loadProfile(p.id);

            sendJson(r, String("{\"ok\":true,\"id\":\"") + p.id + "\"}");
        },
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
            static constexpr size_t kMaxProfileBodyBytes = 16384;
            accumulateBodyChunk(r, data, len, index, total, kMaxProfileBodyBytes);
        }
    );

    http->on(AsyncURIMatcher::exact("/api/v1/profiles/active"), HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        String id;
        if (r->hasArg("id")) id = r->arg("id");
        else if (r->hasParam("id")) id = r->getParam("id")->value();
        else if (r->hasParam("id", true)) id = r->getParam("id", true)->value();
        if (id.isEmpty() && r->hasArg("json")) {
            DynamicJsonDocument d(256);
            if (!deserializeJson(d, r->arg("json"))) {
                id = d["id"] | "";
            }
        }
        id = sanitizeProfileId(id);
        if (id.isEmpty() || !isValidProfileId(id.c_str())) {
            sendJson(r, "{\"ok\":false,\"err\":\"invalid id\"}", 400);
            return;
        }

        // The loaded profile - and with it the phase table - belongs to the control task, which
        // reads the phase table every 10 ms. Swapping it from the web task could rewrite it
        // underneath a running shot, so the web route only queues the request; the control task
        // applies it immediately when the machine is idle, or when it next goes idle.
        const bool busy = (g_brew.state() != BrewState::IDLE);
        File idx = LittleFS.open("/profiles/index.tmp", "w");
        if (idx) {
            idx.printf("{\"active\":\"%s\"}", id.c_str());
            idx.close();
            if (!LittleFS.rename("/profiles/index.tmp", "/profiles/index.json")) {
                LittleFS.remove("/profiles/index.json");
                LittleFS.rename("/profiles/index.tmp", "/profiles/index.json");
            }
        }
        if (!g_brew.loadProfile(id.c_str())) {
            sendJson(r, "{\"ok\":false,\"err\":\"command queue full\"}", 503);
            return;
        }
        sendJson(r, "{\"ok\":true,\"id\":\"" + id + "\",\"deferred\":" +
                       (busy ? "true" : "false") + "}");
    });

    http->on(AsyncURIMatcher::exact("/api/v1/profiles"), HTTP_DELETE, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        String id;
        if (r->hasArg("id")) id = r->arg("id");
        else if (r->hasParam("id")) id = r->getParam("id")->value();
        id = sanitizeProfileId(id);
        if (id.isEmpty() || !isValidProfileId(id.c_str())) {
            sendJson(r, "{\"ok\":false,\"err\":\"invalid id\"}", 400);
            return;
        }
        String path = "/profiles/" + id + ".json";
        sendJson(r, String("{\"ok\":") + (LittleFS.remove(path) ? "true" : "false") + "}");
    });

    http->on(AsyncURIMatcher::exact("/api/v1/profiles/detail"), HTTP_GET, [](AsyncWebServerRequest* r) {
        String id;
        if (r->hasArg("id")) id = r->arg("id");
        else if (r->hasParam("id")) id = r->getParam("id")->value();
        id = sanitizeProfileId(id);
        if (id.isEmpty() || !isValidProfileId(id.c_str())) {
            sendJson(r, "{\"ok\":false,\"err\":\"invalid id\"}", 400);
            return;
        }
        String path = "/profiles/" + id + ".json";
        if (!LittleFS.exists(path)) {
            const char* preset = ProfileEngine::getPresetJson(id.c_str());
            if (preset) {
                if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");
                File wf = LittleFS.open(path, "w");
                if (wf) { wf.print(preset); wf.close(); }
                sendJson(r, preset);
                return;
            }
            sendJson(r, "{\"ok\":false,\"err\":\"profile not found\"}", 404);
            return;
        }
        File f = LittleFS.open(path, "r");
        if (!f) {
            sendJson(r, "{\"ok\":false,\"err\":\"failed to open profile\"}", 500);
            return;
        }
        String body = f.readString();
        f.close();
        sendJson(r, body);
    });

    // ---- calibration ---------------------------------------------------------------------------
    http->on("/api/v1/calibrate/zero", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (g_brew.state() != BrewState::IDLE) {
            sendJson(r, "{\"ok\":false,\"err\":\"cannot calibrate while machine is active\"}", 409);
            return;
        }
        if (g_pressure.isCalibrating()) {
            sendJson(r, "{\"ok\":false,\"err\":\"zero calibration already in progress\"}", 409);
            return;
        }
        const uint8_t n = (uint8_t)constrain(r->hasArg("n") ? (int)r->arg("n").toInt() : 60, 8, 200);
        if (!g_pressure.startZeroCalibration(n)) {
            sendJson(r, "{\"ok\":false,\"err\":\"failed to start calibration\"}", 500);
            return;
        }
        sendJson(r, "{\"ok\":true,\"status\":\"calibrating\"}");
    });

    http->on("/api/v1/calibrate/zero", HTTP_GET, [](AsyncWebServerRequest* r) {
        const char* st = (g_pressure.calState() == CalState::RUNNING) ? "running" :
                         (g_pressure.calState() == CalState::SUCCESS) ? "ok" :
                         (g_pressure.calState() == CalState::FAILED) ? "error" : "idle";
        char b[192];
        snprintf(b, sizeof(b), "{\"ok\":true,\"state\":\"%s\",\"msg\":\"%s\",\"sensorZeroMv\":%.2f}",
                 st, g_pressure.calMessage(), g_config.cfg().sensorZeroMv);
        sendJson(r, String(b));
    });

    // ---- scale ---------------------------------------------------------------------------------
    // Scanning and pairing change state, so they are POST-only: a browser always sends an Origin
    // header with a cross-origin POST, which lets originRejected() refuse it. A GET can be fired
    // by any web page (an <img> tag) without one.
    http->on("/api/v1/scale/scan", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (r->hasArg("discover") && r->arg("discover") == "1") {
            g_scale.startScan(6, /*discoveryOnly=*/true);
            sendJson(r, "{\"ok\":true,\"discovering\":true}");
        } else {
            // Keep the String alive: c_str() of the temporary returned by arg() dangles at once.
            const String mac = r->hasArg("mac") ? r->arg("mac") : String("AUTO");
            g_scale.pairTo(mac.c_str());
            sendJson(r, String("{\"ok\":true,\"scanning\":true,\"target\":\"") + mac + "\"}");
        }
    });
    http->on("/api/v1/scale/discovered", HTTP_GET, [](AsyncWebServerRequest* r) {
        char buf[1024];
        int offset = snprintf(buf, sizeof(buf),
            "{\"scanning\":%s,\"discovering\":%s,\"connected\":%s,\"device_name\":\"%s\","
            "\"device_mac\":\"%s\",\"target_mac\":\"%s\",\"battery\":%d,\"scales\":[",
            g_scale.scanning() ? "true" : "false",
            g_scale.isDiscovering() ? "true" : "false",
            g_scale.connected() ? "true" : "false",
            g_scale.deviceName(),
            g_scale.mac(),
            g_config.cfg().scaleMac,
            g_scale.battery());
        for (uint8_t i = 0; i < g_scale.discoveredCount(); ++i) {
            const DiscoveredScale& ds = g_scale.discoveredScales()[i];
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                "%s{\"mac\":\"%s\",\"name\":\"%s\",\"link\":\"%s\",\"rssi\":%d}",
                i > 0 ? "," : "", ds.mac, ds.name, ds.link, ds.rssi);
        }
        snprintf(buf + offset, sizeof(buf) - offset, "]}");
        sendJson(r, String(buf));
    });
    http->on("/api/v1/scale/disconnect", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        g_scale.disconnect();
        sendJson(r, "{\"ok\":true}");
    });
    http->on("/api/v1/scale/tare", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        char err[64] = {0};
        if (g_brew.requestTare(err, sizeof(err))) {
            sendJson(r, "{\"ok\":true}");
        } else {
            sendJson(r, String("{\"ok\":false,\"err\":\"") + (err[0] ? err : "machine busy") + "\"}", 409);
        }
    });

    // ---- time synchronization -------------------------------------------------------------------
    http->on("/api/v1/time", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (r->hasArg("epoch")) {
            uint64_t ep = strtoull(r->arg("epoch").c_str(), nullptr, 10);
            if (ep > 1000000000000ULL) {
                g_history.setWallTime(ep);
            }
        }
        sendJson(r, "{\"ok\":true}");
    });

    // ---- history --------------------------------------------------------------------------------
    http->on("/api/v1/history", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (r->hasArg("slot")) {
            int slot = r->arg("slot").toInt();
            String shotJson;
            if (g_history.readShot(slot, shotJson)) {
                sendJson(r, shotJson);
            } else {
                sendJson(r, "{\"ok\":false,\"err\":\"slot not found\"}", 404);
            }
        } else {
            sendJson(r, g_history.summaryJson());
        }
    });

    http->on("/api/v1/history", HTTP_POST,
        [](AsyncWebServerRequest* r) {
            RawBodyBuf* rb = static_cast<RawBodyBuf*>(r->_tempObject);
            const bool overCap = (rb && rb->overCap);
            String body;
            if (rb && !rb->overCap) {
                body = String(rb->data, rb->len);
            }
            if (rb) {
                free(rb);
                r->_tempObject = nullptr;
            }
            if (originRejected(r)) return;
            if (overCap) {
                sendJson(r, "{\"ok\":false,\"err\":\"history payload exceeds 32 KB limit\"}", 413);
                return;
            }
            if (body.isEmpty()) {
                if (r->hasArg("json")) {
                    body = r->arg("json");
                } else if (r->hasParam("json", true)) {
                    body = r->getParam("json", true)->value();
                } else if (r->hasArg("plain")) {
                    body = r->arg("plain");
                }
            }
            body.trim();

            if (body.isEmpty()) {
                sendJson(r, "{\"ok\":false,\"err\":\"no body\"}", 400);
                return;
            }

            DynamicJsonDocument doc(16384);
            DeserializationError err = deserializeJson(doc, body);
            if (err) {
                sendJson(r, "{\"ok\":false,\"err\":\"invalid json\"}", 400);
                return;
            }

            const char* profId = doc["profileId"] | "custom";
            const char* profName = doc["profileName"] | "Custom Shot";
            float peak = doc["peakBar"] | 0.0f;
            float dose = doc["doseG"] | (doc["dose_g"] | 0.0f);
            uint32_t dur = doc["durationMs"] | (uint32_t)((doc["duration_s"] | 0.0f) * 1000.0f);
            uint64_t epoch = doc["epochMs"] | 0ULL;

            String sJson = "[]";
            if (doc.containsKey("s")) {
                serializeJson(doc["s"], sJson);
            }

            int slot = g_history.saveExternalShot(profId, profName, peak, dose, dur, epoch, sJson.c_str());
            if (slot >= 0) {
                g_web.broadcastShotSummary(dur, dose);
                sendJson(r, String("{\"ok\":true,\"slot\":") + slot + "}");
            } else {
                sendJson(r, "{\"ok\":false,\"err\":\"save failed\"}", 500);
            }
        },
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
            static constexpr size_t kMaxHistoryBodyBytes = 32768;
            accumulateBodyChunk(r, data, len, index, total, kMaxHistoryBodyBytes);
        }
    );

    http->on("/api/v1/history", HTTP_DELETE, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        const int slot = r->hasArg("slot") ? r->arg("slot").toInt() : -1;
        if (slot < 0) {
            for (int i = 0; i < 15; ++i) g_history.deleteShot(i);
            sendJson(r, "{\"ok\":true,\"cleared\":true}");
        } else {
            sendJson(r, String("{\"ok\":") + (g_history.deleteShot(slot) == 0 ? "true" : "false") + "}");
        }
    });

    // ---- OTA -------------------------------------------------------------------------------------
    http->on("/api/v1/ota/github", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        const String url = r->hasArg("url") ? r->arg("url") : String(g_config.cfg().otaGitHubUrl);
        if (!url.length()) { sendJson(r, "{\"ok\":false,\"err\":\"no url\"}", 400); return; }
        if (g_ota.result() == OtaResult::RUNNING) {
            sendJson(r, "{\"ok\":false,\"err\":\"OTA already in progress\"}", 409);
            return;
        }
        struct OtaParam {
            char url[128];
        };
        auto* p = new OtaParam();
        strlcpy(p->url, url.c_str(), sizeof(p->url));
        xTaskCreatePinnedToCore([](void* arg) {
            auto* param = static_cast<OtaParam*>(arg);
            g_ota.updateFromUrl(param->url);
            delete param;
            vTaskDelete(nullptr);
        }, "ota_pull", 16384, p, 1, nullptr, 0);

        sendJson(r, "{\"ok\":true,\"status\":\"started\"}");
    });

    // Upload firmware binary directly from browser
    http->on("/api/v1/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (originRejected(req)) return;
            bool ok = (g_ota.result() == OtaResult::OK);
            AsyncWebServerResponse* resp = req->beginResponse(ok ? 200 : 500, "text/plain",
                ok ? "OK" : g_ota.lastError());
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                xTaskCreate([](void*) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    ESP.restart();
                }, "reboot", 2048, nullptr, 1, nullptr);
            } else {
                OtaService::rearmAfterFailure();
            }
        },
        [](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
            (void)filename;
            if (!sameOriginRequest(req)) {
                if (index == 0) {
                    SB_LOGW("Rejected cross-origin OTA upload chunk");
                }
                return;
            }
            g_ota.onUploadChunk(data, len, index, req->contentLength(), final);
        }
    );

    http->on("/api/v1/ota/abort", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        g_ota.abort();
        sendJson(r, "{\"ok\":true,\"status\":\"aborted\"}");
    });

    // ---- commands over REST ---------------------------------------------------------------------
    http->on("/api/v1/cmd/START", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        char err[64] = {0};
        if (g_brew.requestStart(err, sizeof(err))) {
            sendJson(r, "{\"ok\":true}");
        } else {
            sendJson(r, String("{\"ok\":false,\"err\":\"") + (err[0] ? err : "busy") + "\"}");
        }
    });
    http->on("/api/v1/cmd/STOP", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (g_brew.requestStop("web stop")) {
            sendJson(r, "{\"ok\":true}");
        } else {
            sendJson(r, "{\"ok\":false,\"err\":\"not running\"}");
        }
    });
    http->on("/api/v1/cmd/FLUSH", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        char err[64] = {0};
        const float secs = r->hasArg("sec") ? r->arg("sec").toFloat() : 5.0f;
        if (g_brew.requestFlush(secs, err, sizeof(err))) {
            sendJson(r, "{\"ok\":true,\"cmd\":\"FLUSH\"}");
        } else {
            sendJson(r, String("{\"ok\":false,\"err\":\"") + (err[0] ? err : "busy") + "\"}");
        }
    });
    http->on("/api/v1/cmd/TARE", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        char err[64] = {0};
        if (g_brew.requestTare(err, sizeof(err))) {
            sendJson(r, "{\"ok\":true}");
        } else {
            sendJson(r, String("{\"ok\":false,\"err\":\"") + (err[0] ? err : "machine busy") + "\"}", 409);
        }
    });
    http->on("/api/v1/cmd/NEXT_PHASE", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        sendJson(r, String("{\"ok\":") + (g_brew.requestNextPhase() ? "true" : "false") + "}");
    });
    // W-04: Expose /api/v1/bench/sim in all builds so HTTP fallback for SIM mode always works
    http->on("/api/v1/bench/sim", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        String on = "off";
        if (r->hasArg("on")) on = r->arg("on");
        else if (r->hasParam("on")) on = r->getParam("on")->value();
        else if (r->hasParam("on", true)) on = r->getParam("on", true)->value();
        const bool enable = (on == "1" || on == "true" || on == "on");
        g_brew.setSimulationMode(enable);
        sendJson(r, String("{\"ok\":true,\"simulator\":") + (g_brew.simulationMode() ? "true" : "false") + "}");
    });
#if defined(SB_BENCH) && (SB_BENCH != 0)
    http->on("/api/v1/cmd/MANUAL_POWER", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (!r->hasParam("pct", true)) {
            sendJson(r, "{\"ok\":false,\"err\":\"pct required\"}", 400);
            return;
        }
        float pct = r->getParam("pct", true)->value().toFloat();
        if (pct < 0.0f) g_brew.clearManualPower();
        else            g_brew.setManualPower(pct);
        sendJson(r, "{\"ok\":true}");
    });

    http->on("/api/v1/cmd/OVERRIDE_PRESSURE", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (r->hasArg("bar")) {
            float bar = r->arg("bar").toFloat();
            g_brew.setPressureOverride(bar);
            sendJson(r, "{\"ok\":true,\"bar\":" + String(bar, 2) + "}");
        } else {
            g_brew.clearPressureOverride();
            sendJson(r, "{\"ok\":true,\"override\":false}");
        }
    });

    // ---- bench / diagnostics ---------------------------------------------------------------------
    http->on("/api/v1/bench/sim/bar", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        const float bar = r->arg("bar").toFloat();
        g_pressure.setSimBar(bar);
        sendJson(r, String("{\"ok\":true,\"bar\":") + String(bar, 2) + "}");
    });
    http->on("/api/v1/bench/sim/weight", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        const float g = r->arg("g").toFloat();
        g_brew.setSimWeight(g);
        sendJson(r, String("{\"ok\":true,\"g\":") + String(g, 1) + "}");
    });
    http->on("/api/v1/bench/duty", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        const float pct = r->arg("pct").toFloat();
        g_brew.setManualPower(pct);
        sendJson(r, String("{\"ok\":true,\"duty\":") + String(g_psm.dutyPct(), 1) + "}");
    });
    // Press the virtual brew button from anywhere on the network, for wiring the opto relay at a
    // machine that is not next to the PC. POST ms=<20..30000> (empty = configured length).
    // POST-only like every state route; the GET below serves a one-page remote for a phone.
    http->on("/api/v1/bench/btn", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        if (g_brew.state() != BrewState::IDLE) {
            sendJson(r, "{\"ok\":false,\"err\":\"machine not idle\"}", 409);
            return;
        }
        const uint32_t ms = r->hasArg("ms") ? (uint32_t)r->arg("ms").toInt() : 0;
        const ButtonResult res = ms ? g_button.pulseFor(ms) : g_button.pulse();
        const uint32_t held = ms ? (ms > 30000 ? 30000 : (ms < 20 ? 20 : ms)) : g_button.pulseWidthMs();
        SB_LOGI("Virtual button %s from web (%u ms)",
                res == ButtonResult::PULSED ? "PULSED" : "BLOCKED", (unsigned)held);
        sendJson(r, String("{\"ok\":") + (res == ButtonResult::PULSED ? "true" : "false") +
                    ",\"ms\":" + String(held) + "}",
                 res == ButtonResult::PULSED ? 200 : 409);
    });
    http->on("/api/v1/bench/btn", HTTP_GET, [](AsyncWebServerRequest* r) {
        static const char kPage[] PROGMEM =
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Brew button test</title>"
            "<style>body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;padding:20px}"
            "button{display:block;width:100%;margin:10px 0;padding:22px;font-size:20px;border:0;"
            "border-radius:12px;background:#06b6d4;color:#042f2e;font-weight:700}"
            "#o{margin-top:14px;font-family:monospace}</style>"
            "<h2>Brew button test (bench build)</h2>"
            "<button onclick='p(0)'>Press (configured length)</button>"
            "<button onclick='p(10000)'>Hold 10 s</button>"
            "<button onclick='p(30000)'>Hold 30 s</button>"
            "<div id=o>ready</div>"
            "<script>function p(ms){o.textContent='sending...';"
            "fetch('/api/v1/bench/btn'+(ms?'?ms='+ms:''),{method:'POST'})"
            ".then(r=>r.json()).then(j=>o.textContent=JSON.stringify(j))"
            ".catch(e=>o.textContent='error: '+e)}</script>";
        r->send_P(200, "text/html", kPage);
    });
#else
    http->on("/api/v1/cmd/MANUAL_POWER", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        sendJson(r, "{\"ok\":false,\"err\":\"bench feature disabled\"}", 403);
    });
    http->on("/api/v1/cmd/OVERRIDE_PRESSURE", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        sendJson(r, "{\"ok\":false,\"err\":\"bench feature disabled\"}", 403);
    });
#endif

    // ---- reboot / reset (asynchronous to avoid blocking async_tcp) -------------------------------
    http->on("/api/v1/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        sendJson(r, "{\"ok\":true,\"rebooting\":true}");
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(400));
            ESP.restart();
        }, "reboot", 2048, nullptr, 1, nullptr);
    });

    registerCaptivePortal(http);

    http->begin();
    SB_LOGI("Web API up: REST endpoints + ws://.../ws");
}

// -------------------------------------------------------------------------------------------------
void WebApi::registerCaptivePortal(AsyncWebServer* http) {
    for (const char* p : captiveProbePaths) {
        http->on(p, HTTP_GET, [](AsyncWebServerRequest* r) {
            r->redirect("http://192.168.4.1/wifi", 302);
        });
    }

    http->on("/wifi", HTTP_GET, [](AsyncWebServerRequest* r) {
        String html = "<!doctype html><html lang='en'><head>"
                      "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<title>Smart Brew Controller — Wi-Fi Setup</title><style>"
                      ":root{--bg:#0b0f14;--panel:#121922;--line:#22303f;--txt:#e6edf3;--dim:#8b9aab;--acc:#4ec9b0;--err:#f87171}"
                      "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--bg);color:var(--txt);"
                      "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;flex-direction:column;align-items:center;justify-content:flex-start;padding:24px 16px}"
                      ".card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:24px;width:100%;max-width:380px;box-shadow:0 8px 24px rgba(0,0,0,0.35)}"
                      "h2{margin:0 0 6px;color:var(--acc);font-size:20px;font-weight:650}"
                      "p{color:var(--dim);font-size:13px;margin:0 0 16px;line-height:1.45}"
                      ".alert{background:rgba(239,68,68,0.15);border:1px solid rgba(239,68,68,0.35);color:var(--err);padding:10px 12px;border-radius:8px;font-size:13px;margin-bottom:16px;line-height:1.4}"
                      "label{display:block;font-size:12px;color:var(--dim);margin-bottom:6px;text-transform:uppercase;letter-spacing:.05em;font-weight:600}"
                      "input{width:100%;padding:11px 12px;border-radius:8px;border:1px solid var(--line);background:#182231;color:var(--txt);font-size:15px;margin-bottom:16px}"
                      "input:focus{outline:none;border-color:var(--acc)}"
                      "button{width:100%;padding:13px;border-radius:8px;border:none;background:var(--acc);color:#06231d;font-size:15px;font-weight:650;cursor:pointer}"
                      "button:active{transform:translateY(1px);opacity:.9}"
                      ".actions{display:flex;justify-content:space-between;align-items:center;margin-top:16px}"
                      ".dash{color:var(--dim);font-size:12px;text-decoration:none}"
                      ".dash:hover{color:var(--acc)}"
                      ".dash-err{color:#ef4444;font-size:12px;text-decoration:none}"
                      ".dash-err:hover{text-decoration:underline}"
                      "form.inline{margin:0}button.dash-err{background:none;border:0;padding:0;margin:0;width:auto;"
                      "cursor:pointer;font:inherit;font-size:12px;font-weight:400;box-shadow:none}"
                      "</style></head><body><div class='card'>"
                      "<h2>Smart Brew Controller</h2>"
                      "<p>Connect the espresso controller to your home Wi-Fi network.</p>";

        if (strlen(g_wifi.lastError()) > 0) {
            html += "<div class='alert'>⚠️ " + String(g_wifi.lastError()) + "</div>";
        }

        const char* curSsid = g_wifi.ssid();
        html += "<form action='/save' method='POST'>"
                "<label for='s'>Wi-Fi Network (SSID)</label>"
                "<input type='text' id='s' name='ssid' required value='" + String(curSsid ? curSsid : "") + "' placeholder='Enter network name' autofocus>"
                "<label for='p'>Password</label>"
                "<input type='password' id='p' name='pass' placeholder='Password (blank keeps the saved one)'>"
                "<button type='submit'>Save & Connect</button>"
                "</form>"
                "<div class='actions'>"
                "<a class='dash' href='/'>&larr; Live Dashboard</a>";
        if (curSsid && *curSsid) {
            html += "<form action='/forget' method='POST' class='inline'>"
                    "<button type='submit' class='dash-err'>Clear Network</button></form>";
        }
        html += "</div></div></body></html>";
        r->send(200, "text/html", html);
    });

    http->on("/save", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        String ssid = r->hasArg("ssid") ? r->arg("ssid") : "";
        String pass = r->hasArg("pass") ? r->arg("pass") : "";
        ssid.trim();
        const bool ok = g_wifi.saveCredentials(ssid.c_str(), pass.c_str());
        String html = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<body style='font-family:-apple-system,sans-serif;background:#0b0f14;color:#e6edf3;"
                      "display:grid;place-items:center;min-height:100vh;margin:0;text-align:center;padding:16px'>"
                      "<div style='background:#121922;border:1px solid #22303f;border-radius:14px;padding:24px;max-width:380px'>"
                      "<h2 style='color:#4ec9b0;margin:0 0 12px'>Smart Brew Controller</h2>"
                      "<p style='font-size:15px;line-height:1.4'>" +
                      String(ok ? "Credentials saved! Restarting to join <b>" + ssid + "</b>..."
                                : "No credentials were submitted.") + "</p>"
                      "<p style='color:#8b9aab;font-size:13px;line-height:1.4'>Reconnect your phone to <b>" + ssid +
                      "</b>, then open:</p>"
                      "<p style='font-size:16px;font-weight:600'><a href='http://smartbrew.local' style='color:#4ec9b0;text-decoration:none'>http://smartbrew.local</a></p>"
                      "</div></body>";
        r->send(200, "text/html", html);
    });

    http->on("/api/v1/wifi", HTTP_GET, [](AsyncWebServerRequest* r) {
        char b[300];
        const bool isAP = (WiFi.getMode() == WIFI_AP);
        snprintf(b, sizeof(b),
                 "{\"ok\":true,\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"portal\":%s}",
                 isAP ? "AP" : "STA",
                 isAP ? g_wifi.apSsid() : WiFi.SSID().c_str(),
                 isAP ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str(),
                 isAP ? 0 : WiFi.RSSI(),
                 g_wifi.inPortal() ? "true" : "false");
        sendJson(r, String(b));
    });

    http->on("/api/v1/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        String ssid = r->hasArg("ssid") ? r->arg("ssid") : "";
        String pass = r->hasArg("pass") ? r->arg("pass") : "";
        if (ssid.isEmpty() && r->hasArg("json")) {
            DynamicJsonDocument d(256);
            if (!deserializeJson(d, r->arg("json"))) {
                ssid = d["ssid"] | "";
                pass = d["pass"] | "";
            }
        }
        ssid.trim();
        const bool ok = g_wifi.saveCredentials(ssid.c_str(), pass.c_str());
        sendJson(r, String("{\"ok\":") + (ok ? "true" : "false") + ",\"ssid\":\"" + ssid + "\"}");
    });

    http->on("/api/v1/logs", HTTP_GET, [](AsyncWebServerRequest* r) {
        String logs = sb_log::getRecentLogs();
        if (r->header("Accept").indexOf("application/json") >= 0 || r->hasArg("json")) {
            DynamicJsonDocument d(logs.length() + 256);
            d["ok"] = true;
            d["logs"] = logs;
            d["drops"] = sb_log::logDrops();
            String out;
            serializeJson(d, out);
            sendJson(r, out);
        } else {
            AsyncWebServerResponse* resp = r->beginResponse(200, "text/plain; charset=utf-8", logs);
            resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
            addCorsHeaders(r, resp);
            r->send(resp);
        }
    });

    http->on("/api/v1/logs", HTTP_DELETE, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        sb_log::clearRecentLogs();
        sendJson(r, "{\"ok\":true,\"cleared\":true}");
    });

    // Clearing the credentials drops the controller off the network, so it is POST-only and guarded
    // against cross-origin requests (see the scale scan note above).
    http->on("/forget", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (originRejected(r)) return;
        g_wifi.forgetCredentials();
        if (r->header("Accept").indexOf("application/json") >= 0) {
            sendJson(r, "{\"ok\":true,\"note\":\"credentials cleared, dropping into setup portal\"}");
        } else {
            r->redirect("/wifi");
        }
    });

    http->onNotFound([](AsyncWebServerRequest* r) {
        if (r->method() == HTTP_OPTIONS) {
            if (originRejected(r)) return;
            AsyncWebServerResponse* resp = r->beginResponse(204);
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "*");
            resp->addHeader("Access-Control-Max-Age", "86400");
            if (r->hasHeader("Origin")) {
                resp->addHeader("Access-Control-Allow-Origin", r->header("Origin"));
                resp->addHeader("Vary", "Origin");
            }
            r->send(resp);
            return;
        }
        if (g_wifi.inPortal()) {
            r->redirect("http://192.168.4.1/wifi");
        } else {
            r->send(404, "text/plain", "Not Found");
        }
    });
}

// -------------------------------------------------------------------------------------------------
// Send one binary telemetry frame to a single client.
// -------------------------------------------------------------------------------------------------
void WebApi::sendTelemetryTo(uint32_t clientId) {
    if (!_ws) return;
    uint8_t frame[SB_TELEMETRY_MAX_LEN];
    size_t len = 0;
    if (!g_brew.telemetryBin(frame, sizeof(frame), &len)) return;
    _ws->binary(clientId, frame, len);
}

// -------------------------------------------------------------------------------------------------
// Send one binary telemetry frame to every connected client.
//
// Telemetry is a live feed: a frame that is already 3 slots behind tells the dashboard nothing it
// does not already know, and delivering it later makes the graph replay old points in a burst. A
// client that has fallen behind is therefore skipped until it catches up, while the other clients
// keep receiving their stream at full rate.
// -------------------------------------------------------------------------------------------------
void WebApi::broadcast(const uint8_t* frame, size_t len) {
    if (!_ws || !frame) return;
    if (len < SB_TELEMETRY_MIN_LEN || len > SB_TELEMETRY_MAX_LEN) return;

    uint32_t ids[kMaxWsTracked];
    uint8_t count = 0;
    portENTER_CRITICAL(&_wsMux);
    count = _wsIdCount;
    for (uint8_t i = 0; i < count; ++i) {
        ids[i] = _wsIds[i];
    }
    portEXIT_CRITICAL(&_wsMux);

    if (count == 0) return;

    for (uint8_t i = 0; i < count; ++i) {
        AsyncWebSocketClient* c = _ws->client(ids[i]);
        if (!c) {
            continue;
        }
        if (c->queueLen() > 2) {
            _wsSkipped++;
            continue;
        }
        _ws->binary(ids[i], frame, len);
    }
}

void WebApi::broadcastShotSummary(uint32_t durMs, float doseG) {
    if (!_ws || _ws->count() == 0) return;
    // The dose is the yield that ended up in the cup. It is passed in rather than read back from
    // the history, because by now the shot has been written and the history has moved on.
    if (doseG < 0.0f) doseG = g_history.doseG();
    char b[256];
    snprintf(b, sizeof(b),
             "{\"evt\":\"shot_end\",\"duration_ms\":%lu,\"state\":\"%s\",\"peak_bar\":%.2f,\"dose_g\":%.1f,"
             "\"shots\":%u,\"faults\":%lu}",
             (unsigned long)durMs,
             brewStateName(g_brew.state()), g_brew.peakBar(), doseG,
             (unsigned)g_brew.shotCount(), (unsigned long)g_brew.faults());
    _ws->textAll(b);
}

uint32_t WebApi::clients() const { return _ws ? (uint32_t)_ws->count() : 0; }

// -------------------------------------------------------------------------------------------------
void WebApi::noteTelemetryCycle(uint32_t bodyUs, uint32_t periodUs, bool active) {
    if (bodyUs > _teleBodyUsMax) _teleBodyUsMax = bodyUs;
    _teleBodyUsAvg = _teleBodyUsAvg ? (_teleBodyUsAvg * 7 + bodyUs) / 8 : bodyUs;
    // The gap between two telemetry cycles is only a stall indicator while the machine is
    // running. At idle the task sleeps for 500 ms on purpose, and counting that would report
    // the designed idle cadence as the worst stall of the session.
    if (active && periodUs > _telePeriodUsMax) _telePeriodUsMax = periodUs;
}

void WebApi::loop() {
    static uint32_t lastCleanup = 0;
    const uint32_t now = millis();
    if (_ws && (now - lastCleanup >= 250)) {
        lastCleanup = now;
        _ws->cleanupClients();
    }
}

WebApi g_web;