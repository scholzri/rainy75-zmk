#!/usr/bin/env bash
#
# verify_stage.sh — Hardware bring-up verification for Wobkey Rainy 75 Pro
#
# Runs automated checks + guided manual tests for each bring-up stage.
# See docs/zmk-firmware.md "Hardware Bring-Up Checklist" for full details.
#
# Usage:
#   ./verify_stage.sh prereqs          # Check all prerequisite tools
#   ./verify_stage.sh 0                # Stage 0: EVK + SWS toolchain
#   ./verify_stage.sh 1                # Stage 1: USB + serial + DFU
#   ./verify_stage.sh 2                # Stage 2: BLE
#   ./verify_stage.sh 3                # Stage 3: RGB underglow
#   ./verify_stage.sh 4                # Stage 4: Battery ADC
#   ./verify_stage.sh 5                # Stage 5: Deep sleep
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BDT="$SCRIPT_DIR/bdt/release/bdt"

# Ensure ~/go/bin is on PATH (for mcumgr installed via 'go install')
export PATH="$HOME/go/bin:$PATH"

# Helper: run a command on the host (distrobox) or locally
# Usage: host_exec command [args...]
host_exec() {
    if command -v flatpak-spawn &>/dev/null; then
        flatpak-spawn --host "$@"
    else
        "$@"
    fi
}

# --- ZMK USB identity (from board DTS + app.conf) ---
ZMK_USB_VID="2fe3"   # ZMK default VID
ZMK_USB_PID=""        # varies — we match by VID or product string
ZMK_PRODUCT="Rainy 75 Pro"
STOCK_USB_VID="320f"
STOCK_USB_PID="5055"

# Serial device (CDC ACM)
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyACM0}"
MCUMGR_CONN="--conntype serial --connstring ${SERIAL_DEV},baud=115200"

# --- Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

pass()     { echo -e "  ${GREEN}[PASS]${NC} $*"; }
fail()     { echo -e "  ${RED}[FAIL]${NC} $*"; }
warn()     { echo -e "  ${YELLOW}[WARN]${NC} $*"; }
info()     { echo -e "  ${CYAN}[INFO]${NC} $*"; }
header()   { echo -e "\n${BOLD}=== $* ===${NC}"; }
ask()      { echo -e "\n${YELLOW}[MANUAL]${NC} $*"; }

# Track pass/fail counts
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

record_pass() { PASS_COUNT=$((PASS_COUNT + 1)); pass "$@"; }
record_fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); fail "$@"; }
record_skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); warn "SKIP: $*"; }

# Ask user to confirm a manual check
manual_check() {
    local description="$1"
    ask "$description"
    read -rp "    Pass? [y/n/s(skip)] " answer
    case "$answer" in
        y|Y) record_pass "$description" ;;
        n|N) record_fail "$description" ;;
        *)   record_skip "$description" ;;
    esac
}

summary() {
    header "Results"
    echo -e "  ${GREEN}Passed: $PASS_COUNT${NC}"
    if [[ $FAIL_COUNT -gt 0 ]]; then
        echo -e "  ${RED}Failed: $FAIL_COUNT${NC}"
    else
        echo -e "  Failed: 0"
    fi
    if [[ $SKIP_COUNT -gt 0 ]]; then
        echo -e "  ${YELLOW}Skipped: $SKIP_COUNT${NC}"
    fi
    echo
    if [[ $FAIL_COUNT -gt 0 ]]; then
        echo -e "${RED}Stage verification FAILED — fix issues before proceeding.${NC}"
        return 1
    else
        echo -e "${GREEN}Stage verification PASSED.${NC}"
        return 0
    fi
}

# ─── PREREQS ───────────────────────────────────────────────────────────────────

