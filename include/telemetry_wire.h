// =================================================================================================
//  telemetry_wire.h - binary telemetry frame shared by the MCU and every WebSocket client.
//
//  The dashboard graph is drawn from a 20 Hz telemetry stream, and until now every frame was a
//  ~350 byte JSON object. That cost three things: JSON.parse() work on every client (the Echo
//  Show 5 is the weakest one in the fleet), a 640 byte snprintf() plus a heap allocation on the
//  MCU for every frame, and 7 kB/s of airtime that filled the TCP send buffer.
//
//  This is the wire format that replaces it. It is deliberately self-contained - every field a
//  client could need is in the frame, so no client has to correlate a telemetry frame with an
//  earlier control message to know what it is looking at.
//
//  The numeric fields are scaled integers, not floats. The scales match the decimal places the
//  JSON used to print, so the numbers a client sees are bit-for-bit the numbers it saw before:
//
//      p_bar        -> centibar   (u16, 0.01 bar,   0 .. 655.35 bar)
//      p_target     -> centi-unit (u16, 0.01 of the phase's own unit: bar, ml/s or %)
//      flow_virt    -> centi-ml/s (u16, 0.01 ml/s)
//      flow_weight  -> centi-g/s  (i16, 0.01 g/s,  signed)
//      weight_g     -> deci-gram  (i16, 0.1 g,    signed, -3276.7 .. 3276.7 g)
//      duty_ff      -> deci-%     (i16, 0.1 %,    signed)
//      flow_slope   -> centi-g/s  (i16, 0.01 g/s,  signed)
//
//  ---------------------------------------------------------------------------
//  LAYOUT
//
//  Version 1 was a fixed 77-byte frame with three fixed string fields. That was a bug waiting
//  to happen: `profile_id[12]` held 11 characters plus the NUL, so a profile called
//  `blooming_allonge` arrived at the dashboard as `blooming_al`. The dashboard then asked the
//  controller for a profile that did not exist, and the controller kept answering with the
//  truncated id, so the two ends ping-ponged between two names forever.
//
//  Version 2 keeps the numeric head byte-for-byte identical and replaces the fixed string fields
//  with a variable-length tail: each string is stored as a length byte followed by that many
//  characters. Nothing can be truncated any more, and a frame carrying short names stays short.
//
//   off  size  field          type      notes
//   ---  ----  -------------  --------  -----------------------------------------------
//    0    1    magic          uint8     0x53 ('S')
//    1    1    version        uint8     SB_TELEMETRY_VERSION
//    2    2    seq            uint16    free running, wraps at 65536
//    4    4    t_ms           uint32    milliseconds since the shot started
//    8    1    state          uint8     SBState
//    9    1    mode           uint8     SBMode (passive / profile)
//   10    1    pump_owner     uint8     SBPumpOwner
//   11    1    flags          uint8     b0 scale connected, b1 sensor open warn, b2 simulation,
//                                    b3 weight settling (see below)
//   12    1    phase_idx      uint8     index of the running phase
//   13    1    phase_pct      uint8     0 .. 100
//   14    1    psm_pct        uint8     TRIAC duty percent
//   15    1    scale_bat      int8      -1 = unknown
//   16    2    ac_hz          uint16    mains frequency
//   18    4    faults         uint32    fault bitmask (see config_schema.h FaultBits)
//   22    2    p_bar          uint16    0.01 bar
//   24    2    p_target       uint16    0.01 of the phase target unit
//   26    2    flow_virt      uint16    centi-ml/s
//   28    2    flow_weight    int16     centi-g/s
//   30    2    weight_g       int16     deci-gram, signed (a tared scale reads below zero
//                                       when the cup is lifted; this was uint16 clamped at 0)
//   32    2    duty_ff        int16     deci-%
//   34    2    flow_slope     int16     centi-g/s
//   36    ..   phase_name     u8 + n    length byte, then the characters (no NUL on the wire)
//        ..   scale_name     u8 + n    length byte, then the characters
//        ..   profile_id     u8 + n    length byte, then the characters
//   end   1    crc8           uint8     CRC-8/ATM over bytes [0 .. end-1]
//
//  A typical frame (a 20-character phase name, an 8-character scale name and a 16-character
//  profile id) is 83 bytes - still about 76 % less traffic than the JSON it replaces.
//
//  ---------------------------------------------------------------------------
//  THE POST-SHOT WEIGHT TAIL
//
//  When the pump stops, the last of the coffee is still draining out of the grouphead, so the cup
//  ends up heavier than the reading that triggered the cutoff. The controller keeps watching the
//  scale through that tail and raises SB_FLAG_WEIGHT_SETTLING while it is doing so.
//
//  A client that latches the cutoff weight would therefore show a yield that is too low. While the
//  flag is set the client must keep taking the live weight (and the highest value it has seen),
//  and only freeze once the flag clears. The settled yield is the highest weight seen during the
//  window, so lifting the cup afterwards does not erase it.
//
//  The flag lives in a spare bit of the existing flags byte, so the frame layout is unchanged and
//  a client that does not know the flag simply ignores it.
//
//  This header is shared by reference, not copied. The MCU compiles it from include/, the Waveshare
//  display reaches it with -I../include in its platformio.ini, and the companion and the browser
//  side re-implement the same layout (see companion-c3-scale/ and data/index.html). If you change
//  the layout here, change it there too - the offsets must stay identical.
// =================================================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SB_TELEMETRY_MAGIC    0x53   // 'S'
#define SB_TELEMETRY_VERSION  2

