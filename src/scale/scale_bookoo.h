// =================================================================================================
//  scale_bookoo.h - Bookoo Themis Ultra / Mini & 0x0FFE Generic Scale BLE driver
//                   Also handles ESP32-C3 companion scale using the 0x0FFE / 0xFF11 protocol.
// =================================================================================================
#pragma once

#include "scale_interface.h"

class BookooDriver : public ScaleDriver {
public:
    const char* name() const override { return "Bookoo Themis"; }
    ScaleLink   link() const override { return ScaleLink::BOOKOO; }

    // Tare per Bookoo protocol: 03 0A 01 00 00 + XOR checksum (0x08)
    static constexpr uint8_t TARE_CMD[6] = {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08};
    static constexpr const char* SERVICE_UUID = "00000ffe-0000-1000-8000-00805f9b34fb";
    static constexpr const char* NOTIFY_UUID = "0000ff11-0000-1000-8000-00805f9b34fb";
    static constexpr const char* WRITE_UUID  = "0000ff12-0000-1000-8000-00805f9b34fb";

    void onConnected(NimBLERemoteService* svc) override;
    void onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) override;
    bool sendTare() override;

    // Exposed for unit testing: decode a 20-byte Bookoo frame.
    static bool decode(const uint8_t* d, size_t len, ScaleReading& out);

private:
    NimBLERemoteCharacteristic* _notify = nullptr;
    NimBLERemoteCharacteristic* _write  = nullptr;
    float    _lastG = 0.0f;
    uint32_t _lastMs = 0;
};