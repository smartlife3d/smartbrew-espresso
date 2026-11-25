// =================================================================================================
//  wifi_portal.cpp - see wifi_portal.h
// =================================================================================================

#include "wifi_portal.h"
#include "logger.h"
#include "network/aux_bridge.h"
#include "scale/scale_manager.h"
#include "brew_controller.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_event.h>

namespace {
Preferences wifiPrefs;
char ipBuf[20] = {0};
char modeBuf[16] = {0};
}  // namespace

const char* WifiPortal::ip() const {
    if (_mode == WifiMode::AP_PORTAL) {
        snprintf(ipBuf, sizeof(ipBuf), "192.168.4.1");
    } else if (_mode == WifiMode::CONNECTED) {
        snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", WiFi.localIP()[0], WiFi.localIP()[1],
                 WiFi.localIP()[2], WiFi.localIP()[3]);
    } else {
        snprintf(ipBuf, sizeof(ipBuf), "0.0.0.0");
    }
    return ipBuf;
}

const char* WifiPortal::modeName() const {
    switch (_mode) {
        case WifiMode::CONNECTING: snprintf(modeBuf, sizeof(modeBuf), "connecting"); break;
        case WifiMode::CONNECTED:  snprintf(modeBuf, sizeof(modeBuf), "connected");  break;
        case WifiMode::AP_PORTAL:  snprintf(modeBuf, sizeof(modeBuf), "ap_portal");  break;
        default:                   snprintf(modeBuf, sizeof(modeBuf), "off");       break;
    }
    return modeBuf;
}

#include "usb_phy.h"

static const char* wifiDisconnectReasonName(uint8_t reason) {
    switch (reason) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE";
        case 3:   return "AUTH_LEAVE";
        case 4:   return "ASSOC_EXPIRE";
        case 5:   return "ASSOC_TOOMANY";
        case 6:   return "NOT_AUTHED";
        case 7:   return "NOT_ASSOCED";
        case 8:   return "ASSOC_LEAVE";
        case 9:   return "ASSOC_NOT_AUTHED";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT (check password)";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND (check 2.4 GHz / SSID name / range)";
        case 202: return "AUTH_FAIL (password incorrect)";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "OTHER";
    }
}

// -------------------------------------------------------------------------------------------------
void WifiPortal::begin(AsyncWebServer* http, const char* apSsidPrefix) {
    _http = http;

    // Ensure LwIP TCP/IP stack and default event loop are initialized so network
    // servers (AsyncWebServer / AsyncTCP) have a valid tcpip_mbox regardless of Wi-Fi state.
    esp_netif_init();
    esp_event_loop_create_default();

    // Ensure USB Serial/JTAG clock remains active whenever PHY recalibrates, and log Wi-Fi state changes
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        sb_usb_phy_keepalive();
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            const uint8_t r = info.wifi_sta_disconnected.reason;
            SB_LOGW("Wi-Fi STA disconnected (reason %d: %s)", r, wifiDisconnectReasonName(r));
            g_wifi.recordDisconnect(r);
        } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            SB_LOGI("Wi-Fi STA associated with AP on channel %d", info.wifi_sta_connected.channel);
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            SB_LOGI("Wi-Fi got IP: %s (GW: %s)",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str(),
                    IPAddress(info.got_ip.ip_info.gw.addr).toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_AP_START) {
            SB_LOGI("SoftAP started");
        } else if (event == ARDUINO_EVENT_WIFI_AP_STOP) {
            SB_LOGI("SoftAP stopped");
        } else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
            const uint8_t* m = info.wifi_ap_staconnected.mac;
            SB_LOGI("SoftAP station joined: %02X:%02X:%02X:%02X:%02X:%02X (AID: %d)",
                    m[0], m[1], m[2], m[3], m[4], m[5], info.wifi_ap_staconnected.aid);
        } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
            SB_LOGI("SoftAP DHCP lease granted to client: %s",
                    IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
            const uint8_t* m = info.wifi_ap_stadisconnected.mac;
            SB_LOGI("SoftAP station left: %02X:%02X:%02X:%02X:%02X:%02X",
                    m[0], m[1], m[2], m[3], m[4], m[5]);
        }
    });
    sb_usb_phy_keepalive();

    wifiPrefs.begin(WIFI_NVS_NS, false);
    String sSsid = wifiPrefs.getString(WIFI_NVS_SSID, "");
    String sPass = wifiPrefs.getString(WIFI_NVS_PASS, "");
    wifiPrefs.end();

    if (sSsid.length() > 0 && sSsid.length() < sizeof(_ssid)) {
        strlcpy(_ssid, sSsid.c_str(), sizeof(_ssid));
        strlcpy(_pass, sPass.c_str(), sizeof(_pass));
        _haveCreds = true;
    } else {
        _ssid[0] = '\0';
        _pass[0] = '\0';
        _haveCreds = false;
    }

    if (_haveCreds) {
        SB_LOGI("Wi-Fi: stored SSID '%s' - connecting", _ssid);
        startSTA();
    } else {
        SB_LOGI("Wi-Fi: no stored credentials - launching setup portal.");
        startPortal();
    }
}

