#!/usr/bin/env python3
"""verify_flash.py - read the flash back off the board and check it against the local build.

Trusting the writer's "Hash of data verified" only proves the chip accepted the bytes over USB.
This re-reads the flash independently and compares it to the artefacts on disk, so a wrong
partition table, a stale image, or a partial write is caught rather than assumed.

The LittleFS partition is reported, not judged: it holds the brew ledger, the brew profiles and
the controller config, and a normal firmware update leaves it alone. The board is therefore
EXPECTED to differ from the local filesystem build. Pass --fs when the operator says the image on
the board should match the local build (that means the filesystem was flashed, which also means
the ledger was replaced).

Run:  ~/.platformio/penv/bin/python tools/verify_flash.py [--port /dev/cu.usbmodemXXXX] [--fs]
"""

import argparse
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

BUILD = os.path.join(ROOT, ".pio", "build", "esp32s3")
ESPTOOL = os.path.expanduser(
    "~/.platformio/packages/tool-esptoolpy/esptool.py")
PY = os.path.expanduser("~/.platformio/penv/bin/python")

# docs/Architecture.md - the layout this project promises.
#
# The labels are the ones the board actually carries: partitions_8MB.csv declares the filesystem
# partition with the "spiffs" subtype, so the on-flash label reads "spiffs". PlatformIO mounts and
# writes it as LittleFS because platformio.ini sets board_build.filesystem = littlefs - the label in
# the partition table is just the CSV's subtype name and carries no meaning of its own.
EXPECTED_PARTITIONS = [
    ("nvs",      0x9000,   0x5000),
    ("otadata",  0xE000,   0x2000),
    ("app0",     0x10000,  0x300000),
    ("app1",     0x310000, 0x300000),
    ("spiffs",   0x610000, 0x1E0000),
    ("coredump", 0x7F0000, 0x10000),
]

APP0_OFF = 0x10000
FS_OFF = 0x610000
FS_SIZE = 0x1E0000
GREEN, RED, YEL, DIM, OFF = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"
if not sys.stdout.isatty() or os.environ.get("NO_COLOR"):
    GREEN = RED = YEL = DIM = OFF = ""

failures = []


def check(ok: bool, label: str, detail: str = "") -> bool:
    mark = f"{GREEN}PASS{OFF}" if ok else f"{RED}FAIL{OFF}"
    print(f"  {mark}  {label}" + (f"  {DIM}{detail}{OFF}" if detail else ""))
    if not ok:
        failures.append(label)
    return ok


def esptool(port, *args, capture=True):
    cmd = [PY, ESPTOOL, "--port", port, "--chip", "esp32s3", *args]
    return subprocess.run(cmd, capture_output=capture, text=True)


def read_flash(port, offset, size, dest):
    r = esptool(port, "read_flash", hex(offset), hex(size), dest)
    return r.returncode == 0 and os.path.exists(dest) and os.path.getsize(dest) == size


