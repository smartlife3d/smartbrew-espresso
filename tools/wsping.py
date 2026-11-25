#!/usr/bin/env python3
"""
wsping.py - measure the Smart Brew MCU's WebSocket command round-trip time.

The telemetry stream tells you how fast frames arrive, but not *where* a stall comes from.
This probe asks the controller a question and times the answer:

    client  --- {"cmd":"TARE"} --->  MCU
    client  <-- {"ok":true,...} ---  MCU

A smooth RTT means the controller is responsive and the socket is healthy, so a stall in the
telemetry stream is a transport/queueing problem rather than a busy controller. A RTT that
stalls in step with the telemetry gaps means the controller itself is blocked.

Stdlib only, no pip install.

    python3 tools/wsping.py --host 192.168.1.50 --count 40 --interval 0.25
"""

import argparse
import json
import os
import socket
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsprobe import FrameReader, WsWriter, ws_handshake, pct  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="Measure MCU WebSocket command round-trip time.")
    ap.add_argument("--host", default="smartbrew.local")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/ws")
    ap.add_argument("--count", type=int, default=40, help="number of probes (default 40)")
    ap.add_argument("--interval", type=float, default=0.25, help="seconds between probes")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=5.0)
    except OSError as e:
        print("err: cannot connect to %s:%d (%s)" % (args.host, args.port, e), file=sys.stderr)
        return 1
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    try:
        rest = ws_handshake(sock, args.host, args.port, args.path)
    except (OSError, ConnectionError) as e:
        print("err: handshake failed: %s" % e, file=sys.stderr)
        sock.close()
        return 1

    reader = FrameReader(sock, rest)
    writer = WsWriter(sock)
    sock.settimeout(3.0)

    rtts = []
    pending = None
    deadline = time.monotonic() + args.interval
    probes = 0

    while probes < args.count and time.monotonic() < deadline + 5:
        now = time.monotonic()
        if pending is None and now >= deadline:
            writer.send_text(json.dumps({"cmd": "TARE"}))
            pending = now
            probes += 1
            deadline = now + args.interval

        try:
            frame = reader.next_frame()
        except socket.timeout:
            continue
        except ConnectionError:
            break
        if frame is None:
            break
        opcode, payload = frame
        if opcode != 0x1:
            continue
        try:
            msg = json.loads(payload.decode(errors="replace"))
        except ValueError:
            continue
        if "ok" in msg and pending is not None:
            rtts.append((time.monotonic() - pending) * 1000.0)
            pending = None

    sock.close()

    if not rtts:
        print("no replies received")
        return 1

    result = {
        "probes": len(rtts),
        "rtt_ms_min": round(min(rtts), 1),
        "rtt_ms_median": round(statistics.median(rtts), 1),
        "rtt_ms_p95": round(pct(rtts, 0.95), 1),
        "rtt_ms_max": round(max(rtts), 1),
        "over_100ms": sum(1 for r in rtts if r > 100),
        "over_200ms": sum(1 for r in rtts if r > 200),
    }
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("replies        %d / %d probes" % (len(rtts), probes))
        print("rtt min/med/p95/max  %s / %s / %s / %s ms" % (
            result["rtt_ms_min"], result["rtt_ms_median"],
            result["rtt_ms_p95"], result["rtt_ms_max"]))
        print("over 100 ms    %d" % result["over_100ms"])
        print("over 200 ms    %d" % result["over_200ms"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