void WifiPortal::recordDisconnect(uint8_t reason) {
    _lastDisconnectReason = reason;
}

void WifiPortal::startSTA() {
    _mode = WifiMode::CONNECTING;
    _connectStartMs = millis();
    _lastRetry = millis();
    _lastDisconnectReason = 0;
    sb_usb_phy_keepalive();
    WiFi.mode(WIFI_STA);
    sb_usb_phy_keepalive();
    WiFi.setSleep(false);              // Keep receiver fully awake during association/handshake
    WiFi.setTxPower(WIFI_POWER_17dBm); // Reduce peak current spikes on 3.3V rail
    WiFi.setAutoReconnect(true);
    WiFi.begin(_ssid, _pass);
    SB_LOGI("Attempting connection to '%s'...", _ssid);
}

// -------------------------------------------------------------------------------------------------
void WifiPortal::startPortal() {
    SB_LOGI("Entering setup portal...");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "SmartBrew-%02X%02X", mac[4], mac[5]);
    strlcpy(_apSsid, ssid, sizeof(_apSsid));

    sb_usb_phy_keepalive();
    WiFi.disconnect(true, true);
    delay(50);
    WiFi.mode(WIFI_AP);
    sb_usb_phy_keepalive();

    // Turn off Wi-Fi power save mode completely.
    // iOS and Android expect immediate 802.11 association and DHCP responses;
    // modem sleep causes beacon drops, packet timeouts, and "Failed to join network".
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Open network (no password required).
    // Start SoftAP interface FIRST before configuring netif DHCP options.
    const bool ok = WiFi.softAP(ssid, nullptr, 1, 0, 4);
    sb_usb_phy_keepalive();

    WiFi.setTxPower(WIFI_POWER_17dBm);

    IPAddress apIP(192, 168, 4, 1);
    IPAddress netmask(255, 255, 255, 0);
    IPAddress dhcpStart(192, 168, 4, 2);
    WiFi.softAPConfig(apIP, apIP, netmask, dhcpStart);

    delay(50);
    SB_LOGI("SoftAP %s on IP %s (SSID: %s, Open / No Password)",
            ok ? "started" : "FAILED", apIP.toString().c_str(), ssid);

    _dns.stop();
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    if (_dns.start(53, "*", apIP)) {
        SB_LOGI("Captive-portal DNS started on port 53");
    } else {
        SB_LOGW("DNS server failed to start");
    }

    _mode = WifiMode::AP_PORTAL;
    SB_BANNER("");
    SB_LOGI("  SETUP HOTSPOT ACTIVE (OPEN - NO PASSWORD):");
    SB_LOGI("    1. Connect phone to Wi-Fi: '%s' (No password)", ssid);
    SB_LOGI("    2. Setup page will open automatically");
    SB_LOGI("    3. Or open in browser:    http://192.168.4.1/wifi");
    SB_BANNER("");
}

void WifiPortal::stopPortal() {
    _dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    _apSsid[0] = '\0';
    _mode = WifiMode::OFF;
}

