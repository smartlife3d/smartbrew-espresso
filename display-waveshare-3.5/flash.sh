#!/usr/bin/env bash
# ==============================================================================
# Flash / Burn Utility for SmartBrew Waveshare 3.5" ESP32-S3 Touch LCD Display
# Supports automatic port detection, interactive port selection, and CLI flags
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PIO_BIN="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"

# The repository's shared serial helpers: the python-with-pyserial lookup for the console.
# shellcheck source=../scripts/portlib.sh
. "$ROOT_DIR/scripts/portlib.sh"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

TARGET_PORT=""
LIST_ONLY=false
BUILD_ONLY=false
UPLOAD_ONLY=false
ERASE_FLASH=false
START_MONITOR=true
UPLOAD_SPEED=""
FORGET_PORT=false
# The port of the last successful flash of THIS board (display-waveshare-3.5/.last_port, separate
# from the controller's and the simulator's). Per machine, so it is git-ignored.
LAST_PORT_FILE="$SCRIPT_DIR/.last_port"

usage() {
    echo -e "${BOLD}Usage:${NC} $0 [options] [port_or_index]

${BOLD}Options:${NC}
  -p, --port <port|index>   Specify serial port path or list index (e.g. 1, 2)
  -l, --list                List detected serial devices and exit
  -b, --baud <speed>        Override upload baud rate (e.g. 460800, 921600)
  -m, --monitor             Open the serial console immediately after flashing (default)
      --no-monitor          Flash without opening the serial console
      --build-only          Compile firmware without uploading
      --upload-only         Upload without recompiling
      --erase               Erase flash chip completely before flashing
      --forget-port         Forget the remembered port and show the selection menu
  -h, --help                Show this help message

${BOLD}Remembered port:${NC}
  After a successful flash the port is saved to .last_port. The next run uses it
  without asking while it is plugged in. -p picks another, which is remembered instead.

${BOLD}Examples:${NC}
  $0                        # Remembered port, or the interactive selection menu
  $0 2                      # Flash device #2 from detection list
  $0 /dev/cu.usbmodem832301 # Flash specific device
  $0 -p 2 -m                # Flash device #2 and open the serial console"
    exit 0
}

# Parse command line flags
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)
            TARGET_PORT="$2"
            shift 2
            ;;
        -l|--list)
            LIST_ONLY=true
            shift
            ;;
        -b|--baud|--speed)
            UPLOAD_SPEED="$2"
            shift 2
            ;;
        -m|--monitor)
            START_MONITOR=true
            shift
            ;;
        --no-monitor)
            START_MONITOR=false
            shift
            ;;
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --upload-only)
            UPLOAD_ONLY=true
            shift
            ;;
        --erase)
            ERASE_FLASH=true
            shift
            ;;
        --forget-port)
            FORGET_PORT=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        -*)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            ;;
        *)
            if [ -z "$TARGET_PORT" ]; then
                TARGET_PORT="$1"
            fi
            shift
            ;;
    esac
done

echo -e "${BOLD}${CYAN}======================================================${NC}"
echo -e "${BOLD}${CYAN}  SmartBrew Waveshare 3.5\" Touch Display Flash Tool   ${NC}"
echo -e "${BOLD}${CYAN}======================================================${NC}"
echo -e "Project Directory : ${MAGENTA}$SCRIPT_DIR${NC}"
echo -e "PlatformIO Binary : ${MAGENTA}$PIO_BIN${NC}"

