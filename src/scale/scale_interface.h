// =================================================================================================
//  scale_interface.h - Abstract BLE scale driver contract
//                      docs/Hardware.md
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

enum class ScaleLink : uint8_t {
    NONE = 0,
    BOOKOO,        // Bookoo Themis Ultra / Mini & 0x0FFE companion scale
    TIMEMORE,      // Timemore Black Mirror Dot / Nano
    ACAIA,         // Acaia Lunar / Pearl / Pyxis
    GENERIC        // Decent / Felicita / Cocinare
};

const char* scaleLinkName(ScaleLink l);

// -------------------------------------------------------------------------------------------------
// One decoded reading handed up from a driver.
// -------------------------------------------------------------------------------------------------
struct ScaleReading {
    float    grams       = 0.0f;
    float    flowGs      = 0.0f;     // driver-side instantaneous rate, -1 when unknown
    int8_t   batteryPct  = -1;       // -1 when the scale does not report battery
    bool     stable      = true;     // driver-side outlier rejection result
    bool     tareActive  = false;
};

// -------------------------------------------------------------------------------------------------
// A driver parses raw notifications for one scale family. Drivers are stateless with respect to
// the manager: they decode, call manager->publishReading(), and store whatever they need for tare.
// -------------------------------------------------------------------------------------------------
class ScaleDriver {
public:
    virtual ~ScaleDriver() = default;

    virtual const char* name() const = 0;
    virtual ScaleLink   link() const = 0;

    // Called once after connect + characteristic discovery.
    virtual void onConnected(NimBLERemoteService* svc) { (void)svc; }

    // Called for every notification on any subscribed characteristic.
    virtual void onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) = 0;

    // Send the family's tare command. Returns true if a command was written.
    virtual bool sendTare() = 0;
    // False when the scale has no known tare command; the manager then tares in software at once
    // instead of queueing a BLE write that cannot be sent.
    virtual bool canTare() const { return true; }

    // Optional per-frame battery request (Acaia needs an explicit poll).
    virtual void service() {}
};

// -------------------------------------------------------------------------------------------------
// Small shared helpers used by more than one driver.
// -------------------------------------------------------------------------------------------------
namespace scale_util {

// Big-endian unsigned helpers - every one of these scales transmits big-endian.
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// XOR checksum used by the Decent / Felicita / Cocinare family: the final byte of the frame.
inline bool xorCheckValid(const uint8_t* d, size_t n) {
    if (n < 4) return false;
    uint8_t x = 0;
    for (size_t i = 0; i < n - 1; ++i) x ^= d[i];
    return x == d[n - 1];
}

}  // namespace scale_util