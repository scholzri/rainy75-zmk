#!/usr/bin/env bash
#
# sws_flash.sh — Telink Burning EVK helper for Wobkey Rainy 75 Pro ISO DE
#
# Wraps BDT commands for Stage 0 (dump/verify/round-trip) and Stage 1+ (flash ZMK).
# Requires: BDT v2.2.x in ../tools/bdt/release/bdt
#
# Usage:
#   ./sws_flash.sh setup          # Install udev rules, check EVK connection
#   ./sws_flash.sh evk-version    # Query EVK firmware version
#   ./sws_flash.sh evk-upgrade    # Upgrade EVK firmware to v4.7
#   ./sws_flash.sh dump           # Stage 0a: Triple-read 1MB flash, verify checksums
#   ./sws_flash.sh analyze        # Stage 0b: Analyze dump, extract calibration
#   ./sws_flash.sh roundtrip      # Stage 0c: Write-back and verify round-trip
#   ./sws_flash.sh flash <file>   # Flash firmware (preserves calibration+MAC)
#   ./sws_flash.sh read <addr> <size> [-o file]  # Read arbitrary flash region
#   ./sws_flash.sh sram <addr> <size> [-o file]  # Read SRAM region
#   ./sws_flash.sh restore        # Restore original firmware from backup
#
set -euo pipefail

# --- Paths ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BDT="$SCRIPT_DIR/bdt/release/bdt"
FW_DIR="$PROJECT_ROOT/reverse/firmware"
UDEV_RULES="$SCRIPT_DIR/bdt/99-telink-evk.rules"

# --- Chip selector ---
# TLSR9511 = B91 family. BDT config maps both "B91" and "9518" to same DUT binary.
# Try B91 first; if it fails, user can set CHIP=9518 in environment.
CHIP="${CHIP:-B91}"

# --- Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
die()   { err "$@"; exit 1; }

# --- Helpers ---
check_bdt() {
    [[ -x "$BDT" ]] || die "BDT not found at $BDT — run 'cd $SCRIPT_DIR/bdt && unzip Telink-BDT-Linux-X64-2.2.1.zip'"
}

run_bdt() {
    info "Running: $BDT $CHIP $*"
    "$BDT" "$CHIP" "$@"
}

activate_chip() {
    info "Activating chip (waking from deep sleep)..."
    run_bdt ac || warn "Activate failed — chip may already be awake, or EVK not connected"
}

check_sws() {
    info "Testing SWS connection..."
    if run_bdt sws; then
        ok "SWS connection OK"
    else
        die "SWS connection failed. Check wiring: SWM→SWS(pad3), GND→GND(pad1), 3.3V→VCC(pad2). Wireless switch OFF."
    fi
}

sha256() {
    sha256sum "$1" | cut -d' ' -f1
}

# --- Commands ---

cmd_setup() {
    info "=== EVK Setup ==="

    check_bdt
    ok "BDT found: $BDT"

    # Check dependencies
    if ldd "$BDT" 2>&1 | grep -q "not found"; then
        err "Missing shared libraries:"
        ldd "$BDT" 2>&1 | grep "not found"
        die "Install missing dependencies"
    fi
    ok "All BDT dependencies satisfied"

    # Install udev rules
    if [[ -f "$UDEV_RULES" ]]; then
        if [[ -f /etc/udev/rules.d/99-telink-evk.rules ]]; then
            ok "udev rules already installed"
        else
            info "Installing udev rules (requires sudo)..."
            sudo cp "$UDEV_RULES" /etc/udev/rules.d/
            sudo udevadm control --reload-rules
            ok "udev rules installed — replug EVK for rules to take effect"
        fi
    fi

    # Check if EVK is connected
    if lsusb 2>/dev/null | grep -qi "248a:826[ab]"; then
        ok "Burning EVK detected on USB"
        run_bdt sws && ok "SWS link OK" || warn "SWS failed — target board may not be connected"
    else
        warn "Burning EVK not detected on USB (ID 248a:826a/826b)"
        info "Connect the EVK and run this command again"
    fi

    echo
    info "Setup complete. Next steps:"
    info "  1. Connect EVK: SWM→SWS(pad3), GND→GND(pad1), 3.3V→VCC(pad2)"
    info "  2. Turn wireless switch OFF (under CapsLock)"
    info "  3. Run: $0 dump"
}

