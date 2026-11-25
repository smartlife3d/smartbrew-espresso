// =================================================================================================
//  wifi_portal.h - STA + captive-portal AP fallback + mDNS   (docs/API.md)
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

enum class WifiMode : uint8_t { OFF, CONNECTING, CONNECTED, AP_PORTAL };

class WifiPortal {
public:
    void begin(AsyncWebServer* http, const char* apSsidPrefix);
    void loop();                       // called from loop() - services DNS + reconnect backoff

    bool connected() const { return _mode == WifiMode::CONNECTED; }
    bool inPortal() const { return _mode == WifiMode::AP_PORTAL; }
    const char* ssid() const   { return _ssid; }
    // The name of the setup hotspot. Empty unless the captive portal is running.
    const char* apSsid() const { return _apSsid; }
    const char* ip() const;
    const char* modeName() const;
    int  rssi() const           { return _rssi; }

    bool saveCredentials(const char* ssid, const char* pass);   // from the captive portal form
    void forgetCredentials();
    void startPortal();
    void recordDisconnect(uint8_t reason);
    uint8_t lastDisconnectReason() const { return _lastDisconnectReason; }
    const char* lastError() const { return _lastError; }

private:
    void stopPortal();
    void startSTA();
    void checkNetworkLiveness();

    WifiMode          _mode = WifiMode::OFF;
    char              _ssid[33] = {0};
    char              _apSsid[33] = {0};
    char              _pass[65] = {0};
    char              _lastError[96] = {0};
    AsyncWebServer*   _http = nullptr;
    DNSServer         _dns;
    int               _rssi = 0;
    uint32_t          _lastRetry = 0;
    uint32_t          _connectStartMs = 0;
    uint32_t          _connectedSince = 0;
    uint32_t          _pendingReboot = 0;
    uint8_t           _lastDisconnectReason = 0;
    bool              _haveCreds = false;

    // Network liveness. WiFi.status() only says the radio is associated, not that traffic is
    // actually flowing: an access point that reboots, or a DHCP server that hands the controller's
    // address to somebody else, leaves the controller looking perfectly connected while nothing
    // can reach it. These counters watch an actual round trip to the gateway.
    uint32_t          _lastLivenessMs = 0;
    uint8_t           _livenessFails = 0;
    bool              _livenessEverWorked = false;
};

extern WifiPortal g_wifi;

// Wi-Fi credentials live in their own NVS namespace so they survive config.json resets.
#define WIFI_NVS_NS   "sbwifi"
#define WIFI_NVS_SSID "ssid"
#define WIFI_NVS_PASS "pass"