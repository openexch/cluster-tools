#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# smoke.sh: build the cluster-tools image from a published set and run its processes once.
#
# Usage: deploy/oci/smoke.sh --set <dir> [--tag <tag>]
#   --set  a directory holding the published set (aeronmd, libaeron.so, libaeron_driver.so, driver.properties,
#          driver-dev.properties, driver-prod.properties, driver-idle-{busy_spin,backoff,sleep}.properties, ct-bridge,
#          ct-forwarder, ...)
#
# Steps: aeronmd under aeronmd-launch publishes its cnc.dat (read-only root, non-root uid); ct-forwarder and
# ct-bridge started without settings refuse with 78 (the worker's own settings check, reached through
# worker-launch); a driver mode in the env that contradicts the command is refused with 78; no command is 78.
# Containers are removed at exit. Docker builds are heavy on a shared box: run this under the box's build lock.
# Exit: 0 every step passed, 1 a step failed.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SET=""
TAG=openexchange/cluster-tools:smoke
while [ $# -gt 0 ]; do
    case "$1" in
        --set) SET=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        *) echo "usage: $0 --set <dir> [--tag <tag>]" >&2; exit 64 ;;
    esac
done
for f in aeronmd libaeron.so libaeron_driver.so driver.properties driver-dev.properties driver-prod.properties \
    driver-idle-busy_spin.properties driver-idle-backoff.properties driver-idle-sleep.properties ct-bridge ct-forwarder; do
    [ -r "$SET/$f" ] || { echo "smoke: $SET/$f is missing (--set <published set dir>)" >&2; exit 64; }
done

ID=ct-smoke-$$
trap 'docker rm -f "$ID-drv" >/dev/null 2>&1 || true' EXIT
FAILS=0
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
pass() { echo "ok   $1"; }
RO=(--read-only --tmpfs "/tmp:rw,size=64m")
BIN=/opt/openexchange/cluster-tools/bin

docker build -q -f "$HERE/oci/Dockerfile" --build-context deploy="$HERE" -t "$TAG" "$SET" >/dev/null
pass "image $TAG built"

docker run -d --name "$ID-drv" "${RO[@]}" --shm-size 256m -e AERON_DIR=/dev/shm/aeron-smoke \
    -e AERON_DRIVER_PROFILE=dev "$TAG" "$BIN/aeronmd-launch" run >/dev/null
if docker exec -e AERON_DIR=/dev/shm/aeron-smoke "$ID-drv" "$BIN/aeronmd-launch" wait-cnc; then
    pass "aeronmd: cnc.dat published (uid $(docker exec "$ID-drv" id -u), read-only root $(docker inspect -f '{{.HostConfig.ReadonlyRootfs}}' "$ID-drv"))"
else
    fail "aeronmd: no cnc.dat"; docker logs --tail 20 "$ID-drv" 2>&1 | sed 's/^/  | /'
fi
docker stop -t 10 "$ID-drv" >/dev/null
rc=$(docker inspect -f '{{.State.ExitCode}}' "$ID-drv")
case "$rc" in 0 | 15) pass "aeronmd: SIGTERM exit $rc (the unit counts 15 as success)" ;; *) fail "aeronmd: SIGTERM exit $rc" ;; esac

expect78() { # what command...
    local what=$1 rc=0 out; shift
    out=$(docker run --rm "${RO[@]}" "$@" 2>&1) || rc=$?
    if [ "$rc" = 78 ]; then pass "$what: exit 78"; else fail "$what: exit $rc, expected 78: $(tail -3 <<<"$out")"; fi
}
expect78 "no command" "$TAG"
expect78 "ct-forwarder without settings" "$TAG" "$BIN/worker-launch" embedded ct-forwarder
expect78 "ct-bridge without settings" "$TAG" "$BIN/worker-launch" embedded ct-bridge
expect78 "env driver mode contradicting the command" -e FWD_DRIVER_MODE=embedded "$TAG" "$BIN/worker-launch" external ct-forwarder
expect78 "aeronmd with an unknown profile" -e AERON_DIR=/dev/shm/x -e AERON_DRIVER_PROFILE=fast "$TAG" "$BIN/aeronmd-launch" run
expect78 "aeronmd with an unknown idle mode" -e AERON_DIR=/dev/shm/x -e AERON_DRIVER_PROFILE=dev -e AERON_DRIVER_IDLE_MODE=yield \
    "$TAG" "$BIN/aeronmd-launch" run

if [ $FAILS -gt 0 ]; then
    echo "smoke: $FAILS step(s) failed"
    exit 1
fi
echo "smoke: all steps passed"