# Function to discover serial ports via PlatformIO or /dev scan
get_ports_json() {
    python3 -c "
import subprocess, json, glob, sys

ports = []
try:
    res = subprocess.run(['$PIO_BIN', 'device', 'list', '--json-output'], capture_output=True, text=True, timeout=5)
    if res.returncode == 0 and res.stdout.strip():
        data = json.loads(res.stdout)
        for item in data:
            port = item.get('port', '')
            desc = item.get('description', '')
            hwid = item.get('hwid', '')
            # Filter out non-hardware devices like Bluetooth audio, debug consoles, virtual terminals
            if any(k in port.lower() for k in ['bluetooth', 'debug-console', 'wireless', 'edifier', 'airpods']):
                continue
            ports.append({'port': port, 'desc': desc, 'hwid': hwid})
except Exception:
    pass

# Fallback: scan /dev/cu.* if pio returned empty
if not ports:
    for dev in sorted(glob.glob('/dev/cu.usbmodem*') + glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/cu.wch*') + glob.glob('/dev/cu.SLAB*')):
        ports.append({'port': dev, 'desc': 'Serial Device', 'hwid': ''})

# Sort ports: put ESP32-S3 USB JTAG / CDC ports first
def sort_key(p):
    hwid = p.get('hwid', '').lower()
    desc = p.get('desc', '').lower()
    port = p.get('port', '').lower()
    if '303a:1001' in hwid or 'jtag' in desc or 'usbmodem' in port:
        return 0
    if '10c4:ea60' in hwid or 'usbserial' in port:
        return 1
    return 2

ports.sort(key=sort_key)
print(json.dumps(ports))
"
}

PORTS_JSON="$(get_ports_json)"
PORT_COUNT=$(python3 -c "import json; print(len(json.loads('''$PORTS_JSON''')))")

# If user asked for -l / --list only
if [ "$LIST_ONLY" = true ]; then
    echo -e "\n${BOLD}Detected Serial Ports:${NC}"
    if [ "$PORT_COUNT" -eq 0 ]; then
        echo -e "${YELLOW}  (No standard USB serial devices detected)${NC}"
    else
        python3 -c "
import json
ports = json.loads('''$PORTS_JSON''')
for idx, p in enumerate(ports, 1):
    hwid = p.get('hwid', '').lower()
    desc = p.get('desc', '')
    is_s3 = '303a:1001' in hwid or 'jtag' in desc.lower() or 'usbmodem' in p['port'].lower()
    tag = ' \033[1;32m[ESP32-S3]\033[0m' if is_s3 else ''
    print(f'  [\033[1;36m{idx}\033[0m] {p[\"port\"]}  ({desc}){tag}')
"
    fi
    exit 0
fi

# Build only shortcut
if [ "$BUILD_ONLY" = true ]; then
    echo -e "\n${BOLD}${CYAN}[1/1] Building firmware only (no upload)...${NC}"
    "$PIO_BIN" run -d "$SCRIPT_DIR"
    echo -e "\n${GREEN}${BOLD}✓ Build succeeded!${NC}"
    exit 0
fi

# Helper to resolve index number (e.g. '1', '2') to port path
resolve_port_input() {
    local input="$1"
    python3 -c "
import json, sys
ports = json.loads('''$PORTS_JSON''')
target = '$input'.strip()

if target.isdigit():
    idx = int(target) - 1
    if 0 <= idx < len(ports):
        print(ports[idx]['port'])
        sys.exit(0)
    else:
        sys.stderr.write(f'Invalid port index {target}. Only {len(ports)} devices detected.\n')
        sys.exit(1)

for p in ports:
    if p['port'] == target or p['port'].endswith(target):
        print(p['port'])
        sys.exit(0)

if target.startswith('/dev/'):
    print(target)
    sys.exit(0)

if target.startswith('cu.') or target.startswith('tty.'):
    print(f'/dev/{target}')
    sys.exit(0)

print(f'/dev/cu.{target}')
"
}

if [ "$FORGET_PORT" = true ]; then
    rm -f "$LAST_PORT_FILE"
    echo -e "${YELLOW}Forgot the remembered port.${NC}"
fi
REMEMBERED_PORT=""
if [ -z "$TARGET_PORT" ] && [ -r "$LAST_PORT_FILE" ]; then
    REMEMBERED_PORT="$(head -n 1 "$LAST_PORT_FILE")"
    # Only while it is plugged in; otherwise fall through to the menu.
    [ -n "$REMEMBERED_PORT" ] && [ -e "$REMEMBERED_PORT" ] || REMEMBERED_PORT=""
fi

if [ -n "$TARGET_PORT" ]; then
    TARGET_PORT="$(resolve_port_input "$TARGET_PORT")"
    echo -e "Target Port Selected : ${GREEN}${BOLD}$TARGET_PORT${NC}"
elif [ -n "$REMEMBERED_PORT" ]; then
    TARGET_PORT="$REMEMBERED_PORT"
    echo -e "Remembered Port      : ${GREEN}${BOLD}$TARGET_PORT${NC}"
    echo -e "  (from the last display flash; -p <port> picks another, --forget-port shows the menu)"
else
    # Interactive Port Selection Menu
    echo -e "\n${BOLD}Available Serial Devices:${NC}"
    if [ "$PORT_COUNT" -eq 0 ]; then
        echo -e "${YELLOW}  (No USB serial devices detected. Is your Waveshare board plugged in?)${NC}"
    else
        python3 -c "
import json
ports = json.loads('''$PORTS_JSON''')
for idx, p in enumerate(ports, 1):
    hwid = p.get('hwid', '').lower()
    desc = p.get('desc', '')
    is_s3 = '303a:1001' in hwid or 'jtag' in desc.lower() or 'usbmodem' in p['port'].lower()
    rec = ' \033[1;32m[ESP32-S3] [RECOMMENDED]\033[0m' if is_s3 else ''
    print(f'  [\033[1;36m{idx}\033[0m] {p[\"port\"]}  ({desc}){rec}')
"
    fi
    echo -e "  [\033[1;36mc\033[0m] Enter custom port path..."
    echo -e "  [\033[1;31mq\033[0m] Quit\n"

    DEFAULT_SEL="1"
    if [ "$PORT_COUNT" -eq 0 ]; then
        DEFAULT_SEL="c"
    fi

    if [ -t 0 ]; then
        read -r -p "Select port [1-$PORT_COUNT / c / q] (default: $DEFAULT_SEL): " USER_CHOICE
        USER_CHOICE="${USER_CHOICE:-$DEFAULT_SEL}"
    else
        USER_CHOICE="$DEFAULT_SEL"
        echo "Non-interactive environment. Auto-selecting [$USER_CHOICE]"
    fi

    case "$USER_CHOICE" in
        q|Q)
            echo "Operation cancelled by user."
            exit 0
            ;;
        c|C)
            read -r -p "Enter full port path (e.g. /dev/cu.usbmodem1234): " CUSTOM_PORT
            TARGET_PORT="$CUSTOM_PORT"
            ;;
        *)
            TARGET_PORT="$(resolve_port_input "$USER_CHOICE")"
            ;;
    esac
