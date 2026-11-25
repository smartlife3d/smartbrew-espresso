#!/usr/bin/env bash
# =================================================================================================
#  scripts/portlib.sh - shared serial-port discovery and selection for flash.sh and monitor.sh
#
#  Source it, do not execute it:
#      . "$(dirname "$0")/scripts/portlib.sh"
#      sb_ports_enumerate
#      sb_ports_print_table
#      sb_ports_select          # sets PORT / HW_ID / RAW_HW / DESC / PORT_SCORE
#
#  Everything here targets bash 3.2 (the /bin/bash that ships with macOS), so it deliberately avoids
#  ${var,,}, associative arrays and other bash-4-only features.
# =================================================================================================

# -------------------------------------------------------------------------------------------------
# Output helpers
# -------------------------------------------------------------------------------------------------
if [[ -t 1 && "${NO_COLOR:-}" == "" ]]; then
    R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[36m'; D=$'\033[2m'; N=$'\033[0m'
else
    R=""; G=""; Y=""; B=""; D=""; N=""
fi

say()  { printf '%s\n' "$*"; }
info() { printf '%s==>%s %s\n' "$B" "$N" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$G" "$N" "$*"; }
warn() { printf '%swarn%s %s\n' "$Y" "$N" "$*" >&2; }

# die() prints AND exits. It must never only print: a call site that continues afterwards would
# fall straight through into a real flash or a real monitor session, which is exactly the kind of
# surprise these scripts exist to prevent. Pass an explicit exit code (default 1).
die() {
    local code="${2:-1}"
    printf '%s err%s %s\n' "$R" "$N" "$1" >&2
    exit "$code"
}

# -------------------------------------------------------------------------------------------------
# Raw enumeration
#
# `pio device list` emits blocks of:
#     /dev/cu.usbmodem101
#     ---------------------
#     Hardware ID: USB VID:PID=303A:1001 SER=... LOCATION=...
#     Description: USB JTAG/serial debug unit
#
# Flattened to "path|hardware_id|description", one per line.
# -------------------------------------------------------------------------------------------------
sb_ports_once() {
    pio device list 2>/dev/null | awk '
        /^\/dev\// || /^COM[0-9]+/ {
            if (path != "") print path "|" hw "|" desc
            path = $0; hw = "n/a"; desc = "n/a"; next
        }
        /^Hardware ID:/ { hw = $0; sub(/^Hardware ID:[[:space:]]*/, "", hw); next }
        /^Description:/  { desc = $0; sub(/^Description:[[:space:]]*/, "", desc); next }
        END { if (path != "") print path "|" hw "|" desc }
    '
}

# macOS intermittently drops a freshly-enumerated USB device out of `pio device list` for a second
# or two - an ESP32 on native USB is the usual culprit. Poll a few times and merge, so a device
# seen in ANY pass is included. Costs a second or two only while the device is settling.
SB_ENUM_ATTEMPTS=4
sb_ports_enumerate() {
    local merged="" line prev path_a path_b i
    for (( i = 1; i <= SB_ENUM_ATTEMPTS; i++ )); do
        while IFS= read -r line; do
            [[ -z "$line" ]] && continue
            path_a="${line%%|*}"
            if ! awk -F'|' -v p="$path_a" '$1 == p { found = 1 } END { exit !found }' <<<"$merged"; then
                merged="${merged}${line}"$'\n'
            else
                # Keep the richest data: a later pass may fill in a VID:PID that was "n/a" before.
                prev="$(awk -F'|' -v p="$path_a" '$1 == p { print; exit }' <<<"$merged")"
                path_b="$(cut -d'|' -f2 <<<"$prev")"
                if [[ "$path_b" == "n/a" ]] && [[ "$(cut -d'|' -f2 <<<"$line")" != "n/a" ]]; then
                    merged="${merged/${prev}/${line}}"
                fi
            fi
        done < <(sb_ports_once)
        (( i < SB_ENUM_ATTEMPTS )) && sleep 0.4
    done
    printf '%s' "${merged%$'\n'}"
}

# -------------------------------------------------------------------------------------------------
# Classification
# -------------------------------------------------------------------------------------------------

# Ports we never want to talk to a microcontroller through.
sb_is_noise_port() {
    case "$1" in
        *Bluetooth*|*debug-console*|*Wireless*|*IrDA*|*irda*|*wlan*|*u.SAMSUNG*|*Printing*)
            return 0 ;;
        *)  return 1 ;;
    esac
}

# PlatformIO reports the hardware ID in a long form on macOS:
#   "USB VID:PID=303A:1001 SER=AA:BB:CC:DD:EE:FF LOCATION=3-1.2.1"
# and as a bare "303a:1001" on some Linux/Windows setups. Normalise both to "vid:pid".
sb_normalise_id() {
    local id
    id="$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')"
    if [[ "$id" =~ vid:pid=([0-9a-f]{4}:[0-9a-f]{4}) ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
    elif [[ "$id" =~ ^([0-9a-f]{4}:[0-9a-f]{4}) ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
    else
        printf 'n/a'
    fi
}

# Confidence that a VID:PID is a USB-serial adapter an ESP32 could sit behind. 100 means
# "Espressif native USB", which is unambiguous.
sb_score_port() {
    local id
    id="$(sb_normalise_id "$1")"
    case "$id" in
        303a:*)      echo 100 ;;   # Espressif USB JTAG/serial debug unit
        10c4:ea60)   echo  90 ;;   # Silicon Labs CP210x
        1a86:7523)   echo  85 ;;   # WCH CH340
        1a86:55d4)   echo  85 ;;   # WCH CH9102
        0403:6001)   echo  70 ;;   # FTDI FT232R
        0403:6015)   echo  70 ;;   # FTDI FT231X
        067b:2303)   echo  65 ;;   # Prolific PL2303
        *)           echo  10 ;;
    esac
}

