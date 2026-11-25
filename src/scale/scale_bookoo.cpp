// =================================================================================================
//  scale_bookoo.cpp - Bookoo Themis Ultra / Mini   (docs/Hardware.md)
//
//  Bookoo's published protocol (github.com/BooKooCode/OpenSource, bookoo_ultra_scale and
//  bookoo_mini_scale/protocols.md). Service 0x0FFE, weight notify 0xFF11, commands on 0xFF12.
//  Every frame is 20 bytes, multi-byte fields big-endian, last byte = XOR of all bytes before it.
//
//  Weight frame (type 0x0B):
//     [0] 0x03 product  [1] 0x0B type  [2..4] ms  [5] unit (01 g, 02 oz)
//     [6] weight sign ('+'/'-')  [7..9] grams * 100
//     [10] flow sign  [11..12] flow g/s * 100  [13] battery %  [14..15] standby  [16] buzzer
//     [17] flow smoothing  [18] 0x00  [19] XOR checksum
//  Other 0x03 frames (0x0D automatic-mode event, 0x0F powder weight) are ignored.
//
//  Tare command written to 0xFF12: 03 0A 01 00 00 <xor = 08>
//
//  The ESP32-C3 companion scale (companion-c3-scale/) speaks an older variant on the same UUIDs:
//  [0] 0x02, [6] sign, [7..9] grams * 100, no checksum. It is still accepted.
// =================================================================================================
#include "scale_bookoo.h"
#include "scale_manager.h"
#include "logger.h"

namespace {
constexpr size_t  FRAME_LEN    = 20;
constexpr uint8_t PRODUCT      = 0x03;
constexpr uint8_t TYPE_WEIGHT  = 0x0B;

uint8_t xorOf(const uint8_t* d, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; ++i) x ^= d[i];
    return x;
}
}  // namespace

void BookooDriver::onConnected(NimBLERemoteService* svc) {
    _notify = nullptr;
    _write = nullptr;
    if (!svc) return;
    _notify = svc->getCharacteristic(NimBLEUUID(NOTIFY_UUID));
    _write  = svc->getCharacteristic(NimBLEUUID(WRITE_UUID));
    if (_notify && _notify->canNotify()) {
        _notify->subscribe(true, ScaleManager::onNotifyTrampoline, /*response=*/true);
        SB_LOGI("Bookoo: subscribed to notify 0xFF11");
    } else {
        SB_LOGE("Bookoo: characteristic 0xFF11 not found or not notifiable");
    }
    if (!_write) {
        // Some firmware revisions expose 0xFF12 as not-writable; fall back to the notify char.
        _write = _notify;
    }
    if (_write && !_write->canWrite() && !_write->canWriteNoResponse()) {
        SB_LOGW("Bookoo: no writable characteristic - software tare only");
    }
}

bool BookooDriver::decode(const uint8_t* d, size_t len, ScaleReading& out) {
    if (!d || len < 10) return false;

    // Published protocol: 03 0B ... with an XOR checksum in the last byte.
    if (d[0] == PRODUCT) {
        if (len < FRAME_LEN || d[1] != TYPE_WEIGHT) return false;   // event, powder weight, ...
        if (xorOf(d, FRAME_LEN - 1) != d[FRAME_LEN - 1]) return false;
        const float g = (float)scale_util::be24(d + 7) / 100.0f;
        out.grams = (d[6] == 0x2D) ? -g : g;
        const float flow = (float)scale_util::be16(d + 11) / 100.0f;
        out.flowGs = (d[10] == 0x2D) ? -flow : flow;
        out.batteryPct = (d[13] <= 100) ? (int8_t)d[13] : (int8_t)-1;
        out.stable = true;
        return true;
    }

    // Companion scale variant: explicit '+'/'-' sign byte, no checksum, no battery.
    const uint8_t sign = d[6];
    if (sign != 0x2B && sign != 0x2D) return false;
    const float g = (float)scale_util::be24(d + 7) / 100.0f;
    out.grams = (sign == 0x2D) ? -g : g;
    out.stable = true;
    return true;
}

void BookooDriver::onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) {
    (void)ch;
    if (!data || len == 0) return;
    // A notification may carry more than one 20-byte frame if they queue up; decode each.
    for (size_t off = 0; off < len; off += FRAME_LEN) {
        const size_t n = (len - off < FRAME_LEN) ? (len - off) : FRAME_LEN;
        ScaleReading r;
        if (decode(data + off, n, r)) {
            g_scale.publishReading(r);
            SB_LOGV("Bookoo %.2f g", r.grams);
        }
    }
}

bool BookooDriver::sendTare() {
    if (!_write) {
        SB_LOGW("Bookoo tare: no write characteristic - taring locally");
        return false;
    }
    const bool ok = _write->writeValue((const uint8_t*)TARE_CMD, sizeof(TARE_CMD), false);
    SB_LOGI("Bookoo tare command sent (%s)", ok ? "ok" : "failed");
    return ok;
}
