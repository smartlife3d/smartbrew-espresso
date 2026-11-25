// wifi_ota.h - Wi-Fi OTA update mode.
//
// The scale normally never associates: Wi-Fi stays unassociated and the weight
// travels over BLE. Update mode is therefore EXPLICIT - `ota` on the serial console:
//
//   1. Wi-Fi associates with the stored AP
//   2. the ArduinoOTA server answers on TCP 3232
//   3. on success the unit reboots into the new slot
//   4. on error or idle timeout it drops the AP and resumes advertising
//
// Credentials are stored in NVS, so they survive reflash and OTA.

#pragma once

#include <Arduino.h>

enum class OtaState : uint8_t {
    Idle,        // offline, advertising over BLE, no OTA server
    Connecting,  // associating with the AP
    Ready,       // OTA server is up on OTA_PORT
    Transfer,    // an update is in flight
    Failed,      // last attempt failed (reason in wifiOtaStatus())
};

void        wifiOtaInit(void);                          // load credentials, stay Idle
bool        wifiOtaRequest(void);                       // enter update mode
OtaState    wifiOtaState(void);
const char* wifiOtaStateName(void);
const char* wifiOtaStatus(void);                       // one-line detail for `status`

bool        wifiOtaHasCredentials(void);
const char* wifiOtaSsid(void);
bool        wifiOtaSetCredentials(const char* ssid, const char* pass);
void        wifiOtaClearCredentials(void);