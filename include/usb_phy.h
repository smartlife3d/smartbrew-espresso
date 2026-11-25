#pragma once

#include <Arduino.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32)
extern "C" void phy_bbpll_en_usb(bool en);

/**
 * @brief Keep ESP32-S3 USB CDC PHY clock alive during WiFi/BT radio state transitions.
 *
 * On ESP32-S3, native USB-CDC and JTAG rely on the BBPLL (baseband PLL) clock.
 * When the Wi-Fi or Bluetooth PHY state machine initializes, recalibrates, or transitions
 * between power states, the internal IDF PHY routines can turn off or glitch the BBPLL,
 * causing USB-CDC communication to drop or hang.
 * Calling phy_bbpll_en_usb(true) forces the BBPLL to remain enabled for the USB subsystem.
 */
inline void sb_usb_phy_keepalive() {
    phy_bbpll_en_usb(true);
}
#else
inline void sb_usb_phy_keepalive() {}
#endif