cmd_prereqs() {
    header "Prerequisite Check"

    # BDT
    if [[ -x "$BDT" ]]; then
        local ver
        ver=$("$BDT" 2>&1 | grep -oP 'version: \K[0-9.]+' || echo "unknown")
        record_pass "BDT binary executable (v$ver)"
    else
        record_fail "BDT binary not found or not executable: $BDT"
    fi

    # BDT shared libs
    if ldd "$BDT" 2>&1 | grep -q "not found"; then
        record_fail "BDT missing shared libraries"
        ldd "$BDT" 2>&1 | grep "not found" | while read -r line; do
            info "  Missing: $line"
        done
    else
        record_pass "BDT shared libraries all present"
    fi

    # EVK firmware
    if [[ -f "$SCRIPT_DIR/bdt/release/fw/Firmware_v4.7.bin" ]]; then
        record_pass "EVK firmware v4.7 present"
    else
        record_fail "EVK firmware v4.7 not found"
    fi

    # udev rules — check both container-local and host
    local udev_ok=false
    if [[ -f /etc/udev/rules.d/99-telink-evk.rules ]]; then
        udev_ok=true
    fi
    # Also check host via flatpak-spawn (distrobox)
    if command -v flatpak-spawn &>/dev/null; then
        if flatpak-spawn --host ls /etc/udev/rules.d/99-telink-evk.rules &>/dev/null; then
            record_pass "udev rules installed on host"
            udev_ok=true
        else
            if $udev_ok; then
                warn "udev rules in container but NOT on host — USB access may fail"
                info "Run on HOST terminal:"
                info "  sudo cp $(realpath "$SCRIPT_DIR/bdt/99-telink-evk.rules") /etc/udev/rules.d/"
                info "  sudo udevadm control --reload-rules && sudo udevadm trigger"
            fi
        fi
    fi
    if $udev_ok; then
        if ! command -v flatpak-spawn &>/dev/null; then
            record_pass "udev rules installed"
        fi
    else
        record_fail "udev rules NOT installed"
        info "Run: sudo cp $SCRIPT_DIR/bdt/99-telink-evk.rules /etc/udev/rules.d/"
        info "Then: sudo udevadm control --reload-rules && sudo udevadm trigger"
    fi

    # mcumgr
    if command -v mcumgr &>/dev/null; then
        record_pass "mcumgr installed ($(mcumgr version 2>&1 | head -1))"
    else
        record_fail "mcumgr NOT installed"
        info "Install: go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest"
        info "Or download binary from https://github.com/apache/mynewt-mcumgr-cli/releases"
    fi

    # picocom (or alternative)
    if command -v picocom &>/dev/null; then
        record_pass "picocom installed"
    elif command -v minicom &>/dev/null; then
        record_pass "minicom installed (alternative to picocom)"
    elif command -v screen &>/dev/null; then
        warn "Only 'screen' available for serial — picocom recommended"
        record_pass "screen installed (alternative to picocom)"
    else
        record_fail "No serial terminal (picocom/minicom/screen)"
        info "Install: pacman -S picocom"
    fi

    # lsusb
    if command -v lsusb &>/dev/null; then
        record_pass "lsusb installed"
    else
        record_fail "lsusb NOT installed (usbutils package)"
    fi

    # bluetoothctl (for Stage 2) — check host if in distrobox
    if host_exec which bluetoothctl &>/dev/null; then
        record_pass "bluetoothctl available ($(command -v flatpak-spawn &>/dev/null && echo 'via host' || echo 'local'))"
    else
        warn "bluetoothctl NOT available — needed for Stage 2 BLE testing"
        info "Install on host: sudo dnf install bluez  (or: sudo pacman -S bluez-utils)"
    fi

    # Build tools
    if command -v west &>/dev/null; then
        record_pass "west installed"
    else
        record_fail "west NOT installed"
    fi

    # Zephyr SDK — check project-local, then env override, then legacy /tmp
    local sdk_dir="${ZEPHYR_SDK_INSTALL_DIR:-$PROJECT_ROOT/toolchain/zephyr-sdk-0.17.0}"
    if [[ -d "$sdk_dir" ]]; then
        record_pass "Zephyr SDK found at $sdk_dir"
    elif [[ -d "/tmp/zephyr-sdk-0.17.0" ]]; then
        record_pass "Zephyr SDK found at /tmp/zephyr-sdk-0.17.0 (legacy path)"
        warn "Consider moving to $PROJECT_ROOT/toolchain/zephyr-sdk-0.17.0"
    else
        record_fail "Zephyr SDK 0.17.0 not found"
        info "Expected at: $PROJECT_ROOT/toolchain/zephyr-sdk-0.17.0"
        info "Install: download minimal SDK, run setup.sh -t riscv64-zephyr-elf"
    fi

    summary
}

