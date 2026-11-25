#!/usr/bin/env bash
# =================================================================================================
#  monitor.sh - open the Smart Brew MCU bench console on the right serial port
#
#  Wraps tools/brewmon.py, which exists because `pio device monitor` drops the connection whenever
#  an ESP32-S3 on native USB resets - the port vanishes for about a second and PlatformIO gives up.
#  brewmon.py reconnects by itself, timestamps every line, understands the ROM downloader state,
#  and can log the session to a file.
#
#  Usage
#    ./monitor.sh                     auto-detect the port, monitor at 115200
#    ./monitor.sh --list              show every port with ESP32 markers, then exit
#    ./monitor.sh -p /dev/cu.usbmodem101
#    ./monitor.sh -p 5                pick by index from the --list numbering
#    ./monitor.sh --reset             reset the board on connect and capture the boot banner
#    ./monitor.sh --log               also write the session to logs/monitor-*.log
#    ./monitor.sh --send status       send one command after connecting, then monitor
#    ./monitor.sh --duration 30       stop automatically after 30 seconds
#    ./monitor.sh -b 9600             use a non-standard baud (warns)
#
#  Keys while monitoring
#    Ctrl-C / Ctrl-]     quit
#    Ctrl-R              reset the board and capture the boot banner
#    Up / Down           command history
#    Tab                 completion over the MCU command set
#
#  Exit codes
#    0 ok   1 usage error   2 no usable port   4 pyserial missing
# =================================================================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

# shellcheck source=scripts/portlib.sh
. "$SCRIPT_DIR/scripts/portlib.sh"

MON_BAUD=115200
DO_RESET=0
DO_LOG=0
DO_LIST=0
SEND_CMD=""
DURATION=0
NO_BANNER=0
RECONNECT_TIMEOUT=60
SB_WANT_PORT=""
SB_WANT_INDEX=""

usage() {
    sed -n '3,27p' "${BASH_SOURCE[0]}" | sed 's/^#//; s/^ //'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)    SB_WANT_PORT="${2:-}"; shift 2 ;;
        -p=*|--port=*) SB_WANT_PORT="${1#*=}"; shift ;;
        -i|--index)   SB_WANT_INDEX="${2:-}"; shift 2 ;;
        -b|--baud)    MON_BAUD="${2:-}"; shift 2 ;;
        -l|--list)    DO_LIST=1; shift ;;
        --reset)      DO_RESET=1; shift ;;
        --log)        DO_LOG=1; shift ;;
        --send)       SEND_CMD="${2:-}"; shift 2 ;;
        --duration)   DURATION="${2:-}"; shift 2 ;;
        --timeout)    RECONNECT_TIMEOUT="${2:-}"; shift 2 ;;
        --no-banner)  NO_BANNER=1; shift ;;
        -h|--help)    usage 0 ;;
        *) die "unknown option '$1'  (try --help)" 1 ;;
    esac
done

# -------------------------------------------------------------------------------------------------
# Pick an interpreter that actually has pyserial. The helper lives in scripts/portlib.sh so the
# flash scripts use exactly the same lookup.
# -------------------------------------------------------------------------------------------------

# -------------------------------------------------------------------------------------------------
# Preflight. Only `pio` is needed up front; the python lookup happens just before launch so that
# --list still works on a machine without pyserial installed.
# -------------------------------------------------------------------------------------------------
command -v pio >/dev/null 2>&1 || die "pio not found on PATH (pip install platformio)" 1

# -------------------------------------------------------------------------------------------------
# Port selection (shared with flash.sh)
# -------------------------------------------------------------------------------------------------
ALL_PORTS="$(sb_ports_enumerate)"

if (( DO_LIST )); then
    if [[ -z "$ALL_PORTS" ]]; then
        warn "pio device list returned nothing - is the board plugged in?"
    else
        sb_ports_print_table
    fi
    exit 0
fi

[[ -z "$ALL_PORTS" ]] && die "pio device list returned nothing. Is the board plugged in?" 2

SB_PROG="monitor.sh"
sb_ports_select || exit 2
sb_ports_banner "$MON_BAUD"

if (( PORT_SCORE < 65 )) && [[ "${FORCE:-0}" != "1" ]]; then
    warn "USB ID $HW_ID is not a known ESP32/USB-serial chip (score $PORT_SCORE)."
    warn "If the output is unreadable, press and hold BOOT, tap RESET, release BOOT, then re-run."
    say ""
fi

# -------------------------------------------------------------------------------------------------
# Launch
# -------------------------------------------------------------------------------------------------
# pyserial is only needed now, once we know we are actually going to open a port.
PYTHON="$(sb_find_python)"
if [[ -z "$PYTHON" ]]; then
    die "no python3 with pyserial found. Install it with:  pip install pyserial" 4
fi

ARGS=( "$SCRIPT_DIR/tools/brewmon.py"
       --port "$PORT"
       --baud "$MON_BAUD"
       --reconnect-timeout "$RECONNECT_TIMEOUT" )

(( DO_RESET ))    && ARGS+=( --reset )
(( DO_LOG ))      && ARGS+=( --log )
(( NO_BANNER ))   && ARGS+=( --no-banner )
(( DURATION > 0 )) && ARGS+=( --duration "$DURATION" )
[[ -n "$SEND_CMD" ]] && ARGS+=( --send "$SEND_CMD" )

exec "$PYTHON" "${ARGS[@]}"