cmd_evk_version() {
    check_bdt
    info "Querying EVK firmware version..."
    # EVK uses 8266 bridge chip
    "$BDT" 8266 up -ev
}

cmd_evk_upgrade() {
    check_bdt
    local fw="$SCRIPT_DIR/bdt/release/fw/Firmware_v4.7.bin"
    [[ -f "$fw" ]] || die "EVK firmware not found: $fw"

    warn "This will upgrade the Burning EVK firmware to v4.7"
    warn "Required for BDT v2.2.0+. Do NOT interrupt!"
    read -rp "Proceed? [y/N] " confirm
    [[ "$confirm" =~ ^[yY]$ ]] || die "Aborted"

    info "Upgrading EVK firmware..."
    "$BDT" 8266 up -i "$fw" -ev
    ok "EVK firmware upgraded. Replug the EVK."
}

cmd_dump() {
    check_bdt
    info "=== Stage 0a: Triple Flash Dump ==="
    info "Reading full 1MB flash three times for verification..."
    echo

    mkdir -p "$FW_DIR"
    activate_chip

    for i in 1 2 3; do
        local out="$FW_DIR/original_dump_${i}.bin"
        info "Read $i/3 → $out"
        run_bdt rf 0x00 -s 1024k -o "$out"

        # Verify size
        local size
        size=$(stat -c%s "$out")
        if [[ "$size" -ne 1048576 ]]; then
            die "Dump $i size is $size bytes (expected 1048576)"
        fi
        ok "Read $i complete: $size bytes"
        echo
    done

    # Compare checksums
    info "Comparing checksums..."
    local h1 h2 h3
    h1=$(sha256 "$FW_DIR/original_dump_1.bin")
    h2=$(sha256 "$FW_DIR/original_dump_2.bin")
    h3=$(sha256 "$FW_DIR/original_dump_3.bin")

    echo "  Dump 1: $h1"
    echo "  Dump 2: $h2"
    echo "  Dump 3: $h3"

    if [[ "$h1" == "$h2" && "$h2" == "$h3" ]]; then
        ok "All three checksums match!"
        cp "$FW_DIR/original_dump_1.bin" "$FW_DIR/original_full_flash.bin"
        ok "Saved verified dump as: $FW_DIR/original_full_flash.bin"
        ok "SHA-256: $h1"

        # Clean up redundant copies
        info "Keeping all 3 dumps for reference"
        echo
        info "Stage 0a PASSED. Next: $0 analyze"
    else
        err "CHECKSUMS DO NOT MATCH!"
        err "SWS connection is unreliable. Check:"
        err "  - Wiring quality (short wires, good solder joints)"
        err "  - SWS speed (try: CHIP=9518 $0 dump)"
        err "  - Signal integrity (probe SWS with logic analyzer)"
        die "Do NOT proceed until all three reads match"
    fi
}

