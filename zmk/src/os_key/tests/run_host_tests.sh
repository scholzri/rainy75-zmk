#!/bin/bash
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
echo "== test_os_key =="
$CC -std=c11 -Wall -Wextra -O1 -o "${TMPDIR:-/tmp}/test_os_key" test_os_key.c ../os_key.c
"${TMPDIR:-/tmp}/test_os_key"
