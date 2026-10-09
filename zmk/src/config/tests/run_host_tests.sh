#!/bin/bash
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
for t in test_cfg_registry test_cfg_codec; do
    echo "== $t =="
    $CC -std=c11 -Wall -Wextra -O1 -o "${TMPDIR:-/tmp}/$t" "$t.c" ../cfg_registry.c ../cfg_codec.c
    "${TMPDIR:-/tmp}/$t"
done
