#!/usr/bin/env bash
# =================================================================================================
#  flash.sh - build, flash and monitor the Smart Brew HIL simulator
#
#  The simulator is the second board on the bench: it emulates the AC dimmer, the pressure
#  transducer, the BLE scale and the brew button, so the controller can be exercised against a
#  realistic machine without any hardware. It is flashed and monitored exactly like the controller,
#  on the same port, with the same console.
#
#  Why the port is asked for: the controller, the simulator and the display are all ESP32-S3
#  boards that look identical to `pio device list`. Guessing the wrong one means flashing the
#  controller with the simulator firmware, so the first choice is never made silently. After a
#  successful flash the port is saved to simulator-esp32s3/.last_port (its own file, separate from
#  the controller's) and reused while it is plugged in. Move the USB cables and it may point at
#  another board: use -p, or --forget-port to be asked again.
#
#  Usage
#    ./flash.sh                          use the remembered port (or pick one), build, flash, monitor
#    ./flash.sh --list                   show every port with ESP32 markers, then exit
#    ./flash.sh -p /dev/cu.usbmodem101    flash an explicit port (no question asked)
#    ./flash.sh -p 2                     flash by index from the --list numbering
#    ./flash.sh --forget-port            forget the remembered port and ask again
#    ./flash.sh --build-only             build without flashing
#    ./flash.sh --upload-only            flash without rebuilding
#    ./flash.sh --erase                  wipe flash before writing
#    ./flash.sh --no-monitor             flash without opening the console
#    ./flash.sh --dry-run                print the commands, run nothing
#
#  The console is the repository's own (tools/brewmon.py via ../monitor.sh), which reconnects by
#  itself when the board resets on native USB - something `pio device monitor` cannot do.
#
#  Exit codes
#    0 success   1 usage error   2 no usable port found   3 upload failed
# =================================================================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR" || exit 1

# shellcheck source=../scripts/portlib.sh
. "$ROOT/scripts/portlib.sh"

# -------------------------------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------------------------------
DO_BUILD=1
DO_UPLOAD=1
DO_ERASE=0
DO_MONITOR=1
DRY_RUN=0
LIST_ONLY=0
PIO_ENV="esp32s3"
SB_WANT_PORT=""
SB_WANT_INDEX=""
FORGET_PORT=0
# The port of the last successful flash of THIS board. Per machine, so it is git-ignored.
LAST_PORT_FILE="$SCRIPT_DIR/.last_port"

usage() {
    sed -n '3,31p' "${BASH_SOURCE[0]}" | sed 's/^#//; s/^ //'
    exit "${1:-0}"
}

# -------------------------------------------------------------------------------------------------
# Argument parsing
# -------------------------------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)    SB_WANT_PORT="${2:-}"; shift 2 ;;
        -p=*|--port=*) SB_WANT_PORT="${1#*=}"; shift ;;
        -i|--index)   SB_WANT_INDEX="${2:-}"; shift 2 ;;
        -l|--list)    LIST_ONLY=1; shift ;;
        --build-only) DO_UPLOAD=0; shift ;;
        --upload-only) DO_BUILD=0; shift ;;
        --erase)      DO_ERASE=1; shift ;;
        --no-monitor) DO_MONITOR=0; shift ;;
        -m|--monitor) DO_MONITOR=1; shift ;;
        --dry-run)    DRY_RUN=1; shift ;;
        --forget-port) FORGET_PORT=1; shift ;;
        -h|--help)    usage 0 ;;
        *) die "unknown option '$1'  (try --help)" 1 ;;
    esac
done

# -------------------------------------------------------------------------------------------------
# Preflight
# -------------------------------------------------------------------------------------------------
command -v pio >/dev/null 2>&1 || die "pio not found on PATH (pip install platformio)" 1

# -------------------------------------------------------------------------------------------------
# Enumerate once - both --list and the picker need it
# -------------------------------------------------------------------------------------------------
ALL_PORTS="$(sb_ports_enumerate)"

if (( LIST_ONLY )); then
    if [[ -z "$ALL_PORTS" ]]; then
        warn "pio device list returned nothing - is the board plugged in?"
    else
        sb_ports_print_table
    fi
    exit 0
fi

[[ -z "$ALL_PORTS" ]] && die "pio device list returned nothing. Is the board plugged in?" 2

SB_PROG="flash.sh"
# The simulator, the controller and the display are indistinguishable from the outside, so the
# operator says which board this is - once. The answer is remembered for the next run.
SB_ALWAYS_ASK=1
if (( FORGET_PORT )); then
    rm -f "$LAST_PORT_FILE"
    info "Forgot the remembered port."
fi
SB_LAST_PORT=""
[[ -r "$LAST_PORT_FILE" ]] && SB_LAST_PORT="$(head -n 1 "$LAST_PORT_FILE")"
sb_ports_select || exit 2
if (( PORT_REMEMBERED )); then
    sb_ports_banner "" "Remembered port"
    printf '%s    (from the last simulator flash; -p <port> picks another, --forget-port asks again)%s\n\n' "$D" "$N"
else
    sb_ports_banner
fi
sb_ports_warn_unknown "flash.sh"

# -------------------------------------------------------------------------------------------------
# Run
# -------------------------------------------------------------------------------------------------
run() {
    if (( DRY_RUN )); then
        printf '%s[dry-run]%s %s\n' "$Y" "$N" "$*"
        return 0
    fi
    "$@"
}

FAILED=0

# --- firmware -------------------------------------------------------------------------------
if (( DO_BUILD )); then
    info "Building the simulator firmware..."
    run pio run -e "$PIO_ENV" || FAILED=1
    if (( FAILED )); then
        say ""
        warn "the build failed - nothing was flashed."
        exit 3
    fi
fi

if (( DO_UPLOAD )); then
    if (( DO_ERASE )); then
        info "Erasing flash, then uploading firmware..."
        run pio run -e "$PIO_ENV" -t erase -t upload --upload-port "$PORT" || FAILED=1
    else
        info "Uploading firmware to $PORT..."
        run pio run -e "$PIO_ENV" -t upload --upload-port "$PORT" || FAILED=1
    fi
    if (( FAILED )); then
        say ""
        warn "firmware upload failed."
        sb_recovery_hints "flash.sh"
        exit 3
    fi
fi

# --- remember the port ------------------------------------------------------------------------
# Only a port that was actually flashed is worth remembering.
if (( DO_UPLOAD )) && (( ! FAILED )) && (( ! DRY_RUN )); then
    printf '%s\n' "$PORT" > "$LAST_PORT_FILE" 2>/dev/null || warn "could not save $LAST_PORT_FILE"
fi

# --- monitor --------------------------------------------------------------------------------
if (( DO_MONITOR )) && (( ! DRY_RUN )); then
    # Hand over to the same console the controller uses, so there is one implementation of it.
    info "Handing over to the console on $PORT (Ctrl-C to exit)..."
    say ""
    exec "$ROOT/monitor.sh" -p "$PORT"
fi

if (( FAILED )); then
    say ""
    warn "one or more steps failed."
    sb_recovery_hints "flash.sh"
    exit 3
fi

say ""
ok "done."
exit 0
