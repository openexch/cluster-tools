#!/bin/bash
# Archive integration run for the C++ bridge's archive side (RC2 S9a contract 9.3). For every
# scenario of ct_archive_it: a fresh in-tree aeronmd for the C++ side, one or two Java Aeron
# Archives (JournalArchiveFixture.java on the pinned aeron-all jar), the scenario, teardown. Then the
# negative controls: the same scenario with a CT_TEST_DISABLE_<rule> seam set must FAIL.
#
# Usage: archive/it/run-it.sh <cmake-build-dir> <aeron-all.jar> [scenario...]
# Env:   IT_WORK      work dir (default: mktemp -d)
#        IT_KEEP=1    keep the work dir
#        IT_SKIP_RED=1  skip the negative controls
#        CT_IT_APPLIED_HEAD  records of appliedHeadKeepsDutyBounded (default 100000; 1000000 in CI)
set -uo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <cmake-build-dir> <aeron-all.jar> [scenario...]" >&2
    exit 64
fi
BUILD=$(cd "$1" && pwd)
JAR=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LOCK="$ROOT/dependencies.lock"
FIXTURES="$ROOT/archive/tests/fixtures/mejournal"

WANT=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["aeron_all_sha256"])' "$LOCK")
GOT=$(sha256sum "$JAR" | cut -d' ' -f1)
if [ "$GOT" != "$WANT" ]; then
    echo "FATAL: $JAR has sha256 $GOT; dependencies.lock pins aeron_all_sha256 $WANT" >&2
    exit 1
fi
IT_BIN=$(find "$BUILD" -name ct_archive_it -type f | head -1)
AERONMD=$(find "$BUILD/aeron" -name aeronmd -type f | head -1)
TAIL=$(find "$BUILD" -name ct-journal-tail -type f | head -1)
TAIL_SEAMED=$(find "$BUILD" -name ct-journal-tail-seamed -type f | head -1)
for bin in "$IT_BIN" "$AERONMD" "$TAIL" "$TAIL_SEAMED"; do
    [ -n "$bin" ] && [ -x "$bin" ] || { echo "FATAL: a binary is missing (ct_archive_it, aeronmd, ct-journal-tail[-seamed])" >&2; exit 1; }
done

W=${IT_WORK:-$(mktemp -d -t ct-archive-it.XXXXXX)}
mkdir -p "$W"
cleanup() {
    rm -rf /dev/shm/ct-archive-it-* 2>/dev/null || true
    if [ "${IT_KEEP:-0}" != "1" ]; then rm -rf "$W"; else echo "work dir kept: $W"; fi
}
trap cleanup EXIT

javac -proc:none -d "$W/classes" -cp "$JAR" "$HERE/JournalArchiveFixture.java" || { echo "FATAL: fixture does not compile" >&2; exit 1; }

if [ $# -gt 0 ]; then SCENARIOS=("$@"); else mapfile -t SCENARIOS < <("$IT_BIN" --list); fi

PORT=19100
run() { # name env... -> 0 pass, 1 fail
    local name=$1
    shift
    local dir="$W/$name-$PORT"
    mkdir -p "$dir"
    rm -rf /dev/shm/ct-archive-it-* 2>/dev/null || true
    env "$@" timeout 900 "$IT_BIN" "$name" "$dir" "$AERONMD" "$JAR" "$W/classes" "$FIXTURES" "$PORT" "$TAIL" "$TAIL_SEAMED" \
        > "$dir/out.log" 2>&1
    PORT=$((PORT + 10))
    cat "$dir/out.log"
    grep -q "^SCENARIO PASS $name" "$dir/out.log"
}

GREEN_FAILED=()
for s in "${SCENARIOS[@]}"; do
    run "$s" || GREEN_FAILED+=("$s")
done

RED_FAILED=()
RED_PASSED=0
if [ "${IT_SKIP_RED:-0}" != "1" ]; then
    echo
    echo "=== negative controls (each must FAIL with its rule off)"
    while IFS='|' read -r scenario rules why expected; do
        case "$scenario" in ''|'#'*) continue ;; esac
        if [ $# -gt 0 ] && ! printf '%s\n' "${SCENARIOS[@]}" | grep -qx "$scenario"; then continue; fi
        envs=()
        IFS=',' read -r -a list <<< "$rules"
        for r in "${list[@]}"; do envs+=("CT_TEST_DISABLE_${r}=1"); done
        [ "$scenario" = appliedHeadKeepsDutyBounded ] && envs+=("CT_IT_APPLIED_HEAD=20000")
        echo "--- RED $scenario with ${rules} off (expected: $why)"
        log="$W/red-$scenario-$PORT.log"
        if run "$scenario" "${envs[@]}" > "$log" 2>&1; then
            echo "FAIL negative control: $scenario still passes with $rules off"
            RED_FAILED+=("$scenario/$rules")
        elif ! grep "^SCENARIO FAIL $scenario: " "$log" | grep -qF -- "$expected"; then
            echo "FAIL negative control: $scenario failed for another reason (expected: $expected)"
            grep -E '^SCENARIO' "$log" || echo "  (no SCENARIO line: a crash is no RED)"
            RED_FAILED+=("$scenario/$rules")
        else
            grep -E '^SCENARIO|fault|^  ' "$log" | tail -4
            echo "RED  $scenario fails with $rules off"
            RED_PASSED=$((RED_PASSED + 1))
        fi
    done < "$HERE/negative-controls.txt"
fi

echo
if [ ${#GREEN_FAILED[@]} -eq 0 ] && [ ${#RED_FAILED[@]} -eq 0 ]; then
    echo "ARCHIVE INTEGRATION PASS: ${#SCENARIOS[@]} scenarios, $RED_PASSED negative controls"
else
    echo "ARCHIVE INTEGRATION FAIL: scenarios [${GREEN_FAILED[*]:-}] negative controls [${RED_FAILED[*]:-}]"
    IT_KEEP=1
    exit 1
fi
