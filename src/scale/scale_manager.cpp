// =================================================================================================
//  scale_manager.cpp - see scale_manager.h
// =================================================================================================

#include "scale_manager.h"
#include "logger.h"
#include "network/aux_bridge.h"

#include <esp_wifi.h>
#include <WiFi.h>
#include <NimBLEBondMigration.h>
// Arduino-ESP32 3.3 frees the BLE controller's RAM before setup() unless a BLE library says it is
// in use. Without that, the heap takes over the controller's memory and esp_bt_controller_init()
// later crashes in malloc (LoadProhibited). Including the core's header registers a constructor
// that keeps the memory reserved; it costs nothing if the library already does the same.
#if __has_include(<esp32-hal-alloc-ble-mem.h>)
#include <esp32-hal-alloc-ble-mem.h>
#endif

// -------------------------------------------------------------------------------------------------
const char* scaleLinkName(ScaleLink l) {
    switch (l) {
        case ScaleLink::BOOKOO:   return "bookoo";
        case ScaleLink::TIMEMORE: return "timemore";
        case ScaleLink::ACAIA:    return "acaia";
        case ScaleLink::GENERIC:  return "generic";
        case ScaleLink::NONE:     break;
    }
    return "none";
}

const char* ScaleManager::stateName() const {
    switch (_state) {
        case ScaleState::IDLE:      return "idle";
        case ScaleState::SCANNING:  return "scanning";
        case ScaleState::CONNECTING:return "connecting";
        case ScaleState::CONNECTED: return "connected";
        case ScaleState::STALE:     return "stale";
    }
    return "idle";
}

#include "usb_phy.h"

namespace {
ScaleManager* s_scaleInstance = nullptr;

// How long to wait before trying another scale scan when the previous one found nothing.
// Fast 3s retry when paired so rebooted or power-cycled scales reconnect quickly.
constexpr uint32_t SCALE_RETRY_INTERVAL_MS = 3000UL;

class SbClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        (void)pClient;
        (void)reason;
        if (s_scaleInstance) {
            s_scaleInstance->onClientDisconnected();
        }
    }
};
static SbClientCallbacks s_clientCallbacks;
}  // namespace

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------
void ScaleManager::begin() {
    s_scaleInstance = this;
    _state = ScaleState::IDLE;
    _bleInit = false;
    // Only look for a scale on our own when one has actually been paired. With nothing paired,
    // the radio stays off and the Wi-Fi link keeps its low-latency mode - the operator can still
    // run a discovery scan from the dashboard at any time.
    _reconnectAfterMs = scaleConfigured() ? (millis() + 2500) : 0;
    SB_LOGI("Scale manager up. BLE initialisation deferred (target=%s, paired=%s)",
            g_config.cfg().scaleMac, scaleConfigured() ? "yes" : "no");
}

void ScaleManager::ensureBleInitialized() {
    if (_bleInit) return;
    SB_LOGI("Initializing NimBLE stack...");
    // Coexistence requires Wi-Fi modem sleep (MIN_MODEM) so BLE and Wi-Fi can share the radio.
    // If WIFI_PS_NONE is active, coex_core_enable() triggers an abort!
    //
    // This is why the BLE stack is only brought up when a scale is actually wanted: modem sleep
    // costs the telemetry link 30-130 ms of latency, which the dashboard graph shows directly.
    // scaleConfigured() keeps the radio down until there is a scale to talk to.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    sb_usb_phy_keepalive();
    delay(50);
    // NimBLE-Arduino 2.x stores bonds in a different NVS layout than 1.x. Convert any 1.x bond
    // (the Timemore Dot's) before the stack reads the store, or the scale would need pairing mode
    // again. The helper tells the formats apart by record size, so records that are already in
    // the 2.x layout are left alone and running it again is harmless. Once per boot is enough.
    static bool bondsMigrated = false;
    if (!bondsMigrated) {
        bondsMigrated = true;
        if (!NimBLEBondMigration::migrateBondStoreToCurrent()) {
            SB_LOGW("BLE bond store migration failed - a bonded scale may need pairing mode again");
        }
    }
    NimBLEDevice::init(std::string("SmartBrewController"));
    sb_usb_phy_keepalive();
    NimBLEDevice::setPower(3);   // +3 dBm (NimBLE 2.x takes dBm, not an esp_power_level_t)
    NimBLEDevice::setMTU(185);
    // Bond ("Just Works", no MITM, LE Secure Connections). The Timemore Dot remembers the device it
    // was paired with and, after a power cycle, advertises in whitelist mode that only admits that
    // bonded device. Without a bond the controller can connect only while the scale is in pairing
    // mode. NimBLE persists the keys in NVS, so the bond survives a reboot of the controller.
    NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    _bleInit = true;
    SB_LOGI("NimBLE initialized successfully.");
}

// -------------------------------------------------------------------------------------------------
// A scale is "configured" once the operator has picked one in Settings. Until then the BLE radio
// stays down, so the Wi-Fi link keeps its low-latency mode.
// -------------------------------------------------------------------------------------------------
bool ScaleManager::scaleConfigured() const {
    const char* want = g_config.cfg().scaleMac;
    if (!want || !want[0]) return false;
    if (strcmp(want, "AUTO") == 0) return false;   // "AUTO" means "not paired yet"
    return true;
}

