#!/usr/bin/env python3
"""fs_backup.py - save and restore the controller's LittleFS partition.

The LittleFS partition is not just a bag of web assets. It is where the controller keeps
everything the user owns:

    /history/*.json    the brew ledger - every recorded shot, with its measured curve
    /profiles/*.json   the brew profiles, including any the user edited or added
    /config.json       the controller configuration

`pio run -t uploadfs` writes a freshly built filesystem image over the WHOLE partition
(0x610000, 0x1E0000 bytes). It does not merge: everything that was in there is gone.
`pio run -t erase` takes the whole flash with it. A plain `pio run -t upload` touches
neither, so a normal firmware update keeps the ledger.

This tool makes the destructive paths recoverable:

    tools/fs_backup.py backup  --port /dev/cu.usbmodem101
    tools/fs_backup.py restore --port /dev/cu.usbmodem101 --file backups/littlefs-....bin

`backup` reads the partition off the board into a file. `restore` writes such a file
back, putting the ledger, the profiles and the config exactly as they were.

Run:  ~/.platformio/penv/bin/python tools/fs_backup.py backup --port /dev/cu.usbmodemXXXX
"""

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESPTOOL = os.path.expanduser(
    "~/.platformio/packages/tool-esptoolpy/esptool.py")
PY = os.path.expanduser("~/.platformio/penv/bin/python")

# The 'spiffs' partition from partitions_8MB.csv - the same entry PlatformIO names
# "littlefs" because board_build.filesystem = littlefs.
FS_OFFSET = 0x610000
FS_SIZE = 0x1E0000
FS_LABEL = "littlefs"

# esptool's default of 115200 baud takes about 3.5 minutes for the 1.9 MB partition through a
# USB-UART bridge. 921600 matches upload_speed in platformio.ini. Native USB ignores the value.
DEFAULT_BAUD = 921600

BACKUP_DIR = os.path.join(ROOT, "backups")

GREEN, RED, YEL, DIM, OFF = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"
if not sys.stdout.isatty() or os.environ.get("NO_COLOR"):
    GREEN = RED = YEL = DIM = OFF = ""


def esptool(port, baud, *args):
    # Output goes straight to the terminal so esptool's progress and errors stay visible.
    cmd = [PY, ESPTOOL, "--port", port, "--chip", "esp32s3", "--baud", str(baud), *args]
    return subprocess.run(cmd)


def resolve_port(explicit):
    if explicit:
        return explicit
    env_port = os.environ.get("PORT", "")
    if env_port:
        return env_port
    # Fall back to whatever the project scripts would pick.
    awk = "cd " + ROOT + " && ./flash.sh --list | awk '$NF==\"ESP32\"{print $2}'"
    r = subprocess.run(["bash", "-c", awk], capture_output=True, text=True)
    lines = [ln for ln in r.stdout.strip().splitlines() if ln.startswith("/")]
    return lines[0] if lines else ""


def default_out():
    os.makedirs(BACKUP_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return os.path.join(BACKUP_DIR, "littlefs-%s.bin" % stamp)


def port_busy(port):
    """True when something else already holds the serial port.

    esptool needs the port to itself: it drives DTR/RTS to reset the board into the downloader.
    A console session (tools/brewmon.py) holding the same port makes that fail with the
    unhelpful "device reports readiness to read but returned no data".
    """
    try:
        out = subprocess.run(["lsof", port], capture_output=True, text=True).stdout
    except (FileNotFoundError, OSError):
        return False
    rows = [ln for ln in out.strip().splitlines()[1:] if ln.strip()]
    if not rows:
        return False
    print(f"  {RED}the port is in use{OFF}")
    for ln in rows[:5]:
        print(f"        {DIM}{ln.strip()[:120]}{OFF}")
    print("\n  Close the console (tools/brewmon.py) or the monitor holding it, then try again.")
    return True


def do_backup(port, out, baud):
    if not port:
        print("  no port - pass --port or set PORT")
        return 2
    if port_busy(port):
        return 1

    print(f"{DIM}reading the {FS_LABEL} partition off {port} ...{OFF}\n")
    tmp = out + ".part"
    r = esptool(port, baud, "read_flash", hex(FS_OFFSET), hex(FS_SIZE), tmp)
    if r.returncode != 0 or not os.path.exists(tmp):
        print(f"\n{RED}read failed{OFF}")
        print("\n  If the board is not in the downloader: hold BOOT, tap RESET, release BOOT.")
        print("  If the link is unreliable at this speed, retry with --baud 115200.")
        return 1

    size = os.path.getsize(tmp)
    if size != FS_SIZE:
        print(f"{RED}short read{OFF}: got {size:,} B, expected {FS_SIZE:,} B")
        os.remove(tmp)
        return 1

    if os.path.exists(out):
        os.remove(out)
    os.rename(tmp, out)
    print(f"\n  {GREEN}saved{OFF}  {out}  ({size:,} B)")
    print(f"{DIM}  the ledger, the profiles and the config live in this image{OFF}")
    print(f"{DIM}  restore it with: tools/fs_backup.py restore --port {port} --file {out}{OFF}")
    return 0


def do_restore(port, path, baud):
    if not port:
        print("  no port - pass --port or set PORT")
        return 2
    if not os.path.exists(path):
        print(f"{RED}no such file{OFF}: {path}")
        return 1
    if port_busy(port):
        return 1

    size = os.path.getsize(path)
    if size != FS_SIZE:
        print(f"{RED}wrong size{OFF}: {path} is {size:,} B, expected {FS_SIZE:,} B")
        return 1

    print(f"{DIM}writing {size:,} B back to {FS_LABEL} at 0x{FS_OFFSET:X} on {port} ...{OFF}\n")
    r = esptool(port, baud, "write_flash", hex(FS_OFFSET), path)
    if r.returncode != 0:
        print(f"\n{RED}write failed{OFF}")
        print("\n  If the board is not in the downloader: hold BOOT, tap RESET, release BOOT.")
        print("  If the link is unreliable at this speed, retry with --baud 115200.")
        return 1

    print(f"\n  {GREEN}restored{OFF}  {path} -> {FS_LABEL} on {port}")
    print(f"{DIM}  reset the board; the controller will re-read /history at boot{OFF}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Back up and restore the controller's LittleFS partition "
                    "(brew ledger, profiles and config).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("backup", help="read the partition off the board into a file")
    b.add_argument("--port", default=os.environ.get("PORT", ""))
    b.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help="serial speed (default: %(default)s)")
    b.add_argument("--out", default="", help="output file (default: backups/littlefs-<time>.bin)")

    r = sub.add_parser("restore", help="write a saved partition image back to the board")
    r.add_argument("--port", default=os.environ.get("PORT", ""))
    r.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help="serial speed (default: %(default)s)")
    r.add_argument("--file", required=True, help="image produced by 'backup'")

    args = ap.parse_args()

    if args.cmd == "backup":
        out = args.out or default_out()
        return do_backup(resolve_port(args.port), out, args.baud)
    return do_restore(resolve_port(args.port), args.file, args.baud)


if __name__ == "__main__":
    raise SystemExit(main())
