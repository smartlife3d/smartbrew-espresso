// =================================================================================================
//  aux_bridge.cpp - see aux_bridge.h
// =================================================================================================

#include "aux_bridge.h"
#include "logger.h"
#include "brew_controller.h"
#include "control/profile_engine.h"
#include "scale/scale_manager.h"
#include "hal/hal_psm.h"
#include "telemetry_wire.h"

#include <HardwareSerial.h>
#include <driver/gpio.h>
#include <WiFi.h>

namespace {
HardwareSerial auxSerial(AUX_UART_NUM);

// Render the NDJSON line for the UART1 link from the binary telemetry frame.
//
// The frame is the single source of truth for every telemetry value - the WebSocket carries the
// very same bytes - so there is no second encoder that could drift away from the first. Only the
// profile *name* is added here, because the frame carries the profile id only (no client needs
// the display name at telemetry rate, and 24 more bytes per frame is not worth it).
void renderTelemetryJson(const SbTelemetryFrame& f, const char* profileName, char* out, size_t cap) {
    static const char* kStates[] = {"IDLE", "STARTING", "BREWING", "FLUSHING", "STOPPING", "FAULT"};
    const char* state = (f.state < sizeof(kStates) / sizeof(kStates[0])) ? kStates[f.state] : "IDLE";

    snprintf(out, cap,
             "{\"t_ms\":%lu,\"state\":\"%s\",\"mode\":\"%s\",\"pump_owner\":\"%s\","
             "\"phase_idx\":%u,\"phase_name\":\"%s\","
             "\"p_bar\":%.2f,\"p_target\":%.2f,\"flow_virt\":%.2f,\"psm_pct\":%u,"
             "\"weight_g\":%.1f,\"flow_weight\":%.2f,\"scale_conn\":%s,\"scale_name\":\"%s\",\"scale_bat\":%d,"
             "\"ac_hz\":%u,\"phase_pct\":%u,\"faults\":%lu,\"duty_ff\":%.1f,\"flow_slope\":%.2f,"
             "\"sensor_open_warn\":%s,\"sim\":%s,\"weight_settling\":%s,"
             "\"profile\":\"%s\",\"profile_id\":\"%s\"}",
             (unsigned long)f.t_ms, state,
             (f.mode == SB_MODE_PROFILE) ? "PROFILE" : "PASSIVE",
             (f.pump_owner == SB_OWNER_PROFILE) ? "PROFILE"
                 : (f.pump_owner == SB_OWNER_STOPPING) ? "STOPPING"
                 : (f.pump_owner == SB_OWNER_FAULT) ? "FAULT" : "PASSIVE",
             f.phase_idx, f.phase_name,
             f.p_bar / 100.0f, f.p_target / 100.0f, f.flow_virt / 100.0f, f.psm_pct,
             f.weight_g / 10.0f, f.flow_weight / 100.0f,
             (f.flags & SB_FLAG_SCALE_CONNECTED) ? "true" : "false", f.scale_name, f.scale_bat,
             f.ac_hz, f.phase_pct, (unsigned long)f.faults,
             f.duty_ff / 10.0f, f.flow_slope / 100.0f,
             (f.flags & SB_FLAG_SENSOR_OPEN) ? "true" : "false",
             (f.flags & SB_FLAG_SIMULATION) ? "true" : "false",
             (f.flags & SB_FLAG_WEIGHT_SETTLING) ? "true" : "false",
             profileName, f.profile_id);
}
}  // namespace

// -------------------------------------------------------------------------------------------------
void AuxBridge::begin() {
    // A software TX ring buffer is mandatory here, not an optimisation.
    //
    // The UART driver's hardware FIFO is 128 bytes, but one telemetry line is ~350 bytes. Without a
    // ring buffer, availableForWrite() can never exceed 128, so the whole-line test in
    // onTelemetryFrame() was never satisfied and the UART1 link stayed silent. The ring buffer is
    // what makes the NDJSON stream actually work.
    //
    // The buffer also keeps the write non-blocking: HardwareSerial::println() only blocks when
    // the ring buffer cannot take the line, and onTelemetryFrame() refuses to start a line it
    // cannot finish. Without that guarantee a single line would stall the 20 Hz telemetry task for
    // the 30 ms it takes to shift 350 bytes out at 115200 baud.
    auxSerial.setTxBufferSize(1024);
    auxSerial.begin(AUX_UART_BAUD, SERIAL_8N1, PIN_AUX_RX, PIN_AUX_TX);
    // With no display attached the RX pin floats, picks up interference from the pump and button
    // wiring, and the UART decodes it as random bytes. The internal pull-up (~45 kOhm) holds it at
    // the UART idle level. gpio_pullup_en() only sets the pad pull; pinMode() would detach the
    // pin from the UART.
    gpio_pullup_en((gpio_num_t)PIN_AUX_RX);
    _uartOk = true;
    _lastTxMs = millis();
    SB_LOGI("Aux UART%d bridge up @%d bps (tx ring 1024 B)",
            AUX_UART_NUM, AUX_UART_BAUD);
    auxSerial.println(F("{\"evt\":\"hello\",\"dev\":\"brew-mcu\",\"fw\":\"" SB_FW_VERSION "\"}"));
    _txLines++;
}