# ─── STAGE 0: EVK + SWS ───────────────────────────────────────────────────────

cmd_stage0() {
    header "Stage 0: EVK + SWS Toolchain Validation"

    # Check EVK on USB
    info "Checking for Burning EVK on USB..."
    if lsusb 2>/dev/null | grep -qi "248a:826[ab]"; then
        record_pass "Burning EVK detected on USB"

        # Test SWS connection
        info "Testing SWS link..."
        if "$BDT" B91 sws 2>&1 | grep -qi "ok\|success\|pass"; then
            record_pass "SWS connection OK"
        else
            # Try alternate chip ID
            if "$BDT" 9518 sws 2>&1 | grep -qi "ok\|success\|pass"; then
                record_pass "SWS connection OK (with CHIP=9518)"
                warn "Use CHIP=9518 for all sws_flash.sh commands"
            else
                record_fail "SWS connection failed"
                info "Check wiring: SWM→pad3(SWS), GND→pad1, 3.3V→pad2"
                info "Ensure wireless switch is OFF (under CapsLock)"
                info "Ensure USB cable is connected to keyboard (keeps MCU awake)"
            fi
        fi

        # Check EVK firmware version
        info "Querying EVK firmware version..."
        local evk_ver
        evk_ver=$("$BDT" 8266 up -ev 2>&1 || true)
        info "EVK firmware: $evk_ver"
        if echo "$evk_ver" | grep -q "4\.[7-9]"; then
            record_pass "EVK firmware is v4.7+"
        else
            warn "EVK firmware may need upgrade — run: sws_flash.sh evk-upgrade"
        fi
    else
        record_fail "Burning EVK NOT detected on USB (ID 248a:826a/826b)"
        info "Connect the EVK via USB and ensure it's powered"
        info "If in distrobox, ensure USB passthrough is working"
    fi

    # Check for existing firmware backup
    local dump="$PROJECT_ROOT/reverse/firmware/original_full_flash.bin"
    if [[ -f "$dump" ]]; then
        local size
        size=$(stat -c%s "$dump")
        if [[ "$size" -eq 1048576 ]]; then
            record_pass "Original firmware backup exists ($size bytes)"
        else
            record_fail "Firmware backup exists but wrong size: $size (expected 1048576)"
        fi
    else
        info "No firmware backup yet — run: sws_flash.sh dump"
    fi

    summary
}

# ─── STAGE 1: USB + Serial + DFU ──────────────────────────────────────────────