// -------------------------------------------------------------------------------------------------
// Hand the radio back to Wi-Fi.
//
// Called after a discovery scan that found nothing while no scale is paired. The BLE stack is
// torn down completely, because ESP-IDF's coexistence core requires Wi-Fi modem sleep for as long
// as BLE is enabled - there is no way to keep the radio awake for Wi-Fi while BLE is up.
// -------------------------------------------------------------------------------------------------
void ScaleManager::releaseBleIfIdle() {
    if (_client || _pendingConnect) return;   // a scale is in use
    if (scaleConfigured()) return;            // a scale is expected, keep looking for it
    if (!_bleInit) return;
    if (_state == ScaleState::SCANNING || _discovering) return;

    SB_LOGI("No scale paired - BLE idle, deinitializing BLE radio.");
    stopScan();
    teardownClient();
    // Same hazard as in teardownClient(): deinit() zeroes the host's timer callouts, so let the
    // FreeRTOS timer task drain the commands that the scan stop and client delete just queued.
    vTaskDelay(pdMS_TO_TICKS(50));
    NimBLEDevice::deinit(true);
    _bleInit = false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    setState(ScaleState::IDLE);
}

void ScaleManager::end() {
    stopScan();
    teardownClient();
    _bleInit = false;
}

// -------------------------------------------------------------------------------------------------
ScaleDriver* ScaleManager::driverFor(ScaleLink l) {
    switch (l) {
        case ScaleLink::BOOKOO:   return &_bookoo;
        case ScaleLink::TIMEMORE: return &_timemore;
        case ScaleLink::ACAIA:    return &_acaia;
        case ScaleLink::GENERIC:  return &_generic;
        default: return nullptr;
    }
}

void ScaleManager::setState(ScaleState s) {
    if (_state == s) return;
    SB_LOGD("Scale state: %s -> %s", stateName(), s == ScaleState::SCANNING ? "scanning" :
            s == ScaleState::CONNECTING ? "connecting" : s == ScaleState::CONNECTED ? "connected" :
            s == ScaleState::STALE ? "stale" : s == ScaleState::CONNECTED ? "connected" : "idle");
    _state = s;
}

// -------------------------------------------------------------------------------------------------
// Scanning / pairing
// -------------------------------------------------------------------------------------------------
void ScaleManager::pairTo(const char* macOrAuto) {
    if (macOrAuto) {
        const char* val = (strcmp(macOrAuto, "AUTO") == 0) ? "" : macOrAuto;
        strlcpy(g_config.cfg().scaleMac, val, sizeof(g_config.cfg().scaleMac));
        g_config.save();
    }
    portENTER_CRITICAL(&_readingsMux);
    strlcpy(_pendingPairMac, macOrAuto ? macOrAuto : "AUTO", sizeof(_pendingPairMac));
    _pendingPairReq = true;
    portEXIT_CRITICAL(&_readingsMux);
}

void ScaleManager::startScan(uint8_t seconds, bool discoveryOnly) {
    if (_brewing) return; // Never start a radio-heavy BLE scan while an espresso shot is running
    portENTER_CRITICAL(&_readingsMux);
    _pendingScanSeconds = seconds;
    _pendingScanDiscoveryOnly = discoveryOnly;
    _discovering = discoveryOnly;
    _pendingScanReq = true;
    portEXIT_CRITICAL(&_readingsMux);
}

void ScaleManager::doStartScan(uint8_t seconds, bool discoveryOnly) {
    if (_brewing) return; // Never start a radio-heavy BLE scan while an espresso shot is running
    if (_state == ScaleState::CONNECTING) {
        SB_LOGW("BLE scan requested while connecting - ignored");
        return;
    }
    ensureBleInitialized();
    s_scaleInstance = this;

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) { setState(ScaleState::IDLE); return; }

    if (scan->isScanning()) {
        // A reconnect scan that is already running covers a second reconnect request. Anything
        // else - a discovery request in particular - restarts the scan in the requested mode.
        if (!discoveryOnly && !_discovering) return;
        scan->stop();   // no scanCompleteCb on stop(); the state is set again just below
    }

    _discovering = discoveryOnly;
    _pendingConnect = false;
    _pendingReleaseBle = false;

    // A connected scale stops advertising, so a discovery scan would never list it. Seed the list
    // with it so the dashboard still shows the paired scale next to the others in range.
    const bool linked = (_client != nullptr && _client->isConnected());
    if (_discovering) {
        _discoveredCount = 0;
        memset(_discoveredScales, 0, sizeof(_discoveredScales));
        if (linked) {
            DiscoveredScale& ds = _discoveredScales[_discoveredCount++];
            strlcpy(ds.mac, _mac, sizeof(ds.mac));
            strlcpy(ds.name, _name[0] ? _name : "Scale", sizeof(ds.name));
            strlcpy(ds.link, scaleLinkName(_link), sizeof(ds.link));
            ds.rssi = (int8_t)_client->getRssi();
        }
    }
    // A discovery scan beside a live link leaves the link state alone: the scale stays CONNECTED.
    if (!linked) setState(ScaleState::SCANNING);

    scan->clearResults();           // Fresh scan results: discard cache so target is immediately seen
    scan->setActiveScan(true);      // we need the device name and service list to pick a family
    // 40 ms window every 160 ms (25% duty cycle) preserves Wi-Fi bandwidth for web & WebSocket
    scan->setInterval(160);
    scan->setWindow(40);
    // Results and the end of the scan arrive through the static callback object.
    scan->setScanCallbacks(&s_scaleCallbacks, /*wantDuplicates=*/false);

    if (!scan->start((uint32_t)seconds * 1000UL, /*isContinue=*/false, /*restart=*/true)) {
        SB_LOGE("Failed to start BLE scan");
        setState(ScaleState::IDLE);
        return;
    }

    SB_LOGI("Scale scan started for %us (target %s, discover=%s)",
            (unsigned)seconds, g_config.cfg().scaleMac, _discovering ? "yes" : "no");
    _scanDeadlineMs = millis() + (uint32_t)seconds * 1000UL + 1500;
}

