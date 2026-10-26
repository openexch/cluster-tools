#!/bin/bash
# Real-cluster integration run for the C++ cluster client (RC2 S11). Starts, on this host:
#   - the C media driver built from the verified Aeron 1.53.1 tree (the client's driver),
#   - a 3-node assets core cluster (assets-cluster.jar, production Main, real authenticator
#     with the assets TestAuth key table),
#   - a 1-node match ME cluster (match-cluster.jar, production Main, no authenticator),
# then runs the ct_client_it scenarios and does the killing (SIGKILL of the core leader, then
# of every remaining core member; SIGKILL of the media driver for the fatal-exit scenario).
#
# Usage: client/it/run-it.sh <cmake-build-dir> <assets-cluster.jar> <match-cluster.jar>
# Env:   IT_WORK   work dir for node state and logs (default: mktemp -d)
#        IT_KEEP=1 keep the work dir
# The jars must be built from the commits dependencies.lock pins (money_schema_ref,
# match_schema_ref): `git archive <ref> | tar -x` then `mvn -o -DskipTests package`.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <cmake-build-dir> <assets-cluster.jar> <match-cluster.jar>" >&2
    exit 64
fi
BUILD=$(cd "$1" && pwd)
AE_JAR=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
ME_JAR=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
IT_BIN=$(find "$BUILD" -name ct_client_it -type f | head -1)
AERONMD=$(find "$BUILD/aeron" -name aeronmd -type f | head -1)
[ -n "$IT_BIN" ] && [ -x "$IT_BIN" ] || { echo "FATAL: ct_client_it not built" >&2; exit 1; }
[ -x "$AERONMD" ] || { echo "FATAL: aeronmd not built (CT_BUILD_MEDIA_DRIVER=ON)" >&2; exit 1; }
[ -f "$AE_JAR" ] && [ -f "$ME_JAR" ] || { echo "FATAL: jars not found" >&2; exit 1; }

W=${IT_WORK:-$(mktemp -d -t ct-client-it.XXXXXX)}
mkdir -p "$W"
SHM=$(mktemp -d /dev/shm/ct-client-it.XXXXXX)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -rf "$SHM"
    if [ "${IT_KEEP:-0}" != "1" ]; then
        rm -rf "$W"
    else
        echo "work dir kept: $W"
    fi
}
trap cleanup EXIT

# assets TestAuth.KEYS: one key per class (the FORWARDER key is 6:FORWARDER:66..66).
KEYS="1:ADMIN:$(printf '11%.0s' {1..32});2:TOOL:$(printf '22%.0s' {1..32});3:BRIDGE:$(printf '33%.0s' {1..32});5:GATEWAY:$(printf '55%.0s' {1..32});6:FORWARDER:$(printf '66%.0s' {1..32})"
AE_BASE=22000
ME_BASE=23000
HOST=127.0.0.1
JAVA_OPTS=(--add-opens java.base/jdk.internal.misc=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED
    -Xms256m -Xmx512m -XX:+UseZGC)

declare -A AE_PID
start_ae_node() {
    local i=$1
    mkdir -p "$W/ae$i"
    env CLUSTER_NODE="$i" CLUSTER_ADDRESSES=localhost,localhost,localhost CLUSTER_PORT_BASE=$AE_BASE \
        CLUSTER_PORT_STRIDE=100 BASE_DIR="$W/ae$i" ASSETS_AUTH_KEYS="$KEYS" ASSETS_NODE_PORT=$((AE_BASE + 90 + i)) \
        java "${JAVA_OPTS[@]}" -Daeron.dir="$SHM/ae" -jar "$AE_JAR" > "$W/ae$i.log" 2>&1 &
    AE_PID[$i]=$!
    PIDS+=($!)
}

start_driver() { # dir log -> pid in DRIVER_PID
    env AERON_DIR="$1" AERON_DIR_DELETE_ON_START=1 AERON_DIR_DELETE_ON_SHUTDOWN=1 "$AERONMD" > "$2" 2>&1 &
    DRIVER_PID=$!
    PIDS+=($!)
    for _ in $(seq 1 100); do
        [ -f "$1/cnc.dat" ] && return 0
        sleep 0.1
    done
    echo "FATAL: media driver at $1 did not start" >&2
    exit 1
}

FAILED=()
run() { # name args...
    local name=$1
    shift
    echo "=== scenario $name"
    if "$IT_BIN" "$@" 2>&1 | tee "$W/it-$name.log"; then
        if ! grep -q '^SCENARIO PASS' "$W/it-$name.log"; then FAILED+=("$name"); fi
    else
        FAILED+=("$name")
    fi
}