cmd_analyze() {
    check_bdt
    info "=== Stage 0b: Dump Analysis ==="

    local dump="$FW_DIR/original_full_flash.bin"
    [[ -f "$dump" ]] || die "No dump found. Run '$0 dump' first."

    local size
    size=$(stat -c%s "$dump")
    info "Dump: $dump ($size bytes)"
    echo

    # Check known locations
    info "--- Known Flash Regions ---"

    # Boot code at 0x0
    local boot_magic
    boot_magic=$(xxd -p -l 4 "$dump" | head -c8)
    info "Boot region (0x00000): magic=$boot_magic"

    # Application code — check for TLNK header
    local app_offset=0x08000
    local tlnk_check
    tlnk_check=$(xxd -p -s $((app_offset + 0x20)) -l 4 "$dump" | head -c8)
    if [[ "$tlnk_check" == "4b4e4c54" ]]; then
        ok "TLNK header found at 0x$(printf '%05X' $((app_offset + 0x20))) (application code)"
    else
        warn "No TLNK header at expected app offset 0x$app_offset+0x20"
    fi

    # VIA keymaps at 0x84000
    info ""
    info "--- VIA Keymaps ---"
    for layer in 0 1 2; do
        local addr=$((0x84000 + layer * 0x1000))
        local first16
        first16=$(xxd -p -s "$addr" -l 16 "$dump")
        local is_empty=true
        if [[ "$first16" != "$(printf '%032s' | tr ' ' 'f')" ]]; then
            is_empty=false
        fi
        info "  Layer $layer (0x$(printf '%05X' "$addr")): ${first16}$(if $is_empty; then echo ' [EMPTY/FF]'; fi)"
    done

    # Macros at 0x89000
    local macro_first
    macro_first=$(xxd -p -s $((0x89000)) -l 16 "$dump")
    info "  Macros (0x89000): $macro_first"

    # Calibration at 0xFE000
    info ""
    info "--- Calibration & MAC ---"
    local cal_data
    cal_data=$(xxd -p -s $((0xFE000)) -l 32 "$dump")
    info "  Calibration (0xFE000): $cal_data"

    local mac_data
    mac_data=$(xxd -p -s $((0xFF000)) -l 6 "$dump")
    info "  BLE MAC (0xFF000): $mac_data"
    info "  MAC address: $(echo "$mac_data" | sed 's/\(..\)/\1:/g;s/:$//' | tr a-f A-F)"

    # Extract calibration+MAC pages
    info ""
    info "--- Extracting Critical Regions ---"
    dd if="$dump" of="$FW_DIR/calibration_0xFE000.bin" bs=1 skip=$((0xFE000)) count=8192 2>/dev/null
    ok "Calibration+MAC saved: $FW_DIR/calibration_0xFE000.bin (8192 bytes, 0xFE000-0xFFFFF)"

    # Compare with existing OTA-extracted firmware
    info ""
    info "--- OTA Image Comparison ---"
    local ota="$FW_DIR/firmware_extracted.bin"
    if [[ -f "$ota" ]]; then
        local ota_size
        ota_size=$(stat -c%s "$ota")
        info "OTA image: $ota ($ota_size bytes)"

        # The OTA image starts at flash offset 0x08000 (after boot code)
        # But it includes the Telink OTA header — the actual code starts after
        # the boot header. Let's check if the OTA image matches starting at 0x08000.
        local flash_region="$FW_DIR/_tmp_flash_region.bin"
        dd if="$dump" of="$flash_region" bs=1 skip=$((0x08000)) count="$ota_size" 2>/dev/null

        if cmp -s "$ota" "$flash_region"; then
            ok "OTA image matches flash at offset 0x08000 (byte-for-byte)"
        else
            # Try to find the OTA image somewhere in the dump
            info "OTA image does NOT match at 0x08000 — searching..."
            # Check first 64 bytes of OTA image
            local ota_head
            ota_head=$(xxd -p -l 64 "$ota")
            local found=false
            for offset in 0x0 0x1000 0x2000 0x4000 0x8000 0x10000; do
                local region_head
                region_head=$(xxd -p -s "$offset" -l 64 "$dump")
                if [[ "$ota_head" == "$region_head" ]]; then
                    ok "OTA image header found at flash offset 0x$(printf '%X' "$offset")"
                    found=true
                    break
                fi
            done
            if ! $found; then
                warn "OTA image header not found at common offsets"
                info "This is expected if the OTA format differs from raw flash layout"
            fi
        fi
        rm -f "$flash_region"
    else
        warn "No OTA image found at $ota — skip comparison"
    fi

    echo
    ok "Stage 0b analysis complete."
    info "Review the output above. Key things to verify:"
    info "  - TLNK header present in application region"
    info "  - VIA keymap layers contain data (not all 0xFF)"
    info "  - Calibration region is NOT all 0xFF (would indicate uncalibrated chip)"
    info "  - MAC address looks valid (not 00:00:00:00:00:00 or FF:FF:FF:FF:FF:FF)"
    echo
    info "Next: $0 roundtrip"
}