void ScaleManager::scanCompleteCb(const NimBLEScanResults& results) {
    SB_LOGI("Scale scan finished: %u device(s) seen.", results.getCount());
    ScaleManager* m = s_scaleInstance;
    if (m) {
        m->_discovering = false;
        m->_scanDeadlineMs = 0;
        if (m->_state == ScaleState::SCANNING) {
            m->setState(m->_client ? ScaleState::CONNECTED : ScaleState::IDLE);
            if (!m->_client && !m->_pendingConnect) {
                if (m->scaleConfigured()) {
                    // Nothing matched - keep looking. The scale is usually switched on after the
                    // controller, so a single missed scan must not end the search.
                    m->_reconnectAfterMs = millis() + SCALE_RETRY_INTERVAL_MS;
                    SB_LOGI("No scale matched - next scan in %us",
                            (unsigned)(SCALE_RETRY_INTERVAL_MS / 1000UL));
                } else {
                    m->_pendingReleaseBle = true;
                }
            }
        }
    }
}

void ScaleManager::onAdvertised(const NimBLEAdvertisedDevice* dev) {
    ScaleManager* m = s_scaleInstance;
    if (!m || !dev) return;
    // Outside discovery, a busy link means there is nothing to look for. A discovery scan lists the
    // scales in range even while one is connected; it never connects (see below).
    if (!m->_discovering &&
        (m->_pendingConnect || m->_state == ScaleState::CONNECTING ||
         m->_state == ScaleState::CONNECTED)) {
        return;
    }

    const std::string rawName = dev->getName();
    char name[28] = {0};
    strlcpy(name, rawName.c_str(), sizeof(name));

    // Collect the advertised service UUIDs so we can identify the scale family.
    char uuidList[256] = {0};
    const uint8_t nSvcs = dev->getServiceUUIDCount();
    for (uint8_t i = 0; i < nSvcs && i < 8; ++i) {
        char tmp[44];
        snprintf(tmp, sizeof(tmp), "%s ", dev->getServiceUUID(i).toString().c_str());
        strlcat(uuidList, tmp, sizeof(uuidList));
    }

    ScaleLink link = ScaleLink::NONE;
    if (!m->matchScale(name, uuidList, link)) return;

    // Timemore open protocol (§2.1): manufacturer data FF FF <broadcast type> <device type>.
    // Broadcast type 01 = whitelist (only the bonded central may connect), 02 = general.
    // Device type 01 = DOT TES017. Right after power-on the Dot advertises without its name, so
    // service 0xFFF0 alone would match the Generic family; the manufacturer data settles it.
    if (dev->haveManufacturerData()) {
        const std::string md = dev->getManufacturerData();
        if (md.size() == 4 && (uint8_t)md[0] == 0xFF && (uint8_t)md[1] == 0xFF &&
            ((uint8_t)md[2] == 0x01 || (uint8_t)md[2] == 0x02) && strcasestr(uuidList, "fff0")) {
            link = ScaleLink::TIMEMORE;
        }
    }

    const std::string macStr = dev->getAddress().toString();

    // Cache into discovered list so UI can display all scales seen by the controller
    m->rememberScale(macStr.c_str(), name, link, (int8_t)dev->getRSSI());

    if (m->_discovering) {
        SB_LOGI("scan: discovered candidate scale %s [%s] rssi=%d name=%s",
                macStr.c_str(), scaleLinkName(link), (int)dev->getRSSI(), name);
        return;
    }

    const char* want = g_config.cfg().scaleMac;
    if (!want || !want[0] || strcmp(want, "AUTO") == 0) {
        // No scale paired - do not auto-connect
        return;
    }

    if (strcasecmp(macStr.c_str(), want) != 0) {
        SB_LOGV("scan: ignoring %s (only want %s)", macStr.c_str(), want);
        return;
    }

    SB_LOGI("scan: found %s [%s] rssi=%d name=%s", macStr.c_str(), scaleLinkName(link),
            (int)dev->getRSSI(), name);

    // Defer stopScan and connect to task context (tick) to avoid deadlocks inside NimBLE callback
    m->_pendingConnect = true;
    m->_pendingAddr = dev->getAddress();
    m->_pendingLink = link;
    strlcpy(m->_name, name[0] ? name : "scale", sizeof(m->_name));
}

