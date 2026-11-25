// =================================================================================================
//  scale_generic.cpp - see scale_generic.h
// =================================================================================================

#include "scale_generic.h"
#include "scale_manager.h"
#include "logger.h"

namespace {
// Tare command variants, tried in order. Kept static so they live in flash, not RAM.
const uint8_t TARE_DECENT[5]   = {0x03, 0x0F, 0x01, 0x00, 0x00};
const uint8_t TARE_BOOKOO[6]   = {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08};
const uint8_t TARE_ASCII_T[1]  = {'T'};
}

void GenericDriver::onConnected(NimBLERemoteService* svc) {
    if (!svc) return;
    // Decent/Felicita put both notify and write on 0xFFF4; Cocinare on 0xFFE1.
    _notify = svc->getCharacteristic(NimBLEUUID(NOTIFY_FFF4));
    _write  = _notify;
    if (!_notify) {
        _notify = svc->getCharacteristic(NimBLEUUID(NOTIFY_FFE1));
        _write  = _notify;
    }
    if (_notify && _notify->canNotify()) {
        _notify->subscribe(true, ScaleManager::onNotifyTrampoline, /*response=*/true);
        SB_LOGI("Generic scale: subscribed to vendor notify characteristic");
    } else {
        SB_LOGE("Generic scale: neither 0xFFF4 nor 0xFFE1 is notifiable");
    }
    if (_write && !_write->canWrite() && !_write->canWriteNoResponse()) {
        SB_LOGW("Generic scale: notify characteristic is not writable - software tare only");
    }
}

// -------------------------------------------------------------------------------------------------
// Accepted shapes (all XOR-validated with the final byte):
//   A. len >= 7, d[0]==0xAA (or 0xBB), d[1]=='A'/'B', d[2] sign, d[3..6] BE32 hundredths of a gram
//   B. len >= 8, d[0]==0xA5, d[1] == 0x01 (weight msg), d[2] sign, d[3..6] BE32 hundredths of a gram
//   C. len >= 6, d[0]==0x03 payload... - treated as an ack
// -------------------------------------------------------------------------------------------------
bool GenericDriver::decode(const uint8_t* d, size_t len, ScaleReading& out) {
    if (!d || len < 5) return false;

    // Never trust a frame whose checksum fails - docs/Hardware.md makes the XOR byte mandatory.
    if (!scale_util::xorCheckValid(d, len)) return false;

    const uint8_t sign_a = (uint8_t)(d[2] == 0x2D || d[2] == 0x2D);
    const uint8_t sign_b = (uint8_t)(d[2] == 0x2D || d[3] == 0x2D);

    // Shape A
    if ((d[0] == 0xAA || d[0] == 0xBB) && len >= 7 && (d[2] == 0x2B || d[2] == 0x2D)) {
        const float g = (float)scale_util::be32(d + 3) / 100.0f;
        out.grams  = (d[2] == 0x2D) ? -g : g;
        out.stable = true;
        if (len >= 8) out.batteryPct = (int8_t)constrain(d[7], 0, 100);
        return true;
    }

    // Shape B
    if (d[0] == 0xA5 && len >= 8 && d[1] == 0x01 && (d[2] == 0x2B || d[2] == 0x2D)) {
        const float g = (float)scale_util::be32(d + 3) / 100.0f;
        out.grams  = (d[2] == 0x2D) ? -g : g;
        out.stable = true;
        if (len >= 9) out.batteryPct = (int8_t)constrain(d[8], 0, 100);
        return true;
    }

    // Shape C-alt: sign carried one byte later, magnitude BE24.
    if (len >= 7 && (d[1] == 0x2B || d[1] == 0x2D)) {
        const float g = (float)scale_util::be24(d + 2) / 100.0f;
        out.grams  = (d[1] == 0x2D) ? -g : g;
        out.stable = true;
        return true;
    }

    (void)sign_a; (void)sign_b;
    return false;
}

void GenericDriver::onNotify(NimBLERemoteCharacteristic* ch, const uint8_t* data, size_t len) {
    (void)ch;
    if (!data || len == 0) return;
    if (data[0] == 0x03) return;              // ack / tare confirmation

    ScaleReading r;
    if (decode(data, len, r)) {
        g_scale.publishReading(r);
        SB_LOGV("Generic %.2f g", r.grams);
    } else {
        _badFrames++;
    }
}

bool GenericDriver::sendTare() {
    if (!_write || (!_write->canWrite() && !_write->canWriteNoResponse())) {
        SB_LOGW("Generic tare: not writable - taring locally");
        return false;
    }
    _tareTry = (_tareTry + 1) % 3;
    switch (_tareTry) {
        case 0: _write->writeValue(TARE_DECENT, sizeof(TARE_DECENT), false); break;
        case 1: _write->writeValue(TARE_BOOKOO, sizeof(TARE_BOOKOO), false); break;
        case 2: _write->writeValue(TARE_ASCII_T, sizeof(TARE_ASCII_T), false); break;
    }
    SB_LOGI("Generic tare variant %u sent", (unsigned)(_tareTry + 1));
    return true;
}