#!/usr/bin/env python3
"""
Compress data/index.html to gzip and generate src/network/web_assets.h as a C PROGMEM byte array.
Also writes data/index.html.gz.

Runs in two contexts and must regenerate exactly once in each:

  * PlatformIO ``pre:`` extra_script - the file is executed as a SConscript. There ``__file__``
    does not exist, but the build environment is exported as ``env``.
  * Standalone ``python3 tools/gen_web_assets.py``.

The previous version guarded the SCons path behind ``Import("env")`` and the standalone path
behind ``__name__ == "__main__"``. Inside SCons the ``Import`` call raised ``NameError`` because
``__file__`` is undefined there, and the blanket ``except NameError`` swallowed it - so the hook
silently did nothing. Editing ``data/index.html`` and rebuilding left
``src/network/web_assets.h`` untouched and the controller kept serving the previous UI from
flash. The header is also a hard build dependency, so a missing one breaks the compile: the
regeneration must not be skippable.

This script only writes files; it never touches the build environment, so it needs nothing from
SCons beyond the project directory. A failure inside ``main()`` is deliberately not caught: it
must abort the build instead of leaving a stale header behind.
"""

import gzip
import os
import sys


def _project_dir():
    """Absolute project directory, in both the SCons and the standalone context."""
    try:
        Import("env")  # noqa: F821 - injected by SCons when PlatformIO runs this as a script
        return str(env.subst("$PROJECT_DIR"))  # noqa: F821
    except Exception:
        # Standalone invocation: __file__ is defined here.
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    root = _project_dir()
    html_path = os.path.join(root, "data", "index.html")
    gz_path = os.path.join(root, "data", "index.html.gz")
    header_path = os.path.join(root, "src", "network", "web_assets.h")

    if not os.path.exists(html_path):
        print("Error: %s not found" % html_path, file=sys.stderr)
        sys.exit(1)

    with open(html_path, "rb") as f:
        html_bytes = f.read()

    # Deterministic gzip with mtime=0 and pinned RFC 1952 header OS byte (0xff = unknown/platform-neutral)
    gz_arr = bytearray(gzip.compress(html_bytes, compresslevel=9, mtime=0))
    if len(gz_arr) >= 10:
        gz_arr[9] = 0xFF
    gz_bytes = bytes(gz_arr)

    with open(gz_path, "wb") as f:
        f.write(gz_bytes)

    lines = [
        "// Auto-generated embedded web assets",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "const size_t index_html_gz_len = %d;" % len(gz_bytes),
        "const uint8_t index_html_gz[] PROGMEM = {",
    ]

    for i in range(0, len(gz_bytes), 16):
        chunk = gz_bytes[i:i + 16]
        hex_vals = ", ".join("0x%02x" % b for b in chunk) + ", "
        lines.append(hex_vals)

    lines.append("};")
    lines.append("")

    with open(header_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("Generated %s (%d bytes)" % (gz_path, len(gz_bytes)))
    print("Generated %s" % header_path)


# Exactly one regeneration per invocation, in every context: PlatformIO executes this file as a
# SConscript (__name__ is not "__main__") and a standalone run sets __name__ to "__main__". Both
# want the header regenerated, so it is done unconditionally here. Failures inside main() are not
# caught on purpose - they must abort the build rather than leave a stale header in flash.
main()
