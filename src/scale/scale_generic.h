// =================================================================================================
//  scale_generic.h - Decent Scale / Felicita / Cocinare   (docs/Hardware.md)
//
//  These three share a vendor frame family: service 0xFFF0 (Decent, Felicita 0xFFE0) with notify
//  and write both on 0xFFF4, or service 0xFFE0 with 0xFFE1 (Cocinare). Frames are 7-18 bytes and
//  are validated with a trailing XOR byte before any field is trusted.
//
//  Supported tare commands, tried in order until the scale acknowledges:
//      {0x03, 0x0F, ...}   Decent / Felicita
//      {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08}   Bookoo-style, Cocinare accepts this
//      {'T'}               Felicita ASCII tare
// =================================================================================================
#pragma once

#include "scale_interface.h"

class GenericDriver : public ScaleDriver {
public:
    const char* name() const override { return "Decent/Felicita/Cocinare"; }
    ScaleLink   link() const override { return ScaleLink::GENERIC; }

    static constexpr const char* SERVICE_FFF0 = "0000fff0-0000-1000-8000-00805f9b34fb";
    static constexpr const char* NOTIFY_FFF4  = "0000fff4-0000-1000-8000-00805f9b34fb";
    static constexpr const char* SERVICE_FFE0 = "0000ffe0-0000-1000-8000-00805f9b34fb";
    static constexpr const char* NOTIFY_FFE1  = "0000ffe1-0000-1000-8000-00805f9b34fb";

    void onConnected(NimBLERemoteService* svc) override;
    void onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) override;
    bool sendTare() override;

    // XOR-validated frame decode. Returns false for acks, battery frames and short frames.
    static bool decode(const uint8_t* d, size_t len, ScaleReading& out);

    uint32_t badFrames() const { return _badFrames; }

private:
    NimBLERemoteCharacteristic* _notify = nullptr;
    NimBLERemoteCharacteristic* _write  = nullptr;
    uint32_t _badFrames = 0;
    uint8_t  _tareTry   = 0;
};