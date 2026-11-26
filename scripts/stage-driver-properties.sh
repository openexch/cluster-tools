#!/bin/bash
# Stages the external media driver's properties into a publish directory:
#
#   driver.properties       a byte copy of match's deploy/media-driver/driver.properties at the
#                           commit dependencies.lock pins (driver_properties_ref), verified against
#                           driver_properties_sha256. Never vendored into this repo and never taken
#                           from a branch tip: two copies drift, a moving ref is how a changed value
#                           ships unnoticed. The engines' embedded driver sets the same values in code.
#   driver-dev.properties   this repo's profile overlays (deploy/media-driver/): threading and idle strategies
#   driver-prod.properties  only, loaded by aeronmd after driver.properties.
#   driver-idle-busy_spin.properties, driver-idle-backoff.properties, driver-idle-sleep.properties
#                           this repo's idle overlays: the idle strategy of every agent role and its init args
#                           only (never the threading mode), loaded last:
#                           aeronmd driver.properties driver-<dev|prod>.properties driver-idle-<mode>.properties
#
# A newer driver.properties is taken by bumping driver_properties_ref and driver_properties_sha256
# in the lock, deliberately, in a reviewed change.
#
# Usage: scripts/stage-driver-properties.sh <out-dir> <match-checkout> [<lock> [<overlay-dir>]]
# (the optional lock and overlay dir exist for scripts/check-stage-driver-properties.sh)
set -euo pipefail

if [ $# -lt 2 ] || [ $# -gt 4 ]; then
    echo "usage: $0 <out-dir> <match-checkout> [<lock> [<overlay-dir>]]" >&2
    exit 64
fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
MATCH=$2
LOCK=${3:-$HERE/dependencies.lock}
OVERLAYS=${4:-$HERE/deploy/media-driver}

lock() {
    python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$LOCK" "$1"
}

REF=$(lock driver_properties_ref)
PATH_IN_REPO=$(lock driver_properties_path)
WANT=$(lock driver_properties_sha256)

git -C "$MATCH" cat-file -e "$REF^{commit}" 2>/dev/null || {
    echo "FATAL: $MATCH has no commit $REF (fetch it first)" >&2
    exit 1
}
git -C "$MATCH" show "$REF:$PATH_IN_REPO" > "$OUT/driver.properties.tmp"
GOT=$(sha256sum "$OUT/driver.properties.tmp" | cut -d' ' -f1)
if [ "$GOT" != "$WANT" ]; then
    echo "FATAL: $MATCH:$REF:$PATH_IN_REPO sha256 $GOT, lock pins $WANT" >&2
    rm -f "$OUT/driver.properties.tmp"
    exit 1
fi
mv "$OUT/driver.properties.tmp" "$OUT/driver.properties"
echo "verified driver.properties $GOT ($REF)" >&2

# An overlay may set only the threading mode and the idle strategies, and must set the
# threading mode of its profile: everything else is driver.properties' business.
check_overlay() { # file profile threading
    python3 -I - "$1" "$2" "$3" <<'PY'
import sys
path, profile, want = sys.argv[1], sys.argv[2], sys.argv[3]
allowed = {'aeron.threading.mode', 'aeron.conductor.idle.strategy', 'aeron.sender.idle.strategy',
           'aeron.receiver.idle.strategy', 'aeron.shared.idle.strategy', 'aeron.sharednetwork.idle.strategy'}
problems, threading = [], None
for n, raw in enumerate(open(path, encoding='utf-8'), 1):
    line = raw.strip()
    if not line or line.startswith('#'):
        continue
    key, sep, value = line.partition('=')
    key, value = key.strip(), value.strip()
    if not sep or key not in allowed:
        problems.append(f'line {n}: {key!r} is not an overlay key (threading and idle strategies only)')
        continue
    if key == 'aeron.threading.mode':
        threading = value
if threading != want:
    problems.append(f'aeron.threading.mode={threading}, the {profile} overlay must set {want}')
if problems:
    print(f'FATAL: {path}: ' + '; '.join(problems), file=sys.stderr)
    sys.exit(1)
PY
}
for profile in dev prod; do
    case "$profile" in
        dev) threading=SHARED ;;
        prod) threading=DEDICATED ;;
    esac
    src="$OVERLAYS/driver-$profile.properties"
    [ -f "$src" ] || { echo "FATAL: missing $src" >&2; exit 1; }
    check_overlay "$src" "$profile" "$threading"
    cp "$src" "$OUT/driver-$profile.properties"
    echo "checked driver-$profile.properties" >&2
done

# An idle overlay may set only the idle strategy of the five agent roles and their init args, and must set every
# role's strategy (a role left out would keep the profile overlay's value): never the threading mode.
check_idle_overlay() { # file mode
    python3 -I - "$1" "$2" <<'PY'
import sys
path, mode = sys.argv[1], sys.argv[2]
roles = ['conductor', 'sender', 'receiver', 'shared', 'sharednetwork']
strategies = {f'aeron.{r}.idle.strategy' for r in roles}
allowed = strategies | {f'aeron.{r}.idle.strategy.init.args' for r in roles}
problems, seen = [], set()
for n, raw in enumerate(open(path, encoding='utf-8'), 1):
    line = raw.strip()
    if not line or line.startswith('#'):
        continue
    key, sep, value = line.partition('=')
    key = key.strip()
    if not sep or key not in allowed:
        problems.append(f'line {n}: {key!r} is not an idle overlay key (idle strategies and their init args only)')
        continue
    seen.add(key)
for key in sorted(strategies - seen):
    problems.append(f'{key} is not set (the {mode} idle overlay sets every agent role)')
if problems:
    print(f'FATAL: {path}: ' + '; '.join(problems), file=sys.stderr)
    sys.exit(1)
PY
}
for mode in busy_spin backoff sleep; do
    src="$OVERLAYS/driver-idle-$mode.properties"
    [ -f "$src" ] || { echo "FATAL: missing $src" >&2; exit 1; }
    check_idle_overlay "$src" "$mode"
    cp "$src" "$OUT/driver-idle-$mode.properties"
    echo "checked driver-idle-$mode.properties" >&2
done
