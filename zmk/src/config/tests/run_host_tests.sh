#!/bin/bash
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
for t in test_cfg_registry test_cfg_codec; do
    echo "== $t =="
    $CC -std=c11 -Wall -Wextra -O1 -o "${TMPDIR:-/tmp}/$t" "$t.c" ../cfg_registry.c ../cfg_codec.c
    "${TMPDIR:-/tmp}/$t"
done
# The real settings table (cfg_table.c) on the Zephyr stubs in stub/: with the
# real effect table (12 effects, 13 with the walker diagnostic) and with 16
# and 17 stub effect names (17 is one more than a list setting may have, so
# the table is refused).
FX="../../rainy_rgb/effects.c ../../rainy_rgb/color.c"
table_test() { # label, then extra compiler arguments and sources
    local label=$1
    shift
    echo "== test_cfg_table ($label) =="
    $CC -std=c11 -Wall -Wextra -O1 -Istub -I../.. -o "${TMPDIR:-/tmp}/test_cfg_table" \
        test_cfg_table.c ../cfg_table.c ../cfg_registry.c "$@" -lm
    "${TMPDIR:-/tmp}/test_cfg_table"
}
table_test "real effects" $FX
table_test "real effects, walker" -DCONFIG_RAINY_RGB_WALKER $FX
table_test "16 stub effects" -DSTUB_EFFECTS=16
table_test "17 stub effects" -DSTUB_EFFECTS=17
