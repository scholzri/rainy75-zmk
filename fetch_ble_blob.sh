#!/bin/bash
# Fetch the Telink B91 Bluetooth LE controller library.
#
# This is a proprietary binary blob and is NOT redistributable — its license
# (confidential / non-transferable, under NDA; see NOTICE) does not permit us to
# ship it in this repository. The build links it, but it is gitignored and never
# committed. This script downloads the exact pinned version from Telink's own
# public repository and verifies its SHA-256 before use.
set -euo pipefail
cd "$(dirname "$0")"

BLOB="zmk/lib/liblt_9518_zephyr.a"
SHA256="354b2f972f4a9012a66c2015a7092591eec18f29a06ba8f50ec25bd0d3cf9a31"
PIN="fc489d7106aa3ff748c47af255abf5f9aed88908"
URL="https://raw.githubusercontent.com/telink-semi/zephyr_hal_telink_b91_ble_lib/${PIN}/liblt_9518_zephyr.a"

if [ -f "$BLOB" ] && echo "${SHA256}  ${BLOB}" | sha256sum -c --status 2>/dev/null; then
    echo "BLE blob present and verified: $BLOB"
    exit 0
fi

echo "Fetching Telink B91 BLE blob from telink-semi (pinned ${PIN})..."
mkdir -p "$(dirname "$BLOB")"
curl -fsSL -o "$BLOB" "$URL"

if ! echo "${SHA256}  ${BLOB}" | sha256sum -c --status; then
    echo "ERROR: SHA-256 mismatch on $BLOB — refusing to use it." >&2
    rm -f "$BLOB"
    exit 1
fi
echo "BLE blob fetched and verified: $BLOB"
