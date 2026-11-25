#include "setup_web.h"

#include <DNSServer.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "scale_config.h"
#include "scale_identity.h"
#include "wifi_ota.h"

static WebServer   s_server(SETUP_WEB_PORT);
static DNSServer   s_dns;
static bool        s_active  = false;
static char        s_ssid[40] = {0};
static uint32_t    s_lastActivity = 0;
static bool        s_updateOk = false;
static char        s_updateMsg[64] = {0};

// ---------------------------------------------------------------------------
// Captive portal + AP
// ---------------------------------------------------------------------------
static void restoreBroadcastMode(const char* why) {
    s_server.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_active = false;
    Serial.printf("[setup] left setup mode (%s)\n", why);
}

// ---------------------------------------------------------------------------
// Web page (single page, inline CSS/JS, no external assets)
// ---------------------------------------------------------------------------
static const char kPage[] PROGMEM = R"RAWHTTP(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Espresso Scale setup</title><style>
*{box-sizing:border-box}
body{font:15px -apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#12141a;color:#e8eaf0}
.wrap{max-width:640px;margin:0 auto;padding:18px 16px 60px}
h1{font-size:20px;margin:0 0 4px}
.sub{color:#8b93a7;font-size:13px;margin-bottom:18px}
.card{background:#1c1f28;border:1px solid #2a2f3c;border-radius:12px;padding:14px;margin-bottom:14px}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:#9aa3b8;margin:0 0 10px}
.live{display:flex;gap:18px;align-items:baseline}
.big{font-size:34px;font-weight:600;letter-spacing:-.5px}
.unit{color:#8b93a7;font-size:14px}
label{display:block;font-size:12px;color:#9aa3b8;margin:10px 0 4px}
input[type=text],input[type=password]{width:100%;padding:10px;border-radius:8px;border:1px solid #2f3542;background:#12141a;color:#e8eaf0;font-size:15px}
button{width:100%;margin-top:12px;padding:11px;border:0;border-radius:8px;background:#3b82f6;color:#fff;font-size:15px;font-weight:600}
button.ghost{background:#2a2f3c;color:#c8cede}
button.warn{background:#b45309}
.row{display:flex;gap:10px} .row button{width:auto;flex:1}
#log{margin-top:10px;font-size:13px;color:#9aa3b8;white-space:pre-wrap}
.bar{height:6px;background:#2a2f3c;border-radius:4px;overflow:hidden;margin-top:10px}
.bar>i{display:block;height:100%;width:0;background:#3b82f6;transition:width .2s}
.kv{font-size:13px;color:#9aa3b8;line-height:1.7}
.kv b{color:#e8eaf0;font-weight:600}
</style></head><body><div class="wrap">
<h1>Espresso Scale</h1><div class="sub" id="hdr">loading&hellip;</div>

<div class="card">
 <h2>Live</h2>
 <div class="live"><span class="big" id="g">--</span><span class="unit" id="u">g</span>
 <span class="unit" id="batt">battery --%</span></div>
</div>

<div class="card">
 <h2>Home Wi-Fi</h2>
 <label>SSID</label><input type="text" id="ssid" autocomplete="off" placeholder="your network">
 <label>Password</label><input type="password" id="pass" autocomplete="off" placeholder="leave blank for open">
 <button class="ghost" onclick="saveWifi()">Save Wi-Fi</button>
 <div id="wifilog"></div>
</div>

<div class="card">
 <h2>Firmware</h2>
 <label>firmware.bin</label><input type="file" id="fw" accept=".bin">
 <button class="warn" onclick="upload()">Upload &amp; install</button>
 <div class="bar"><i id="bar"></i></div><div id="fwlog"></div>
 <button class="ghost" onclick="location.reload()">Refresh page</button>
</div>

<div class="card">
 <h2>Device</h2>
 <label>Bluetooth name (max 28 chars)</label><input type="text" id="bname" maxlength="28">
 <div class="row"><button class="ghost" onclick="saveName()">Save name</button>
 <button class="ghost" onclick="reboot()">Reboot</button></div>
 <div id="devlog"></div>
</div>
<div id="log"></div></div>
<script>
function j(u,o){return fetch(u,o).then(r=>r.json())}
function post(u,b){return j(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})}
function tick(){
 j('/api/state').then(d=>{
   document.getElementById('hdr').textContent='id '+d.id+' · fw '+d.fw+' · '+d.name+' · ch '+(d.ch||1)+(d.chAuto?' (auto)':'');
   document.getElementById('g').textContent=d.g.toFixed(2);
   document.getElementById('batt').textContent='battery '+d.batt+'%';
   if(!document.getElementById('ssid').value)d.ssidSet&&(document.getElementById('ssid').value='');
   if(!document.getElementById('bname').value)document.getElementById('bname').value=d.name;
 }).catch(()=>{});
}
setInterval(tick,1000);tick();
function saveWifi(){
 const ssid=document.getElementById('ssid').value,pass=document.getElementById('pass').value;
 post('/wifi',{ssid:ssid,pass:pass}).then(r=>{
  document.getElementById('wifilog').textContent=r.ok?('Saved. Rebooting into "'+ssid+'" saved config…'):('Error: '+r.msg);
 });
}
function saveName(){
 const n=document.getElementById('bname').value;
 post('/name',{name:n}).then(r=>{
  document.getElementById('devlog').textContent=r.ok?'Name saved (applies after reboot).':'Error: '+r.msg;
 });
}
function reboot(){post('/reboot',{}).then(()=>document.getElementById('log').textContent='Rebooting…');}
function upload(){
 const f=document.getElementById('fw').files[0];
 if(!f){document.getElementById('fwlog').textContent='Choose a firmware.bin first.';return;}
 const fd=new FormData();fd.append('firmware',f,f.name);
 const x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>{if(e.lengthComputable)document.getElementById('bar').style.width=(e.loaded*100/e.total).toFixed(0)+'%'};
 x.onload=()=>{document.getElementById('fwlog').textContent=x.responseText||('HTTP '+x.status);
   if(x.status===200&&/OK/.test(x.responseText))setTimeout(reboot,4000);};
 x.onerror=()=>document.getElementById('fwlog').textContent='Upload failed (lost connection).';
 document.getElementById('fwlog').textContent='Uploading '+f.name+' ('+Math.round(f.size/1024)+' kB)…';
 x.send(fd);
}
</script></body></html>)RAWHTTP";

// ---------------------------------------------------------------------------
// API handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
    s_server.send(200, "text/html", kPage);
}

static void handleApiState() {
    const ScaleSample s = stateRead();

    // Escape the user-settable name so quotes or backslashes cannot break JSON.
    char name[SCALE_NAME_MAX * 2 + 1];
    size_t o = 0;
    for (const char* in = scaleDeviceName(); *in != '\0' && o < sizeof(name) - 2; in++) {
        const char c = *in;
        if (c == '"' || c == '\\') name[o++] = '\\';
        if (o < sizeof(name) - 1) name[o++] = c;
    }
    name[o] = '\0';

    char json[350];
    snprintf(json, sizeof(json),
             "{\"fw\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"g\":%.2f,\"batt\":%u,"
             "\"ssidSet\":%s,\"ap\":\"%s\",\"ch\":%u,\"chAuto\":%s}",
             SCALE_FW_VERSION, name, scaleIdentityShortId(),
             (double)s.grams, (unsigned)s.batteryPct,
             wifiOtaHasCredentials() ? "true" : "false", s_ssid,
             (unsigned)WiFi.channel(), "false");
    s_server.send(200, "application/json", json);
}

static void handleSetWifi() {
    if (!s_server.hasArg("ssid")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"msg\":\"no ssid\"}");
        return;
    }
    const String ssid = s_server.arg("ssid");
    const String pass = s_server.arg("pass");
    if (!wifiOtaSetCredentials(ssid.c_str(), pass.c_str())) {
        s_server.send(400, "application/json", "{\"ok\":false,\"msg\":\"bad ssid\"}");
        return;
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSetName() {
    const String wanted = s_server.arg("name");
    if (wanted.length() == 0) {
        s_server.send(400, "application/json", "{\"ok\":false,\"msg\":\"empty\"}");
        return;
    }
    if (!scaleIdentitySetName(wanted.c_str())) {
        s_server.send(400, "application/json",
                      "{\"ok\":false,\"msg\":\"1-28 printable ASCII chars\"}");
        return;
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleReboot() {
    s_server.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
}

static void handleUploadResult() {
    char json[160];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"msg\":\"%s\"}",
             s_updateOk ? "true" : "false", s_updateMsg);
    s_server.send(s_updateOk ? 200 : 500, "application/json", json);
}

// Multipart firmware upload streamed straight into the spare app slot.
static void handleUpload() {
    HTTPUpload& upload = s_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[setup] firmware upload: %s\n", upload.filename.c_str());
        Update.begin(UPDATE_SIZE_UNKNOWN);
        strncpy(s_updateMsg, "writing", sizeof(s_updateMsg) - 1);
        s_updateOk = false;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.isRunning()) {
            Update.write(upload.buf, upload.currentSize);
        } else {
            strncpy(s_updateMsg, "write aborted", sizeof(s_updateMsg) - 1);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true) && !Update.hasError()) {
            s_updateOk = true;
            strncpy(s_updateMsg, "OK - rebooting into the new image", sizeof(s_updateMsg) - 1);
            Serial.println(F("[setup] firmware OK - rebooting"));
        } else {
            snprintf(s_updateMsg, sizeof(s_updateMsg), "error %d (old image kept)",
                     (int)Update.getError());
            Serial.printf("[setup] firmware FAILED: %s\n", s_updateMsg);
        }
    }
}

static void registerRoutes() {
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/state", HTTP_GET, handleApiState);
    s_server.on("/wifi", HTTP_POST, handleSetWifi);
    s_server.on("/name", HTTP_POST, handleSetName);
    s_server.on("/reboot", HTTP_POST, handleReboot);
    s_server.on("/update", HTTP_POST, handleUploadResult, handleUpload);
    s_server.onNotFound([]() { s_server.send(302, "text/plain", ""); });
    static const char* kHeaders[] = {"Authorization"};
    s_server.collectHeaders(kHeaders, 1);
}

// ---------------------------------------------------------------------------
// Portal task
// ---------------------------------------------------------------------------
static void portalTask(void* /*arg*/) {
    for (;;) {
        if (!s_active) {           // closed from the console (`ap exit`)
            vTaskDelete(NULL);
            return;
        }
        s_server.handleClient();
        s_dns.processNextRequest();

        if (WiFi.softAPgetStationNum() > 0) {
            s_lastActivity = millis();
        }
#if SETUP_IDLE_TIMEOUT_MS > 0
        if ((uint32_t)(millis() - s_lastActivity) > SETUP_IDLE_TIMEOUT_MS) {
            Serial.println(F("[setup] idle timeout - leaving setup mode"));
            setupWebLeave(false);
            vTaskDelete(NULL);
            return;
        }
#endif
        delay(2);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void startPortal() {
    snprintf(s_ssid, sizeof(s_ssid), "%s%s", OTA_HOSTNAME_PREFIX, scaleIdentityShortId());

    WiFi.mode(WIFI_AP);
    const bool ok = WiFi.softAP(s_ssid, SETUP_AP_PASSWORD, SETUP_AP_CHANNEL, 0,
                                SETUP_AP_MAX_CONNECTIONS);
    if (!ok) {
        Serial.println(F("[setup] softAP failed - continuing"));
        return;
    }

    // Captive portal: resolve every hostname to the portal address so the phone's
    // "sign in to network" popup lands on the page.
    s_dns.setTTL(3600);
    s_dns.start(53, "*", WiFi.softAPIP());

    registerRoutes();
    s_server.begin();
    s_active = true;
    s_lastActivity = millis();

    if (xTaskCreate(portalTask, "portal", SETUP_WEB_TASK_STACK, nullptr, 3, nullptr) != pdPASS) {
        Serial.println(F("[setup] cannot start portal task"));
        setupWebLeave(false);
        return;
    }

    Serial.printf("[setup] SETUP MODE: join Wi-Fi \"%s\" (password \"%s\"), open http://%s/\n",
                  s_ssid, SETUP_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

bool setupWebEnter(void) {
    if (s_active) {
        Serial.println(F("[setup] already in setup mode"));
        return false;
    }
    startPortal();
    return s_active;
}

void setupWebLeave(bool reboot) {
    if (s_active) restoreBroadcastMode("console");
    if (reboot) ESP.restart();
}

bool setupWebActive(void) { return s_active; }
const char* setupWebApSsid(void) { return s_ssid; }
uint32_t setupWebClientCount(void) { return (uint32_t)WiFi.softAPgetStationNum(); }

void setupWebInit(void) {
#if SETUP_HOLD_BOOT_TO_ENTER
    pinMode(PIN_TOUCH_OUT, INPUT);
    // The TTP223 output settles a little after power-up, so require the pad to
    // still read "touched" at the END of the window rather than at the start.
    const uint32_t start = millis();
    const uint8_t active = TOUCH_ACTIVE_HIGH ? HIGH : LOW;
    bool held = false;
    while ((uint32_t)(millis() - start) < SETUP_BOOT_HOLD_MIN_MS) {
        held = digitalRead(PIN_TOUCH_OUT) == active;
        if (!held) break;          // released early -> normal boot
        delay(20);
    }
    if (held) {
        Serial.println(F("[setup] touch held during boot - entering setup mode"));
        setupWebEnter();
        return;
    }
#endif
#if SETUP_AUTO_WHEN_UNCONFIGURED
    if (!wifiOtaHasCredentials()) {
        Serial.println(F("[setup] no Wi-Fi configured - entering setup mode"));
        setupWebEnter();
        return;
    }
#endif
    Serial.println(F("[setup] not required - normal broadcast mode"));
}