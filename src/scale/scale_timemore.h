// =================================================================================================
//  scale_timemore.h - Timemore Black Mirror Dot / Basic3 Link / Nano   (docs/Hardware.md)
//
//  Two GATT layouts are supported:
//
//  1. Black Mirror Dot (TES017) and Basic3 Link (TES016) - Timemore's published protocol v1.0.3,
//     github.com/TIMEMORE-COFFEE/open-scale-protocol:
//       service 0xFFF0, notify 0xFFF1 (uplink), write-without-response 0xFFF2 (downlink).
//       frame: A5 5A | opcode | cmd | len (BE16, payload bytes) | payload | CRC-16 (BE16)
//       opcode 0x01 notify, 0x02 read, 0x03 write. Multi-byte fields are big-endian.
//       cmd 0x01 weight report, every 100 ms: int32 weight 0.1 g | int16 flow 0.1 g/s |
//                uint16 timer s | uint8 overload
//       cmd 0x05 battery: uint8 bars | uint8 percent
//       cmd 0x0D tare (write, no payload); the scale answers 03 0D 00 01 <01 ok / 00 fail>
//       The Dot sends 0x0000 in the CRC field of its notifications, so received CRCs are only
//       checked when non-zero.
//
//  2. Older models, per Beanconqueror (github.com/graphefruit/Beanconqueror,
//     src/classes/devices/timemoreScale.ts):
//       service 0x181D, notify 0x2A9D, commands on vendor char 553f4e49-..., tare = 0x00.
//       frame: [1..2] int16 little-endian weight in 0.1 g.
// =================================================================================================
#pragma once
#include "scale_interface.h"
class TimemoreDriver : public ScaleDriver {
public:
    const char* name() const override { return "Timemore Black Mirror"; }
    ScaleLink   link() const override { return ScaleLink::TIMEMORE; }
    // Published protocol (Dot / Basic3 Link)
    static constexpr const char* DOT_NOTIFY_UUID = "0000fff1-0000-1000-8000-00805f9b34fb";
    static constexpr const char* DOT_WRITE_UUID  = "0000fff2-0000-1000-8000-00805f9b34fb";
    // Older layout
    static constexpr const char* SERVICE_UUID = "0000181d-0000-1000-8000-00805f9b34fb";
    static constexpr const char* NOTIFY_UUID  = "00002a9d-0000-1000-8000-00805f9b34fb";
    static constexpr const char* CMD_UUID     = "553f4e49-bf21-4468-9c6c-0e4fb5b17697";
    static constexpr const char* WRITE_UUID   = "00002a9e-0000-1000-8000-00805f9b34fb"; // fallback
    void onConnected(NimBLERemoteService* svc) override;
    void onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) override;
    bool sendTare() override;
    void service() override;
    bool canTare() const override;
    // CRC-16/IBM as specified (poly 0x8005 reflected, init 0xFFFF) over every byte before the CRC.
    static uint16_t crc16(const uint8_t* d, size_t n);
    // Weight report payload (cmd 0x01): int32 BE weight 0.1 g, int16 BE flow 0.1 g/s, ...
    static bool decodeDotWeight(const uint8_t* payload, size_t n, ScaleReading& out);
    // Older layout: [1..2] int16 little-endian, 0.1 g.
    static bool decodeLegacy(const uint8_t* d, size_t len, ScaleReading& out);
private:
    enum class Mode : uint8_t { NONE, DOT, LEGACY };
    bool writeDot(uint8_t opcode, uint8_t cmd);
    void onDotFrame(uint8_t opcode, uint8_t cmd, const uint8_t* payload, size_t n);

    Mode                        _mode   = Mode::NONE;
    NimBLERemoteCharacteristic* _notify = nullptr;
    NimBLERemoteCharacteristic* _write  = nullptr;
    volatile int8_t             _battery   = -1;
    volatile bool               _overload  = false;
    // Hardware tare bookkeeping. The ack arrives on the NimBLE host task, the timeout is checked on
    // the "ble" task. A tare that is not acknowledged disables hardware tare for this connection
    // and is redone in software.
    volatile uint32_t           _tareSentMs   = 0;
    volatile uint32_t           _tareLastSentMs = 0;   // kept after a timeout, to time a late answer
    volatile bool               _tareNoAck    = false;
};
extern TimemoreDriver  g_timemoreDummy;
