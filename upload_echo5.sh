#!/usr/bin/env bash
# =================================================================================================
#  upload_echo5.sh - Build and upload Smart Brew Android Companion App to Echo Show 5
#
#  Usage:
#    ./upload_echo5.sh                   Sync web assets, build debug APK, install, and launch
#    ./upload_echo5.sh -s <device>       Target a specific ADB device serial or IP:port
#    ./upload_echo5.sh -c, --clean       Perform clean build (gradlew clean assembleDebug)
#    ./upload_echo5.sh --no-sync         Skip synchronizing data/index.html into Android assets
#    ./upload_echo5.sh --no-launch       Install APK only, do not launch activity
#    ./upload_echo5.sh -l, --logcat      Stream filtered Logcat logs after launching
#    ./upload_echo5.sh -h, --help        Show this help message
#
#  Exit codes:
#    0 success   1 build failed   2 adb/device error   3 install failed
# =================================================================================================
set -eo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR"
ANDROID_DIR="$ROOT_DIR/android-app"

# ANSI Color codes
BOLD="\033[1m"
DIM="\033[2m"
CYAN="\033[36m"
GREEN="\033[32m"
YELLOW="\033[33m"
RED="\033[31m"
RESET="\033[0m"

log_info()  { echo -e "${CYAN}${BOLD}[Echo5]${RESET} $*"; }
log_ok()    { echo -e "${GREEN}${BOLD}[✓]${RESET} $*"; }
log_warn()  { echo -e "${YELLOW}${BOLD}[!]${RESET} $*"; }
log_err()   { echo -e "${RED}${BOLD}[✗]${RESET} $*" >&2; }

# Defaults
TARGET_DEVICE=""
DO_CLEAN=0
DO_SYNC=1
DO_LAUNCH=1
STREAM_LOGS=0
BUILD_TYPE="debug"
PACKAGE_NAME="com.smartbrew.echo5.debug"
MAIN_ACTIVITY="com.smartbrew.echo5.MainActivity"

usage() {
    echo -e "${BOLD}Smart Brew Echo Show 5 App Builder & Installer${RESET}"
    echo ""
    echo "Usage:"
    echo "  ./upload_echo5.sh [options]"
    echo ""
    echo "Options:"
    echo "  -s, --device <serial>  Target specific ADB device (e.g. 192.168.1.50:5555)"
    echo "  -c, --clean            Clean build before compiling"
    echo "  --no-sync              Skip syncing data/index.html web assets"
    echo "  --no-launch            Install APK only, do not launch on device"
    echo "  -l, --logcat           Stream filtered logcat after launching"
    echo "  -h, --help             Show this help message"
    echo ""
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -s|--device)
            TARGET_DEVICE="${2:-}"
            shift 2
            ;;
        -s=*|--device=*)
            TARGET_DEVICE="${1#*=}"
            shift
            ;;
        -c|--clean)
            DO_CLEAN=1
            shift
            ;;
        --no-sync)
            DO_SYNC=0
            shift
            ;;
        --no-launch)
            DO_LAUNCH=0
            shift
            ;;
        -l|--logcat)
            STREAM_LOGS=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            log_err "Unknown argument: $1"
            usage
            ;;
    esac
done

# -----------------------------------------------------------------------------
# 1. Locate ADB Binary
# -----------------------------------------------------------------------------
ADB_BIN=""
if command -v adb >/dev/null 2>&1; then
    ADB_BIN="$(command -v adb)"
elif [ -x "$HOME/Library/Android/sdk/platform-tools/adb" ]; then
    ADB_BIN="$HOME/Library/Android/sdk/platform-tools/adb"
elif [ -x "/opt/homebrew/bin/adb" ]; then
    ADB_BIN="/opt/homebrew/bin/adb"
fi

if [ -z "$ADB_BIN" ]; then
    log_err "adb binary not found in PATH or standard locations."
    log_info "Please install android-platform-tools (e.g. 'brew install android-platform-tools')."
    exit 2
fi

# -----------------------------------------------------------------------------
# 2. Synchronize Web Assets (unless --no-sync)
# -----------------------------------------------------------------------------
if [ "$DO_SYNC" -eq 1 ]; then
    log_info "Synchronizing web assets (data/index.html -> android-app)..."
    if [ -f "$ANDROID_DIR/sync_web_assets.sh" ]; then
        bash "$ANDROID_DIR/sync_web_assets.sh" >/dev/null
        log_ok "Web assets synchronized."
    else
        log_warn "sync_web_assets.sh not found, skipping web sync."
    fi
