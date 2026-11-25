// =================================================================================================
//  scale_acaia.cpp - see scale_acaia.h
// =================================================================================================

#include "scale_acaia.h"
#include "scale_manager.h"
#include "logger.h"

namespace {
// Acaia message types we care about.
constexpr uint8_t MSG_WEIGHT  = 0x05;
constexpr uint8_t MSG_BATTERY = 0x06;
constexpr uint8_t MSG_READY   = 0x0B;   // "scale is ready" / handshake response

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
}  // namespace

void AcaiaDriver::onConnected(NimBLERemoteService* svc) {
    if (!svc) return;
    _notify = svc->getCharacteristic(NimBLEUUID(WEIGHT_UUID));
    _write  = _notify;
    if (!_notify) _notify = svc->getCharacteristic(NimBLEUUID(NOTIFY_UUID));

    if (_notify && _notify->canNotify()) {
        _notify->subscribe(true, ScaleManager::onNotifyTrampoline, /*response=*/true);
        SB_LOGI("Acaia: subscribed to 0x2A80");
    } else {
        SB_LOGE("Acaia: 0x2A80 not found");
        return;
    }
    if (!_notify->canWrite() && !_notify->canWriteNoResponse()) {
        SB_LOGW("Acaia: 0x2A80 is not writable - heartbeat and tare unavailable");
    }

    // Answer the "EF DD 00" handshake the scale sends immediately after connecting.
    if (_notify && (_notify->canWrite() || _notify->canWriteNoResponse())) {
        const uint8_t ready[8] = {0xEF, 0xDD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        _notify->writeValue(ready, sizeof(ready), false);
    }
    _needsHandshake = false;
    _lastHeartbeatMs = millis();
}

// -------------------------------------------------------------------------------------------------
// Message 0x05 layout (little-endian):
//   [0] 0xEF  [1] 0xDD  [2] msgType(0x05)  [3] weight(4 bytes LE, mg)
//   [7] stable flag / timer  [8] battery  [9] tare
// -------------------------------------------------------------------------------------------------
bool AcaiaDriver::decodeWeightMsg(const uint8_t* d, size_t len, ScaleReading& out) {
    if (!d || len < 10) return false;
    if (d[0] != 0xEF || d[1] != 0xDD) return false;
    if (d[2] != MSG_WEIGHT) return false;

    const uint32_t mg = le32(d + 3);
    out.grams  = (float)mg / 1000.0f;
    out.stable = (d[7] & 0x01) != 0;

    if (len >= 9) out.batteryPct = (int8_t)constrain(d[8], 0, 100);
    return true;
}

void AcaiaDriver::onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) {
    (void)ch;
    if (!data || len < 3) return;
    if (data[0] != 0xEF || data[1] != 0xDD) return;

    ScaleReading r;
    if (decodeWeightMsg(data, len, r)) {
        _lastG = r.grams;
        g_scale.publishReading(r);
        SB_LOGV("Acaia %.2f g", r.grams);
        return;
    }
    if (data[2] == MSG_BATTERY && len >= 4) {
        ScaleReading b; b.batteryPct = (int8_t)constrain(data[3], 0, 100); b.grams = _lastG;
        g_scale.publishReading(b);
        SB_LOGI("Acaia battery %d%%", b.batteryPct);
    }
}

// -------------------------------------------------------------------------------------------------
bool AcaiaDriver::sendTare() {
    if (!_write || (!_write->canWrite() && !_write->canWriteNoResponse())) {
        SB_LOGW("Acaia tare: not writable - taring locally");
        return false;
    }
    // The Acaia tare command is msgType 0x04 with a zero weight.
    const uint8_t tare[10] = {0xEF, 0xDD, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const bool ok = _write->writeValue(tare, sizeof(tare), false);
    SB_LOGI("Acaia tare sent (%s)", ok ? "ok" : "failed");
    return ok;
}

void AcaiaDriver::heartbeat() {
    // docs/Hardware.md: the scale drops the link without a 3 s heartbeat. Payload is a bare msgType 0x01.
    const uint8_t hb[3] = {0xEF, 0xDD, 0x01};
    if (_write && (_write->canWrite() || _write->canWriteNoResponse())) {
        _write->writeValue(hb, sizeof(hb), false);
    }
}

void AcaiaDriver::service() {
    const uint32_t now = millis();
    if (now - _lastHeartbeatMs >= 3000) {
        _lastHeartbeatMs = now;
        heartbeat();
    }
}