// Adds a scale to the discovered list, or refreshes its entry. A real name replaces the "Scale"
// placeholder, because the Dot advertises without its name right after power-on. Called from the
// NimBLE host task (advertisements) and from the "ble" task (connect), hence the lock.
void ScaleManager::rememberScale(const char* mac, const char* name, ScaleLink link, int8_t rssi) {
    if (!mac || !*mac) return;
    const bool haveName = name && *name;
    portENTER_CRITICAL(&_readingsMux);
    DiscoveredScale* ds = nullptr;
    for (uint8_t i = 0; i < _discoveredCount; ++i) {
        if (strcasecmp(_discoveredScales[i].mac, mac) == 0) { ds = &_discoveredScales[i]; break; }
    }
    if (!ds && _discoveredCount < 8) {
        ds = &_discoveredScales[_discoveredCount++];
        strlcpy(ds->mac, mac, sizeof(ds->mac));
        strlcpy(ds->name, "Scale", sizeof(ds->name));
    }
    if (ds) {
        if (haveName) strlcpy(ds->name, name, sizeof(ds->name));
        strlcpy(ds->link, scaleLinkName(link), sizeof(ds->link));
        ds->rssi = rssi;
    }
    portEXIT_CRITICAL(&_readingsMux);
}

void ScaleManager::stopScan() {
    _discovering = false;
    if (_bleInit) {
        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan) scan->stop();
    }
    if (_state == ScaleState::SCANNING) setState(_client ? ScaleState::CONNECTED : ScaleState::IDLE);
}

// -------------------------------------------------------------------------------------------------
bool ScaleManager::matchScale(const char* name, const char* uuidList, ScaleLink& outLink) const {
    const auto has = [&](const char* u) { return (uuidList && strcasestr(uuidList, u) != nullptr); };

    // Name matching MUST be case-insensitive. A scale advertises whatever spelling the vendor
    // firmware happens to use - "Dot", "DOT", "dot", "Black Mirror", "black mirror", "Nano"...
    const auto hasName = [&](const char* n) {
        return (name && *name) && (strcasestr(name, n) != nullptr);
    };

    // Bookoo / Themis / 0x0FFE companion scale check (service: "00000ffe", "0ffe", or matching name)
    if (has("00000ffe") || has("0ffe") || hasName("bookoo") || hasName("themis") ||
        hasName("smartbrew") || hasName("espresso_scale")) {
        outLink = ScaleLink::BOOKOO;
        return true;
    }
    // Timemore Black Mirror / Dot / Nano
    if (has("2a9d") || has("181d") || has("2a9e") ||
        hasName("black mirror") || hasName("dot") || hasName("nano") || hasName("timemore")) {
        outLink = ScaleLink::TIMEMORE;
        return true;
    }
    // Acaia Lunar / Pearl / Pyxis
    if (has("2a80") || has("1820") || has("49535343") ||
        hasName("lunar") || hasName("pearl") || hasName("pyxis") || hasName("acaia")) {
        outLink = ScaleLink::ACAIA;
        return true;
    }
    // Generic Espresso Scales: Decent, Felicita Arc, Cocinare, Skale, or device with "scale" in name
    if (hasName("decent") || hasName("felicita") || hasName("arc") || hasName("cocinare") ||
        hasName("skale") || hasName("scale")) {
        outLink = ScaleLink::GENERIC;
        return true;
    }

    // In targeted mode (explicit MAC configured in settings), allow vendor service UUIDs
    const char* want = g_config.cfg().scaleMac;
    const bool autoPair = (strcmp(want, "AUTO") == 0);
    if (!autoPair && (has("ffe0") || has("fff0") || has("ffe1") || has("fff4"))) {
        outLink = ScaleLink::GENERIC;
        return true;
    }
    return false;
}