// --- the numeric head is fixed -------------------------------------------------------------------
#define SB_TELEMETRY_FIXED_LEN 36

// --- string limits ------------------------------------------------------------------------------
// The controller's own buffers are the ceiling: SB_PHASE_NAME_LEN (28) for a phase name, the
// scale manager's 28-byte device name, and the 32-character profile id limit that both
// sanitizeProfileId() and the dashboard's cleanProfileId() enforce.
#define SB_TELEMETRY_MAX_PHASE_NAME 27
#define SB_TELEMETRY_MAX_SCALE_NAME 27
#define SB_TELEMETRY_MAX_PROFILE_ID  32

// --- frame length bounds ------------------------------------------------------------------------
// The shortest frame carries three empty strings; the longest carries three full-length ones.
#define SB_TELEMETRY_MIN_LEN \
    (SB_TELEMETRY_FIXED_LEN + 3 + 1)
#define SB_TELEMETRY_MAX_LEN \
    (SB_TELEMETRY_FIXED_LEN + (1 + SB_TELEMETRY_MAX_PHASE_NAME) + \
     (1 + SB_TELEMETRY_MAX_SCALE_NAME) + (1 + SB_TELEMETRY_MAX_PROFILE_ID) + 1)

// --- state --------------------------------------------------------------------------------------
enum SBState : uint8_t {
    SB_STATE_IDLE     = 0,
    SB_STATE_STARTING = 1,
    SB_STATE_BREWING  = 2,
    SB_STATE_FLUSHING = 3,
    SB_STATE_STOPPING = 4,
    SB_STATE_FAULT    = 5,
};

enum SBMode : uint8_t {
    SB_MODE_PASSIVE = 0,
    SB_MODE_PROFILE = 1,
};

enum SBPumpOwner : uint8_t {
    SB_OWNER_PASSIVE  = 0,
    SB_OWNER_PROFILE  = 1,
    SB_OWNER_STOPPING = 2,
    SB_OWNER_FAULT    = 3,
};

// --- flag bits ----------------------------------------------------------------------------------
#define SB_FLAG_SCALE_CONNECTED 0x01
#define SB_FLAG_SENSOR_OPEN     0x02
#define SB_FLAG_SIMULATION      0x04
// The post-shot weight is still settling: the last of the coffee is still arriving in the cup, so
// the client has to keep showing the live weight instead of the latched cutoff value.
#define SB_FLAG_WEIGHT_SETTLING 0x08

// --- the numeric head, exactly as it goes on the wire --------------------------------------------
struct __attribute__((packed)) SbTelemetryHead {
    uint8_t  magic;          // 0
    uint8_t  version;        // 1
    uint16_t seq;            // 2
    uint32_t t_ms;           // 4
    uint8_t  state;          // 8
    uint8_t  mode;           // 9
    uint8_t  pump_owner;     // 10
    uint8_t  flags;          // 11
    uint8_t  phase_idx;      // 12
    uint8_t  phase_pct;      // 13
    uint8_t  psm_pct;        // 14
    int8_t   scale_bat;      // 15
    uint16_t ac_hz;          // 16
    uint32_t faults;         // 18
    uint16_t p_bar;          // 22  0.01 bar
    uint16_t p_target;       // 24  0.01 of the phase target unit
    uint16_t flow_virt;      // 26  centi-ml/s
    int16_t  flow_weight;    // 28  centi-g/s
    int16_t  weight_g;       // 30  deci-gram, signed
    int16_t  duty_ff;        // 32  deci-%
    int16_t  flow_slope;     // 34  centi-g/s
};

static_assert(sizeof(SbTelemetryHead) == SB_TELEMETRY_FIXED_LEN,
              "the telemetry head must be exactly SB_TELEMETRY_FIXED_LEN bytes");

