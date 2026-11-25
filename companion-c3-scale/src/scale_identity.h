// scale_identity.h - per-unit BLE identity.
//
// The advertised device name carries no protocol meaning, so it can be anything
// the owner wants to sell under. Resolution order at boot:
//
//   1. name stored in NVS (set by `name <text>` on the serial console)
//   2. SCALE_DEVICE_NAME from scale_config.h (generic by default)
//
// The change takes effect after a reboot, because the name is baked into the
// advertising data when NimBLEDevice::init() runs.

#pragma once

#include <Arduino.h>

// Loads the stored name (if any) into RAM. Call BEFORE bookooInit().
void        scaleIdentityInit(void);

// Resolved advertised name, never NULL, at most SCALE_NAME_MAX characters.
const char* scaleDeviceName(void);

// Last 4 hex digits of the chip MAC - a stable per-unit identifier for labels,
// logs and pairing records.
const char* scaleIdentityShortId(void);

// Validates and persists a new advertised name.
// Returns false (and changes nothing) for empty, over-long or non-printable
// input. The stored name applies after a reboot.
bool        scaleIdentitySetName(const char* name);

// Drops the stored name; the compiled default applies again after a reboot.
void        scaleIdentityClearName(void);