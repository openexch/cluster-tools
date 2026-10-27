#!/bin/bash
# Negative-control run (RC2 S9a contract 9 preamble): for every line of the table, the named test
# must FAIL with its rule switched off (CT_TEST_DISABLE_<rule>=1, a seam compiled only into the
# seamed test build) and PASS with it on. A RED counts only as an assertion failure (doctest exit
# status 1, never a crash) whose output carries the line's expected text (review N-10).
#
# Table line: test | rule | why | expected text in the failure output
# Usage: run-negative-controls.sh <ct_archive_tests binary> <negative-controls.txt> [<filter regex>]
set -uo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <test-binary> <table> [filter]" >&2
    exit 64
fi
BIN=$1
TABLE=$2
FILTER=${3:-.}
PASSED=0
FAILED=0
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
while IFS='|' read -r test rule why expected; do
    case "$test" in ''|'#'*) continue ;; esac
    if ! [[ "$test|$rule" =~ $FILTER ]]; then continue; fi
    env "CT_TEST_DISABLE_${rule}=1" "$BIN" --test-case="$test" --no-colors=true > "$OUT" 2>&1
    status=$?
    if [ "$status" -eq 0 ]; then
        echo "FAIL $test with $rule off: still passes (expected: $why)"
        FAILED=$((FAILED + 1))
        continue
    fi
    if [ "$status" -ne 1 ] || ! grep -q 'ERROR: ' "$OUT"; then
        echo "FAIL $test with $rule off: not an assertion failure (exit $status, a crash is no RED)"
        FAILED=$((FAILED + 1))
        continue
    fi
    if [ -n "$expected" ] && ! grep -qF -- "$expected" "$OUT"; then
        echo "FAIL $test with $rule off: failed for another reason (expected text: $expected)"
        grep 'ERROR: ' "$OUT" | head -2
        FAILED=$((FAILED + 1))
        continue
    fi
    if ! "$BIN" --test-case="$test" --no-colors=true > /dev/null 2>&1; then
        echo "FAIL $test with $rule on: does not pass"
        FAILED=$((FAILED + 1))
        continue
    fi
    echo "RED  $test fails with $rule off ($why) [$expected]; GREEN with it on"
    PASSED=$((PASSED + 1))
done < "$TABLE"
echo
if [ "$FAILED" -eq 0 ] && [ "$PASSED" -gt 0 ]; then
    echo "NEGATIVE CONTROLS PASS: $PASSED"
else
    echo "NEGATIVE CONTROLS FAIL: $FAILED of $((PASSED + FAILED))"
    exit 1
fi
