#!/usr/bin/env python3
"""
brewmon.py - serial monitor for the Smart Brew MCU.

Invoked by ./monitor.sh. Everything here exists because `pio device monitor` is awkward for this
job:

  * An ESP32-S3 on native USB *disappears from the port list for ~1 s on every reset*. PlatformIO's
    monitor either exits or sits dead; this one reconnects by itself and tells you it did.
  * The firmware has a rich command console (`help`, `status`, `zc`, `phase`...). This monitor keeps
    the MCU's own echo intact, adds timestamps, and logs to a file so a 10-second transient is
    still readable afterwards.
  * If the chip is sitting in the ROM downloader, all you get is "waiting for download". This
    detects that case and tells you the one thing that fixes it.

Keyboard
  Ctrl-C          quit
  Ctrl-]          quit (pio convention)
  Ctrl-R          pulse the ESP32 reset line and capture the boot banner
  Up/Down         command history
  Tab             complete from the MCU's known command set

Only the standard library plus pyserial are used.
"""

import argparse
import atexit
import os
import select
import sys
import queue
import termios
import threading
import time
import tty

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "err pyserial is not available to this interpreter.\n"
        "    Try:  ~/.platformio/penv/bin/python -c 'import serial'\n"
    )
    raise SystemExit(4)

# ------------------------------------------------------------------------------------------------
# MCU command set, for tab-completion and the on-screen hint
# ------------------------------------------------------------------------------------------------
MCU_COMMANDS = [
    "help", "status", "zc", "heap", "led", "scale", "phase", "profiles",
    "sim", "press", "zero", "weight", "start", "stop", "tare", "next",
    "set", "manual", "duty", "btn", "disarm", "arm", "use",
]

# The ROM downloader is a state you can get stuck in; recognise it so we can explain it. The whole
# banner has to be matched, not just the obvious lines: the chip interleaves
#   ESP-ROM: / Build:Mar ... / rst:0x15 (...DOWNLOAD...) / Saved PC:... / waiting for download
# and anything unmatched between two matched lines would look like "application output" and clear
# the one-shot hint.
ROM_MARKERS = (
    b"ESP-ROM:",
    b"Build:Mar",
    b"rst:0x",
    b"Saved PC:",
    b"waiting for download",
    b"boot:0x0 (DOWNLOAD",
    b"boot:0x1 (SPI_FAST_FLASH",
    b"checksum failed",
)

def _ends_mid_utf8(chunk: bytes) -> bool:
    """True if `chunk` stops part-way through a multi-byte UTF-8 character.

    A tty read can cut a character in half. Emitting the lead byte on its own would put an invalid
    sequence on the wire to the MCU, so the splitter holds such bytes back until the rest arrives.
    """
    continuations = 0
    for i in range(len(chunk) - 1, -1, -1):
        b = chunk[i]
        if (b & 0xC0) == 0x80:                 # 10xxxxxx continuation
            continuations += 1
            if continuations > 3:
                return False                   # not UTF-8 continuation noise, give up
            continue
        if (b & 0x80) == 0:                    # plain ASCII lead: always complete
            return False
        need = 1 if (b & 0xE0) == 0xC0 else (2 if (b & 0xF0) == 0xE0 else 3)
        return continuations < need
    return continuations > 0                   # trailing continuations with no lead byte


RESET = "\033[0m"
DIM = "\033[2m"
RED = "\033[31m"
GRN = "\033[32m"
YEL = "\033[33m"
CYN = "\033[36m"
BOLD = "\033[1m"


def colour_enabled() -> bool:
    return sys.stdout.isatty() and os.environ.get("NO_COLOR", "") == ""


class C:
    """Colour codes, or empty strings when the output is not a terminal or NO_COLOR is set."""

    def __init__(self, enabled: bool):
        self.on = enabled
        self.red = RED if enabled else ""
        self.grn = GRN if enabled else ""
        self.yel = YEL if enabled else ""
        self.cyn = CYN if enabled else ""
        self.dim = DIM if enabled else ""
        self.bold = BOLD if enabled else ""
        self.off = RESET if enabled else ""


