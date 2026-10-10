#!/bin/bash
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
echo "== test_sleep_policy =="
$CC -std=c11 -Wall -Wextra -O1 -o "${TMPDIR:-/tmp}/test_sleep_policy" test_sleep_policy.c ../sleep_policy.c
"${TMPDIR:-/tmp}/test_sleep_policy"