fi

if [ -z "$TARGET_PORT" ]; then
    echo -e "${RED}Error: No valid serial port selected!${NC}"
    exit 1
fi

echo -e "\n${BOLD}Configuration Summary:${NC}"
echo -e "  Target Port : ${GREEN}${BOLD}$TARGET_PORT${NC}"
if [ -n "$UPLOAD_SPEED" ]; then
    echo -e "  Upload Baud : ${CYAN}$UPLOAD_SPEED baud${NC}"
fi

# Release port if currently held by a serial monitor or other process
LOCKED_PIDS=$(lsof -t "$TARGET_PORT" 2>/dev/null || true)
if [ -n "$LOCKED_PIDS" ]; then
    echo -e "${YELLOW}Releasing port $TARGET_PORT held by PID(s): $LOCKED_PIDS...${NC}"
    kill $LOCKED_PIDS 2>/dev/null || true
    sleep 0.5
fi

# Step 1: Compile Firmware
if [ "$UPLOAD_ONLY" = false ]; then
    echo -e "\n${BOLD}${CYAN}[1/2] Compiling Waveshare ESP32-S3 Firmware...${NC}"
    "$PIO_BIN" run -d "$SCRIPT_DIR"
else
    echo -e "\n${YELLOW}[1/2] Skipping compilation (--upload-only specified).${NC}"
fi

# Optional: Erase Flash Chip
if [ "$ERASE_FLASH" = true ]; then
    echo -e "\n${BOLD}${YELLOW}[Optional] Erasing ESP32-S3 Flash Memory...${NC}"
    "$PIO_BIN" run -d "$SCRIPT_DIR" -t erase --upload-port "$TARGET_PORT"
fi

# Step 2: Upload / Flash Firmware
echo -e "\n${BOLD}${CYAN}[2/2] Flashing firmware to $TARGET_PORT...${NC}"
UPLOAD_ARGS=("-d" "$SCRIPT_DIR" "-t" "upload" "--upload-port" "$TARGET_PORT")

if "$PIO_BIN" run "${UPLOAD_ARGS[@]}"; then
    echo -e "\n${GREEN}${BOLD}✓ SUCCESS! Waveshare ESP32-S3 firmware flashed successfully!${NC}"
    printf '%s\n' "$TARGET_PORT" > "$LAST_PORT_FILE" 2>/dev/null ||
        echo -e "${YELLOW}Could not save $LAST_PORT_FILE${NC}"
else
    EXIT_CODE=$?
    echo -e "\n${RED}${BOLD}✗ Firmware flashing failed (Exit Code: $EXIT_CODE).${NC}"
    echo -e "${YELLOW}Troubleshooting Tips:${NC}"
    echo "  1. Verify the USB-C cable is connected to the USB port on the Waveshare board."
    echo "  2. If the upload fails to sync, place the board in bootloader mode:"
    echo "     - Hold down the [BOOT] button on the board"
    echo "     - Press and release the [RST] button once"
    echo "     - Release the [BOOT] button, then retry flashing"
    exit $EXIT_CODE
fi

# Step 3: Serial Console
if [ "$START_MONITOR" = true ]; then
    echo -e "\n${BOLD}${CYAN}Opening the serial console on $TARGET_PORT (Ctrl+C to exit)...${NC}\n"
    # The repository's own console (tools/brewmon.py), the same one the controller and the
    # simulator use. It reconnects by itself when the board resets on native USB - the port
    # vanishes for about a second and `pio device monitor` gives up at that point.
    MON_PY="$(sb_find_python)"
    if [ -z "$MON_PY" ]; then
        echo -e "${YELLOW}No python3 with pyserial found - skipping the console.${NC}"
        echo -e "  Install it with:  pip install pyserial"
        exit 0
    fi
    exec "$MON_PY" "$ROOT_DIR/tools/brewmon.py" \
        --port "$TARGET_PORT" --baud 115200 --log-dir "$ROOT_DIR/logs"
fi