class Logger:
    """Tee: everything printed also goes to the session log file, without ANSI escapes."""

    def __init__(self, path: str | None, colours: C):
        self.c = colours
        self.fh = None
        if path:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.fh = open(path, "a", encoding="utf-8", buffering=1)
            self.write_file(
                f"--- brewmon session started {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")

    def write_file(self, text: str) -> None:
        if self.fh:
            self.fh.write(text)

    def local(self, text: str) -> None:
        """A message from the monitor itself, not from the board."""
        stamp = time.strftime("%H:%M:%S")
        sys.stdout.write(f"{self.c.dim}{stamp}{self.c.off} {self.c.cyn}monitor{self.c.off}  {text}\r\n")
        sys.stdout.flush()
        self.write_file(f"[monitor] {text}\n")

    def notice(self, text: str) -> None:
        sys.stdout.write(f"{self.c.yel}{text}{self.c.off}\r\n")
        sys.stdout.flush()
        self.write_file(f"[monitor] {text}\n")

    def line_from_device(self, raw: bytes) -> None:
        """One line from the board: timestamped and coloured, plain in the log."""
        text = raw.rstrip(b"\r\n").decode("utf-8", "replace")
        stamp = time.strftime("%H:%M:%S")
        colour = ""
        if text.startswith("!"):
            colour = self.c.red
        elif text.startswith("*"):
            colour = self.c.yel
        elif "SHOT END" in text or "WEIGHT CUTOFF" in text:
            colour = self.c.grn + self.c.bold
        sys.stdout.write(f"{self.c.dim}{stamp}{self.c.off} {colour}{text}{self.c.off}\r\n")
        sys.stdout.flush()
        self.write_file(text + "\n")

    def raw_line(self, text: str) -> None:
        """A non-timestamped line (used for the literal ROM banner)."""
        sys.stdout.write(text + "\r\n")
        sys.stdout.flush()
        self.write_file(text + "\n")


