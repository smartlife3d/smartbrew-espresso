// =================================================================================================
//  scale_timemore.cpp - see scale_timemore.h
// =================================================================================================
#include "scale_timemore.h"
#include "scale_manager.h"
#include "logger.h"

namespace {
// The Dot answers a tare within ~100-200 ms (measured on hardware). 2 s leaves plenty of margin;
// an answer after that is still honoured (see onDotFrame).
constexpr uint32_t TARE_ACK_TIMEOUT_MS = 2000;

// Published protocol constants
constexpr uint8_t HDR0 = 0xA5, HDR1 = 0x5A;
constexpr uint8_t OP_NOTIFY = 0x01, OP_READ = 0x02, OP_WRITE = 0x03;
constexpr uint8_t CMD_WEIGHT = 0x01, CMD_BATTERY = 0x05, CMD_TARE = 0x0D;


// Search the cached characteristics of every service (the manager ran a full discovery).
NimBLERemoteCharacteristic* findAnywhere(NimBLEClient* client, const char* uuid) {
    if (!client) return nullptr;
    const NimBLEUUID want(uuid);
    for (auto* s : client->getServices(false)) {
        for (auto* c : s->getCharacteristics(false)) {
            if (c->getUUID() == want) return c;
        }
    }
    return nullptr;
}

// The CCCD must be written with a Write Request (response=true). NimBLE 1.4.x defaulted to a Write
// Command, which the Timemore Dot silently ignores: subscribe() reports success and no notification
// ever arrives. 2.x defaults to a request, but the flag stays explicit. The Bluetooth Core spec only defines the CCCD write as a Write Request.
bool subscribeTo(NimBLERemoteCharacteristic* c) {
    if (!c || (!c->canNotify() && !c->canIndicate())) return false;
    return c->subscribe(c->canNotify(), ScaleManager::onNotifyTrampoline, /*response=*/true);
}
}  // namespace

// -------------------------------------------------------------------------------------------------
uint16_t TimemoreDriver::crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

bool TimemoreDriver::writeDot(uint8_t opcode, uint8_t cmd) {
    if (!_write) return false;
    uint8_t f[8] = {HDR0, HDR1, opcode, cmd, 0x00, 0x00, 0, 0};
    const uint16_t crc = crc16(f, 6);
    f[6] = (uint8_t)(crc >> 8);
    f[7] = (uint8_t)(crc & 0xFF);
    return _write->writeValue(f, sizeof(f), false);   // 0xFFF2 is write-without-response
}

// -------------------------------------------------------------------------------------------------
void TimemoreDriver::onConnected(NimBLERemoteService* svc) {
    _mode = Mode::NONE;
    _notify = nullptr;
    _write = nullptr;
    _battery = -1;
    _overload = false;
    _tareSentMs = 0;
    _tareLastSentMs = 0;
    _tareNoAck = false;
    if (!svc) return;
    NimBLEClient* client = svc->getClient();

    // 1. Published protocol (Dot / Basic3 Link): uplink 0xFFF1, downlink 0xFFF2.
    _notify = findAnywhere(client, DOT_NOTIFY_UUID);
    if (_notify && subscribeTo(_notify)) {
        _mode = Mode::DOT;
        _write = findAnywhere(client, DOT_WRITE_UUID);
        SB_LOGI("Timemore: open protocol, subscribed to 0xFFF1, downlink %s",
                _write ? "0xFFF2" : "missing (software tare only)");
        if (_write) writeDot(OP_READ, CMD_BATTERY);   // battery is otherwise only sent on change
        return;
    }

    // 2. Older models: 0x2A9D notify, commands on the vendor characteristic.
    _notify = findAnywhere(client, NOTIFY_UUID);
    if (_notify && subscribeTo(_notify)) {
        _mode = Mode::LEGACY;
        _write = findAnywhere(client, CMD_UUID);
        if (!_write) _write = findAnywhere(client, WRITE_UUID);
        SB_LOGI("Timemore: 0x2A9D layout, command char %s",
                _write ? _write->getUUID().toString().c_str() : "none");
        return;
    }

    _notify = nullptr;
    SB_LOGE("Timemore: no known weight characteristic (0xFFF1 or 0x2A9D) on this scale");
}

// -------------------------------------------------------------------------------------------------
bool TimemoreDriver::decodeDotWeight(const uint8_t* p, size_t n, ScaleReading& out) {
    if (!p || n < 4) return false;
    out.grams  = (float)(int32_t)scale_util::be32(p) / 10.0f;
    out.flowGs = (n >= 6) ? (float)(int16_t)scale_util::be16(p + 4) / 10.0f : -1.0f;
    out.stable = true;
    return true;
}

bool TimemoreDriver::decodeLegacy(const uint8_t* d, size_t len, ScaleReading& out) {
    // [0] header, [1..2] int16 little-endian weight in 0.1 g.
    if (!d || len < 3) return false;
    const int16_t raw = (int16_t)((uint16_t)d[1] | ((uint16_t)d[2] << 8));
    out.grams  = (float)raw / 10.0f;
    out.stable = true;
    return true;
}

