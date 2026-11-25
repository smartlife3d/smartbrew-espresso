#!/usr/bin/env python3
"""
wsprobe.py - measure the real telemetry cadence of the Smart Brew MCU.

The cockpit dashboard draws its graph from the MCU's WebSocket telemetry stream. When the
graph "feels sluggish" it is almost never the browser that is at fault: either the MCU is
not producing frames at the rate it thinks it is, or the frames arrive in bursts.

This probe opens a raw WebSocket to the controller and reports, per connection:

  * frame arrival rate (measured, not assumed)
  * the distribution of inter-arrival gaps: min / median / p95 / max
  * every gap longer than --gap-ms, with the MCU's own shot clock (t_ms) either side of it
  * whether the MCU's shot clock advanced in step with wall-clock time, or stalled
  * the MCU's advertised state, so a stall can be tied to a brew phase

It is stdlib-only (no websockets / no pip install) so it runs anywhere python3 does.

    python3 tools/wsprobe.py                       # default host, 20 s
    python3 tools/wsprobe.py --host smartbrew.local
    python3 tools/wsprobe.py --duration 60 --gap-ms 250
    python3 tools/wsprobe.py --json                # machine-readable summary
"""

import argparse
import base64
import json
import os
import socket
import statistics
import struct
import sys
import time

DEFAULT_HOST = "smartbrew.local"
DEFAULT_PORT = 80
DEFAULT_PATH = "/ws"

# --- binary telemetry frame (see include/telemetry_wire.h) -----------------------------------------
SB_TELEMETRY_VERSION = 2
SB_TELEMETRY_FIXED_LEN = 36
SB_TELEMETRY_MAX_PHASE_NAME = 27
SB_TELEMETRY_MAX_SCALE_NAME = 27
SB_TELEMETRY_MAX_PROFILE_ID = 32
SB_TELEMETRY_MIN_LEN = SB_TELEMETRY_FIXED_LEN + 4
SB_TELEMETRY_MAX_LEN = (SB_TELEMETRY_FIXED_LEN + (1 + SB_TELEMETRY_MAX_PHASE_NAME) +
                        (1 + SB_TELEMETRY_MAX_SCALE_NAME) + (1 + SB_TELEMETRY_MAX_PROFILE_ID) + 1)
SB_STATE_NAMES = ["IDLE", "STARTING", "BREWING", "FLUSHING", "STOPPING", "FAULT"]
# States in which the MCU runs the telemetry task at 20 Hz. A gap here is a real stall; a gap
# while the machine is IDLE is just the 2 Hz idle cadence and must not be counted as one.
SB_ACTIVE_STATES = {"STARTING", "BREWING", "FLUSHING", "STOPPING"}


