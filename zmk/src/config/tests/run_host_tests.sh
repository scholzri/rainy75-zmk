#!/bin/bash
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
echo "== test_cfg_registry =="
$CC -std=c11 -Wall -Wextra -O1 -o "${TMPDIR:-/tmp}/test_cfg_registry" test_cfg_registry.c ../cfg_registry.c
"${TMPDIR:-/tmp}/test_cfg_registry"
