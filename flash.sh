#!/usr/bin/env bash
# =================================================================================================
#  flash.sh - build, flash and monitor the Smart Brew MCU with serial-port selection
#
#  Why this exists: PlatformIO picks the first /dev/cu.* it finds, which on a developer machine is
#  usually a Bluetooth pseudo-terminal. This script enumerates the ports, identifies the Espressif
#  ones by USB VID:PID, lets you choose explicitly, and then drives the right build/upload targets.
#  Port discovery lives in scripts/portlib.sh, shared with monitor.sh.
#
#  Usage
#    ./flash.sh                          auto-detect; prompt only if genuinely ambiguous
#    ./flash.sh --list                   show every port with ESP32 markers, then exit
#    ./flash.sh -p /dev/cu.usbmodem101    flash an explicit port
#    ./flash.sh -p 2                     flash by index from the --list numbering
#    ./flash.sh --forget-port            forget the remembered port, then detect as usual
#    (after a successful flash the port is saved to .last_port and reused while it is plugged in;
#     -p or -i overrides it, and the new choice is remembered in its place)
#    ./flash.sh --app-only               push firmware only (this is the default)
#    ./flash.sh --fs                     ALSO push a fresh LittleFS image  (destroys data)
#    ./flash.sh --fs-only                push LittleFS only                (destroys data)
#    ./flash.sh --erase                  wipe flash before writing           (destroys data)
#    ./flash.sh --no-backup              do not take a LittleFS backup before a destructive step
#    ./flash.sh --yes                    do not ask before a destructive step
#    ./flash.sh --no-reset               flash without toggling DTR/RTS (for ROM bootloader mode)
#    ./flash.sh --no-monitor             flash without opening the console
#    ./flash.sh -m                       open the monitor afterwards (same as ./monitor.sh)
#    (the console opens automatically after a successful flash, on the same port)
#    (upload speed is set by 'upload_speed' in platformio.ini, currently 921600)
#    ./flash.sh --dry-run                print the commands, run nothing
#
#  DATA SAFETY
#    The LittleFS partition holds the brew ledger (/history), the brew profiles (/profiles) and
#    the controller config (/config.json). It is NOT just web assets: the web UI is compiled
#    into the firmware as a gzip blob (src/network/web_assets.h), so pushing the filesystem
#    image gains the UI nothing and costs the user their data.
#
#      pio run -t uploadfs   writes a freshly built image over the WHOLE partition and does
#                            not merge - every recorded shot, every profile and the config
#                            are gone. That is what --fs and --fs-only do.
#      pio run -t erase      takes the entire flash, LittleFS included.
#      pio run -t upload     touches the firmware only. The ledger survives it.
#
#    A normal restart loses nothing: the controller re-reads /history at every boot.
#
#    Before any destructive step this script saves the partition to backups/ (override with
#    --no-backup) and puts the backup back with:
#        tools/fs_backup.py restore --port <port> --file backups/littlefs-<stamp>.bin
#
#  Exit codes
#    0 success   1 usage error   2 no usable port found   3 upload failed
# =================================================================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

# shellcheck source=scripts/portlib.sh
. "$SCRIPT_DIR/scripts/portlib.sh"

# -------------------------------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------------------------------
DO_APP=1
DO_FS=0
DO_ERASE=0
DO_BACKUP=1
ASSUME_YES=0
NO_RESET=0
OPEN_MONITOR=1
UPLOAD_SPEED="921600"
DRY_RUN=0
LIST_ONLY=0
SB_WANT_PORT=""
SB_WANT_INDEX=""
FORGET_PORT=0
# The port of the last successful flash. Per machine, so it is git-ignored.
LAST_PORT_FILE="$SCRIPT_DIR/.last_port"