bool ScaleManager::connectTo(NimBLEAddress addr, ScaleLink link, const char* name) {
    setState(ScaleState::CONNECTING);

    // Teardown previous client and state BEFORE setting the new driver
    teardownClient();

    _driver = driverFor(link);
    if (!_driver) {
        setState(ScaleState::IDLE);
        return false;
    }
    _link = link;
    strlcpy(_name, (name && *name) ? name : "scale", sizeof(_name));
    strlcpy(_mac, addr.toString().c_str(), sizeof(_mac));

    _client = NimBLEDevice::createClient();
    if (!_client) {
        _driver = nullptr;
        setState(ScaleState::IDLE);
        return false;
    }
    _client->setClientCallbacks(&s_clientCallbacks, false);
    _client->setConnectTimeout(4000); // 4 s (milliseconds in NimBLE 2.x) to avoid long blocking hangs
    // NimBLE 2.x retries a failed connection establishment on its own (2 retries by default). The
    // loop below already retries and logs each attempt, so switch the library's retries off.
    _client->setConnectRetries(0);

    SB_LOGI("Connecting to %s (%s) at %s...", _name, scaleLinkName(link), _mac);
    // "Connection Failed to be Established" (HCI 0x3E, NimBLE status 574) is usually transient:
    // the peer missed the first connection events, often because Wi-Fi held the shared radio.
    // Retry the connect straight away instead of paying for another 6 s scan.
    constexpr int CONNECT_ATTEMPTS = 3;
    bool linked = false;
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS && !linked; ++attempt) {
        linked = _client->connect(addr);
        if (!linked) {
            SB_LOGW("BLE connect attempt %d/%d to %s failed (status %d)", attempt,
                    CONNECT_ATTEMPTS, _mac, _client->getLastError());
            if (attempt < CONNECT_ATTEMPTS) vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    if (!linked) {
        SB_LOGE("BLE connect failed to %s", _mac);
        teardownClient();
        setState(ScaleState::IDLE);
        return false;
    }

    // Discover every characteristic of every service up front. Without this, each
    // getCharacteristic(uuid) below runs NimBLE's "discover characteristics by UUID" procedure,
    // which some scale firmwares answer wrongly, so a characteristic that exists is reported missing.
    // A full discovery uses the plain procedure every peripheral supports, and later lookups by
    // UUID are served from the cache it fills.
    const std::vector<NimBLERemoteService*>& svcs = _client->getServices(/*refresh=*/true);
    for (auto* s : svcs) s->getCharacteristics(/*refresh=*/true);


    // A scale that advertised without its name (the Dot after power-on) still has one in the GAP
    // Device Name characteristic (0x2A00). Read it so the dashboard shows "TIMEMORE_Dot", not "scale".
    if (!_name[0] || strcmp(_name, "scale") == 0) {
        NimBLERemoteService* gap = _client->getService(NimBLEUUID((uint16_t)0x1800));
        NimBLERemoteCharacteristic* dn =
            gap ? gap->getCharacteristic(NimBLEUUID((uint16_t)0x2A00)) : nullptr;
        if (dn && dn->canRead()) {
            const std::string devName = dn->readValue();
            if (!devName.empty()) strlcpy(_name, devName.c_str(), sizeof(_name));
        }
    }

    // The family was guessed from one advertisement. After a power cycle the Dot advertises without
    // its name, so only service 0xFFF0 is seen and it matches the Generic family. Timemore's
    // protocol guarantees Manufacturer Name String (0x2A29) = "TIMEMORE", so read it and correct
    // the family before choosing the service.
    if (link != ScaleLink::TIMEMORE) {
        NimBLERemoteService* dis = _client->getService(NimBLEUUID((uint16_t)0x180A));
        NimBLERemoteCharacteristic* mfr =
            dis ? dis->getCharacteristic(NimBLEUUID((uint16_t)0x2A29)) : nullptr;
        if (mfr && mfr->canRead()) {
            const std::string m = mfr->readValue();
            if (strcasestr(m.c_str(), "timemore") != nullptr) {
                SB_LOGI("Device Information says '%s' - using the timemore driver instead of %s",
                        m.c_str(), scaleLinkName(link));
                link = ScaleLink::TIMEMORE;
                _link = link;
                _driver = driverFor(link);
                if (!_name[0] || strcmp(_name, "scale") == 0) {
                    strlcpy(_name, "TIMEMORE", sizeof(_name));
                }
            }
        }
    }

    // Timemore: bond on the first connect (scale in pairing mode), encrypt with the stored keys on
    // later ones. Only this family is asked, so scales without security support are not disturbed.
    // A failure is logged and the session continues - the link works unencrypted while it lasts,
    // only the reconnect after a power cycle needs the bond.
    if (link == ScaleLink::TIMEMORE) {
        const bool bonded = NimBLEDevice::isBonded(addr);
        if (_client->secureConnection()) {
            SB_LOGI("Timemore: link encrypted (%s)", bonded ? "existing bond" : "new bond stored");
        } else {
            SB_LOGW("Timemore: bonding failed (status %d) - the scale will need pairing mode again "
                    "after a power cycle", _client->getLastError());
        }
        if (!_client->isConnected()) {
            SB_LOGE("Scale dropped the link during bonding");
            teardownClient();
            setState(ScaleState::IDLE);
            return false;
        }
    }

    // Locate the actual vendor scale service (skip standard Bluetooth services 0x1800/0x1801).
    // Lookups read the cache filled above. getCharacteristic() would fall back to a by-UUID
    // discovery round trip for every service that lacks the UUID, which cost ~6 s on the Timemore Dot.
    const auto hasChr = [](const NimBLERemoteService* s, const char* uuid) {
        const NimBLEUUID want(uuid);
        for (auto* c : s->getCharacteristics(false)) {
            if (c->getUUID() == want) return true;
        }
        return false;
    };
    NimBLERemoteService* scaleSvc = nullptr;
    {
        for (auto* s : svcs) {
            std::string uuidStr = s->getUUID().toString();
            if (uuidStr.find("1800") != std::string::npos || uuidStr.find("1801") != std::string::npos) {
                continue;
            }
            if (link == ScaleLink::BOOKOO && (hasChr(s, "FF11") || uuidStr.find("0ffe") != std::string::npos)) {
                scaleSvc = s; break;
            } else if (link == ScaleLink::TIMEMORE && (hasChr(s, "FFF1") || hasChr(s, "2A9D") || hasChr(s, "2A9E") || uuidStr.find("181d") != std::string::npos)) {
                scaleSvc = s; break;
            } else if (link == ScaleLink::ACAIA && (hasChr(s, "2A80") || uuidStr.find("1820") != std::string::npos || uuidStr.find("49535343") != std::string::npos)) {
                scaleSvc = s; break;
            } else if (link == ScaleLink::GENERIC && (hasChr(s, "FFF4") || hasChr(s, "FFE1") || uuidStr.find("fff0") != std::string::npos || uuidStr.find("ffe0") != std::string::npos)) {
                scaleSvc = s; break;
            }
            if (!scaleSvc) scaleSvc = s;
        }
    }

    if (!scaleSvc) {
        SB_LOGE("No matching scale service found on %s", _mac);
        teardownClient();
        setState(ScaleState::IDLE);
        return false;
    }

    if (_driver) {
        _driver->onConnected(scaleSvc);
    }

    _updateCount   = 0;
    _lastUpdateMs  = millis();
    _tareActive    = false;
    resetRegression();
    setState(ScaleState::CONNECTED);
    // The connected scale stops advertising, so list it here - otherwise a dashboard opened after
    // the connect would find the paired scale missing from "Discovered Scales" until a new scan.
    rememberScale(_mac, _name, link, (int8_t)_client->getRssi());
    SB_LOGI("Scale connected: %s [%s] at %s", _name, scaleLinkName(link), _mac);
    return true;
}

void ScaleManager::onClientDisconnected() {
    _driver = nullptr;
    setState(ScaleState::IDLE);
    _pendingTeardownClient = true;
    _reconnectAfterMs = millis() + 1500;
    SB_LOGW("Scale disconnected - auto-reconnect scheduled in 1.5 s");
}

// Deleting a connected NimBLE client is not safe as NimBLEDevice::deleteClient() does it:
//   - disconnect() arms the client's disconnect timer, a FreeRTOS software timer. Arming it only
//     queues a command for the FreeRTOS timer task, which runs at priority 1.
//   - deleteClient() then busy-waits with taskYIELD() until the link is down. taskYIELD() never
//     hands the CPU to a lower-priority task, so the timer task starves while this task (priority 2)
//     spins. The queued "start" and "stop" commands pile up behind it.
//   - ~NimBLEClient() zeroes the timer's callout struct. When the timer task finally runs, it
//     processes the stale "start" command, finds the timer already expired, and calls the
//     zeroed callback: InstrFetchProhibited at PC 0x00000000 in prvProcessReceivedCommands.
// So disconnect first and wait with vTaskDelay(), which lets the timer task drain its queue,
// and only delete the client once the link is down. This runs on the "ble" task only.
void ScaleManager::teardownClient() {
    if (_client) {
        NimBLEClient* c = _client;
        _client = nullptr;
        _driver = nullptr;
        if (_bleInit) {
            if (c->isConnected()) {
                c->disconnect();
                const uint32_t t0 = millis();
                while (c->isConnected() && (millis() - t0) < 3000UL) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (c->isConnected()) {
                    SB_LOGW("Scale link did not close within 3 s - deleting the client anyway");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));   // let the timer task process the timer stop command
            NimBLEDevice::deleteClient(c);
        }
    }
    _driver  = nullptr;
}

void ScaleManager::disconnect() {
    portENTER_CRITICAL(&_readingsMux);
    _pendingDisconnect = true;
    portEXIT_CRITICAL(&_readingsMux);
}

// -------------------------------------------------------------------------------------------------
// Reading ingestion
// -------------------------------------------------------------------------------------------------
void ScaleManager::publishReading(const ScaleReading& r) {
    if (isnan(r.grams)) return;
    const uint32_t now = millis();

    float netGrams = 0.0f;
    portENTER_CRITICAL(&_readingsMux);
    _rawGrams     = r.grams;
    _grams        = _rawGrams - _tareOffsetG;
    netGrams      = _grams;
    _battery      = r.batteryPct;
    _valid        = true;
    _tareActive   = r.tareActive;
    if (r.tareActive) _tareAcked = true;   // only the scale itself can assert this
    _lastUpdateMs = now;
    _updateCount++;
    pushReading(netGrams, now);
    portEXIT_CRITICAL(&_readingsMux);

    if (_state != ScaleState::CONNECTED) setState(ScaleState::CONNECTED);

    g_aux.onWeight(netGrams, r.batteryPct);
}

// -------------------------------------------------------------------------------------------------
// Predictive cutoff window - fed from every scale transport (BLE notify today).
// Must be called with _readingsMux held.
// -------------------------------------------------------------------------------------------------
void ScaleManager::pushReading(float g, uint32_t nowMs) {
    // The post-shot tail feeds the regression window too, so the weight flow stays live while the
    // last of the coffee is still arriving in the cup.
    //
    // The lockout does NOT stop the window from filling. The lockout only suppresses the cutoff
    // decision (see tick()); if the window stayed empty through it, the first cutoff evaluation
    // after the lockout would run on one or two readings - exactly when the prediction matters.
    if (!_brewing && !_tailActive) return;

    _slopeWin.push(nowMs, g, g_config.cfg().scaleSlopeWindowMs);
    _flowGs = _slopeWin.slope();
}

void ScaleManager::onNotifyTrampoline(NimBLERemoteCharacteristic* ch, uint8_t* data,
                                      size_t len, bool isNotify) {
    (void)isNotify;
    if (!s_scaleInstance || !ch || !data) return;
    ScaleManager* m = s_scaleInstance;
    if (!m->_driver) return;
    m->_driver->onNotify(ch, data, len);
}

// -------------------------------------------------------------------------------------------------
// Commands
// -------------------------------------------------------------------------------------------------
bool ScaleManager::tare() {
    const uint32_t now = millis();
    const bool hasBleScale = (_driver != nullptr && _driver->canTare() &&
                              (_state == ScaleState::CONNECTED || _state == ScaleState::STALE));

    portENTER_CRITICAL(&_readingsMux);
    _tareAcked = false;     // cleared: only a fresh device report can re-assert it
    _tareCount++;
    _slopeWin.reset();
    _flowGs    = 0.0f;
    _cutoff    = false;

    if (hasBleScale) {
        // C-02: Do NOT apply a local software offset (_tareOffsetG = _rawGrams) when sending a
        // hardware BLE tare command. Otherwise an in-flight pre-tare BLE packet looks like 0.0 g
        // and the subsequent hardware-zeroed BLE packet becomes negative (-_tareOffsetG).
        _tareOffsetG     = 0.0f;
        _tareCommandSent = true;
        _pendingTare     = true;
    } else {
        // Software-only tare fallback when no BLE scale driver is active.
        _tareOffsetG     = _rawGrams;
        _grams           = 0.0f;
        _tareActive      = true;
        _tareCommandSent = false;
        _valid           = true;
        _lastUpdateMs    = now;
    }
    const float rawG = _rawGrams;
    const float offG = _tareOffsetG;
    portEXIT_CRITICAL(&_readingsMux);

    SB_LOGI("Scale tare requested (%s, raw=%.1fg, offset=%.1fg)",
            hasBleScale ? "hardware BLE command queued" : "local software tare only", rawG, offG);
    return hasBleScale;
}

void ScaleManager::dropSoftwareTare() {
    portENTER_CRITICAL(&_readingsMux);
    _tareOffsetG     = 0.0f;
    _grams           = _rawGrams;
    _tareCommandSent = true;
    portEXIT_CRITICAL(&_readingsMux);
}

void ScaleManager::resetRegression() {
    portENTER_CRITICAL(&_readingsMux);
    _slopeWin.reset();
    _flowGs        = 0.0f;
    _cutoffWeightG = 0.0f;
    _cutoffSlopeGs = 0.0f;
    portEXIT_CRITICAL(&_readingsMux);
}

bool ScaleManager::takeCutoffSignal() {
    portENTER_CRITICAL(&_readingsMux);
    const bool c = _cutoff;
    _cutoff = false;
    portEXIT_CRITICAL(&_readingsMux);
    return c;
}

// -------------------------------------------------------------------------------------------------
// Core 1 100 Hz control tick: strictly non-blocking (no BLE stack / GATT calls).
// -------------------------------------------------------------------------------------------------
void ScaleManager::tick(uint32_t nowMs, float targetG, bool brewing, bool lockoutActive,
                        bool tailActive) {
    _brewing = brewing;
    _lockoutActive = lockoutActive;
    _tailActive = tailActive;

    const int32_t linkAgeMs = (int32_t)(nowMs - _lastUpdateMs);
    if (_state == ScaleState::CONNECTED && linkAgeMs > 500) {
        setState(ScaleState::STALE);
    }

    if (!brewing) {
        // The post-shot tail keeps the weight flow alive so the client can show the drip. Once the
        // tail is over the flow goes back to zero, because nothing is being poured any more.
        if (!_tailActive) {
            portENTER_CRITICAL(&_readingsMux);
            _slopeWin.reset();
            _flowGs  = 0.0f;
            _cutoff  = false;
            portEXIT_CRITICAL(&_readingsMux);
        } else {
            portENTER_CRITICAL(&_readingsMux);
            _cutoff = false;
            portEXIT_CRITICAL(&_readingsMux);
        }
        return;
    }

    // Predictive cutoff (docs/PumpFlow.md). The lockout suppresses the DECISION only - the regression
    // window has been filling all along (see pushReading()).
    if (lockoutActive || targetG <= 0.1f || !_valid) {
        return;
    }

    float curGrams = 0.0f;
    float curSlope = 0.0f;
    bool alreadyCutoff = false;
    portENTER_CRITICAL(&_readingsMux);
    curGrams = _grams;
    curSlope = _slopeWin.slope();
    alreadyCutoff = _cutoff;
    portEXIT_CRITICAL(&_readingsMux);

    const float slopeClamped = sbCutoffSlopeClamp(curSlope);
    const float predicted    = slopeClamped * g_config.cfg().scaleLagSec;
    const float stopAt       = targetG - predicted;

    if (curGrams >= stopAt && !alreadyCutoff) {
        portENTER_CRITICAL(&_readingsMux);
        _cutoff = true;
        _cutoffEvents++;
        _cutoffWeightG = curGrams;
        _cutoffSlopeGs = curSlope;
        portEXIT_CRITICAL(&_readingsMux);
        SB_LOGI("WEIGHT CUTOFF: %.1f g >= %.1f g target - %.2f g predicted (slope %.2f g/s)",
                curGrams, targetG, predicted, curSlope);
    }
}

// -------------------------------------------------------------------------------------------------
// Core 0 BLE state machine & GATT servicing (called from the "ble" task on Core 0, which is not on
// the task watchdog: connect and service discovery below block for seconds).
// -------------------------------------------------------------------------------------------------
void ScaleManager::serviceBle(uint32_t nowMs) {
    // 1. Process queued disconnect / pair / scan / tare requests from HTTP, WS, or Core 1
    bool doDisc = false;
    bool doPair = false;
    bool doScan = false;
    uint8_t scanSec = 8;
    bool scanDiscOnly = false;
    bool doTare = false;

    portENTER_CRITICAL(&_readingsMux);
    if (_pendingDisconnect) { _pendingDisconnect = false; doDisc = true; }
    if (_pendingPairReq)    { _pendingPairReq = false;    doPair = true; }
    if (_pendingScanReq) {
        _pendingScanReq = false;
        doScan = true;
        scanSec = _pendingScanSeconds;
        scanDiscOnly = _pendingScanDiscoveryOnly;
    }
    if (_pendingTare)       { _pendingTare = false;       doTare = true; }
    portEXIT_CRITICAL(&_readingsMux);

    if (doDisc) {
        stopScan();
        teardownClient();
        setState(ScaleState::IDLE);
        if (!scaleConfigured()) {
            _pendingReleaseBle = true;
        }
        SB_LOGI("Scale disconnected");
    }

    if (doPair) {
        teardownClient();
        // Forget the bonds with every scale except the one now configured. Only scales bond with
        // this controller, and a stale bond would make the old scale reject a later re-pair.
        if (_bleInit) {
            for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; --i) {
                const NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
                if (strcasecmp(a.toString().c_str(), g_config.cfg().scaleMac) == 0) continue;
                const bool ok = NimBLEDevice::deleteBond(a);
                SB_LOGI("Removed the bond with %s (%s)", a.toString().c_str(), ok ? "ok" : "failed");
            }
        }
        setState(ScaleState::IDLE);
        if (scaleConfigured()) {
            doStartScan(8, /*discoveryOnly=*/false);
        } else {
            _pendingReleaseBle = true;
        }
    }

    if (doScan) {
        doStartScan(scanSec, scanDiscOnly);
    }

    if (doTare) {
        bool sent = false;
        if (_driver && (_state == ScaleState::CONNECTED || _state == ScaleState::STALE)) {
            sent = _driver->sendTare();
        }
        portENTER_CRITICAL(&_readingsMux);
        if (sent) {
            _tareOffsetG     = 0.0f;
            _tareCommandSent = true;
        } else {
            // Fallback to software tare if the scale driver could not send a BLE tare
            _tareOffsetG     = _rawGrams;
            _grams           = 0.0f;
            _tareActive      = true;
            _tareCommandSent = false;
            _valid           = true;
            _lastUpdateMs    = nowMs;
        }
        portEXIT_CRITICAL(&_readingsMux);
        SB_LOGI("Scale BLE tare processed (%s)", sent ? "hardware write ok" : "software fallback");
    }

    // 2. Stop active scan if a shot started and either a scale is already connected or none is paired
    const bool brewing = _brewing;
    if (brewing && _state == ScaleState::SCANNING && (_client || !scaleConfigured())) {
        SB_LOGI("Shot started - stopping the BLE scan to keep the telemetry link responsive.");
        stopScan();
        _reconnectAfterMs = 0;
    }

    // 3. Safety timeout for discovery scan
    if (_discovering && _scanDeadlineMs != 0 && nowMs > _scanDeadlineMs) {
        _discovering = false;
        _scanDeadlineMs = 0;
        if (_state == ScaleState::SCANNING) {
            stopScan();
        }
        if (!scaleConfigured()) {
            _pendingReleaseBle = true;
        }
    }

    // 4. Liveness & dead-link teardown when idle
    const int32_t linkAgeMs = (int32_t)(nowMs - _lastUpdateMs);
    if (_state == ScaleState::CONNECTED || _state == ScaleState::STALE) {
        if (linkAgeMs > 500 && _state == ScaleState::CONNECTED) {
            setState(ScaleState::STALE);
        }
        if (!brewing && linkAgeMs > 2000) {
            SB_LOGW("Scale link dead (no data for %ld ms) - tearing down to auto-reconnect",
                    (long)linkAgeMs);
            teardownClient();
            setState(ScaleState::IDLE);
            _reconnectAfterMs = nowMs + 1000;
        }
    }

    if (_driver) {
        _driver->service();
    }

    // 5. Deferred connect after scan or scheduled auto-reconnect (never interrupt an active brew!)
    if (!brewing) {
        if (_pendingTeardownClient) {
            _pendingTeardownClient = false;
            teardownClient();
        }
        if (_pendingReleaseBle) {
            _pendingReleaseBle = false;
            releaseBleIfIdle();
        }
        if (_pendingConnect) {
            _pendingConnect = false;
            stopScan();
            connectTo(_pendingAddr, _pendingLink, _name);
        } else if (scaleConfigured() && !_client && _state == ScaleState::IDLE) {
            if (_reconnectAfterMs == 0) {
                _reconnectAfterMs = nowMs + 500;
            } else if (nowMs >= _reconnectAfterMs) {
                _reconnectAfterMs = 0;
                doStartScan(6, /*discoveryOnly=*/false);
            }
        }
    }
}

// -------------------------------------------------------------------------------------------------
// The shim type itself lives in the header (it has to be complete there so the static member can be
// declared); only its definition and the singleton binding live here.
// -------------------------------------------------------------------------------------------------
SbScaleScanCallbacks ScaleManager::s_scaleCallbacks{};

void ScaleManager::scanResult(const NimBLEAdvertisedDevice* dev) {
    if (s_scaleInstance) s_scaleInstance->onAdvertised(dev);
}

ScaleManager g_scale;