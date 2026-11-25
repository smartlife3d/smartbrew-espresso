#!/usr/bin/env python3
"""
test_brewmon.py - unit tests for the parts of brewmon that are easy to get subtly wrong.

Run:  ~/.platformio/penv/bin/python tools/test_brewmon.py

No hardware required; nothing here opens a serial port.

These exist because _split_keys() shipped with a bug that silently discarded every keystroke, and
the ROM-banner matcher shipped with a gap that reprinted its warning on every line. Neither showed
up as an exception - both just quietly did the wrong thing.
"""

import argparse
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import brewmon  # noqa: E402


class _NullLog:
    """Stand-in for Logger so methods under test can print without a session file."""

    def __init__(self):
        self.notes = []

    def notice(self, text):
        self.notes.append(text)

    def local(self, text):
        self.notes.append(text)


def make_monitor():
    """Build a Monitor without running main() and without touching a serial port."""
    m = brewmon.Monitor.__new__(brewmon.Monitor)
    m.c = brewmon.C(False)
    m.log = _NullLog()
    m.ser = None
    m.connected = False
    m.stop = False
    m.history = []
    m.hidx = 0
    m._esc = b""
    m._esc_since = 0.0
    m.inbox = __import__("queue").Queue()
    m.args = argparse.Namespace(port="/dev/null", baud=115200, duration=0, send="",
                                reset=False, log=False, log_dir="/tmp", no_banner=True,
                                reconnect_timeout=5)
    return m


class TestSplitKeys(unittest.TestCase):
    """_split_keys must decode a raw tty read into discrete keys."""

    def setUp(self):
        self.m = make_monitor()

    def test_plain_ascii_is_one_run(self):
        # Text arrives as runs, so a fast typist produces one key event per read, not one per
        # character - that is what keeps multi-byte characters intact.
        self.assertEqual(self.m._split_keys(b"help"), [b"help"])

    def test_enter(self):
        self.assertEqual(self.m._split_keys(b"help\r"), [b"help", b"\r"])

    def test_run_is_bounded_by_control_bytes(self):
        self.assertEqual(self.m._split_keys(b"ab\x7fcd"), [b"ab", b"\x7f", b"cd"])

    def test_arrow_keys_in_one_read(self):
        self.assertEqual(self.m._split_keys(b"\x1b[A\x1b[B"), [b"\x1b[A", b"\x1b[B"])

    def test_arrow_after_text(self):
        self.assertEqual(self.m._split_keys(b"st\x1b[A"), [b"st", b"\x1b[A"])

    def test_escape_split_across_reads(self):
        # The classic case: ESC arrives in its own write, the rest follows later.
        self.assertEqual(self.m._split_keys(b"\x1b"), [])
        self.assertEqual(self.m._split_keys(b"[A"), [b"\x1b[A"])

    def test_partial_csi_split(self):
        self.assertEqual(self.m._split_keys(b"\x1b["), [])
        self.assertEqual(self.m._split_keys(b"C"), [b"\x1b[C"])

    def test_lone_escape_emits_after_idle(self):
        self.m._split_keys(b"\x1b")
        time.sleep(brewmon.Monitor.ESC_IDLE + 0.02)
        self.assertEqual(self.m._split_keys(b""), [b"\x1b"])

    def test_escape_followed_by_text(self):
        self.assertEqual(self.m._split_keys(b"\x1bx"), [b"\x1b", b"x"])

    def test_escape_ends_a_run(self):
        self.assertEqual(self.m._split_keys(b"ab\x1b[A"), [b"ab", b"\x1b[A"])

    def test_backspace_and_ctrl(self):
        self.assertEqual(self.m._split_keys(b"ab\x7f\x03"), [b"ab", b"\x7f", b"\x03"])

    def test_utf8_character_delivered_whole(self):
        # Splitting a multi-byte character would put mojibake on the wire to the MCU.
        self.assertEqual(self.m._split_keys("π".encode()), ["π".encode()])

    def test_successive_reads_accumulate(self):
        self.assertEqual(self.m._split_keys(b"ab"), [b"ab"])
        self.assertEqual(self.m._split_keys(b"cd"), [b"cd"])

    def test_arrow_between_words(self):
        self.assertEqual(
            self.m._split_keys(b"st\x1b[Datus"),
            [b"st", b"\x1b[D", b"atus"])

    def test_utf8_split_across_reads_is_reassembled(self):
        # The byte run must survive being cut in the middle by the read boundary: the lead byte is
        # held back, then the whole character is emitted once its continuation arrives.
        raw = "πμ".encode("utf-8")
        self.assertEqual(self.m._split_keys(raw[:1]), [])     # incomplete, held back
        self.assertEqual(self.m._split_keys(raw[1:]), [raw])  # both characters, reassembled

    def test_ascii_is_not_held_back(self):
        self.assertEqual(self.m._split_keys(b"a"), [b"a"])


class TestRomMarkers(unittest.TestCase):
    """If any line of the real ROM banner is unmatched, the one-shot hint re-arms mid-banner and
    prints the same advice once per line."""

    def test_every_line_of_a_rom_banner_is_matched(self):
        banner = [
            b"ESP-ROM:esp32s3-20210327",
            b"Build:Mar 27 2021",
            b"rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))",
            b"Saved PC:0x40041a76",
            b"waiting for download",
        ]
        for line in banner:
            self.assertTrue(any(m in line for m in brewmon.ROM_MARKERS),
                            f"unmatched ROM banner line: {line!r}")

    def test_application_output_is_not_matched(self):
        # Firmware log lines must clear the latch, otherwise the hint never re-arms.
        for line in (b"|  Pressure source: ADS1115 @0x48",
                     b"|State IDLE -> STARTING (soft button pulse)",
                     b"  ok done."):
            self.assertFalse(any(m in line for m in brewmon.ROM_MARKERS),
                             f"application line wrongly matched as ROM: {line!r}")


class TestCompletion(unittest.TestCase):
    def test_unique_prefix_completes(self):
        m = make_monitor()
        self.assertTrue(m.complete("hel").startswith("help"))

    def test_ambiguous_prefix_lists_matches(self):
        m = make_monitor()
        self.assertEqual(m.complete("s"), "s")   # too many matches -> left alone
        self.assertTrue(any("matches:" in n for n in m.log.notes))

    def test_empty_buffer_is_untouched(self):
        m = make_monitor()
        self.assertEqual(m.complete(""), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)