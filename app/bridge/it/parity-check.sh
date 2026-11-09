#!/bin/bash
# Parity: every @Test method of the frozen Java bridge (assets-bridge/src/test at the given assets tree) must
# have a row in a PARITY.md (the bridge's domain/bridge/tests/PARITY.md, and archive/tests/PARITY.md for
# ChainResumeMemoTest). Usage: parity-check.sh <assets-tree> <PARITY.md> [<PARITY.md> ...]
set -euo pipefail
TREE=$1; shift
TESTS=$(find "$TREE/assets-bridge/src/test" -name '*Test.java' | sort)
missing=0 total=0
for f in $TESTS; do
    for m in $(grep -A3 '@Test' "$f" | grep -oE 'void [A-Za-z0-9_]+' | awk '{ print $2 }'); do
        total=$((total + 1))
        if ! cat "$@" | grep -qw -- "$m"; then
            echo "MISSING $(basename "$f" .java).$m"
            missing=$((missing + 1))
        fi
    done
done
echo "parity: $total Java tests, $missing without a row"
[ "$missing" -eq 0 ]