cmd_stage1() {
    header "Stage 1: USB + Serial Console + DFU"

    # 1a. USB enumeration
    header "1a. USB Enumeration"

    # Check for ZMK USB device
    local usb_found=false
    if lsusb 2>/dev/null | grep -qi "$ZMK_PRODUCT\|${ZMK_USB_VID}:"; then
        record_pass "ZMK USB device found"
        lsusb 2>/dev/null | grep -i "$ZMK_PRODUCT\|${ZMK_USB_VID}:" | while read -r line; do
            info "$line"
        done
        usb_found=true
    else
        # Fallback: check for any new USB HID that isn't the stock firmware
        if lsusb 2>/dev/null | grep -qi "${STOCK_USB_VID}:${STOCK_USB_PID}"; then
            record_fail "Stock firmware still running (VID:PID ${STOCK_USB_VID}:${STOCK_USB_PID})"
            info "Flash ZMK via sws_flash.sh first"
        else
            record_fail "No ZMK USB device found"
            info "Expected: USB device with product string '${ZMK_PRODUCT}'"
        fi
    fi

    # Check for CDC ACM serial device
    if [[ -e "$SERIAL_DEV" ]]; then
        record_pass "Serial device present: $SERIAL_DEV"
    else
        record_fail "Serial device $SERIAL_DEV not found"
        info "Check if CDC ACM driver loaded: ls /dev/ttyACM*"
        # Try to find any ACM device
        local acm_devs
        acm_devs=$(ls /dev/ttyACM* 2>/dev/null || true)
        if [[ -n "$acm_devs" ]]; then
            info "Found: $acm_devs"
            info "Set SERIAL_DEV=<device> to use a different device"
        fi
    fi

    # Try to read serial output (non-blocking, 3 second timeout)
    header "1a. Serial Console"
    if [[ -e "$SERIAL_DEV" ]]; then
        info "Reading serial output for 3 seconds..."
        local serial_out
        serial_out=$(timeout 3 cat "$SERIAL_DEV" 2>/dev/null || true)
        if [[ -n "$serial_out" ]]; then
            record_pass "Serial console producing output"
            echo "$serial_out" | head -20 | while read -r line; do
                info "  $line"
            done
        else
            warn "No serial output captured (may need reset or reconnect)"
            info "Try: picocom $SERIAL_DEV -b 115200"
        fi

        # Check for specific boot messages
        if echo "$serial_out" | grep -qi "mcuboot\|booting\|zephyr"; then
            record_pass "Boot messages detected in serial output"
        fi
        if echo "$serial_out" | grep -qi "Image confirmed\|img_confirmed"; then
            record_pass "MCUboot image confirmation detected"
        fi
        if echo "$serial_out" | grep -qi "watchdog\|wdt.*disabled\|wdt.*stopped"; then
            record_pass "WDT lifecycle messages detected"
        fi
    else
        record_skip "Serial console (no device)"
    fi

    # 1b. Key matrix
    header "1b. Key Matrix"
    manual_check "Key presses register as USB HID input (test with xev/evtest/showkey)"
    manual_check "Multiple keys across different rows/columns work"
    manual_check "No ghost keys or stuck keys"

    # 1c. mcumgr DFU
    header "1c. mcumgr DFU"
    if command -v mcumgr &>/dev/null && [[ -e "$SERIAL_DEV" ]]; then
        info "Querying MCUboot image list..."
        local img_list
        img_list=$(mcumgr $MCUMGR_CONN image list 2>&1) || true
        if echo "$img_list" | grep -qi "slot=0\|image=0\|hash="; then
            record_pass "mcumgr image list returns slot0 image"
            echo "$img_list" | head -15 | while read -r line; do
                info "  $line"
            done
        else
            record_fail "mcumgr image list failed"
            info "Output: $img_list"
        fi

        # Check OS echo (basic mcumgr connectivity)
        info "Testing mcumgr OS echo..."
        local echo_result
        echo_result=$(mcumgr $MCUMGR_CONN echo hello 2>&1) || true
        if echo "$echo_result" | grep -qi "hello"; then
            record_pass "mcumgr echo works"
        else
            warn "mcumgr echo failed: $echo_result"
        fi
    else
        if ! command -v mcumgr &>/dev/null; then
            record_skip "mcumgr not installed"
        else
            record_skip "mcumgr (no serial device)"
        fi
    fi

    # 1d. WDT crash revert (manual — dangerous, only on first bring-up)
    header "1d. WDT Crash Revert"
    info "This test requires building a deliberately broken image."
    info "See docs/zmk-firmware.md 'Stage 1d' for instructions."
    info "Only run this once during initial bring-up."
    manual_check "WDT crash revert tested and working (MCUboot swaps back after 10s)"

    summary
}

# ─── STAGE 2: BLE ─────────────────────────────────────────────────────────────

