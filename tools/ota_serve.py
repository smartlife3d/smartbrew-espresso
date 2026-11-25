#!/usr/bin/env python3
"""ota_serve.py - serve a firmware image to the controller's URL update.

    python3 tools/ota_serve.py [file] [--port 8000]

Then start the update on the controller:

    curl -X POST http://smartbrew.local/api/v1/ota/github \\
         --data-urlencode 'url=http://<this PC's IP>:8000/<file name>'

The file is sent in 8 KB bursts with a 150 ms pause after each. Firmware before the
"fix(ota): no busy-wait in URL update" commit reads the download with Stream::readBytes(), which
busy-waits without yielding while a partial buffer is waiting. That keeps the core 0 idle task off
the CPU and the task watchdog resets the board mid-update. During each pause nothing is waiting,
the loop takes its 1 ms sleep and the idle task runs. Newer firmware does not need the pauses, but
they do no harm. stdlib only.
"""
import argparse, http.server, os, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("file", nargs="?", default=".pio/build/esp32s3/firmware_ota_pad.bin")
ap.add_argument("--port", type=int, default=8000)
ap.add_argument("--burst", type=int, default=8192, help="bytes per burst (default 8192)")
ap.add_argument("--pause", type=float, default=0.15, help="pause after each burst, s (default 0.15)")
args = ap.parse_args()
path = os.path.abspath(args.file)
if not os.path.isfile(path):
    sys.exit(f"no such file: {path}")
name = os.path.basename(path)
size = os.path.getsize(path)

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.lstrip("/") != name:
            self.send_error(404); return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        sent, t0 = 0, time.time()
        with open(path, "rb") as f:
            while True:
                b = f.read(args.burst)
                if not b:
                    break
                try:
                    self.wfile.write(b)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    print(f"\nconnection dropped at {sent} / {size} bytes"); return
                sent += len(b)
                print(f"\r  {sent // 1024:5d} / {size // 1024} kB", end="", flush=True)
                time.sleep(args.pause)
        print(f"\r  {size // 1024} / {size // 1024} kB sent in {time.time() - t0:.0f} s - "
              "the controller restarts once it has written the image")
    def log_message(self, fmt, *a):
        print(f"{self.client_address[0]} {fmt % a}")

est = size / args.burst * args.pause
print(f"serving {name} ({size} bytes) on port {args.port}, {args.burst} B bursts, "
      f"{args.pause * 1000:.0f} ms pauses (~{est:.0f} s)")
http.server.ThreadingHTTPServer(("", args.port), Handler).serve_forever()