def sb_crc8_atm(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def _read_len_str(payload, off, max_len):
    """Reads one length-prefixed string. Returns (text, next_offset) or (None, off)."""
    if off >= len(payload):
        return None, off
    length = payload[off]
    if length > max_len or off + 1 + length > len(payload):
        return None, off
    try:
        text = payload[off + 1:off + 1 + length].decode("utf-8", errors="replace")
    except Exception:
        text = ""
    return text, off + 1 + length


def decode_telemetry(payload):
    """Decode one binary telemetry frame. Returns a dict or None if it is not one."""
    if len(payload) < SB_TELEMETRY_MIN_LEN or len(payload) > SB_TELEMETRY_MAX_LEN:
        return None
    if payload[0] != 0x53 or payload[1] != SB_TELEMETRY_VERSION:
        return None
    if sb_crc8_atm(payload[:len(payload) - 1]) != payload[len(payload) - 1]:
        return None

    import struct
    (seq, t_ms) = struct.unpack_from("<HI", payload, 2)
    flags = payload[11]

    off = SB_TELEMETRY_FIXED_LEN
    phase_name, off = _read_len_str(payload, off, SB_TELEMETRY_MAX_PHASE_NAME)
    if phase_name is None:
        return None
    scale_name, off = _read_len_str(payload, off, SB_TELEMETRY_MAX_SCALE_NAME)
    if scale_name is None:
        return None
    profile_id, off = _read_len_str(payload, off, SB_TELEMETRY_MAX_PROFILE_ID)
    if profile_id is None:
        return None
    if off != len(payload) - 1:      # trailing bytes: not a frame we understand
        return None

    return {
        "seq": seq,
        "t_ms": t_ms,
        "state": SB_STATE_NAMES[payload[8]] if payload[8] < len(SB_STATE_NAMES) else "IDLE",
        "phase_name": phase_name,
        "scale_name": scale_name,
        "profile_id": profile_id,
        "scale_conn": bool(flags & 0x01),
        "sim": bool(flags & 0x04),
    }


# ------------------------------------------------------------------------------------------------
# Minimal RFC 6455 client
# ------------------------------------------------------------------------------------------------
def ws_handshake(sock, host, port, path, timeout=5.0):
    """Send the upgrade request and read the 101 response headers."""
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        # The controller refuses cross-origin handshakes; same host as the Host header.
        f"Origin: http://{host}:{port}\r\n"
        f"\r\n"
    )
    sock.sendall(req.encode())

    buf = b""
    sock.settimeout(timeout)
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("connection closed during handshake")
        buf += chunk
        if len(buf) > 65536:
            raise ConnectionError("handshake response too large")
    head, _, rest = buf.partition(b"\r\n\r\n")
    status = head.split(b"\r\n", 1)[0].decode(errors="replace")
    if "101" not in status:
        raise ConnectionError(f"handshake refused: {status}")
    return rest


class WsWriter:
    """Writes masked client frames (RFC 6455 requires client->server frames to be masked)."""

    def __init__(self, sock):
        self.sock = sock

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        header = bytearray([0x81])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header.extend(struct.pack(">H", n))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack(">Q", n))
        header.extend(mask)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)


class FrameReader:
    """Reads whole WebSocket frames out of a stream socket."""

    def __init__(self, sock, initial=b""):
        self.sock = sock
        self.buf = bytearray(initial)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buf.extend(chunk)

    def next_frame(self):
        """Returns (opcode, payload_bytes) or None on a clean close."""
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        length = b1 & 0x7F
        idx = 2
        if length == 126:
            self._need(4)
            length = struct.unpack(">H", bytes(self.buf[2:4]))[0]
            idx = 4
        elif length == 127:
            self._need(10)
            length = struct.unpack(">Q", bytes(self.buf[2:10]))[0]
            idx = 10
        mask = b""
        if masked:
            self._need(idx + 4)
            mask = bytes(self.buf[idx:idx + 4])
            idx += 4
        self._need(idx + length)
        payload = bytes(self.buf[idx:idx + length])
        del self.buf[:idx + length]
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if opcode == 0x8:  # close
            return None
        return opcode, payload


