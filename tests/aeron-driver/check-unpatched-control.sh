#!/bin/bash
# The RED control of aeron_driver.elicit_setup_retry: the test binary built with the PRISTINE dispatcher must fail as an
# assertion (doctest exit status 1, never a crash) whose output carries the expected text. The pristine file must
# differ from the patched one, or the control proves nothing.
# Usage: check-unpatched-control.sh <ct_aeron_driver_unpatched_tests> <pristine.c> <patched.c> <expected text>
set -uo pipefail
if [ $# -ne 4 ]; then
    echo "usage: $0 <unpatched-test-binary> <pristine.c> <patched.c> <expected text>" >&2
    exit 64
fi
BIN=$1 PRISTINE=$2 PATCHED=$3 EXPECTED=$4
if cmp -s "$PRISTINE" "$PATCHED"; then
    echo "FAIL the patched dispatcher equals the pristine one: the patch is not in the build tree"
    exit 1
fi
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
"$BIN" --no-colors=true > "$OUT" 2>&1
status=$?
cat "$OUT"
if [ "$status" -eq 0 ]; then
    echo "FAIL the unpatched driver passes: the test does not detect the missing pending setup"
    exit 1
fi
if [ "$status" -ne 1 ] || ! grep -q 'ERROR: ' "$OUT"; then
    echo "FAIL the unpatched run is not an assertion failure (exit $status, a crash is no RED)"
    exit 1
fi
if ! grep -qF -- "$EXPECTED" "$OUT"; then
    echo "FAIL the unpatched run failed for another reason (expected text: $EXPECTED)"
    exit 1
fi
echo "RED  the unpatched driver fails [$EXPECTED]; the patched build is aeron_driver.elicit_setup_retry"
