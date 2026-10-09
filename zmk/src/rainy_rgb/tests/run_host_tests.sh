#!/bin/bash
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
SRCS="../color.c ../effects.c ../led_map.c ../overlay.c ../ble_status.c ../lighting.c"
for t in test_color test_effects test_overlay test_ble_status test_lighting; do
    [ -f "$t.c" ] || continue
    echo "== $t =="
    $CC -std=c11 -Wall -Wextra -O1 -o "${TMPDIR:-/tmp}/$t" "$t.c" $SRCS -lm
    "${TMPDIR:-/tmp}/$t"
done
# ANSI LED map: the overlay's key table (Enter = wide Enter, pos 56)
echo "== test_overlay (ANSI) =="
$CC -std=c11 -Wall -Wextra -O1 -DCONFIG_RAINY_RGB_ANSI_LEDMAP -o "${TMPDIR:-/tmp}/test_overlay_ansi" \
    test_overlay.c $SRCS -lm
"${TMPDIR:-/tmp}/test_overlay_ansi"
