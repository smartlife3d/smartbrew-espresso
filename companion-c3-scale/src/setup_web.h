// setup_web.h - SoftAP setup portal: Wi-Fi credentials, BLE name and browser OTA.
//
// Entered automatically on a unit that has no Wi-Fi credentials stored, or by
// holding the touch key while powering on, or with `ap` on the serial console.
// While the portal is up the scale hosts its own WPA2 access point and serves a
// single page at http://192.168.4.1/ - no laptop, no terminal, no internet.
//
// BLE and the SoftAP share the C3's one radio and time-slice it, so the scale
// keeps advertising while the portal is up.

#pragma once

#include <Arduino.h>

void        setupWebInit(void);          // apply the auto/boot-hold entry rules
bool        setupWebEnter(void);         // force setup mode (serial `ap`)
void        setupWebLeave(bool reboot);  // back to broadcast mode
bool        setupWebActive(void);
const char* setupWebApSsid(void);        // e.g. espresso-scale-A3F2
uint32_t    setupWebClientCount(void);