// =================================================================================================
//  scale_acaia.h - Acaia Lunar / Pearl / Pyxis   (docs/Hardware.md)
//
//  Acaia units speak a proprietary framed protocol on service 0x1820 (and the Pearl/Pyxis also
//  expose the vendor service 49535343-...). On connect the scale sends an "EF DD 00 ..." handshake
//  and expects a 3 s heartbeat write; weight arrives inside a message whose type byte is 0x05.
//  All multi-byte values in the Acaia protocol are little-endian.
// =================================================================================================
#pragma once

#include "scale_interface.h"

class AcaiaDriver : public ScaleDriver {
public:
    const char* name() const override { return "Acaia Lunar/Pearl"; }
    ScaleLink   link() const override { return ScaleLink::ACAIA; }

    static constexpr const char* SERVICE_UUID = "00001820-0000-1000-8000-00805f9b34fb";
    static constexpr const char* VENDOR_UUID  = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
    static constexpr const char* NOTIFY_UUID  = "00002a80-0000-1000-8000-00805f9b34fb";
    static constexpr const char* WEIGHT_UUID  = "00002a80-0000-1000-8000-00805f9b34fb";

    void onConnected(NimBLERemoteService* svc) override;
    void onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) override;
    bool sendTare() override;
    void service() override;                  // heartbeat + battery poll

    // Decodes a message whose type byte (offset 1) is 0x05.
    static bool decodeWeightMsg(const uint8_t* d, size_t len, ScaleReading& out);

    uint32_t lastHeartbeatMs() const { return _lastHeartbeatMs; }

private:
    void heartbeat();

    NimBLERemoteCharacteristic* _notify = nullptr;
    NimBLERemoteCharacteristic* _write  = nullptr;
    uint32_t _lastHeartbeatMs = 0;
    float    _lastG = 0.0f;
    bool     _needsHandshake = true;
};

extern AcaiaDriver g_acaiaDummy;