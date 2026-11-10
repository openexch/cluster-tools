#!/bin/bash
# Proves scripts/stage-driver-properties.sh refuses every drifted or malformed input: a
# driver.properties whose bytes differ from the lock's sha256, a pinned ref the checkout does
# not have, an overlay that sets a key outside threading and idle strategies, an overlay with
# the other profile's threading mode, and a missing overlay. The positive control stages the
# set. Registered as the ctest driver_properties.stage_gate; runnable by hand too.
#
# Usage: scripts/check-stage-driver-properties.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
STAGE="$HERE/scripts/stage-driver-properties.sh"
WORK=$(mktemp -d -t ct-driver-props.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }

# A stand-in match repository holding a driver.properties at one commit.
REPO="$WORK/match"
mkdir -p "$REPO/deploy/media-driver"
printf 'aeron.mtu.length=8192\naeron.dir.delete.on.start=true\n' > "$REPO/deploy/media-driver/driver.properties"
git -C "$REPO" init -q
git -C "$REPO" -c user.email=t@t -c user.name=t add -A
git -C "$REPO" -c user.email=t@t -c user.name=t commit -q -m pin
REF=$(git -C "$REPO" rev-parse HEAD)
SUM=$(sha256sum "$REPO/deploy/media-driver/driver.properties" | cut -d' ' -f1)

lockfile() { # path ref sha
    printf '{"driver_properties_ref": "%s", "driver_properties_path": "deploy/media-driver/driver.properties", "driver_properties_sha256": "%s"}\n' \
        "$2" "$3" > "$1"
}
overlays() { # dir dev-body prod-body
    mkdir -p "$1"
    [ -z "$2" ] || printf '%b' "$2" > "$1/driver-dev.properties"
    [ -z "$3" ] || printf '%b' "$3" > "$1/driver-prod.properties"
}
DEV='aeron.threading.mode=SHARED\naeron.shared.idle.strategy=backoff\n'
PROD='aeron.threading.mode=DEDICATED\naeron.conductor.idle.strategy=noop\n'

expect() { # name want(ok|fail) message-substring lock overlay-dir
    local name=$1 want=$2 msg=$3 lock=$4 ov=$5 out rc=0
    out=$(bash "$STAGE" "$WORK/out-$name" "$REPO" "$lock" "$ov" 2>&1) || rc=$?
    if [ "$want" = ok ]; then
        if [ "$rc" -eq 0 ] && cmp -s "$WORK/out-$name/driver.properties" "$REPO/deploy/media-driver/driver.properties" \
            && [ -f "$WORK/out-$name/driver-dev.properties" ] && [ -f "$WORK/out-$name/driver-prod.properties" ]; then
            pass "$name"
        else
            fail "$name (rc=$rc: $out)"
        fi
    elif [ "$rc" -ne 0 ] && grep -qF -- "$msg" <<< "$out"; then
        pass "$name"
    else
        fail "$name (rc=$rc, want a refusal naming '$msg': $out)"
    fi
}

lockfile "$WORK/good.lock" "$REF" "$SUM"
overlays "$WORK/ov-good" "$DEV" "$PROD"
expect positive ok "" "$WORK/good.lock" "$WORK/ov-good"
# The repository's own overlays pass the same checks.
expect repo-overlays ok "" "$WORK/good.lock" "$HERE/deploy/media-driver"

lockfile "$WORK/badsum.lock" "$REF" "$(printf '0%.0s' {1..64})"
expect sha-mismatch fail "lock pins" "$WORK/badsum.lock" "$WORK/ov-good"

lockfile "$WORK/badref.lock" "0123456789abcdef0123456789abcdef01234567" "$SUM"
expect unknown-ref fail "has no commit" "$WORK/badref.lock" "$WORK/ov-good"

overlays "$WORK/ov-extra" "${DEV}aeron.mtu.length=1408\n" "$PROD"
expect overlay-extra-key fail "'aeron.mtu.length' is not an overlay key" "$WORK/good.lock" "$WORK/ov-extra"

overlays "$WORK/ov-threading" "$PROD" "$PROD"
expect overlay-wrong-threading fail "the dev overlay must set SHARED" "$WORK/good.lock" "$WORK/ov-threading"

overlays "$WORK/ov-missing" "$DEV" ""
expect overlay-missing fail "missing" "$WORK/good.lock" "$WORK/ov-missing"

if [ "$FAILED" -ne 0 ]; then
    echo "$FAILED case(s) failed"
    exit 1
fi
echo "all driver-properties stage cases passed"