cmd_roundtrip() {
    check_bdt
    info "=== Stage 0c: Write/Read Round-Trip Test ==="

    local dump="$FW_DIR/original_full_flash.bin"
    [[ -f "$dump" ]] || die "No dump found. Run '$0 dump' first."

    warn "This will ERASE and REWRITE the entire flash with the original firmware."
    warn "The keyboard will be non-functional during the write."
    warn "Calibration and MAC will be preserved (they're part of the original dump)."
    read -rp "Proceed? [y/N] " confirm
    [[ "$confirm" =~ ^[yY]$ ]] || die "Aborted"

    activate_chip

    # Write full 1MB back
    info "Writing 1MB flash (this takes ~30-60 seconds)..."
    run_bdt wf 0x00 -i "$dump" -e
    ok "Flash write complete"

    # Read back
    info "Reading back for verification..."
    local readback="$FW_DIR/original_readback.bin"
    run_bdt rf 0x00 -s 1024k -o "$readback"
    ok "Readback complete"

    # Compare
    local h_orig h_back
    h_orig=$(sha256 "$dump")
    h_back=$(sha256 "$readback")

    echo "  Original: $h_orig"
    echo "  Readback: $h_back"

    if [[ "$h_orig" == "$h_back" ]]; then
        ok "ROUND-TRIP VERIFIED — checksums match!"
        echo
        ok "=== Stage 0 COMPLETE ==="
        info "The SWS toolchain is fully validated. You can now proceed to Stage 1."
        info ""
        info "Next steps:"
        info "  1. Build MCUboot + ZMK app (see docs/zmk-firmware.md Stage 1)"
        info "  2. Flash: $0 flash <combined_image.bin>"
        info ""
        info "To restore the original firmware at any time:"
        info "  $0 restore"
    else
        err "ROUND-TRIP FAILED — checksums differ!"
        err "Original: $h_orig"
        err "Readback: $h_back"
        die "SWS write or read is unreliable. Do NOT proceed to Stage 1."
    fi
}

cmd_flash() {
    check_bdt
    local firmware="$1"
    [[ -f "$firmware" ]] || die "Firmware file not found: $firmware"

    local fw_size
    fw_size=$(stat -c%s "$firmware")
    info "=== Flash Firmware ==="
    info "File: $firmware ($fw_size bytes)"

    # Safety check: firmware must not overwrite calibration/MAC region
    if [[ "$fw_size" -gt $((0xFE000)) ]]; then
        die "Firmware too large ($fw_size bytes) — would overwrite calibration at 0xFE000!"
    fi

    # Check we have backup
    if [[ ! -f "$FW_DIR/original_full_flash.bin" ]]; then
        warn "No original firmware backup found!"
        warn "It is STRONGLY recommended to run '$0 dump' before flashing custom firmware."
        read -rp "Flash anyway? [y/N] " confirm
        [[ "$confirm" =~ ^[yY]$ ]] || die "Aborted. Run '$0 dump' first."
    fi

    # Check we have calibration backup
    if [[ ! -f "$FW_DIR/calibration_0xFE000.bin" ]]; then
        warn "No calibration backup found!"
        warn "Run '$0 analyze' to extract calibration data before flashing."
        read -rp "Flash anyway? [y/N] " confirm
        [[ "$confirm" =~ ^[yY]$ ]] || die "Aborted"
    fi

    warn "This will erase and write flash from 0x00000 to 0x$(printf '%05X' "$fw_size")"
    warn "Calibration (0xFE000) and MAC (0xFF000) will NOT be touched."
    read -rp "Proceed? [y/N] " confirm
    [[ "$confirm" =~ ^[yY]$ ]] || die "Aborted"

    activate_chip

    info "Flashing $fw_size bytes..."
    run_bdt wf 0x00 -i "$firmware" -e
    ok "Flash complete"

    # Verify
    info "Reading back for verification..."
    local readback="$FW_DIR/_flash_readback.bin"
    run_bdt rf 0x00 -s "$fw_size" -o "$readback"

    local h_fw h_rb
    h_fw=$(sha256 "$firmware")
    h_rb=$(sha256 "$readback")

    if [[ "$h_fw" == "$h_rb" ]]; then
        ok "Flash verified — firmware matches!"
        rm -f "$readback"
    else
        err "Flash verification FAILED"
        err "  Written: $h_fw"
        err "  Readback: $h_rb"
        warn "Readback saved: $readback"
        die "Flash may be corrupted. Use '$0 restore' to recover."
    fi

    info "Resetting chip..."
    run_bdt rst
    ok "Done. The keyboard should enumerate in ~3 seconds."
}