# -------------------------------------------------------------------------------------------------
# Presentation
# -------------------------------------------------------------------------------------------------
# Expects $ALL_PORTS to be populated by the caller.
sb_ports_print_table() {
    local path hw desc score marker hw_short idx=0
    printf '%s%-4s %-34s %-11s %-8s %s%s\n' "$D" "#" "PORT" "HARDWARE ID" "SCORE" "DESCRIPTION" "$N"
    printf '%s%s%s\n' "$D" "--------------------------------------------------------------------------" "$N"
    while IFS='|' read -r path hw desc; do
        [[ -z "$path" ]] && continue
        idx=$((idx + 1))
        score="$(sb_score_port "$hw")"
        hw_short="$(sb_normalise_id "$hw")"
        if sb_is_noise_port "$path" || sb_is_noise_port "$desc"; then
            marker="(system)"
        elif (( score >= 85 )); then
            marker="<== ESP32"
        else
            marker=""
        fi
        printf '%-4s %-34s %-11s %-8s %s %s\n' "$idx" "$path" "$hw_short" "$score" "$desc" "$marker"
    done <<<"$ALL_PORTS"
    say ""
    printf '%s  %d port(s) found%s\n' "$D" "$idx" "$N"
}

# Exact (non-regex) lookup - port names contain '.', which is a regex wildcard.
sb_port_row() {
    awk -F'|' -v want="$1" '$1 == want { print; exit }' <<<"$ALL_PORTS"
}

# -------------------------------------------------------------------------------------------------
# Selection
#
#   in : ALL_PORTS (enumerated), SB_WANT_PORT, SB_WANT_INDEX, SB_PROG (name used in messages),
#        SB_LAST_PORT (optional: the port remembered from the last successful run)
#   out: PORT, HW_ID, RAW_HW, DESC, PORT_SCORE, PORT_REMEMBERED (1 when SB_LAST_PORT was used)
#
# Returns 0 on success, 2 when no usable port exists.
# -------------------------------------------------------------------------------------------------
sb_ports_select() {
    local want_port="${SB_WANT_PORT:-}"
    local want_index="${SB_WANT_INDEX:-}"
    local prog="${SB_PROG:-script}"

    # A bare integer means "use this index from the listing".
    if [[ -n "$want_port" ]] && [[ "$want_port" =~ ^[0-9]+$ ]]; then
        want_index="$want_port"; want_port=""
    fi
    # Nothing asked for on the command line: reuse the remembered port while it is plugged in.
    # This also skips SB_ALWAYS_ASK - the operator picked this port for this board last time, and
    # each board's script keeps its own file. A port that has gone away falls through to the
    # normal detection below.
    PORT_REMEMBERED=0
    if [[ -z "$want_port" && -z "$want_index" && -n "${SB_LAST_PORT:-}" ]] &&
       [[ -n "$(sb_port_row "$SB_LAST_PORT")" ]]; then
        want_port="$SB_LAST_PORT"
        PORT_REMEMBERED=1
    fi

    if [[ -n "$want_port" ]]; then
        if [[ -z "$(sb_port_row "$want_port")" ]]; then
            warn "port '$want_port' not found. Available:"
            sb_ports_print_table
            return 2
        fi
    else
        local BEST="" BEST_SCORE=0 BEST_IDX=0 HIGH_COUNT=0 CANDIDATE_COUNT=0 IDX=0 sc
        while IFS='|' read -r path hw desc; do
            [[ -z "$path" ]] && continue
            IDX=$((IDX + 1))
            sb_is_noise_port "$path" && continue
            sb_is_noise_port "$desc" && continue
            CANDIDATE_COUNT=$((CANDIDATE_COUNT + 1))
            sc="$(sb_score_port "$hw")"
            (( sc >= 85 )) && HIGH_COUNT=$((HIGH_COUNT + 1))
            if (( sc > BEST_SCORE )); then
                BEST="$path"; BEST_SCORE="$sc"; BEST_IDX="$IDX"
            fi
        done <<<"$ALL_PORTS"

        if (( CANDIDATE_COUNT == 0 )); then
            warn "no usable serial port found. Plug the board in, then: ./$prog --list"
            return 2
        fi

        # Only interrupt when the choice is genuinely ambiguous: several known USB-serial adapters,
        # or no known one and several possibilities. SB_ALWAYS_ASK=1 forces the question even when
        # there is only one candidate, for the scripts where picking the wrong board matters.
        # An index given on the command line is always honoured - the operator already answered.
        local AMBIGUOUS=0
        (( HIGH_COUNT > 1 )) && AMBIGUOUS=1
        (( HIGH_COUNT == 0 && CANDIDATE_COUNT > 1 )) && AMBIGUOUS=1

        if [[ -z "$want_index" ]] && { (( AMBIGUOUS )) || [[ "${SB_ALWAYS_ASK:-0}" == "1" ]]; }; then
            say ""
            if (( AMBIGUOUS )); then
                warn "more than one serial port could be an ESP32 - pick one:"
            else
                warn "pick the serial port to use:"
            fi
            sb_ports_print_table
            local answer
            read -r -p "port number [${BEST_IDX}]: " answer
            want_index="${answer:-$BEST_IDX}"
        fi

        if [[ -n "$want_index" ]]; then
            BEST="$(awk -F'|' -v n="$want_index" 'BEGIN{i=0} /^\//{i++; if(i==n){print $1; exit}}' <<<"$ALL_PORTS")"
            [[ -z "$BEST" ]] && { warn "no port with index $want_index"; return 2; }
        fi
        want_port="$BEST"
    fi

    local row
    row="$(sb_port_row "$want_port")"
    PORT="$want_port"
    RAW_HW="$(cut -d'|' -f2 <<<"$row")"
    HW_ID="$(sb_normalise_id "$RAW_HW")"
    DESC="$(cut -d'|' -f3 <<<"$row")"
    # NB: score into a variable first. `(( $(sb_score_port ...) < 65 ))` would hand the VID:PID
    # string to the arithmetic evaluator and blow up on the colon.
    PORT_SCORE="$(sb_score_port "$RAW_HW")"
    return 0
}