void TimemoreDriver::onDotFrame(uint8_t opcode, uint8_t cmd, const uint8_t* p, size_t n) {
    if (opcode == OP_NOTIFY && cmd == CMD_WEIGHT) {
        ScaleReading r;
        if (!decodeDotWeight(p, n, r)) return;
        r.batteryPct = _battery;
        const bool overload = (n >= 9) && p[8] != 0;
        if (overload != _overload) {
            _overload = overload;
            if (overload) SB_LOGW("Timemore: scale reports overload (%.1f g)", r.grams);
        }
        g_scale.publishReading(r);
        SB_LOGV("Timemore %.1f g, flow %.1f g/s", r.grams, r.flowGs);
    } else if (cmd == CMD_BATTERY && (opcode == OP_NOTIFY || opcode == OP_READ) && n >= 2) {
        const int8_t pct = (int8_t)constrain(p[1], 0, 100);
        // The scale repeats its battery report; log only when the level actually changes.
        if (pct != _battery) SB_LOGI("Timemore battery %d%% (%u bars)", (int)pct, (unsigned)p[0]);
        _battery = pct;
    } else if (opcode == OP_WRITE && cmd == CMD_TARE && n >= 1) {
        const uint32_t sent = _tareSentMs ? _tareSentMs : _tareLastSentMs;
        const unsigned waitedMs = sent ? (unsigned)(millis() - sent) : 0;
        _tareSentMs = 0;
        if (p[0] == 0x01) {
            if (_tareNoAck) {
                // The answer came after the timeout, so the manager already tared in software. The
                // scale has zeroed itself as well, so keeping the software offset would subtract
                // the same weight twice. Drop it and go back to hardware tare.
                _tareNoAck = false;
                g_scale.dropSoftwareTare();
                SB_LOGI("Timemore: tare acknowledged late (after %u ms) - software offset removed, "
                        "hardware tare back on", waitedMs);
            } else {
                SB_LOGI("Timemore: tare acknowledged after %u ms", waitedMs);
            }
        } else {
            SB_LOGW("Timemore: scale rejected the tare - switching to software tare");
            _tareNoAck = true;
            g_scale.tare();   // canTare() is false now, so this tares in software
        }
    }
}

void TimemoreDriver::onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    if (ch != _notify) return;

    if (_mode == Mode::LEGACY) {
        ScaleReading r;
        if (decodeLegacy(data, len, r)) g_scale.publishReading(r);
        return;
    }
    if (_mode != Mode::DOT) return;

    // A5 5A | opcode | cmd | len BE16 | payload[len] | CRC BE16
    // The Dot packs several frames into one notification when they queue up (seen on hardware:
    // two weight reports in 34 B, a weight report plus the battery reply in 27 B), so walk them all.
    size_t off = 0;
    while (len - off >= 8) {
        const uint8_t* f = data + off;
        if (f[0] != HDR0 || f[1] != HDR1) {
            SB_LOGV("Timemore: no frame header at offset %u - rest of notification dropped",
                    (unsigned)off);
            return;
        }
        const size_t plen = scale_util::be16(f + 4);
        const size_t flen = 6 + plen + 2;
        if (len - off < flen) return;                       // truncated frame
        const uint16_t got = scale_util::be16(f + 6 + plen);
        // The Dot leaves the CRC field at 0x0000 in its notifications; check only real CRCs.
        if (got == 0 || got == crc16(f, 6 + plen)) {
            onDotFrame(f[2], f[3], f + 6, plen);
        } else {
            SB_LOGV("Timemore: CRC mismatch on cmd 0x%02X - frame dropped", f[3]);
        }
        off += flen;
    }
}

// -------------------------------------------------------------------------------------------------
bool TimemoreDriver::canTare() const {
    if (_mode == Mode::DOT)    return _write != nullptr && !_tareNoAck;
    if (_mode == Mode::LEGACY) return _write != nullptr;
    return false;
}

bool TimemoreDriver::sendTare() {
    if (!canTare()) return false;
    bool ok = false;
    if (_mode == Mode::DOT) {
        ok = writeDot(OP_WRITE, CMD_TARE);
        if (ok) {
            // 0 means "no tare pending", so a send at millis() == 0 is stored as 1. Never round
            // upwards otherwise: a timestamp in the future makes the unsigned age below wrap to
            // ~4e9 and fires the timeout at once.
            const uint32_t now = millis();
            _tareSentMs = now ? now : 1;
            _tareLastSentMs = _tareSentMs;
        }
    } else {
        // Older models: a single 0x00 byte on the vendor command characteristic (Beanconqueror).
        const uint8_t zero[1] = {0x00};
        ok = _write->writeValue(zero, 1, _write->canWrite() && !_write->canWriteNoResponse());
    }
    SB_LOGI("Timemore tare sent (%s)", ok ? "ok" : "failed");
    return ok;
}

void TimemoreDriver::service() {
    // A Dot tare without an acknowledgement means the command was not understood. Redo it in
    // software so the shot does not start from a non-zero weight, and stop sending hardware tares.
    const uint32_t sent = _tareSentMs;
    if (sent != 0 && (int32_t)(millis() - sent) > (int32_t)TARE_ACK_TIMEOUT_MS) {
        _tareSentMs = 0;
        _tareNoAck = true;
        SB_LOGW("Timemore: no tare acknowledgement in %u ms - switching to software tare",
                (unsigned)TARE_ACK_TIMEOUT_MS);
        g_scale.tare();
    }
}