// -------------------------------------------------------------------------------------------------
bool WifiPortal::saveCredentials(const char* ssid, const char* pass) {
    if (!ssid || !*ssid) return false;
    // A blank password for the network that is already stored means "keep the saved one". Both
    // the dashboard and the setup page pre-fill the SSID but never the password, so pressing
    // Save & Connect after changing something else used to wipe it: the controller then tried
    // the WPA2 network without a password and never joined (reason 210, no AP with compatible
    // security). To switch the same SSID to an open network, Forget it first.
    const bool keepPass = (!pass || !*pass) && _haveCreds && _pass[0] != '\0' &&
                          strcmp(ssid, _ssid) == 0;
    if (keepPass) {
        SB_LOGI("Wi-Fi: no password entered for '%s' - keeping the saved one", ssid);
    } else {
        strlcpy(_ssid, ssid, sizeof(_ssid));
        strlcpy(_pass, pass ? pass : "", sizeof(_pass));
    }

    wifiPrefs.begin(WIFI_NVS_NS, false);
    wifiPrefs.putString(WIFI_NVS_SSID, _ssid);
    wifiPrefs.putString(WIFI_NVS_PASS, _pass);
    wifiPrefs.end();
    _haveCreds = true;
    _lastError[0] = '\0';
    _lastDisconnectReason = 0;

    SB_LOGI("Saved Wi-Fi credentials for '%s'. Restarting in 1.2s to connect...", _ssid);
    // Reboot cleanly in 1.2 seconds so the browser receives the HTTP confirmation response,
    // and the controller boots fresh into station mode without race conditions or memory fragmentation.
    _pendingReboot = millis() + 1200;
    return true;
}

void WifiPortal::forgetCredentials() {
    wifiPrefs.begin(WIFI_NVS_NS, false);
    wifiPrefs.remove(WIFI_NVS_SSID);
    wifiPrefs.remove(WIFI_NVS_PASS);
    wifiPrefs.end();
    _ssid[0] = _pass[0] = '\0';
    _haveCreds = false;
    _lastError[0] = '\0';
    _lastDisconnectReason = 0;
    SB_LOGW("Wi-Fi credentials cleared.");
}

// -------------------------------------------------------------------------------------------------
void WifiPortal::loop() {
    if (_pendingReboot != 0 && millis() >= _pendingReboot) {
        _pendingReboot = 0;
        ESP.restart();
    }
    switch (_mode) {
        case WifiMode::CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                _mode = WifiMode::CONNECTED;
                _connectedSince = millis();
                _rssi = WiFi.RSSI();
                _lastError[0] = '\0';
                _lastDisconnectReason = 0;
                const char* hname = g_config.cfg().mcuHostname[0] ? g_config.cfg().mcuHostname : "smartbrew";
                SB_LOGI("Wi-Fi connected: %s (RSSI %d dBm, Ch %d) http://%s.local",
                        ip(), _rssi, WiFi.channel(), hname);

                // Radio power save must stay OFF.
                //
                // WIFI_PS_MIN_MODEM lets the radio sleep between beacons, so every TX segment is
                // held in the driver until the next wake-up. On a telemetry socket that carries
                // 20 frames/s this shows up as jitter of hundreds of milliseconds and, when a
                // segment was lost, as a stall that lasted for a full TCP retransmission timeout.
                // It also fights the NimBLE scale link, which needs the radio awake anyway.
                //
                // The controller is mains powered (it lives inside the machine's 5 V rail), so the
                // power this saves is not worth the latency.
                esp_wifi_set_ps(WIFI_PS_NONE);

                if (MDNS.begin(hname)) {
                    MDNS.setInstanceName(hname);
                    MDNS.addService("http", "tcp", 80);
                    MDNS.addService("ws",   "tcp", 80);
                    MDNS.addServiceTxt("http", "tcp", "name", (const char*)hname);
                }

                // Keep the controller's own clock, so the recorded shots are stamped with the real
                // time even when no browser is open. Until this existed the timestamp came from
                // whichever dashboard happened to be loaded, and after a restart every new shot was
                // stamped with the uptime instead of a date.
                //
                // The offset is 0, so the clock runs on UTC - the same as the bench's own time zone.
                configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
                SB_LOGI("Syncing the clock from the network (UTC)...");
            } else if (millis() - _lastRetry > 10000 && WiFi.status() != WL_CONNECTED) {
                const uint32_t elapsed = millis() - _connectStartMs;
                SB_LOGI("Still trying to connect to '%s'... (%u s, status=%d)",
                        _ssid, (unsigned)(elapsed / 1000), WiFi.status());
                _lastRetry = millis();
                if (elapsed > 180000UL) {
                    SB_LOGW("Could not connect to '%s' within 3 minutes; falling back to setup portal", _ssid);
                    startPortal();
                } else if (elapsed > 25000 && (elapsed % 30000) < 10000) {
                    WiFi.reconnect();
                }
            }
            break;

        case WifiMode::CONNECTED: {
            _rssi = WiFi.RSSI();
            if (WiFi.status() != WL_CONNECTED) {
                SB_LOGW("Wi-Fi lost - attempting reconnection");
                _mode = WifiMode::CONNECTING;
                _connectStartMs = millis();
                _lastRetry = millis();
                _lastDisconnectReason = 0;
                WiFi.reconnect();
            } else {
                checkNetworkLiveness();
            }
            break;
        }

        case WifiMode::AP_PORTAL:
            _dns.processNextRequest();
            break;

        default:
            break;
    }
}