# Print the standard "here is what we picked" banner. extra_baud is optional.
sb_ports_banner() {
    local baud="${1:-}"
    say ""
    printf '%s==>%s %s\n' "$B" "$N" "${2:-Selected port} : $PORT"
    printf '%s    hardware    : %s%s\n' "$D" "$HW_ID" "$N"
    [[ "$RAW_HW" == "n/a" || -z "$RAW_HW" ]] || printf '%s    raw         : %s%s\n' "$D" "$RAW_HW" "$N"
    [[ -z "$DESC" || "$DESC" == "n/a" ]] || printf '%s    description : %s%s\n' "$D" "$DESC" "$N"
    [[ -z "$baud" ]] || printf '%s    speed       : %s baud%s\n' "$D" "$baud" "$N"
    say ""
}

# Warn when the VID:PID is not a recognised USB-serial chip. force=1 skips it.
sb_ports_warn_unknown() {
    local prog="${1:-script}"
    (( PORT_SCORE >= 65 )) && return 0
    [[ "${FORCE:-0}" == "1" ]] && return 0
    warn "USB ID $HW_ID is not a known ESP32/USB-serial chip (score $PORT_SCORE)."
    warn "If this fails, press and hold BOOT, tap RESET, release BOOT, then re-run."
    warn "(or set FORCE=1 ./$prog to skip this notice)"
    say ""
}

# Shared recovery guidance for "the board did not answer".
sb_recovery_hints() {
    local prog="${1:-script}"
    say ""
    say "If the board was not found:"
    say "  1. hold BOOT, tap RESET, release BOOT   (forces the ROM downloader)"
    say "  2. re-run:  ./$prog --list"
    say "  3. pick the port explicitly:  ./$prog -p <port>"
    say ""
    say "To get back to normal running firmware afterwards, tap RESET on its own"
    say "without holding BOOT - the ROM only enters the downloader when BOOT/GPIO0"
    say "is low at reset."
}

# -------------------------------------------------------------------------------------------------
# Python with pyserial
#
# The console (tools/brewmon.py) needs pyserial. PlatformIO ships one with every install, so this
# works even on a machine where the system python3 has no serial module.
#
# NOTE: test the path with -x, never by executing it bare. Running `python` with no arguments
# starts an interactive REPL and hangs the script waiting on stdin.
#
# Prints the interpreter path on stdout, or nothing when none has pyserial.
# -------------------------------------------------------------------------------------------------
sb_find_python() {
    local candidates=() py

    local piopy="$HOME/.platformio/penv/bin/python"
    [[ -x "$piopy" ]] && candidates+=("$piopy")

    local which_py
    which_py="$(command -v python3 2>/dev/null)"
    [[ -n "$which_py" ]] && candidates+=("$which_py")
    which_py="$(command -v python 2>/dev/null)"
    [[ -n "$which_py" ]] && candidates+=("$which_py")

    if [[ "${FORCE_PYTHON:-}" != "" ]]; then
        candidates=("$FORCE_PYTHON" ${candidates[@]+"${candidates[@]}"})
    fi

    for py in ${candidates[@]+"${candidates[@]}"}; do
        [[ -z "$py" || ! -x "$py" ]] && continue
        if "$py" -c "import serial" >/dev/null 2>&1; then
            printf '%s' "$py"
            return 0
        fi
    done
    return 1
}