cmd_read() {
    check_bdt
    local addr="$1"
    local size="$2"
    shift 2

    activate_chip
    run_bdt rf "$addr" -s "$size" "$@"
}

cmd_sram() {
    check_bdt
    local addr="$1"
    local size="$2"
    shift 2

    activate_chip
    run_bdt rc "$addr" -s "$size" "$@"
}

cmd_restore() {
    check_bdt
    local dump="$FW_DIR/original_full_flash.bin"
    [[ -f "$dump" ]] || die "No backup found at $dump — cannot restore"

    info "=== Restore Original Firmware ==="
    warn "This will erase the ENTIRE flash and write the original 1MB dump."
    read -rp "Proceed? [y/N] " confirm
    [[ "$confirm" =~ ^[yY]$ ]] || die "Aborted"

    activate_chip

    info "Writing 1MB original firmware..."
    run_bdt wf 0x00 -i "$dump" -e
    ok "Write complete"

    info "Verifying..."
    local readback="$FW_DIR/_restore_readback.bin"
    run_bdt rf 0x00 -s 1024k -o "$readback"

    local h_orig h_back
    h_orig=$(sha256 "$dump")
    h_back=$(sha256 "$readback")

    if [[ "$h_orig" == "$h_back" ]]; then
        ok "Restore verified — original firmware intact!"
        rm -f "$readback"
        run_bdt rst
        ok "Keyboard reset. It should work normally now."
    else
        err "Restore verification FAILED"
        err "  Original: $h_orig"
        err "  Readback: $h_back"
        die "Flash may be corrupted. Try again or check SWS connection."
    fi
}

cmd_help() {
    echo "Usage: $0 <command> [args...]"
    echo ""
    echo "EVK Management:"
    echo "  setup          Install udev rules, verify EVK connection"
    echo "  evk-version    Query Burning EVK firmware version"
    echo "  evk-upgrade    Upgrade EVK firmware to v4.7 (required for BDT 2.2+)"
    echo ""
    echo "Stage 0 (Original Firmware Backup):"
    echo "  dump           Read 1MB flash 3x, verify checksums"
    echo "  analyze        Analyze dump: keymaps, calibration, MAC, OTA comparison"
    echo "  roundtrip      Write original back + readback verify"
    echo ""
    echo "Flashing:"
    echo "  flash <file>   Flash firmware (preserves calibration+MAC at 0xFE000+)"
    echo "  restore        Restore original firmware from backup"
    echo ""
    echo "Low-level:"
    echo "  read <addr> <size> [-o file]   Read flash region"
    echo "  sram <addr> <size> [-o file]   Read SRAM region"
    echo ""
    echo "Environment:"
    echo "  CHIP=B91       Chip selector (default: B91, try 9518 if B91 fails)"
    echo ""
    echo "Files:"
    echo "  BDT binary:     $BDT"
    echo "  Firmware dir:    $FW_DIR"
    echo "  udev rules:     $UDEV_RULES"
}

# --- Main ---
case "${1:-help}" in
    setup)       cmd_setup ;;
    evk-version) cmd_evk_version ;;
    evk-upgrade) cmd_evk_upgrade ;;
    dump)        cmd_dump ;;
    analyze)     cmd_analyze ;;
    roundtrip)   cmd_roundtrip ;;
    flash)       cmd_flash "${2:?Usage: $0 flash <firmware.bin>}" ;;
    read)        cmd_read "${2:?addr}" "${3:?size}" "${@:4}" ;;
    sram)        cmd_sram "${2:?addr}" "${3:?size}" "${@:4}" ;;
    restore)     cmd_restore ;;
    help|--help|-h) cmd_help ;;
    *)           err "Unknown command: $1"; cmd_help; exit 1 ;;
esac