def parse_partition_table(d):
    """Parse the on-flash partition table into [(label, offset, size), ...].

    Each entry is 32 bytes, read little-endian:
        +0  magic 0xAA50
        +2  type | subtype<<8
        +4  offset
        +8  size
        +12 label[16]
        +28 flags
    The entries begin at the very start of the region - there is no MD5 header in front of them.
    """
    got, off = [], 0
    while off + 32 <= len(d):
        if d[off:off + 2] != b"\xAA\x50":
            break
        o = int.from_bytes(d[off + 4:off + 8], "little")
        sz = int.from_bytes(d[off + 8:off + 12], "little")
        lbl = d[off + 12:off + 28].split(b"\x00")[0].decode("ascii", "replace")
        if o == 0 and sz == 0:
            break
        got.append((lbl, o, sz))
        off += 32
    return got


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("PORT", ""))
    ap.add_argument("--fs", action="store_true",
                    help="require the littlefs on the board to match the local build "
                         "(by default a difference is expected and only reported)")
    args = ap.parse_args()

    port = args.port
    if not port:
        # Fall back to whatever the project scripts would pick.
        awk = "cd " + ROOT + " && ./flash.sh --list | awk '$NF==\"ESP32\"{print $2}'"
        r = subprocess.run(["bash", "-c", awk], capture_output=True, text=True)
        lines = [ln for ln in r.stdout.strip().splitlines() if ln.startswith("/")]
        port = lines[0] if lines else ""
    if not port:
        print("  no port - pass --port or set PORT")
        return 2
    print(f"{DIM}verifying flash on {port}{OFF}\n")

    # ---- 1. chip identity ---------------------------------------------------------------------
    r = esptool(port, "chip_id")
    ident = [ln for ln in r.stdout.splitlines() if "revision" in ln or "MAC" in ln]
    check(r.returncode == 0, "chip responds to esptool", ident[0].strip() if ident else "")

    # ---- 2. partition table ------------------------------------------------------------------
    tmp = "/tmp/sb_verify_part.bin"
    if not check(read_flash(port, 0x8000, 0x1000, tmp), "read partition table @0x8000"):
        return 1
    d = open(tmp, "rb").read()
    if not check(d[0] == 0xAA and d[1] == 0x50, "partition table magic AA50",
                 f"got {d[0]:02X}{d[1]:02X}"):
        return 1

    # On-flash entry layout (32 bytes), read little-endian:
    #   +0 magic 0xAA50 | +2 type|subtype<<8 | +4 offset | +8 size | +12 label[16] | +28 flags
    # Entries begin at the very start of the region - there is no MD5 header in front of them.
    got = parse_partition_table(d)
    for name, o, s in got:
        print(f"        {name:<10} 0x{o:06x}  0x{s:06x}  ({s // 1024} kB)")
    # The offsets and the sizes are the layout. The labels are the partition CSV's subtype names
    # and are cosmetic - a difference there is worth noting, not failing over.
    got_layout = [(o, s) for _n, o, s in got]
    want_layout = [(o, s) for _n, o, s in EXPECTED_PARTITIONS]
    check(got_layout == want_layout, "partition layout matches docs/Architecture.md",
          f"{len(got)} entries" if got_layout == want_layout else
          f"expected {[(n, hex(o), hex(s)) for n, o, s in EXPECTED_PARTITIONS]}")
    if got_layout == want_layout:
        renamed = [(g[0], w[0]) for g, w in zip(got, EXPECTED_PARTITIONS) if g[0] != w[0]]
        if renamed:
            print(f"        {DIM}labels differ (cosmetic): " +
                  ", ".join(f"{g} != {w}" for g, w in renamed) + f"{OFF}")

    # ---- 3. app0 header ----------------------------------------------------------------------
    fw = os.path.join(BUILD, "firmware.bin")
    if not check(os.path.exists(fw), "local firmware.bin exists"):
        return 1
    local = open(fw, "rb").read()
    probe = 512
    tmp = "/tmp/sb_verify_app.bin"
    if not check(read_flash(port, APP0_OFF, probe, tmp), f"read {probe} B of app0 @0x{APP0_OFF:X}"):
        return 1
    onchip = open(tmp, "rb").read()
    check(onchip == local[:probe], "app0 header matches local firmware.bin",
          f"sha256 {hashlib.sha256(onchip).hexdigest()[:16]}")

    # ---- 4. whole-app hash -------------------------------------------------------------------
    size = len(local)
    tmp = "/tmp/sb_verify_app_full.bin"
    if not check(read_flash(port, APP0_OFF, size, tmp), f"read all {size:,} B of app0"):
        return 1
    back = open(tmp, "rb").read()
    a = hashlib.sha256(back).hexdigest()
    b = hashlib.sha256(local).hexdigest()
    check(a == b, "app0 full-image SHA-256 matches", a[:32])
    if a != b:
        for i in range(min(len(back), len(local))):
            if back[i] != local[i]:
                print(f"        {RED}first differing byte at 0x{i:X}: "
                      f"flash 0x{back[i]:02X} vs local 0x{local[i]:02X}{OFF}")
                break

    # ---- 5. the web UI lives in the firmware --------------------------------------------------
    # The dashboard is a gzip blob in PROGMEM (src/network/web_assets.h), not a file in LittleFS.
    # That is where the controller actually serves it from, so this is the check that matters.
    gz = os.path.join(ROOT, "data", "index.html.gz")
    if os.path.exists(gz):
        packed = open(gz, "rb").read()
        check(packed in local, f"gzip SPA ({len(packed):,} B) embedded in the firmware",
              "data/index.html.gz")
    else:
        print(f"  {YEL}warn{OFF}  data/index.html.gz missing "
              f"(run: python3 tools/gen_web_assets.py)")

    # ---- 6. LittleFS: the user's own data ----------------------------------------------------
    # This partition holds the brew ledger (/history), the brew profiles (/profiles) and the
    # controller config (/config.json). A normal firmware update leaves it untouched, and the
    # board is EXPECTED to differ from the local filesystem build - so by default this is a
    # report, not a pass/fail check. With --fs the operator says the image on the board should
    # match the local build, and a mismatch is a failure.
    tmp = "/tmp/sb_verify_fs.bin"
    if not check(read_flash(port, FS_OFF, FS_SIZE, tmp),
                 f"read the {FS_SIZE:,} B littlefs partition"):
        return 1

    backfs = open(tmp, "rb").read()
    fs_img = os.path.join(BUILD, "littlefs.bin")
    if not os.path.exists(fs_img):
        fs_img = os.path.join(BUILD, "spiffs.bin")
    if os.path.exists(fs_img):
        localfs = open(fs_img, "rb").read()
        same = (hashlib.sha256(backfs).hexdigest() ==
                hashlib.sha256(localfs).hexdigest())
        if same:
            check(True, "littlefs matches the locally built image")
        elif args.fs:
            check(False, "littlefs matches the locally built image",
                  "the board holds a different filesystem - run: pio run -t uploadfs")
        else:
            print(f"  {YEL}note{OFF}  littlefs on the board differs from the local build")
            print(f"        {DIM}expected: the board keeps its own ledger, profiles and config,{OFF}")
            print(f"        {DIM}and a firmware flash does not touch them. Use --fs to require{OFF}")
            print(f"        {DIM}a match.{OFF}")

    # What the user owns, read back out of the bytes on the board. LittleFS stores directory
    # names as plain text, so a name that is present means the directory is there.
    for label, needle in (("brew ledger (/history)", b"history"),
                          ("brew profiles (/profiles)", b"profiles"),
                          ("controller config (/config.json)", b"config.json")):
        if needle in backfs:
            print(f"        {GREEN}present{OFF}  {label}")
        else:
            print(f"        {YEL}absent{OFF}   {label}")

    print()
    if failures:
        print(f"{RED}{len(failures)} check(s) FAILED:{OFF}")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"{GREEN}All flash verification checks passed.{OFF}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())