class Monitor:
    def __init__(self, args):
        self.args = args
        self.c = C(colour_enabled())
        logdir = os.path.join(os.path.dirname(os.path.abspath(args.log_dir)), "")
        stamp = time.strftime("%Y%m%d-%H%M%S")
        logpath = os.path.join(args.log_dir, f"monitor-{stamp}.log") if args.log_dir else None
        self.log = Logger(logpath, self.c)
        self.logpath = logpath

        self.ser = None
        self.rx = b""
        self.had_rom_banner = False
        self.connected = False
        self.stop = False
        self.history: list[str] = []
        self.hidx = 0
        self.reconnects = 0
        # Escape-sequence reassembly state: an arrow key can arrive split across two reads.
        self._esc = b""
        self._esc_since = 0.0
        self._last_tip = -1
        self.inbox: queue.Queue = queue.Queue()

        # A board that vanished for good should not spin the CPU forever.
        self.reconnect_deadline = time.monotonic() + args.reconnect_timeout

    # ---------------------------------------------------------------------------------------------
    # Port handling
    # ---------------------------------------------------------------------------------------------
    def open_port(self) -> bool:
        try:
            self.ser = serial.Serial(
                self.args.port,
                self.args.baud,
                timeout=0.05,
                write_timeout=1.0,
            )
        except Exception as exc:  # noqa: BLE001
            self.ser = None
            self.log.notice(f"cannot open {self.args.port}: {exc}")
            return False

        # pyserial asserts DTR and RTS as soon as the port opens. On the ESP32 DevKit auto-reset
        # circuit both lines are inverted: RTS -> GPIO0 (BOOT) and DTR -> EN. Leaving RTS asserted
        # therefore holds GPIO0 LOW, and every reset lands in the ROM downloader instead of the
        # application - which looks exactly like a dead board. De-assert both before anything else.
        try:
            self.ser.rts = False      # GPIO0 high -> normal boot
            self.ser.dtr = False      # EN high      -> not held in reset
        except Exception as exc:  # noqa: BLE001
            self.log.notice(f"could not set modem control lines: {exc}")

        self.connected = True
        self.had_rom_banner = False
        self.reconnect_deadline = time.monotonic() + self.args.reconnect_timeout
        self.log.notice(
            f"{self.c.grn}connected{self.c.off} to {self.args.port} @ {self.args.baud} baud"
            f"{self.c.dim} (GPIO0 de-asserted){self.c.off}"
        )
        if self.args.send:
            self.write_line(self.args.send)
        if self.args.reset:
            time.sleep(0.25)
            self.pulse_reset()
        return True

    def close_port(self) -> None:
        if self.ser:
            try:
                self.ser.close()
            except Exception:  # noqa: BLE001
                pass
        self.ser = None
        self.connected = False

    def pulse_reset(self) -> None:
        """Assert the ESP32's EN line briefly to reset it, leaving BOOT/IO0 de-asserted.

        The DevKit auto-reset circuit inverts both lines: RTS -> GPIO0 (BOOT), DTR -> EN.
        open_port() has already de-asserted RTS, so GPIO0 is high and the chip will boot the
        application. We only ever pulse DTR (EN).
        """
        if not self.ser:
            return
        try:
            self.ser.dtr = True   # EN low  -> reset
            time.sleep(0.15)
            self.ser.dtr = False  # EN high -> boot from flash with GPIO0 high
            self.log.notice("reset pulsed (GPIO0 held high)")
        except Exception as exc:  # noqa: BLE001
            self.log.notice(f"reset failed: {exc}")

    def write_line(self, text: str) -> None:
        if not self.ser:
            return
        try:
            self.ser.write((text + "\r\n").encode())
            self.ser.flush()
        except Exception as exc:  # noqa: BLE001
            self.log.notice(f"write failed: {exc}")
        self.log.write_file(f"> {text}\n")

    # ---------------------------------------------------------------------------------------------
    # Incoming
    # ---------------------------------------------------------------------------------------------
    def pump_rx(self) -> None:
        if not self.ser:
            return
        try:
            waiting = select.select([self.ser], [], [], 0)[0]
            if not waiting:
                return
            chunk = self.ser.read(4096)
        except Exception as exc:  # noqa: BLE001
            self.log.notice(f"read failed ({exc}) - will reconnect")
            self.close_port()
            return
        if not chunk:
            return

        self.rx += chunk
        while b"\n" in self.rx:
            line, self.rx = self.rx.split(b"\n", 1)

            if any(m in line for m in ROM_MARKERS):
                self.log.raw_line(self.c.yel + line.decode("utf-8", "replace") + self.c.off)
                # The ROM prints its banner over several lines. Explain it ONCE per reset, not once
                # per line, otherwise a stuck board produces the same advice four times.
                if not self.had_rom_banner:
                    self.had_rom_banner = True
                    self.log.notice("")
                    self.log.notice(
                        "  the chip is in the ROM downloader - no application is running.\n"
                        "  tap RESET on the board WITHOUT holding BOOT to boot normally,\n"
                        "  or run: ./flash.sh --erase   (it can drive the chip from this state)."
                    )
                    self.log.notice("")
                continue

            # Real application output means we are out of the downloader; re-arm the hint.
            self.had_rom_banner = False
            self.log.line_from_device(line)

        # Partial line left over (no newline yet) - show it raw so nothing looks truncated.
        if self.rx:
            self.log.raw_line(self.c.dim + self.rx.decode("utf-8", "replace").rstrip() + self.c.off)
            self.rx = b""

    # ---------------------------------------------------------------------------------------------
    # User input
    #
    # Design note: stdin is read by a dedicated blocking thread, not by select() in the main loop.
    # select() on a tty file descriptor was observed returning "not ready" while bytes were already
    # queued on some pty setups, which silently swallowed every keystroke. A blocking os.read() in a
    # thread is immune to that, and it also means the serial port can be pumped at its own rate
    # instead of sharing one loop with the keyboard.
    #
    # The thread is a daemon: when the main loop sets self.stop and the process exits, the thread is
    # abandoned mid-read, which is harmless.
    # ---------------------------------------------------------------------------------------------
    def _stdin_reader(self) -> None:
        fd = sys.stdin.fileno()
        while not self.stop:
            try:
                data = os.read(fd, 1024)
            except (OSError, ValueError):
                return          # stdin closed, or the fd was swapped out by termios restore
            if not data:
                time.sleep(0.02)
                continue
            self.inbox.put(data)

    def handle_input(self) -> None:
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            # setraw leaves VMIN=1/VTIME=0 and disables OPOST (which causes staircase output).
            # Force OPOST and ONLCR back on so newlines automatically reset the cursor to column 0.
            attrs = termios.tcgetattr(fd)
            attrs[1] |= (termios.OPOST | termios.ONLCR)
            attrs[6][termios.VMIN] = 1
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        except termios.error:
            self.log.notice("could not put the terminal into raw mode - keys will not be captured")

        reader = threading.Thread(target=self._stdin_reader, daemon=True)
        reader.start()

        buf = ""
        try:
            while not self.stop:
                self.pump_rx()
                if not self.connected:
                    self._try_reconnect()

                try:
                    data = self.inbox.get_nowait()
                except queue.Empty:
                    time.sleep(0.005)
                    continue

                for key in self._split_keys(data):
                    buf = self._handle_key(key, buf)
        finally:
            self.stop = True
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
            if buf:
                self.write_prompt(buf)

    def _handle_key(self, key: bytes, buf: str) -> str:
        """Apply one key to the input line. Returns the new buffer contents."""
        code = key[0]

        if code == 0x03:                              # Ctrl-C
            self.stop = True
        elif code == 0x1d:                            # Ctrl-]
            self.stop = True
        elif code == 0x12:                            # Ctrl-R
            self.write_prompt(buf)
            self.pulse_reset()
            buf = ""
        elif key in (b"\r", b"\n"):
            self.write_prompt(buf)
            if buf.strip():
                self.history.append(buf)
                self.hidx = len(self.history)
                self.write_line(buf.strip())
            buf = ""
        elif code in (0x7f, 0x08):                    # backspace / delete
            if buf:
                buf = buf[:-1]
                self._redraw(buf)
        elif code == 0x09:                            # Tab
            buf = self.complete(buf)
            self._redraw(buf)
        elif key == b"\x1b[A":                        # Up
            buf = self.recall_history(-1)
        elif key == b"\x1b[B":                        # Down
            buf = self.recall_history(1)
        elif code == 0x1b:                            # bare Escape clears the line
            self._redraw("")
            buf = ""
        elif code >= 0x20 and code != 0x7f:
            # An ordinary text run (possibly several characters, possibly multi-byte UTF-8).
            buf += key.decode("utf-8", "replace")
            self._redraw(buf)
        # Any other control key is ignored on purpose.
        return buf

    # ---------------------------------------------------------------------------------------------
    # Key decoding
    #
    # `for ch in data` yields ints in Python 3, not one-byte objects, so every branch needs whole
    # bytes keys. _split_keys() also reassembles arrow keys that straddle a read boundary - the
    # classic "ESC arrives in its own write" problem.
    # ---------------------------------------------------------------------------------------------
    ESC_IDLE = 0.05

    def _split_keys(self, data: bytes) -> list:
        """Turn a raw tty read into a list of discrete keys.

        Items are one of:
          * a 3-byte CSI sequence (arrow keys, Home)
          * a single control byte (Enter, backspace, Ctrl-C, ...)
          * a bare ESC
          * a RUN of ordinary bytes

        Emitting text as runs rather than one byte per key matters: a multi-byte UTF-8 character
        would otherwise be split between two keys and put mojibake on the wire to the MCU. UTF-8
        continuation bytes are all >= 0x80, so a run scan that stops only at ESC/control bytes keeps
        every character intact.

        `self._esc` is the pending-input buffer (nothing to do with escapes in particular):
        everything arriving is appended and decoded from the front.
        """
        keys: list = []
        if not self._esc and data[:1] == b"\x1b":
            self._esc_since = time.monotonic()
        self._esc += data

        def is_control(b: int) -> bool:
            return b == 0x1b or b < 0x20 or b == 0x7f

        while self._esc:
            b0 = self._esc[0]

            if b0 == 0x1b:
                # ESC: wait briefly for the rest of a possible sequence before treating it as a
                # bare Escape key press.
                if len(self._esc) == 1:
                    if time.monotonic() - self._esc_since > self.ESC_IDLE:
                        keys.append(b"\x1b")
                        self._esc = b""
                        self._esc_since = 0.0
                    break

                if self._esc[1:2] == b"[":
                    if len(self._esc) < 3:
                        break                    # partial CSI, wait for the final byte
                    if self._esc[2:3] in (b"A", b"B", b"C", b"D", b"H"):
                        keys.append(self._esc[:3])   # up / down / right / left / home
                        self._esc = self._esc[3:]
                        self._esc_since = 0.0
                        continue

                # ESC followed by something that is not a CSI: emit the ESC on its own and let the
                # remaining bytes decode on the next iteration.
                keys.append(b"\x1b")
                self._esc = self._esc[1:]
                self._esc_since = 0.0
                continue

            if is_control(b0):
                keys.append(self._esc[:1])
                self._esc = self._esc[1:]
                continue

            # Ordinary text: take everything up to the next control byte or ESC, then back off
            # any partial UTF-8 character at the end so it can be completed by the next read.
            i = 1
            while i < len(self._esc) and not is_control(self._esc[i]):
                i += 1
            while i > 0 and _ends_mid_utf8(self._esc[:i]):
                i -= 1
            if i == 0:
                break                         # nothing complete yet; wait for more bytes
            keys.append(self._esc[:i])
            self._esc = self._esc[i:]

        return keys

    def _redraw(self, buf: str) -> None:
        sys.stdout.write("\r\033[K> " + buf)
        sys.stdout.flush()

    def recall_history(self, direction: int) -> str:
        """Move through the history; returns what the input buffer should now hold."""
        if not self.history:
            return ""
        self.hidx = max(0, min(len(self.history), self.hidx + direction))
        text = self.history[self.hidx] if self.hidx < len(self.history) else ""
        self._redraw(text)
        return text

    def complete(self, buf: str) -> str:
        if not buf:
            return buf
        matches = [c for c in MCU_COMMANDS if c.startswith(buf)]
        if len(matches) == 1:
            sys.stdout.write(matches[0][len(buf):] + " ")
            sys.stdout.flush()
            return matches[0] + " "
        if len(matches) > 1:
            self.log.notice("matches: " + "  ".join(matches))
        return buf

    def write_prompt(self, buf: str) -> None:
        sys.stdout.write("\r\033[K")
        sys.stdout.flush()
        self.log.write_file(f"> {buf.strip()}\n")
        sys.stdout.write(self.c.cyn + "mcu> " + self.c.off + buf + "\r\n")
        sys.stdout.flush()

    # ---------------------------------------------------------------------------------------------
    def run(self) -> int:
        if not self.args.no_banner:
            self.banner()

        if not self.open_port():
            self.log.notice("starting without a connection; will keep retrying")

        # Exactly ONE owner reads the serial port. A second reader would race the first and both
        # would silently drop and duplicate bytes, so the tty and piped paths each read alone.
        try:
            if sys.stdin.isatty():
                self.handle_input()        # owns the port: pumps rx from its key loop
            else:
                self._rx_loop_blocking()   # owns the port: plain streaming
        except KeyboardInterrupt:
            pass
        finally:
            self.stop = True
            self.close_port()

        self.log.notice("session ended")
        if self.logpath:
            self.log.notice(f"log written to {self.logpath}")
        return 0

    def _try_reconnect(self) -> None:
        """Called by whichever loop owns the port, when it is not connected."""
        if self.connected:
            return
        if self.open_port():
            return
        remaining = self.reconnect_deadline - time.monotonic()
        if remaining <= 0:
            self.log.notice(
                f"gave up reconnecting after {self.args.reconnect_timeout}s "
                f"({self.reconnects} attempt(s)). Ctrl-C to exit."
            )
            self.stop = True
            return
        tip = int(remaining)
        if getattr(self, "_last_tip", -1) != tip:
            self._last_tip = tip
            self.log.notice(f"waiting for {self.args.port} to come back ({tip}s left)")
        time.sleep(0.5)

    def _rx_loop_blocking(self) -> None:
        """Non-interactive streaming (piped output, CI, --duration)."""
        end = time.monotonic() + self.args.duration if self.args.duration else 0
        while not self.stop:
            if not self.connected:
                self._try_reconnect()
                continue
            before = self.reconnects
            self.pump_rx()
            if not self.connected:
                self.reconnects = before + 1
                self.log.notice("port went away - reconnecting (the board is probably resetting)")
            if end and time.monotonic() > end:
                return
            time.sleep(0.005)

    def banner(self) -> None:
        c = self.c
        print(f"{c.bold}Smart Brew MCU monitor{c.off}")
        print(f"  port     {self.args.port}")
        print(f"  baud     {self.args.baud}")
        if self.logpath:
            print(f"  log      {self.logpath}")
        print(f"  keys     ^C or ^] quit   ^R reset board   up/down history   Tab complete")
        print(f"  commands type {c.cyn}help{c.off} on the board for the full list")
        print()


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--duration", type=int, default=0,
                    help="stop after N seconds (0 = run until interrupted)")
    ap.add_argument("--send", default="", help="send this command once after connecting")
    ap.add_argument("--reset", action="store_true",
                    help="pulse the ESP32 reset line after connecting, then capture the banner")
    ap.add_argument("--log", action="store_true", help="also write the session to logs/")
    ap.add_argument("--log-dir", default="logs")
    ap.add_argument("--no-banner", action="store_true")
    ap.add_argument("--reconnect-timeout", type=int, default=60)
    args = ap.parse_args()

    # `pio device monitor` uses 115200 for this firmware; warn loudly rather than silently
    # producing a screen full of mojibake.
    if args.baud != 115200:
        print(f"{YEL}note:{RESET} the MCU console runs at 115200 baud; "
              f"{args.baud} will produce garbage")

    return Monitor(args).run()


if __name__ == "__main__":
    atexit.register(lambda: None)
    raise SystemExit(main())