// =================================================================================================
//  Network liveness
//
//  WiFi.status() only reports what the RADIO thinks. It keeps saying "connected" after an access
//  point has rebooted, or after the DHCP server has handed the controller's address to some other
//  device - and in that state the controller is invisible: it is on the network, it answers mDNS,
//  but nothing can reach it and it cannot reach anything either.
//
//  The cure for that state is a fresh association, because that is what makes the controller ask
//  for an address again. So the controller watches a real round trip to its gateway and forces a
//  re-association when the round trip stops working while the radio still claims to be fine.
//
//  The probe is a TCP handshake to the gateway. A gateway that does not answer on port 80 is not a
//  fault - it is simply a gateway that cannot be probed, and the controller learns that on the
//  first probe and never forces a reconnect on its account.
// =================================================================================================
void WifiPortal::checkNetworkLiveness() {
    const uint32_t now = millis();
    if (now - _lastLivenessMs < 60000UL) return;   // once a minute is plenty
    _lastLivenessMs = now;

    // Give a fresh association a moment to settle before judging it.
    if (now - _connectedSince < 30000UL) {
        _livenessFails = 0;
        return;
    }

    const IPAddress gw = WiFi.gatewayIP();
    if (gw == IPAddress(0, 0, 0, 0)) {
        // No gateway known - the address has not been assigned. The reconnect logic covers that.
        return;
    }

    bool reachable = false;
    WiFiClient probe;
    probe.setTimeout(1200);
    if (probe.connect(gw, 80)) {
        reachable = true;
        probe.stop();
    }

    if (reachable) {
        if (!_livenessEverWorked) {
            _livenessEverWorked = true;
            SB_LOGI("Gateway %s answers - the network path is working.", gw.toString().c_str());
        } else if (_livenessFails > 0) {
            SB_LOGI("The network path is working again (after %u failed probe(s)).", _livenessFails);
        }
        _livenessFails = 0;
        return;
    }

    // A gateway that has never answered is not a gateway we can judge by. Say so once and then
    // leave it alone rather than reconnecting every minute for the rest of the day.
    if (!_livenessEverWorked) {
        SB_LOGW("Gateway %s does not answer on port 80 - it cannot be probed, so the controller "
                "will not force a reconnect on its account.", gw.toString().c_str());
        return;
    }

    _livenessFails++;
    SB_LOGW("The gateway stopped answering (%u/3) while the radio still says connected.", _livenessFails);

    if (_livenessFails < 3) return;

    // Three minutes of a dead network path while the radio claims everything is fine. The address
    // is probably no longer ours. Re-associate from scratch, which asks the DHCP server for a new
    // one.
    //
    // Not during a shot, though: the pump control and the weight cutoff are local and do not need
    // the network, so there is no reason to disturb a brew that is running perfectly well.
    if (g_brew.state() != BrewState::IDLE) {
        SB_LOGW("The network is still unreachable, but a shot is running - leaving it alone "
                "until the machine is idle.");
        _livenessFails = 2;   // keep the count just below the threshold so it fires right after
        return;
    }

    SB_LOGW("The network has been unreachable for %u minutes - re-associating to get a fresh "
            "address.", (unsigned)_livenessFails);
    _livenessFails = 0;
    _livenessEverWorked = false;
    _mode = WifiMode::CONNECTING;
    _connectStartMs = now;
    _lastRetry = now;
    WiFi.disconnect(true, false);
    WiFi.begin(_ssid, _pass);
}

WifiPortal g_wifi;