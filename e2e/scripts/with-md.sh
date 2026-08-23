#!/bin/bash
# Runs INSIDE the ccfg-client container: ensure one aeronmd is up for AERON_DIR, then
# exec the given command (e.g. /e2e/dist/switch-behavior ... or /e2e/dist/poc).
# Connection env (AERON_DIR, SELF_IP, INGRESS_ENDPOINTS, LD_LIBRARY_PATH) comes from
# the compose file; a stale cnc.dat with no live driver behind it is cleaned up.
set -uo pipefail

AERON_DIR=${AERON_DIR:-/dev/shm/aeron-e2e}
PID_FILE=/tmp/aeronmd.pid

driver_alive() {
    [ -f "$PID_FILE" ] && [ -d "/proc/$(cat "$PID_FILE")" ]
}

if ! driver_alive; then
    rm -rf "$AERON_DIR"
    /e2e/dist/aeronmd > /tmp/aeronmd.log 2>&1 &
    echo $! > "$PID_FILE"
    for _ in $(seq 1 50); do
        [ -f "$AERON_DIR/cnc.dat" ] && break
        sleep 0.2
    done
    if [ ! -f "$AERON_DIR/cnc.dat" ]; then
        echo "FATAL: aeronmd did not start"
        cat /tmp/aeronmd.log
        exit 3
    fi
fi

exec "$@"