// --- a decoded frame, for clients ---------------------------------------------------------------
struct SbTelemetryFrame {
    uint32_t t_ms;
    uint8_t  state;
    uint8_t  mode;
    uint8_t  pump_owner;
    uint8_t  flags;
    uint8_t  phase_idx;
    uint8_t  phase_pct;
    uint8_t  psm_pct;
    int8_t   scale_bat;
    uint16_t ac_hz;
    uint32_t faults;
    uint16_t p_bar;
    uint16_t p_target;
    uint16_t flow_virt;
    int16_t  flow_weight;
    int16_t  weight_g;
    int16_t  duty_ff;
    int16_t  flow_slope;
    char     phase_name[SB_TELEMETRY_MAX_PHASE_NAME + 1];
    char     scale_name[SB_TELEMETRY_MAX_SCALE_NAME + 1];
    char     profile_id[SB_TELEMETRY_MAX_PROFILE_ID + 1];
};

// --- CRC-8/ATM ----------------------------------------------------------------------------------
inline uint8_t sbCrc8Atm(const uint8_t* d, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// --- encoding helpers (the controller side) ------------------------------------------------------

// Writes a length-prefixed string. The string is truncated to maxLen so the declared length can
// never lie. Returns the number of bytes written, or 0 when it does not fit.
inline size_t sbTelemetryPutString(uint8_t* dst, size_t cap, const char* s, size_t maxLen) {
    size_t len = (s && *s) ? strlen(s) : 0;
    if (len > maxLen) len = maxLen;
    if (cap < len + 1) return 0;
    dst[0] = (uint8_t)len;
    if (len) memcpy(dst + 1, s, len);
    return len + 1;
}

// --- decoding helpers (the client side) ----------------------------------------------------------

// Reads a length-prefixed string at off. On success returns true, fills dst (NUL terminated) and
// advances off past the string. maxLen bounds the destination buffer.
inline bool sbTelemetryGetString(const uint8_t* d, size_t n, size_t& off,
                                 char* dst, size_t dstCap, size_t maxLen) {
    if (off >= n) return false;
    const size_t len = d[off];
    if (len > maxLen) return false;
    if (off + 1 + len > n) return false;
    const size_t copy = (len < dstCap - 1) ? len : dstCap - 1;
    if (copy) memcpy(dst, d + off + 1, copy);
    dst[copy] = '\0';
    off += 1 + len;
    return true;
}

// Decodes a complete frame. Returns false for anything that is not a valid, intact frame.
inline bool sbTelemetryDecode(const uint8_t* d, size_t n, SbTelemetryFrame& out) {
    if (!d || n < SB_TELEMETRY_MIN_LEN || n > SB_TELEMETRY_MAX_LEN) return false;
    if (d[0] != SB_TELEMETRY_MAGIC || d[1] != SB_TELEMETRY_VERSION) return false;
    if (sbCrc8Atm(d, n - 1) != d[n - 1]) return false;

    SbTelemetryHead h;
    memcpy(&h, d, sizeof(h));

    out.t_ms        = h.t_ms;
    out.state       = h.state;
    out.mode        = h.mode;
    out.pump_owner  = h.pump_owner;
    out.flags       = h.flags;
    out.phase_idx   = h.phase_idx;
    out.phase_pct   = h.phase_pct;
    out.psm_pct     = h.psm_pct;
    out.scale_bat   = h.scale_bat;
    out.ac_hz       = h.ac_hz;
    out.faults      = h.faults;
    out.p_bar       = h.p_bar;
    out.p_target    = h.p_target;
    out.flow_virt   = h.flow_virt;
    out.flow_weight = h.flow_weight;
    out.weight_g    = h.weight_g;
    out.duty_ff     = h.duty_ff;
    out.flow_slope  = h.flow_slope;

    size_t off = SB_TELEMETRY_FIXED_LEN;
    if (!sbTelemetryGetString(d, n, off, out.phase_name, sizeof(out.phase_name),
                              SB_TELEMETRY_MAX_PHASE_NAME)) return false;
    if (!sbTelemetryGetString(d, n, off, out.scale_name, sizeof(out.scale_name),
                              SB_TELEMETRY_MAX_SCALE_NAME)) return false;
    if (!sbTelemetryGetString(d, n, off, out.profile_id, sizeof(out.profile_id),
                              SB_TELEMETRY_MAX_PROFILE_ID)) return false;
    // The three strings must end exactly on the CRC byte - no padding, no trailing garbage.
    if (off != n - 1) return false;
    return true;
}