// -------------------------------------------------------------------------------------------------
void AuxBridge::onTelemetryFrame(const uint8_t* frame, size_t len) {
    if (!_uartOk || !frame) return;
    if (len < SB_TELEMETRY_MIN_LEN || len > SB_TELEMETRY_MAX_LEN) return;

    // The UART1 link runs at its own, lower rate (10 Hz during a shot, 2 Hz idle) as documented in
    // aux_bridge.h. The WebSocket carries the full 20 Hz stream for the dashboard graph. Running
    // the UART at 20 Hz as well would push 7 kB/s through a 115200 baud link - 60 % of the whole
    // bandwidth - and spend a large part of the telemetry task's budget shifting bytes nobody may
    // be listening for.
    const uint32_t now = millis();
    const uint32_t period = (g_brew.state() == BrewState::BREWING) ? 100u : 500u;
    if ((now - _lastTxMs) < period) return;
    _lastTxMs = now;

    // The NDJSON line is rendered from the very same binary frame the WebSocket carries, so the
    // two transports can never disagree about a value.
    SbTelemetryFrame f;
    if (!sbTelemetryDecode(frame, len, f)) return;

    Telemetry t;
    g_brew.snapshot(t);
    char line[512];
    const char* profileName = t.profileName[0] ? t.profileName : "Default";
    renderTelemetryJson(f, profileName, line, sizeof(line));

    // Queue the line only if it fits whole. A partial line would corrupt the NDJSON framing, and
    // blocking here would cost the telemetry task a whole frame period.
    if (auxSerial.availableForWrite() >= (int)strlen(line) + 2) {
        auxSerial.println(line);
        _txLines++;
    }
}

void AuxBridge::onWeight(float grams, int8_t batteryPct) {
    (void)batteryPct;
    (void)grams;   // weight is already part of the telemetry object
}

void AuxBridge::onShotEnd(uint16_t durationMs, float peakBar, float doseG) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"evt\":\"shot_end\",\"duration_ms\":%u,\"peak_bar\":%.2f,\"dose_g\":%.1f,\"faults\":%u}",
             durationMs, peakBar, doseG, (unsigned)g_brew.faults());
    if (_uartOk) { auxSerial.println(buf); _txLines++; }
}

// -------------------------------------------------------------------------------------------------
void AuxBridge::loop(uint32_t nowMs) {
    (void)nowMs;

    // ---- inbound UART --------------------------------------------------------------------------
    while (auxSerial.available()) {
        const char c = (char)auxSerial.read();
        if (c == '\n' || c == '\r') {
            if (_rxLen) {
                _rxBuf[_rxLen] = '\0';
                _rxLines++;
                handleLine(_rxBuf);
                _rxLen = 0;
            }
            continue;
        }
        if (_rxLen < sizeof(_rxBuf) - 1) _rxBuf[_rxLen++] = c;
    }

    // The telemetry line is driven from taskTelemetry (10 Hz brew, 2 Hz idle).
}

// -------------------------------------------------------------------------------------------------
void AuxBridge::handleLine(char* line) {
    // Trim leading spaces and ignore obvious noise.
    while (*line == ' ') ++line;
    if (!*line) return;
    // Every command is a JSON object. A line that does not start with '{' or carries control /
    // non-ASCII bytes is line noise (a floating or unterminated RX pin), not a malformed command:
    // drop it without answering. Say so once per boot, so the log is not flooded.
    bool noise = (*line != '{');
    for (const char* p = line; !noise && *p; ++p) {
        const unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch > 0x7E) noise = true;
    }
    if (noise) {
        if (++_rxNoise == 1) {
            SB_LOGI("AUX RX: ignoring line noise on GPIO %d (nothing attached, or a baud mismatch) "
                    "- further noise is dropped silently", PIN_AUX_RX);
        }
        return;
    }

    SB_LOGV("AUX RX: %s", line);

    String resp;
    if (!g_brew.handleJsonCommand(line, resp)) {
        SB_LOGW("AUX command rejected: %s", resp.c_str());
        if (_uartOk) { auxSerial.println(resp); _txLines++; }
        return;
    }
    if (_uartOk) {
        char ack[160];
        snprintf(ack, sizeof(ack), "{\"evt\":\"ack\",\"raw\":%s}", resp.c_str());
        auxSerial.println(ack);
        _txLines++;
    }
}

AuxBridge g_aux;