usage() {
    cat <<'EOF'
flash.sh - build, flash and monitor the Smart Brew controller

  ./flash.sh                          auto-detect; prompt only if genuinely ambiguous
  ./flash.sh --list                   show every port with ESP32 markers, then exit
  ./flash.sh -p /dev/cu.usbmodem101    flash an explicit port
  ./flash.sh -p 2                     flash by index from the --list numbering
  ./flash.sh --forget-port            forget the remembered port, then detect as usual
  ./flash.sh --app-only               push firmware only (this is the default)
  ./flash.sh --fs                     ALSO push a fresh LittleFS image  (destroys data)
  ./flash.sh --fs-only                push LittleFS only                (destroys data)
  ./flash.sh --erase                  wipe flash before writing           (destroys data)
  ./flash.sh --no-backup              skip the LittleFS backup taken before a destructive step
  ./flash.sh --yes                    do not ask before a destructive step
  ./flash.sh --no-reset               flash without toggling DTR/RTS (for ROM bootloader mode)
  ./flash.sh --no-monitor             flash without opening the console
  ./flash.sh -m                       open the monitor afterwards (same as ./monitor.sh)
  ./flash.sh --dry-run                print the commands, run nothing
  ./flash.sh -h|--help                this text

  DATA SAFETY
    The LittleFS partition holds the brew ledger (/history), the brew profiles
    (/profiles) and the controller config (/config.json). Pushing a filesystem image
    overwrites the WHOLE partition - it does not merge - so every recorded shot is
    lost. The web UI needs none of this: it is compiled into the firmware as a gzip
    blob. A plain firmware flash (the default) and a plain restart both keep the
    ledger. A backup of the partition is taken automatically before a destructive step.

  REMEMBERED PORT
    After a successful flash the port is saved to .last_port. The next run uses it
    without asking as long as it is plugged in; otherwise it detects as usual.
    -p / -i pick another port, which is then remembered instead.

  Exit codes: 0 success  1 usage error  2 no usable port found  3 upload failed
EOF
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
        -b|--baud)    UPLOAD_SPEED="${2:-}"; shift 2 ;;   # see the note below: advisory only
        -m|--monitor) OPEN_MONITOR=1; shift ;;
        --no-monitor) OPEN_MONITOR=0; shift ;;
        -l|--list)    LIST_ONLY=1; shift ;;
        --app-only)   DO_APP=1; DO_FS=0; shift ;;
        --fs)         DO_FS=1; shift ;;
        --fs-only)    DO_APP=0; DO_FS=1; shift ;;
        --erase)      DO_ERASE=1; shift ;;
        --no-backup)  DO_BACKUP=0; shift ;;
        -y|--yes)     ASSUME_YES=1; shift ;;
        --no-reset)   NO_RESET=1; shift ;;
        --dry-run)    DRY_RUN=1; shift ;;
        --forget-port) FORGET_PORT=1; shift ;;
        -h|--help)    usage 0 ;;
        *) die "unknown option '$1'  (try --help)" 1 ;;
    esac
done

# -------------------------------------------------------------------------------------------------
# Preflight
# -------------------------------------------------------------------------------------------------
command -v pio  >/dev/null 2>&1 || die "pio not found on PATH (pip install platformio)" 1
command -v awk  >/dev/null 2>&1 || die "awk not found" 1

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
if (( FORGET_PORT )); then
    rm -f "$LAST_PORT_FILE"
    info "Forgot the remembered port."
fi
SB_LAST_PORT=""
[[ -r "$LAST_PORT_FILE" ]] && SB_LAST_PORT="$(head -n 1 "$LAST_PORT_FILE")"
sb_ports_select || exit 2
if (( PORT_REMEMBERED )); then
    sb_ports_banner "$UPLOAD_SPEED" "Remembered port"
    printf '%s    (from the last flash; -p <port> picks another, --forget-port clears it)%s\n\n' "$D" "$N"
else
    sb_ports_banner "$UPLOAD_SPEED"
fi
sb_ports_warn_unknown "flash.sh"

# A console session (tools/brewmon.py) or any other monitor holds the serial port open, and esptool
# then fails with "device reports readiness to read but returned no data (multiple access on
# port)". Release the port before writing anything.
SB_LOCKED="$(lsof -t "$PORT" 2>/dev/null || true)"
if [[ -n "$SB_LOCKED" ]]; then
    info "Releasing $PORT (held by PID $SB_LOCKED)..."
    kill $SB_LOCKED 2>/dev/null || true
    sleep 1
fi

# `pio run` has no --upload-speed option in PlatformIO 6; the wire speed is set by the
# upload_speed key in platformio.ini. Tell the user rather than pretending we applied it.
if [[ "$UPLOAD_SPEED" != "921600" ]]; then
    warn "note: pio run has no --upload-speed option - the upload speed comes from"
    warn "      'upload_speed' in platformio.ini (currently 921600)."
    warn "      Edit that line if you need ${UPLOAD_SPEED} baud."
    say ""
fi

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

# -------------------------------------------------------------------------------------------------
# Data safety
#
# The LittleFS partition holds the brew ledger, the brew profiles and the controller config.
# Both destructive steps below rewrite it wholesale, and `uploadfs` does not merge: everything
# that was in there is gone. So ask before doing it, and try to save the partition first.
# -------------------------------------------------------------------------------------------------

