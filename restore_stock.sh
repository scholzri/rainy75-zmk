#!/bin/bash
# Restore stock Evision firmware on a Rainy 75 Pro running ZMK.
#
# Uses the custom flash_mgmt mcumgr group to write the original firmware
# directly to flash offset 0x0, then resets. All over USB CDC ACM.
#
# Prerequisites:
#   - Keyboard connected via USB, running ZMK firmware (VID 1d50:615e)
#   - Original firmware backup at reverse/firmware/firmware_ota.bin
#
# Usage:
#   ./restore_stock.sh                              # interactive
#   ./restore_stock.sh -y                            # non-interactive
#   ./restore_stock.sh path/to/firmware_ota.bin      # custom firmware file
#   ./restore_stock.sh --port /dev/ttyACM1           # custom serial port
#   ./restore_stock.sh --no-verify                   # skip read-back verify

set -euo pipefail
cd "$(dirname "$0")"

# ── Colors (disabled when stdout is not a terminal) ──────────
if [[ -t 1 ]]; then
    BOLD='\033[1m' RED='\033[0;31m' GREEN='\033[0;32m'
    YELLOW='\033[1;33m' CYAN='\033[0;36m' NC='\033[0m'
else
    BOLD='' RED='' GREEN='' YELLOW='' CYAN='' NC=''
fi
info()   { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()     { echo -e "${GREEN}[ OK ]${NC} $*"; }
warn()   { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
err()    { echo -e "${RED}[ERR ]${NC} $*" >&2; }
die()    { err "$@"; exit 1; }
header() { echo -e "\n${BOLD}$*${NC}"; }

# ── Defaults ─────────────────────────────────────────────────
FIRMWARE="reverse/firmware/firmware_ota.bin"
SERIAL_PORT="${SERIAL_PORT:-/dev/ttyACM0}"
EXTRA_ARGS=""
AUTO_YES=0
STOCK_VID="320f"
STOCK_PID="5055"
ZMK_VID="1d50"
ZMK_PID="615e"

# ── Parse arguments ──────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)      SERIAL_PORT="$2"; shift 2 ;;
        --no-verify) EXTRA_ARGS="$EXTRA_ARGS --no-verify"; shift ;;
        -y|--yes)    AUTO_YES=1; shift ;;
        --help|-h)
            echo "Usage: $0 [-y] [firmware.bin] [--port /dev/ttyACMx] [--no-verify]"
            exit 0 ;;
        -*) die "Unknown option: $1" ;;
        *)  FIRMWARE="$1"; shift ;;
    esac
done

# ── Helpers ──────────────────────────────────────────────────
check_usb() {
    lsusb -d "$1:$2" >/dev/null 2>&1
}

wait_for_usb() {
    local vid="$1" pid="$2" desc="$3" timeout="${4:-30}"
    info "Waiting for $desc..."
    local elapsed=0
    while ! check_usb "$vid" "$pid"; do
        sleep 1
        elapsed=$((elapsed + 1))
        if [[ $elapsed -ge $timeout ]]; then
            die "Timeout waiting for $desc after ${timeout}s."
        fi
    done
    ok "$desc detected (${elapsed}s)"
}

# ── Validate prerequisites ───────────────────────────────────
[[ -f "$FIRMWARE" ]] || die "Firmware file not found: $FIRMWARE"

# ── Check current state ─────────────────────────────────────
START_TIME=$SECONDS

header "=== Rainy 75 Pro: Restore Stock Firmware ==="

if check_usb "$STOCK_VID" "$STOCK_PID"; then
    ok "Keyboard is already running stock firmware."
    exit 0
fi

if ! check_usb "$ZMK_VID" "$ZMK_PID"; then
    die "Keyboard not found. Expected ZMK firmware (${ZMK_VID}:${ZMK_PID}) on USB."
fi

info "ZMK firmware detected."
info "  Firmware: $FIRMWARE ($(stat -c%s "$FIRMWARE") bytes)"
info "  Port: $SERIAL_PORT"

if [[ $AUTO_YES -eq 0 ]]; then
    echo ""
    info "This will erase ZMK and write the original Evision firmware."
    info "If interrupted, the keyboard may require EVK to recover."
    read -rp "Proceed? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
        info "Aborted."
        exit 0
    fi
fi

# ── Run restore tool ────────────────────────────────────────
echo ""
python3 reverse/tools/restore_original.py --yes --port "$SERIAL_PORT" $EXTRA_ARGS "$FIRMWARE"

# Wait for stock firmware to appear
wait_for_usb "$STOCK_VID" "$STOCK_PID" "stock firmware" 30

ELAPSED=$((SECONDS - START_TIME))
echo ""
ok "Restore complete (${ELAPSED}s)"
info "Keyboard is running stock Evision firmware (${STOCK_VID}:${STOCK_PID})."
info "To reinstall ZMK: ./install_zmk.sh"