fi

# -----------------------------------------------------------------------------
# 3. Build APK
# -----------------------------------------------------------------------------
cd "$ANDROID_DIR"

GRADLE_TASKS=()
if [ "$DO_CLEAN" -eq 1 ]; then
    GRADLE_TASKS+=("clean")
fi
GRADLE_TASKS+=("assembleDebug")

log_info "Building Android app: ./gradlew ${GRADLE_TASKS[*]}..."
if ! ./gradlew "${GRADLE_TASKS[@]}"; then
    log_err "Gradle build failed!"
    exit 1
fi
log_ok "Build successful."

APK_PATH="$ANDROID_DIR/app/build/outputs/apk/debug/app-debug.apk"
if [ ! -f "$APK_PATH" ]; then
    log_err "APK not found at expected path: $APK_PATH"
    exit 1
fi

APK_SIZE="$(ls -lh "$APK_PATH" | awk '{print $5}')"
log_ok "Generated APK: ${BOLD}$APK_PATH${RESET} (${APK_SIZE})"

# -----------------------------------------------------------------------------
# 4. Detect / Select ADB Device
# -----------------------------------------------------------------------------
ADB_ARGS=()
if [ -n "$TARGET_DEVICE" ]; then
    ADB_ARGS+=("-s" "$TARGET_DEVICE")
else
    # Find active online devices
    ONLINE_DEVICES=($("$ADB_BIN" devices | awk 'NR>1 && $2=="device" {print $1}'))
    
    if [ ${#ONLINE_DEVICES[@]} -eq 0 ]; then
        log_err "No connected ADB devices detected!"
        echo ""
        log_info "To connect to Amazon Echo Show 5 over Wi-Fi:"
        echo -e "   ${BOLD}$ADB_BIN connect <echo5-ip-address>:5555${RESET}"
        echo ""
        log_info "Or plug in the device via USB with USB debugging enabled."
        exit 2
    elif [ ${#ONLINE_DEVICES[@]} -eq 1 ]; then
        TARGET_DEVICE="${ONLINE_DEVICES[0]}"
        ADB_ARGS+=("-s" "$TARGET_DEVICE")
        log_info "Targeting device: ${BOLD}$TARGET_DEVICE${RESET}"
    else
        # Multiple devices detected
        log_warn "Multiple ADB devices detected:"
        for i in "${!ONLINE_DEVICES[@]}"; do
            echo "   [$((i+1))] ${ONLINE_DEVICES[$i]}"
        done
        TARGET_DEVICE="${ONLINE_DEVICES[0]}"
        ADB_ARGS+=("-s" "$TARGET_DEVICE")
        log_info "Defaulting to first device: ${BOLD}$TARGET_DEVICE${RESET} (use -s to override)"
    fi
fi

# -----------------------------------------------------------------------------
# 5. Install APK
# -----------------------------------------------------------------------------
log_info "Installing APK to ${BOLD}$TARGET_DEVICE${RESET}..."
if ! "$ADB_BIN" "${ADB_ARGS[@]}" install -r "$APK_PATH"; then
    log_err "Failed to install APK on device."
    exit 3
fi
log_ok "APK installed successfully."

# -----------------------------------------------------------------------------
# 6. Launch App (unless --no-launch)
# -----------------------------------------------------------------------------
if [ "$DO_LAUNCH" -eq 1 ]; then
    log_info "Restarting app (${PACKAGE_NAME}/${MAIN_ACTIVITY})..."
    "$ADB_BIN" "${ADB_ARGS[@]}" shell am force-stop "$PACKAGE_NAME" >/dev/null 2>&1 || true
    sleep 0.5
    "$ADB_BIN" "${ADB_ARGS[@]}" shell am start -n "${PACKAGE_NAME}/${MAIN_ACTIVITY}"
    log_ok "Smart Brew app launched on Echo Show 5!"
fi

# -----------------------------------------------------------------------------
# 7. Optional Log Streaming
# -----------------------------------------------------------------------------
if [ "$STREAM_LOGS" -eq 1 ]; then
    echo ""
    log_info "Streaming logs (Ctrl+C to stop)..."
    "$ADB_BIN" "${ADB_ARGS[@]}" logcat -c
    "$ADB_BIN" "${ADB_ARGS[@]}" logcat -v time | grep --line-buffered -E "SmartBrew|Echo5|McuBridge|BleScale|chromium|AndroidRuntime"
fi
