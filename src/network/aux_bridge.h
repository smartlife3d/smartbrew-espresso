// =================================================================================================
//  aux_bridge.h - External display bridge (UART1 NDJSON)
//
//  One transport remains: the UART1 NDJSON link.
//
//   UART1   GPIO17 TX -> display RX, GPIO18 RX <- display TX, 115200 8N1, newline-terminated JSON.
//           Outbound: the telemetry object as NDJSON at 10 Hz during a shot, 2 Hz idle.
//           Inbound : {"cmd":"START"} / STOP / TARE / NEXT_PHASE / OVERRIDE_PRESSURE / MANUAL_POWER
//
//  The ESP-NOW transport that used to sit beside this one has been removed. It broadcast 16-byte
//  frames to a peer that never existed in this project, and it put a second Wi-Fi station on the
//  same channel as the dashboard's telemetry stream, competing with it for airtime. The companion
//  scale MCU now reaches the controller over BLE instead - see src/scale/scale_manager.cpp.
//
//  The NDJSON line is rendered from the same binary telemetry frame that the WebSocket carries, so
//  there is exactly one encoder for the telemetry values.
// =================================================================================================
#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

class AuxBridge {
public:
    void begin();
    void loop(uint32_t nowMs);        // Core 0 task - reads UART, emits the idle-rate telemetry

    bool uartConnected() const { return _uartOk; }
    uint32_t txLines() const  { return _txLines; }
    uint32_t rxLines() const  { return _rxLines; }

    // Callbacks into the brew controller
    void onTelemetryFrame(const uint8_t* frame, size_t len);
    void onWeight(float grams, int8_t batteryPct);
    void onShotEnd(uint16_t durationMs, float peakBar, float doseG);

private:
    void handleLine(char* line);

    bool     _uartOk = false;
    char     _rxBuf[256]{};
    uint8_t  _rxLen = 0;
    uint32_t _lastTxMs = 0;
    uint32_t _txLines = 0;
    uint32_t _rxLines = 0;
    uint32_t _rxNoise = 0;   // inbound lines dropped as line noise
};

extern AuxBridge g_aux;