cmd_stage2() {
    header "Stage 2: BLE"

    # Regression: USB still works
    header "Regression: USB"
    if lsusb 2>/dev/null | grep -qi "$ZMK_PRODUCT\|${ZMK_USB_VID}:"; then
        record_pass "USB device still enumerated (regression OK)"
    else
        record_fail "USB device NOT found (regression failure!)"
    fi
    if [[ -e "$SERIAL_DEV" ]]; then
        record_pass "Serial device still present (regression OK)"
    else
        record_fail "Serial device gone (regression failure!)"
    fi

    # Regression: mcumgr
    if command -v mcumgr &>/dev/null && [[ -e "$SERIAL_DEV" ]]; then
        local img_list
        img_list=$(mcumgr $MCUMGR_CONN image list 2>&1) || true
        if echo "$img_list" | grep -qi "slot=0\|image=0"; then
            record_pass "mcumgr still works (regression OK)"
        else
            record_fail "mcumgr broken (regression failure!)"
        fi
    fi

    # BLE advertising — scan via host bluetoothctl
    header "BLE Advertising"
    if host_exec which bluetoothctl &>/dev/null; then
        info "Scanning for BLE advertisements (10 seconds)..."
        local scan_out
        scan_out=$(timeout 12 host_exec bluetoothctl --timeout 10 scan on 2>&1 || true)
        if echo "$scan_out" | grep -qi "Rainy\|rainy"; then
            record_pass "BLE advertising detected: '$ZMK_PRODUCT'"
            echo "$scan_out" | grep -i "rainy" | while read -r line; do
                info "  $line"
            done
        else
            warn "BLE advertisement for '$ZMK_PRODUCT' not found in scan"
            info "Check serial logs for BLE init messages"
            info "Try manually on host: bluetoothctl scan on"
        fi
    else
        info "bluetoothctl not available — use phone BLE scanner app"
    fi

    manual_check "BLE advertises '$ZMK_PRODUCT' (visible in BLE scanner)"
    manual_check "BLE pairing succeeds (laptop or phone)"
    manual_check "BLE HID input works — key presses arrive over BLE"
    manual_check "USB/BLE switching works (plug/unplug USB cable)"
    manual_check "BLE connection stable over 5+ minutes of use"
    manual_check "BLE reconnects after keyboard power cycle"

    # Serial log check for BLE init
    header "Serial Log: BLE Init"
    if [[ -e "$SERIAL_DEV" ]]; then
        info "Capturing serial output for 3 seconds (press a key to trigger activity)..."
        local serial_out
        serial_out=$(timeout 3 cat "$SERIAL_DEV" 2>/dev/null || true)
        if echo "$serial_out" | grep -qi "bt_hci\|ble\|advertising\|bluetooth"; then
            record_pass "BLE-related messages in serial output"
        else
            info "No BLE messages captured (may need reset to see init)"
        fi
    fi

    summary
}

# ─── STAGE 3: RGB Underglow ───────────────────────────────────────────────────

cmd_stage3() {
    header "Stage 3: RGB Underglow"

    # Regression: USB + BLE
    header "Regression: USB + BLE"
    if lsusb 2>/dev/null | grep -qi "$ZMK_PRODUCT\|${ZMK_USB_VID}:"; then
        record_pass "USB device still enumerated (regression OK)"
    else
        record_fail "USB device NOT found (regression failure!)"
    fi
    if [[ -e "$SERIAL_DEV" ]]; then
        record_pass "Serial device still present (regression OK)"
    else
        record_fail "Serial device gone (regression failure!)"
    fi
    manual_check "BLE still works (regression check)"

    # RGB checks (all manual — can't automate LED visual inspection)
    header "RGB LED Verification"
    manual_check "112 WS2812 LEDs light up on boot (ZMK default underglow effect)"
    manual_check "ZMK RGB brightness up/down works"
    manual_check "ZMK RGB effect cycle works"
    manual_check "ZMK RGB hue/saturation controls work"
    manual_check "ZMK RGB off command works (LEDs turn off completely)"
    manual_check "No visible flicker or color artifacts (SPI timing)"

    # Serial log check
    header "Serial Log: RGB"
    if [[ -e "$SERIAL_DEV" ]]; then
        info "Capturing serial output for 3 seconds..."
        local serial_out
        serial_out=$(timeout 3 cat "$SERIAL_DEV" 2>/dev/null || true)
        if echo "$serial_out" | grep -qi "dma\|spi\|led_strip\|rgb"; then
            info "LED/SPI-related messages found in serial output"
        fi
        if echo "$serial_out" | grep -qi "error\|fail"; then
            record_fail "Error messages in serial output"
            echo "$serial_out" | grep -i "error\|fail" | while read -r line; do
                info "  $line"
            done
        else
            record_pass "No DMA/SPI errors in serial output"
        fi
    fi

    summary
}

# ─── STAGE 4: Battery ADC ─────────────────────────────────────────────────────

