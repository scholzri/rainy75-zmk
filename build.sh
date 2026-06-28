#!/bin/bash
# Build ZMK firmware for Rainy 75 Pro
# Usage: ./build.sh [-p] [-v] [-m] [-c] [-o] [-b] [-a] (--iso | --ansi)
#   -p  pristine build (clean rebuild)
#   -v  verbose output
#   -m  build MCUboot bootloader
#   -c  create combined flash image (MCUboot + app)
#   -o  create OTA-ready image (combined + Telink header + CRC32)
#   -b  build OTA bridge (monolithic, for stock-to-ZMK transition)
#   -a  all: MCUboot + app + combined + OTA + bridge
#   --iso / --ansi   physical layout, REQUIRED for any app build (no default)
#                    e.g. ./build.sh -pa --iso   or   ./build.sh -pa --ansi

set -e

cd "$(dirname "$0")"

source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR="$(pwd)/toolchain/zephyr-sdk-0.17.0"

PRISTINE=""
VERBOSE_CMAKE=""
BUILD_MCUBOOT=0
BUILD_COMBINED=0
BUILD_OTA=0
BUILD_BRIDGE=0
BUILD_APP=1
LAYOUT=""             # "iso" or "ansi" — REQUIRED for app builds, no default
ANSI_DTFLAG=""        # set when LAYOUT=ansi

