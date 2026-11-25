// bookoo_ble.h - transport 1: Bookoo Themis BLE protocol spoof.
//
// Advertises with service 0x0FFE, notify char 0xFF11 and write char 0xFF12 so the
// scale is accepted by the parent ESP32-S3 firmware and by any app or controller that
// supports Bookoo-protocol scales. The advertised NAME is generic by default and per-unit
// configurable - see scale_identity.h.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "scale_config.h"

// Creates the GATT server and starts advertising. Returns true on success.
bool     bookooInit(void);

// Builds the 20-byte Bookoo weight frame for a sample (exposed for tests/logs).
uint8_t  bookooBuildFrame(const ScaleSample& s, uint8_t* out);

// Called from the BLE task at BOOKOO_RATE_HZ; notifies subscribers if any.
void     bookooTick(const ScaleSample& s);

// Consumes a pending software-tare request (true once per request).
bool     bookooTakeTareRequest(void);

// True while at least one central is connected.
bool     bookooIsConnected(void);

// Number of frames actually pushed to a subscriber.
uint32_t bookooNotifyCount(void);