# ------------------------------------------------------------------------------------------------
# Measurement
# ------------------------------------------------------------------------------------------------
def pct(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    k = (len(ordered) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(ordered) - 1)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def probe(host, port, path, duration, gap_ms, quiet, sim=False, start=False, stop=False):
    try:
        sock = socket.create_connection((host, port), timeout=5.0)
    except OSError as e:
        print(f"err: cannot connect to {host}:{port} ({e})", file=sys.stderr)
        return None

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        rest = ws_handshake(sock, host, port, path)
    except (OSError, ConnectionError) as e:
        print(f"err: handshake failed: {e}", file=sys.stderr)
        sock.close()
        return None

    reader = FrameReader(sock, rest)
    writer = WsWriter(sock)
    frames = []          # (wall_time, payload_text)
    states = []          # (wall_time, state)
    tms = []             # (wall_time, t_ms)
    start = time.monotonic()
    sock.settimeout(1.0)

    if sim:
        writer.send_text(json.dumps({"cmd": "SIM", "on": True}))
        time.sleep(0.4)
    if start:
        writer.send_text(json.dumps({"cmd": "START"}))
        start = time.monotonic()
    if stop:
        writer.send_text(json.dumps({"cmd": "STOP"}))

    while time.monotonic() - start < duration:
        try:
            frame = reader.next_frame()
        except socket.timeout:
            continue
        except ConnectionError:
            break
        if frame is None:
            break
        opcode, payload = frame
        now = time.monotonic()
        if opcode == 0x2:  # binary telemetry frame
            msg = decode_telemetry(payload)
            if msg is None:
                continue
        elif opcode == 0x1:  # JSON control message
            try:
                msg = json.loads(payload.decode(errors="replace"))
            except ValueError:
                continue
        else:
            continue
        frames.append((now, msg))
        if "state" in msg:
            states.append((now, msg.get("state")))
        if "t_ms" in msg:
            tms.append((now, msg["t_ms"]))

    sock.close()
    if len(frames) < 2:
        print(f"only {len(frames)} frame(s) received - nothing to analyse")
        return {"frames": len(frames)}

    wall = [f[0] for f in frames]
    gaps_ms = [(wall[i] - wall[i - 1]) * 1000.0 for i in range(1, len(wall))]
    span = wall[-1] - wall[0]
    rate = (len(frames) - 1) / span if span > 0 else 0.0

    stalls = []
    idle_gaps = 0
    for i, g in enumerate(gaps_ms):
        if g >= gap_ms:
            before = frames[i][1]
            after = frames[i + 1][1]
            # A gap is only a stall if the machine was running. At idle the task wakes every
            # 500 ms on purpose, so counting those gaps would drown the real ones.
            active = (before.get("state") in SB_ACTIVE_STATES) or \
                     (after.get("state") in SB_ACTIVE_STATES)
            if not active:
                idle_gaps += 1
                continue
            # A sequence jump of more than one means the MCU generated frames that this client
            # never received (the MCU skips frames for a client that fell behind). A jump of one
            # means everything the MCU sent arrived, just late.
            seq_delta = None
            if "seq" in before and "seq" in after:
                seq_delta = (after["seq"] - before["seq"]) & 0xFFFF
            stalls.append({
                "gap_ms": round(g, 1),
                "at_s": round(wall[i] - wall[0], 2),
                "state_before": before.get("state"),
                "state_after": after.get("state"),
                "t_ms_before": before.get("t_ms"),
                "t_ms_after": after.get("t_ms"),
                "shot_clock_advanced_ms": (after.get("t_ms") or 0) - (before.get("t_ms") or 0),
                "seq_delta": seq_delta,
                "frames_lost": (seq_delta - 1) if seq_delta else 0,
            })

    # Did the MCU's own shot clock keep up with wall-clock time?
    clock_skew = []
    for i in range(1, len(tms)):
        dt_wall = (tms[i][0] - tms[i - 1][0]) * 1000.0
        dt_mcu = tms[i][1] - tms[i - 1][1]
        if dt_wall > 0:
            clock_skew.append(dt_mcu - dt_wall)

    # Sequence numbers separate "the frame never existed" from "the frame arrived late".
    # Every telemetry frame carries a free-running counter, so a jump of more than one means
    # frames were generated and never reached this client, while a gap of exactly one means the
    # MCU generated everything it sent and the wall-clock jitter is pure delivery delay.
    seqs = [(f[0], f[1]["seq"]) for f in frames if "seq" in f[1]]
    seq_lost = 0
    seq_jumps = 0
    if len(seqs) > 1:
        for i in range(1, len(seqs)):
            delta = (seqs[i][1] - seqs[i - 1][1]) & 0xFFFF
            if delta > 1:
                seq_lost += delta - 1
                seq_jumps += 1
    seq_span = ((seqs[-1][1] - seqs[0][1]) & 0xFFFF) + 1 if seqs else 0

    result = {
        "frames": len(frames),
        "span_s": round(span, 2),
        "rate_hz": round(rate, 2),
        "gap_ms_min": round(min(gaps_ms), 1),
        "gap_ms_median": round(statistics.median(gaps_ms), 1),
        "gap_ms_p95": round(pct(gaps_ms, 0.95), 1),
        "gap_ms_max": round(max(gaps_ms), 1),
        "stalls_over_threshold": len(stalls),
        "idle_gaps_over_threshold": idle_gaps,
        "threshold_ms": gap_ms,
        "states_seen": sorted({s for _, s in states}),
        "shot_clock_skew_ms_median": round(statistics.median(clock_skew), 1) if clock_skew else None,
        "shot_clock_skew_ms_min": round(min(clock_skew), 1) if clock_skew else None,
        "shot_clock_skew_ms_max": round(max(clock_skew), 1) if clock_skew else None,
        "seq_frames": len(seqs),
        "seq_lost": seq_lost,
        "seq_jumps": seq_jumps,
        "seq_span": seq_span,
        "stall_detail": stalls[:40],
    }

    if not quiet:
        print(f"host            {host}:{port}{path}")
        print(f"frames          {result['frames']} in {result['span_s']} s")
        print(f"measured rate   {result['rate_hz']} Hz")
        print(f"gap min/med/p95/max  "
              f"{result['gap_ms_min']} / {result['gap_ms_median']} / "
              f"{result['gap_ms_p95']} / {result['gap_ms_max']} ms")
        print(f"shot-clock skew (mcu_dt - wall_dt)  "
              f"median {result['shot_clock_skew_ms_median']} ms  "
              f"range {result['shot_clock_skew_ms_min']}..{result['shot_clock_skew_ms_max']} ms")
        if seqs:
            verdict = "no frame loss - the jitter is delivery delay only" if seq_lost == 0 \
                else f"{seq_lost} frame(s) never arrived"
            print(f"sequence check   {result['seq_frames']} frames, {result['seq_span']} counters "
                  f"spanned, {seq_lost} lost ({seq_jumps} jump(s)) -> {verdict}")
        print(f"states seen     {', '.join(result['states_seen'])}")
        print(f"stalls >= {gap_ms} ms while running   {result['stalls_over_threshold']}"
              f"   (idle 2 Hz cadence gaps excluded: {result['idle_gaps_over_threshold']})")
        for s in stalls[:15]:
            lost = f"  lost {s['frames_lost']}" if s["frames_lost"] else "  (delayed, not lost)"
            print(f"   gap {s['gap_ms']:>8.1f} ms at t+{s['at_s']:>6.2f}s  "
                  f"{s['state_before']}->{s['state_after']}  "
                  f"shot clock +{s['shot_clock_advanced_ms']} ms{lost}")
    return result


def main():
    ap = argparse.ArgumentParser(description="Measure the Smart Brew MCU WebSocket telemetry cadence.")
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"MCU host or IP (default {DEFAULT_HOST})")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"HTTP port (default {DEFAULT_PORT})")
    ap.add_argument("--path", default=DEFAULT_PATH, help=f"WebSocket path (default {DEFAULT_PATH})")
    ap.add_argument("--duration", type=float, default=20.0, help="seconds to sample (default 20)")
    ap.add_argument("--gap-ms", type=float, default=200.0, help="stall threshold in ms (default 200)")
    ap.add_argument("--json", action="store_true", help="print the summary as JSON")
    ap.add_argument("--quiet", action="store_true", help="suppress the human-readable report")
    ap.add_argument("--sim", action="store_true", help="enable MCU simulation mode before sampling")
    ap.add_argument("--start", action="store_true", help="start a brew before sampling")
    ap.add_argument("--stop", action="store_true", help="stop the brew after sampling")
    args = ap.parse_args()

    result = probe(args.host, args.port, args.path, args.duration, args.gap_ms,
                   args.quiet or args.json, sim=args.sim, start=args.start, stop=args.stop)
    if result is None:
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