# Ask for the confirmation. Non-interactive runs without --yes are refused outright: a script
# that cannot ask must not guess.
sb_confirm_destructive() {
    local headline="${1:-These steps destroy the recorded shots, the profiles and the config.}"

    say ""
    warn "$headline"
    warn "The brew ledger (/history), the brew profiles (/profiles) and the controller"
    warn "config (/config.json) live in the LittleFS partition and will be destroyed."
    warn "This cannot be undone from the board - only from a backup."
    say ""

    if (( ASSUME_YES )); then
        warn "--yes given, continuing."
        say ""
        return 0
    fi
    if (( DRY_RUN )); then
        return 0
    fi
    if [[ ! -t 0 ]]; then
        die "refusing to destroy data without confirmation. Re-run with --yes to proceed." 1
    fi

    local answer
    read -r -p "type 'wipe' to continue: " answer
    if [[ "$answer" != "wipe" ]]; then
        die "aborted - nothing was written." 1
    fi
    say ""
}

# Save the LittleFS partition off the board so the destructive step is recoverable.
sb_backup_littlefs() {
    # fs_backup.py only needs the standard library, and it drives the PlatformIO esptool with the
    # PlatformIO interpreter itself, so any python3 can run it. Prefer the PlatformIO one anyway.
    local py=""
    if [[ -x "$HOME/.platformio/penv/bin/python" ]]; then
        py="$HOME/.platformio/penv/bin/python"
    else
        py="$(command -v python3 2>/dev/null)"
    fi
    if [[ -z "$py" ]]; then
        warn "no python3 found - the partition cannot be saved."
        return 1
    fi

    info "Saving the LittleFS partition (ledger, profiles, config)..."
    if "$py" "$SCRIPT_DIR/tools/fs_backup.py" backup --port "$PORT"; then
        return 0
    fi
    warn "the backup did not succeed."
    return 1
}

FAILED=0

PIO_ENV="esp32s3"
if (( NO_RESET )); then
    PIO_ENV="esp32s3-bootloader"
fi

# --- data safety ------------------------------------------------------------------------------
# --erase takes the whole flash, --fs/--fs-only replace the whole LittleFS partition. Both lose
# the ledger, so they are asked for and backed up before anything is written.
if (( DO_FS || DO_ERASE )); then
    if (( DRY_RUN )); then
        warn "dry run: the LittleFS partition would be replaced (ledger, profiles and config lost)."
    elif (( DO_BACKUP )); then
        sb_confirm_destructive "This step rewrites the LittleFS partition."
        # A failed backup is not a warning - it means the data is about to be destroyed with no way
        # back. Stop, and say why, rather than continuing and losing it.
        if ! sb_backup_littlefs; then
            warn "the backup failed, so nothing was written."
            warn "Close whatever is holding the serial port (the console) and try again, or pass"
            warn "--no-backup to skip the backup on purpose."
            die "no backup, no flash." 1
        fi
        ok "the partition was saved - restore it with tools/fs_backup.py restore"
    else
        sb_confirm_destructive "This step rewrites the LittleFS partition and --no-backup was given."
    fi
fi

# --- firmware -------------------------------------------------------------------------------
if (( DO_APP )); then
    CMD=(pio run -e "$PIO_ENV" -t upload --upload-port "$PORT")
    if (( DO_ERASE )); then
        CMD=(pio run -e "$PIO_ENV" -t erase -t upload --upload-port "$PORT")
        info "Erasing flash, then uploading firmware..."
    else
        info "Building and uploading firmware..."
    fi
    run "${CMD[@]}" || FAILED=1
    if (( FAILED )); then
        say ""
        warn "firmware upload failed."
        sb_recovery_hints "flash.sh"
        exit 3
    fi
fi

# --- filesystem -----------------------------------------------------------------------------
if (( DO_FS )); then
    info "Building and replacing the LittleFS image (this wipes the ledger, profiles and config)..."
    run pio run -e "$PIO_ENV" -t buildfs -t uploadfs --upload-port "$PORT" || FAILED=1
fi

# --- remember the port ------------------------------------------------------------------------
# Only a port that was actually flashed is worth remembering.
if (( ! FAILED )) && (( ! DRY_RUN )); then
    printf '%s\n' "$PORT" > "$LAST_PORT_FILE" 2>/dev/null || warn "could not save $LAST_PORT_FILE"
fi

# --- monitor --------------------------------------------------------------------------------
if (( OPEN_MONITOR )) && (( ! DRY_RUN )); then
    # Hand over to the same monitor script users get from ./monitor.sh, so there is one
    # implementation of the console, not two. It opens on the SAME port the firmware was just
    # written to - which matters, because the board resets on native USB and the port briefly
    # disappears and comes back.
    info "Opening the console on $PORT (Ctrl-C to exit)..."
    say ""
    exec "$SCRIPT_DIR/monitor.sh" -p "$PORT"
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