# ── Apply upstream patches if needed ──────────────────────────
apply_patches() {
    local repo="$1" dir="$2"
    for patch in "patches/$repo"/*.patch; do
        [ -f "$patch" ] || continue
        if git -C "$dir" apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            continue  # already applied
        fi
        echo "Applying patch: $repo/$(basename "$patch")"
        git -C "$dir" am --3way "$PWD/$patch" || {
            echo "WARNING: Patch failed — may need manual resolution." >&2
            git -C "$dir" am --abort 2>/dev/null || true
        }
    done
}
apply_patches zephyr zephyr
apply_patches mcuboot bootloader/mcuboot
apply_patches hal_telink modules/hal/hal_telink
apply_patches zmk-src zmk-src

# ── Fetch the (non-redistributable) Telink BLE blob if missing ──
./fetch_ble_blob.sh

# Allow long forms --iso / --ansi as aliases for -I / -A.
ARGS=(); for a in "$@"; do case "$a" in
    --iso)  ARGS+=("-I");;
    --ansi) ARGS+=("-A");;
    *)      ARGS+=("$a");;
esac; done
set -- "${ARGS[@]}"

while getopts "pvmcobaIA" opt; do
    case $opt in
        p) PRISTINE="-p" ;;
        v) VERBOSE_CMAKE="-DCMAKE_VERBOSE_MAKEFILE=ON" ;;
        m) BUILD_MCUBOOT=1; BUILD_APP=0 ;;
        c) BUILD_COMBINED=1 ;;
        o) BUILD_OTA=1 ;;
        b) BUILD_BRIDGE=1; BUILD_APP=0 ;;
        a) BUILD_MCUBOOT=1; BUILD_COMBINED=1; BUILD_OTA=1; BUILD_BRIDGE=1; BUILD_APP=1 ;;
        I) [ "$LAYOUT" = ansi ] && { echo "Error: --iso and --ansi are mutually exclusive" >&2; exit 1; }; LAYOUT="iso" ;;
        A) [ "$LAYOUT" = iso  ] && { echo "Error: --iso and --ansi are mutually exclusive" >&2; exit 1; }; LAYOUT="ansi"; ANSI_DTFLAG="-DDTS_EXTRA_CPPFLAGS=-DRAINY75_ANSI" ;;
        *) echo "Usage: $0 [-p] [-v] [-m] [-c] [-o] [-b] [-a] (--iso | --ansi)"; exit 1 ;;
    esac
done

# The app build needs an explicit physical layout — no silent default (an ANSI
# owner must not get an ISO build by accident, and vice-versa).
if [ "$BUILD_APP" -eq 1 ] && [ -z "$LAYOUT" ]; then
    echo "Error: choose a layout for the app build: --iso or --ansi" >&2
    echo "  ./build.sh -pa --iso    # ISO DE  (the original board)" >&2
    echo "  ./build.sh -pa --ansi   # ANSI    (community-verified)" >&2
    exit 1
fi

# ── MCUboot build ──────────────────────────────────────────────
if [ "$BUILD_MCUBOOT" -eq 1 ]; then
    echo "=== Building MCUboot ==="
    west build $PRISTINE -b rainy75 -d build-mcuboot \
        bootloader/mcuboot/boot/zephyr -- \
        -DEXTRA_CONF_FILE="$(pwd)/conf/mcuboot.conf" \
        -DEXTRA_DTC_OVERLAY_FILE="$(pwd)/conf/mcuboot.overlay" \
        "-DDTS_ROOT=$(pwd)/zmk;$(pwd)/zmk-src/app" \
        "-DZMK_EXTRA_MODULES=$(pwd)/zmk;$(pwd)/zmk-src/app" \
        $VERBOSE_CMAKE

    SIZE=$(stat -c%s build-mcuboot/zephyr/zephyr.bin)
    echo "MCUboot binary: $SIZE bytes (max 65536 for 64KB boot partition)"
    if [ "$SIZE" -gt 65536 ]; then
        echo "ERROR: MCUboot exceeds 64KB boot partition!" >&2
        exit 1
    fi
fi

# ── App build ──────────────────────────────────────────────────
if [ "$BUILD_APP" -eq 1 ]; then
    echo "=== Building ZMK app (${LAYOUT^^} layout) ==="
    west build $PRISTINE -b rainy75 zmk-src/app -- \
        -DZMK_CONFIG="$(pwd)/zmk/boards/rainy75" \
        -DZMK_EXTRA_MODULES="$(pwd)/zmk" \
        -DEXTRA_CONF_FILE="$(pwd)/conf/app.conf" \
        -DEXTRA_DTC_OVERLAY_FILE="$(pwd)/zmk/boards/rainy75/rainy75.keymap;$(pwd)/conf/mcumgr.overlay" \
        $ANSI_DTFLAG \
        $VERBOSE_CMAKE
fi

# ── Combined image ─────────────────────────────────────────────
if [ "$BUILD_COMBINED" -eq 1 ]; then
    echo "=== Creating combined flash image ==="
    if [ ! -f build-mcuboot/zephyr/zephyr.bin ]; then
        echo "ERROR: MCUboot binary not found. Build with -m first." >&2
        exit 1
    fi
    if [ ! -f build/zephyr/zmk.signed.bin ]; then
        echo "ERROR: Signed app binary not found. Build app first." >&2
        exit 1
    fi

    python3 -c "
mcuboot = open('build-mcuboot/zephyr/zephyr.bin','rb').read()
app = open('build/zephyr/zmk.signed.bin','rb').read()
pad = 0x10000 - len(mcuboot)  # 64KB boot partition
assert pad > 0, f'MCUboot too large: {len(mcuboot)} bytes'
combined = mcuboot + (b'\xff' * pad) + app
open('build/combined.bin','wb').write(combined)
print(f'MCUboot:  {len(mcuboot):,} bytes')
print(f'App:      {len(app):,} bytes (at offset 0x10000)')
print(f'Combined: {len(combined):,} bytes')
"
    echo "Output: build/combined.bin"
fi

# ── OTA image ─────────────────────────────────────────────────
if [ "$BUILD_OTA" -eq 1 ]; then
    echo "=== Creating OTA-ready image ==="
    if [ ! -f build/combined.bin ]; then
        echo "ERROR: Combined image not found. Build with -c first." >&2
        exit 1
    fi

    python3 reverse/tools/prepare_ota.py build/combined.bin -o build/combined_ota.bin
fi

# ── Bridge build (monolithic, no MCUboot) ─────────────────────
if [ "$BUILD_BRIDGE" -eq 1 ]; then
    echo "=== Building OTA bridge ==="
    west build $PRISTINE -b rainy75 -d build-bridge zmk-src/app -- \
        -DZMK_CONFIG="$(pwd)/zmk/boards/rainy75" \
        -DZMK_EXTRA_MODULES="$(pwd)/zmk" \
        -DEXTRA_CONF_FILE="$(pwd)/conf/ota-bridge.conf" \
        -DEXTRA_DTC_OVERLAY_FILE="$(pwd)/zmk/boards/rainy75/rainy75.keymap;$(pwd)/conf/mcumgr.overlay" \
        $VERBOSE_CMAKE

    if [ ! -f build-bridge/zephyr/zmk.bin ]; then
        echo "ERROR: Bridge binary not found after build." >&2
        exit 1
    fi

    SIZE=$(stat -c%s build-bridge/zephyr/zmk.bin)
    echo "Bridge binary: $SIZE bytes"

    # Safety: bridge + ZMK combined must both fit below calibration (0xFE000)
    if [ "$SIZE" -gt 262144 ]; then
        echo "ERROR: Bridge exceeds 256KB — too large for bank 1!" >&2
        exit 1
    fi

    echo "=== Creating bridge OTA image ==="
    python3 reverse/tools/prepare_ota.py build-bridge/zephyr/zmk.bin \
        -o build-bridge/bridge_ota.bin
    echo "Output: build-bridge/bridge_ota.bin"
fi