cmd_stage4() {
    header "Stage 4: Battery ADC"

    # Regression
    header "Regression: USB + BLE + RGB"
    if lsusb 2>/dev/null | grep -qi "$ZMK_PRODUCT\|${ZMK_USB_VID}:"; then
        record_pass "USB device still enumerated (regression OK)"
    else
        record_fail "USB device NOT found (regression failure!)"
    fi
    manual_check "BLE and RGB still work (regression check)"

    # ADC scan mode (Step A)
    header "Step A: ADC Channel Scan"
    info "If CONFIG_BATTERY_B91_ADC_SCAN=y is enabled:"
    info "The serial console should show voltages for all 10 ADC channels + VBAT/3"
    if [[ -e "$SERIAL_DEV" ]]; then
        info "Capturing serial output for 5 seconds (device may need reset)..."
        local serial_out
        serial_out=$(timeout 5 cat "$SERIAL_DEV" 2>/dev/null || true)
        if echo "$serial_out" | grep -qi "adc\|channel\|voltage\|battery\|mV\|scan"; then
            record_pass "ADC-related messages found in serial output"
            echo "$serial_out" | grep -i "adc\|channel\|voltage\|battery\|mV\|scan" | while read -r line; do
                info "  $line"
            done
        else
            info "No ADC messages captured — device may need reset to see boot scan"
        fi
    fi

    manual_check "ADC scan output visible on serial console (after reset)"
    manual_check "One channel shows battery voltage (3.0-4.2V range)"

    # Sensor mode (Step B)
    header "Step B: Battery Sensor"
    info "After identifying the correct ADC channel and enabling CONFIG_BATTERY_B91_ADC=y:"

    manual_check "Serial logs show battery voltage readings"
    manual_check "Battery percentage reported over BLE (phone shows battery level)"
    manual_check "Voltage changes when charging vs discharging"
    manual_check "Percentage is reasonable (not stuck at 0% or 100%)"

    summary
}

# ─── STAGE 5: Deep Sleep PM ───────────────────────────────────────────────────

cmd_stage5() {
    header "Stage 5: Deep Sleep Power Management"

    # Regression
    header "Regression: All Features"
    if lsusb 2>/dev/null | grep -qi "$ZMK_PRODUCT\|${ZMK_USB_VID}:"; then
        record_pass "USB device still enumerated (regression OK)"
    else
        record_fail "USB device NOT found (regression failure!)"
    fi
    manual_check "BLE, RGB, and battery still work (regression check)"

    # Sleep/wake tests (all manual)
    header "Sleep/Wake Cycle"
    manual_check "Keyboard enters deep sleep after idle timeout"
    manual_check "Keypress wakes keyboard promptly"
    manual_check "USB re-enumerates after wake (if connected)"
    manual_check "BLE reconnects after wake"
    manual_check "RGB resumes correct state after wake"
    manual_check "Battery reporting still works after wake"
    manual_check "No data loss or stuck keys after wake"

    # Stress test
    header "Reliability"
    manual_check "3+ consecutive sleep/wake cycles work reliably"
    manual_check "Sleep works from both USB and BLE modes"

    summary
}

# ─── HELP ──────────────────────────────────────────────────────────────────────

cmd_help() {
    cat <<'EOF'
Usage: verify_stage.sh <command>

Commands:
  prereqs    Check all prerequisite tools and dependencies
  0          Stage 0: EVK connection + SWS validation
  1          Stage 1: USB enumeration + serial console + DFU
  2          Stage 2: BLE advertising + pairing + HID
  3          Stage 3: RGB underglow (112 LEDs)
  4          Stage 4: Battery ADC (scan + sensor)
  5          Stage 5: Deep sleep power management

Environment variables:
  SERIAL_DEV     Serial device path (default: /dev/ttyACM0)

Each stage includes:
  - Automated checks (USB detection, serial output, mcumgr queries)
  - Regression tests (previous stages still work)
  - Guided manual checks (y/n/s prompts)

Run 'prereqs' first, then stages in order (0 → 1 → 2 → 3 → 4 → 5).
Do not skip stages — each depends on the previous one passing.
EOF
}

# ─── MAIN ──────────────────────────────────────────────────────────────────────

case "${1:-help}" in
    prereqs|pre)  cmd_prereqs ;;
    0|stage0)     cmd_stage0 ;;
    1|stage1)     cmd_stage1 ;;
    2|stage2)     cmd_stage2 ;;
    3|stage3)     cmd_stage3 ;;
    4|stage4)     cmd_stage4 ;;
    5|stage5)     cmd_stage5 ;;
    help|--help|-h) cmd_help ;;
    *)            echo "Unknown command: $1"; cmd_help; exit 1 ;;
esac