DRIVER_DIR="$SHM/client-driver"
start_driver "$DRIVER_DIR" "$W/aeronmd.log"
for i in 0 1 2; do start_ae_node "$i"; done
mkdir -p "$W/me0"
env CLUSTER_NODE=0 CLUSTER_ADDRESSES=localhost CLUSTER_PORT_BASE=$ME_BASE BASE_DIR="$W/me0" \
    METRICS_PORT=$((ME_BASE + 90)) java "${JAVA_OPTS[@]}" -Daeron.dir="$SHM/me" -jar "$ME_JAR" > "$W/me0.log" 2>&1 &
PIDS+=($!)

AE_INGRESS="0=localhost:$((AE_BASE + 2)),1=localhost:$((AE_BASE + 102)),2=localhost:$((AE_BASE + 202))"
ME_INGRESS="0=localhost:$((ME_BASE + 2))"

run auth auth "$DRIVER_DIR" "$AE_INGRESS" "$HOST"
run keepalive keepalive "$DRIVER_DIR" "$AE_INGRESS" "$HOST"
run two-clusters two-clusters "$DRIVER_DIR" "$AE_INGRESS" "$ME_INGRESS" "$HOST"

# leader-kill: wait for READY, SIGKILL that member; wait for PHASE2, SIGKILL the rest.
echo "=== scenario leader-kill"
LK_LOG="$W/it-leader-kill.log"
: > "$LK_LOG"
"$IT_BIN" leader-kill "$DRIVER_DIR" "$AE_INGRESS" "$HOST" > "$LK_LOG" 2>&1 &
LK_PID=$!
PIDS+=($LK_PID)
for _ in $(seq 1 600); do grep -q '^READY' "$LK_LOG" && break; sleep 0.1; done
LEADER=$(sed -n 's/^READY leader=\([0-9]*\).*/\1/p' "$LK_LOG")
if [ -n "$LEADER" ]; then
    echo "SIGKILL core leader member $LEADER (pid ${AE_PID[$LEADER]})"
    kill -9 "${AE_PID[$LEADER]}"
    for _ in $(seq 1 600); do grep -q '^PHASE2' "$LK_LOG" && break; sleep 0.1; done
    for i in 0 1 2; do
        if [ "$i" != "$LEADER" ]; then
            echo "SIGKILL core member $i (pid ${AE_PID[$i]})"
            kill -9 "${AE_PID[$i]}" 2>/dev/null || true
        fi
    done
fi
if wait "$LK_PID"; then LK_RC=0; else LK_RC=$?; fi
cat "$LK_LOG"
if [ "$LK_RC" -ne 0 ] || ! grep -q '^SCENARIO PASS' "$LK_LOG"; then FAILED+=(leader-kill); fi

# fatal: a client on its own driver; SIGKILL the driver; the process must exit 70 within
# driverTimeout (1 s) + 3 s, although its own thread is asleep.
echo "=== scenario fatal-exit"
FATAL_DIR="$SHM/fatal-driver"
start_driver "$FATAL_DIR" "$W/aeronmd-fatal.log"
FATAL_DRIVER=$DRIVER_PID
FE_LOG="$W/it-fatal.log"
"$IT_BIN" fatal "$FATAL_DIR" > "$FE_LOG" 2>&1 &
FE_PID=$!
for _ in $(seq 1 100); do grep -q '^READY' "$FE_LOG" && break; sleep 0.1; done
KILLED_AT=$(date +%s.%N)
kill -9 "$FATAL_DRIVER"
if wait "$FE_PID"; then FE_RC=0; else FE_RC=$?; fi
EXITED_AT=$(date +%s.%N)
TOOK=$(python3 -I -c "import sys; print(f'{float(sys.argv[2]) - float(sys.argv[1]):.2f}')" "$KILLED_AT" "$EXITED_AT")
cat "$FE_LOG"
if [ "$FE_RC" -eq 70 ] && python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) <= 4.0 else 1)" "$TOOK"; then
    echo "PASS driver SIGKILL -> client process exited 70 after ${TOOK} s (driver timeout 1 s)"
else
    echo "FAIL driver SIGKILL -> exit code $FE_RC after ${TOOK} s"
    FAILED+=(fatal-exit)
fi

echo
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "INTEGRATION PASS: auth keepalive two-clusters leader-kill fatal-exit"
else
    echo "INTEGRATION FAIL: ${FAILED[*]}"
    IT_KEEP=1
    exit 1
fi
