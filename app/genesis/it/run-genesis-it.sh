#!/bin/bash
# The ct-genesis integration run on real processes, one host, serial. Each topology starts from wiped state:
#   - the C media driver built from the verified Aeron tree (the tool's external mode and the bridge),
#   - a 3-node core (core-cluster.jar), the key table of the runner (ADMIN 1, TOOL 2, BRIDGE 3),
#   - a 3-node ME (match-cluster.jar, config mode, the settlement journal and its archive), ADMIN key 4,
#   - ct-bridge (this build) in external mode, started after arm_money_journal as the cut's order does,
#   - ct-genesis (this build) and ct_genesis_tc (the seams and CT_GENESIS_TC_EXIT_AFTER_SENDS) for the controls.
# The genesis input is the repo's genesis.example.json with a random genesisId per topology.
#
# Topologies (default all, in this order):
#   full-external   every phase in order with `status` after each (the last complete phase named, the previous verb
#                   refused), `verify` DONE (behaviour 4, request cap 2048, gate OPEN, the record), a second me_genesis
#                   refused `cluster is not empty: genesis refused`, the InitTradeHighWater input refused, and the RED of
#                   the emptiness check (ct_genesis_tc EMPTINESS_CHECK off must run: `genesis ran on a cluster with history`)
#   killed-fund     phases to configure_risk, then ct_genesis_tc killed after the third deposit: status shows fund
#                   PARTIAL and NOT_A_PREFIX, fund refused `genesis state is not a prefix of the plan: ...` naming wipe and
#                   redo; the RED of the prefix check (PREFIX_CHECK off must continue: `genesis continued from an unknown
#                   state`)
#   killed-core     ct_genesis_tc killed after CoreConfig 1: core_genesis PARTIAL, every later verb refused
#   limits-red      the whole genesis with ct_genesis_tc LIMITS_STEP off: the cap read back must not be 2048 (`genesis
#                   left the request cap unset`), the RED of the CoreLimitsSet step
#   full-embedded   every phase with GENESIS_DRIVER_MODE=embedded, over the activation record the earlier topology left
#                   (a second genesis after a wipe starts clean)
#   full-external-engines  the full-external rows with the engines on the external driver: every core and ME node
#                   runs TRANSPORT_DRIVER_MODE=external on its own aeronmd (driver.properties + driver-dev.properties,
#                   the console's launch), the tool in external mode on the host aeronmd started the same way
#
# Engine driver modes: every topology above except full-external-engines runs the engines on their embedded driver.
# full-external-engines needs IT_DRIVER_PROPERTIES: the driver.properties dependencies.lock pins (its sha256 is
# checked against driver_properties_sha256; scripts/stage-driver-properties.sh stages it from a match checkout).
#
# Usage: app/genesis/it/run-genesis-it.sh <cmake-build-dir> <core-cluster.jar> <match-cluster.jar> [topology ...]
# Env: IT_WORK (work dir, default mktemp), IT_KEEP=1 keep it, IT_DRIVER_PROPERTIES (see above).
set -uo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <cmake-build-dir> <core-cluster.jar> <match-cluster.jar> [topology ...]" >&2
    exit 64
fi
BUILD=$(cd "$1" && pwd)
CORE_JAR=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
ME_JAR=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
shift 3
TOPOLOGIES=("$@")
[ ${#TOPOLOGIES[@]} -eq 0 ] && TOPOLOGIES=(full-external killed-fund killed-core limits-red full-embedded full-external-engines)
HERE=$(cd "$(dirname "$0")" && pwd)
EXAMPLE="$HERE/../genesis.example.json"
TOOL=$(find "$BUILD/app/genesis" -maxdepth 1 -name ct-genesis -type f -perm -u+x | head -1)
TC=$(find "$BUILD/app/genesis" -maxdepth 1 -name ct_genesis_tc -type f -perm -u+x | head -1)
BRIDGE=$(find "$BUILD/app/bridge" -maxdepth 1 -name ct-bridge -type f -perm -u+x | head -1)
AERONMD=$(find "$BUILD/aeron" -name aeronmd -type f | head -1)
for f in TOOL TC BRIDGE AERONMD; do [ -x "${!f}" ] || { echo "FATAL: $f not built" >&2; exit 1; }; done

W=${IT_WORK:-$(mktemp -d -t ct-genesis-it.XXXXXX)}
mkdir -p "$W"
declare -A PID
SHM=""; D=""
teardown() {
    for name in "${!PID[@]}"; do kill -9 "${PID[$name]}" 2>/dev/null || true; done
    local orphan
    for orphan in $(jobs -p); do kill -9 "$orphan" 2>/dev/null || true; done
    wait 2>/dev/null || true
    PID=()
    [ -n "$SHM" ] && rm -rf "$SHM"
    SHM=""
}
cleanup() {
    teardown
    if [ "${IT_KEEP:-0}" != "1" ]; then rm -rf "$W"; else echo "work dir kept: $W"; fi
}
trap cleanup EXIT
trap 'exit 143' TERM INT HUP

hex() { printf "$1%.0s" $(seq 1 32); }
CORE_KEYS="1:ADMIN:$(hex 11)::users=*;2:TOOL:$(hex 22)::users=*;3:BRIDGE:$(hex 33)::users=*"
ME_KEYS="4:ADMIN:$(hex 44)::users=*"
CORE_BASE=32000
ME_BASE=33000
BRIDGE_PORT=34009
CORE_INGRESS="0=localhost:$((CORE_BASE + 2)),1=localhost:$((CORE_BASE + 102)),2=localhost:$((CORE_BASE + 202))"
ME_INGRESS="0=localhost:$((ME_BASE + 2)),1=localhost:$((ME_BASE + 102)),2=localhost:$((ME_BASE + 202))"
JAVA_OPTS=(--add-opens java.base/jdk.internal.misc=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -Xms128m -Xmx384m -XX:+UseZGC)
NODE_ENV=(TRANSPORT_IDLE_MODE=backoff TRANSPORT_DRIVER_THREADING=shared TRANSPORT_TERM_LENGTH=4m
    TRANSPORT_LOG_TERM_LENGTH=16m TRANSPORT_SNAPSHOT_TERM_LENGTH=8m)

log() { echo "[$(date +%H:%M:%S)] $*"; }
FAILED=()
PASSED=()
result() { # name ok|fail detail
    if [ "$2" = ok ]; then PASSED+=("$1"); log "PASS $1: $3"; else FAILED+=("$1"); log "FAIL $1: $3"; fi
}

metric() { curl -s --max-time 2 "$1" 2>/dev/null | awk -v n="$2" '$1 == n { print $2; exit }'; }
leader_of() { # ae|me
    local base=$CORE_BASE name=core_cluster_role
    [ "$1" = me ] && { base=$ME_BASE; name=match_cluster_role; }
    for i in 0 1 2; do
        [ "$(metric "http://127.0.0.1:$((base + 90 + i))/metrics" "$name")" = "2" ] && { echo "$i"; return 0; }
    done
    return 1
}
wait_leader() { for _ in $(seq 1 600); do leader_of "$1" > /dev/null && return 0; sleep 0.2; done; return 1; }

ENGINE_DRIVER=embedded
DEV_OVERLAY="$HERE/../../../deploy/media-driver/driver-dev.properties"
start_driver() { # dir [name]: with ENGINE_DRIVER=external every driver runs the console's launch (properties + dev overlay)
    local name=${2:-driver} props=()
    [ "$ENGINE_DRIVER" = external ] && props=("$IT_DRIVER_PROPERTIES" "$DEV_OVERLAY")
    env AERON_DIR="$1" AERON_DIR_DELETE_ON_START=1 AERON_DIR_DELETE_ON_SHUTDOWN=1 "$AERONMD" "${props[@]}" > "$D/aeronmd-$name.log" 2>&1 &
    PID[$name]=$!
    for _ in $(seq 1 100); do [ -f "$1/cnc.dat" ] && return 0; sleep 0.1; done
    echo "FATAL: media driver $name did not start: $(tail -2 "$D/aeronmd-$name.log")" >&2
    return 1
}
engine_driver_env() { # ae|me index -> the engine's driver keys (embedded: the in-process driver at -Daeron.dir)
    if [ "$ENGINE_DRIVER" = external ]; then echo "TRANSPORT_DRIVER_MODE=external AERON_DIR=$SHM/drv-$1$2";
    else echo "TRANSPORT_DRIVER_MODE=embedded"; fi
}
engine_dir_opt() { [ "$ENGINE_DRIVER" = external ] || echo "-Daeron.dir=$SHM/$1$2"; }
# External engines: every node's driver serves it (its dir holds the node's publications and images) and no node
# made a driver dir of its own.
check_engine_drivers() {
    local bad=""
    for e in ae me; do for i in 0 1 2; do
        [ -e "$SHM/$e$i" ] && bad="$bad $e$i:own-driver-dir"
        [ -n "$(ls -A "$SHM/drv-$e$i/publications" 2>/dev/null)" ] || bad="$bad $e$i:no-publication-on-its-aeronmd"
    done; done
    [ -z "$bad" ] || { echo "$bad"; return 1; }
}
start_ae() {
    local i=$1
    mkdir -p "$D/ae$i"
    # shellcheck disable=SC2046 # the driver keys are words
    env "${NODE_ENV[@]}" $(engine_driver_env ae "$i") CLUSTER_NODE="$i" CLUSTER_ADDRESSES=localhost,localhost,localhost CLUSTER_PORT_BASE=$CORE_BASE \
        CLUSTER_PORT_STRIDE=100 BASE_DIR="$D/ae$i" CORE_AUTH_KEYS="$CORE_KEYS" CORE_NODE_PORT=$((CORE_BASE + 90 + i)) \
        java "${JAVA_OPTS[@]}" $(engine_dir_opt ae "$i") -jar "$CORE_JAR" >> "$D/ae$i.log" 2>&1 &
    PID[ae$i]=$!
}
start_me() {
    local i=$1
    mkdir -p "$D/me$i"
    # shellcheck disable=SC2046
    env "${NODE_ENV[@]}" $(engine_driver_env me "$i") SETTLEMENT_JOURNAL_ENABLED=true SETTLEMENT_JOURNAL_DIR="$D/me-journal$i" CLUSTER_NODE="$i" \
        CLUSTER_ADDRESSES=localhost,localhost,localhost CLUSTER_PORT_BASE=$ME_BASE CLUSTER_PORT_STRIDE=100 \
        BASE_DIR="$D/me$i" ME_AUTH_KEYS="$ME_KEYS" METRICS_PORT=$((ME_BASE + 90 + i)) MATCH_ENGINE_FROM_CONFIG=true \
        java "${JAVA_OPTS[@]}" -Xmx768m $(engine_dir_opt me "$i") -jar "$ME_JAR" >> "$D/me$i.log" 2>&1 &
    PID[me$i]=$!
}
start_bridge() {
    env -i PATH="$PATH" BRIDGE_ME_JOURNAL_ARCHIVES="localhost:$((ME_BASE + 10)),localhost:$((ME_BASE + 110)),localhost:$((ME_BASE + 210))" \
        BRIDGE_JOURNAL_CONTROL_STREAM=4010 BRIDGE_JOURNAL_STREAM=4001 BRIDGE_JOURNAL_CHANNEL=aeron:ipc BRIDGE_REPLAY_STREAM=9101 \
        BRIDGE_JOURNAL_MTU=1408 BRIDGE_LOCAL_HOST=localhost BRIDGE_DRIVER_MODE=external AERON_DIR="$DRIVER_DIR" \
        BRIDGE_DRIVER_TIMEOUT_MS=5000 BRIDGE_CORE_CLUSTER_ADDRESSES=localhost,localhost,localhost \
        BRIDGE_CORE_PORT_BASE=$CORE_BASE CLUSTER_PORT_STRIDE=100 BRIDGE_CORE_EGRESS_ENDPOINT=127.0.0.1:0 \
        CORE_AUTH_CREDENTIAL="3:BRIDGE:$(hex 33)" BRIDGE_INSTANCE_ID=1 BRIDGE_HTTP_BIND="127.0.0.1:$BRIDGE_PORT" \
        taskset -c 0-11 "$BRIDGE" >> "$D/bridge.log" 2>&1 &
    PID[bridge]=$!
}

GENESIS_ID=0
PLAN=""
RECORD="$W/activation.json" # one path for every topology: a later topology meets the earlier record
topology_up() { # name
    teardown
    D="$W/$1"; mkdir -p "$D"
    SHM=$(mktemp -d /dev/shm/ct-genesis-it.XXXXXX)
    DRIVER_DIR="$SHM/client-driver"
    mkdir -p "$SHM/embedded"
    start_driver "$DRIVER_DIR" || return 1
    if [ "$ENGINE_DRIVER" = external ]; then
        for i in 0 1 2; do start_driver "$SHM/drv-ae$i" "drv-ae$i" || return 1; start_driver "$SHM/drv-me$i" "drv-me$i" || return 1; done
    fi
    for i in 0 1 2; do start_ae "$i"; start_me "$i"; done
    wait_leader ae || { log "no core leader"; return 1; }
    wait_leader me || { log "no ME leader"; return 1; }
    GENESIS_ID=$(python3 -I -c "import secrets; print(secrets.randbits(62) | 1)")
    PLAN="$D/genesis.json"
    python3 -I - "$EXAMPLE" "$PLAN" "$GENESIS_ID" <<'PY'
import json, sys
plan = json.load(open(sys.argv[1]))
plan['genesisId'] = int(sys.argv[3])
open(sys.argv[2], 'w').write(json.dumps(plan, indent=2) + '\n')
PY
    log "topology $1 up (engines $ENGINE_DRIVER driver): core leader $(leader_of ae), ME leader $(leader_of me), genesis $GENESIS_ID"
}

MODE=external
tool_env() { # the environment of one ct-genesis call
    local env=(PATH="$PATH" GENESIS_CORE_INGRESS="$CORE_INGRESS" GENESIS_ME_INGRESS="$ME_INGRESS"
        GENESIS_RESPONSE_CHANNEL="aeron:udp?endpoint=127.0.0.1:0" GENESIS_CORE_ADMIN_CREDENTIAL="1:ADMIN:$(hex 11)"
        GENESIS_CORE_TOOL_CREDENTIAL="2:TOOL:$(hex 22)" GENESIS_ME_ADMIN_CREDENTIAL="4:ADMIN:$(hex 44)"
        GENESIS_ACTIVATION_RECORD="$RECORD" GENESIS_DRIVER_TIMEOUT_MS=10000)
    if [ "$MODE" = external ]; then env+=(GENESIS_DRIVER_MODE=external AERON_DIR="$DRIVER_DIR");
    else env+=(GENESIS_DRIVER_MODE=embedded GENESIS_DRIVER_DIR="$SHM/embedded"); fi
    printf '%s\n' "${env[@]}"
}
OUT=""; RC=0
g() { # [VAR=value ...] binary verb [plan] -> RC, OUT (stdout + stderr), logged to $D/tool.log
    local extra=()
    while [[ "$1" == *=* ]]; do extra+=("$1"); shift; done
    local bin=$1 verb=$2 plan=${3:-$PLAN}
    mapfile -t envs < <(tool_env)
    OUT=$(env -i "${envs[@]}" "${extra[@]}" timeout 600 "$bin" "$verb" "$plan" 2>&1); RC=$?
    { echo "\$ ${extra[*]} $(basename "$bin") $verb (exit $RC)"; echo "$OUT"; } >> "$D/tool.log"
}
has() { grep -qF -- "$1" <<< "$OUT"; }

PHASES=(me_genesis arm_money_journal core_genesis configure_risk fund verify_funded activate)
LAST_STATE=(me_genesis arm_money_journal core_genesis configure_risk fund fund activate)

# One phase (the bridge started before core_genesis, as the cut's order does), then status names it as the last
# complete phase and the previous verb is refused. Called in this shell (it starts the bridge): the reason is in WHY.
WHY=""
phase_step() { # index binary
    local k=$1 bin=$2
    local verb=${PHASES[$k]}
    [ "$verb" = core_genesis ] && [ -z "${PID[bridge]:-}" ] && { start_bridge; sleep 2; }
    g "$bin" "$verb"
    if [ $RC -ne 0 ]; then WHY="$verb exit $RC: $(tail -3 <<< "$OUT")"; return 1; fi
    g "$TOOL" status
    if [ $RC -ne 0 ] || ! has "last complete phase: ${LAST_STATE[$k]}"; then
        WHY="status after $verb does not name ${LAST_STATE[$k]}: $(grep -E 'last complete|state:' <<< "$OUT")"; return 1
    fi
    if [ "$k" -gt 0 ]; then
        g "$TOOL" "${PHASES[$((k - 1))]}"
        if [ $RC -ne 3 ]; then WHY="${PHASES[$((k - 1))]} after $verb: exit $RC, not refused"; return 1; fi
    fi
    return 0
}

full_genesis() { # label -> result lines
    local label=$1
    for k in 0 1 2 3 4 5 6; do
        phase_step "$k" "$TOOL" || { result "$label" fail "$WHY"; return 1; }
    done
    g "$TOOL" verify
    if [ $RC -ne 0 ] || ! has "verify: genesis $GENESIS_ID DONE"; then result "$label" fail "verify exit $RC: $(tail -2 <<< "$OUT")"; return 1; fi
    g "$TOOL" status
    if ! has "state: COMPLETE" || ! has "requestPerUserCap 2048" || ! has "config: version 4 behaviour 4 genesisId $GENESIS_ID, gate OPEN" ||
        ! has "activation record: present, genesis $GENESIS_ID"; then
        result "$label" fail "status after activate: $(grep -E 'state:|limits:|config:|activation' <<< "$OUT" | tr '\n' ';')"; return 1
    fi
    result "$label" ok "every phase in order, status after each, previous verbs refused; verify DONE; $(grep -E '^limits:' <<< "$OUT"); $(grep -E '^config:' <<< "$OUT")"
}

run_topology() {
    case "$1" in
    full-external|full-external-engines)
        MODE=external
        local sfx=""
        if [ "$1" = full-external-engines ]; then
            sfx="-ext-engines"
            local want have
            want=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["driver_properties_sha256"])' "$HERE/../../../dependencies.lock")
            have=$(sha256sum "${IT_DRIVER_PROPERTIES:-/nonexistent}" 2>/dev/null | cut -c1-64)
            [ -n "$have" ] && [ "$have" = "$want" ] || { result "$1" fail "IT_DRIVER_PROPERTIES must be the pinned driver.properties (sha256 $want), got '${have:-no file}'"; return; }
            ENGINE_DRIVER=external
        fi
        topology_up "$1" || { ENGINE_DRIVER=embedded; result "$1" fail "topology did not come up"; return; }
        ENGINE_DRIVER=embedded # the drivers are up; later topologies start embedded engines
        if [ -n "$sfx" ]; then
            local why; why=$(check_engine_drivers) || { result "$1" fail "the engines are not on their external drivers:$why"; return; }
            result engines-on-external-drivers ok "6 nodes, each with publications on its own aeronmd ($(ls -d "$SHM"/drv-* | xargs -n1 basename | tr '\n' ' ')), no in-process driver dir"
        fi
        g "$TOOL" status
        if [ $RC -eq 0 ] && has "state: EMPTY (next: me_genesis)"; then result "empty-status$sfx" ok "status on the new pair: EMPTY";
        else result "empty-status$sfx" fail "exit $RC: $(tail -3 <<< "$OUT")"; fi
        full_genesis "$1" || return
        g "$TOOL" me_genesis
        if [ $RC -eq 3 ] && has "cluster is not empty: genesis refused"; then result "second-genesis-refused$sfx" ok "exit 3: $(head -2 <<< "$OUT" | tail -1)";
        else result "second-genesis-refused$sfx" fail "exit $RC: $(tail -3 <<< "$OUT")"; fi
        local bad="$D/init-trade.json"
        python3 -I -c "import json,sys; p=json.load(open(sys.argv[1])); p['InitTradeHighWater']={'tradeId': 1}; json.dump(p, open(sys.argv[2],'w'))" "$PLAN" "$bad"
        g "$TOOL" core_genesis "$bad"
        if [ $RC -eq 78 ] && has "ct-genesis: InitTradeHighWater is not part of a genesis"; then result "init-trade-refused$sfx" ok "exit 78: InitTradeHighWater is not part of a genesis";
        else result "init-trade-refused$sfx" fail "exit $RC: $(tail -2 <<< "$OUT")"; fi
        # the RED: with the emptiness check off the tool must run on this cluster
        g CT_TEST_DISABLE_EMPTINESS_CHECK=1 "$TC" me_genesis
        if [ $RC -eq 3 ]; then result "emptiness-red$sfx" fail "still refused with EMPTINESS_CHECK off";
        else result "emptiness-red$sfx" ok "RED: genesis ran on a cluster with history (ct_genesis_tc EMPTINESS_CHECK off: exit $RC, $(grep -c 'EngineConfig FRESH offered' <<< "$OUT") EngineConfig offered)"; fi ;;
    killed-fund)
        MODE=external
        topology_up killed-fund || { result killed-fund fail "topology did not come up"; return; }
        for k in 0 1 2 3; do
            phase_step "$k" "$TOOL" || { result killed-fund fail "$WHY"; return; }
        done
        # the admission reads are five queries; the eighth command is the third deposit
        g CT_GENESIS_TC_EXIT_AFTER_SENDS=8 "$TC" fund
        [ $RC -eq 137 ] || { result killed-fund fail "the killed fund exited $RC"; return; }
        g "$TOOL" status
        if ! has "state: NOT_A_PREFIX" || ! grep -qE '^  fund +PARTIAL +3 of ' <<< "$OUT"; then
            result killed-fund fail "status after the kill: $(grep -E 'fund|state:' <<< "$OUT" | tr '\n' ';')"; return
        fi
        local st; st=$(grep -E '^  fund|^state:|^last complete' <<< "$OUT" | tr '\n' ';')
        g "$TOOL" fund
        if [ $RC -eq 3 ] && has "genesis state is not a prefix of the plan: fund is PARTIAL" && has "wipe both clusters and redo"; then
            result killed-fund ok "status: $st fund refused: $(head -2 <<< "$OUT" | tail -1)"
        else result killed-fund fail "fund after the kill: exit $RC: $(tail -3 <<< "$OUT")"; fi
        for v in verify_funded activate me_genesis; do
            g "$TOOL" "$v"; [ $RC -eq 3 ] || result "killed-fund-$v" fail "$v exit $RC, not refused"
        done
        g CT_TEST_DISABLE_PREFIX_CHECK=1 "$TC" fund
        if [ $RC -eq 3 ]; then result prefix-red fail "still refused with PREFIX_CHECK off";
        else result prefix-red ok "RED: genesis continued from an unknown state (ct_genesis_tc PREFIX_CHECK off: exit $RC, $(grep -E 'deposit' <<< "$OUT" | head -1))"; fi ;;
    killed-core)
        MODE=external
        topology_up killed-core || { result killed-core fail "topology did not come up"; return; }
        for k in 0 1; do
            phase_step "$k" "$TOOL" || { result killed-core fail "$WHY"; return; }
        done
        start_bridge; sleep 2
        g CT_GENESIS_TC_EXIT_AFTER_SENDS=6 "$TC" core_genesis
        [ $RC -eq 137 ] || { result killed-core fail "the killed core_genesis exited $RC"; return; }
        g "$TOOL" status
        if ! grep -qE '^  core_genesis +PARTIAL +CoreConfig 1 of 3 recorded' <<< "$OUT" || ! has "state: NOT_A_PREFIX"; then
            result killed-core fail "status: $(grep -E 'core_genesis|state:' <<< "$OUT" | tr '\n' ';')"; return
        fi
        local refused=0
        for v in core_genesis configure_risk fund verify_funded activate; do g "$TOOL" "$v"; [ $RC -eq 3 ] && refused=$((refused + 1)); done
        [ $refused -eq 5 ] && result killed-core ok "core_genesis PARTIAL (CoreConfig 1 of 3), NOT_A_PREFIX, 5 of 5 later verbs refused" \
            || result killed-core fail "$refused of 5 verbs refused" ;;
    limits-red)
        MODE=external
        topology_up limits-red || { result limits-red fail "topology did not come up"; return; }
        for k in 0 1 2 3 4 5 6; do
            [ "${PHASES[$k]}" = core_genesis ] && { start_bridge; sleep 2; }
            g CT_TEST_DISABLE_LIMITS_STEP=1 "$TC" "${PHASES[$k]}"
            [ $RC -eq 0 ] || { result limits-red fail "${PHASES[$k]} with LIMITS_STEP off: exit $RC: $(tail -2 <<< "$OUT")"; return; }
        done
        g "$TOOL" status
        local cap; cap=$(sed -n 's/^limits: version [0-9]* requestPerUserCap \([0-9]*\).*/\1/p' <<< "$OUT")
        if [ "$cap" = 2048 ]; then result limits-red fail "the cap is 2048 without CoreLimitsSet";
        else result limits-red ok "RED: genesis left the request cap unset (requestPerUserCap $cap after a genesis without CoreLimitsSet)"; fi ;;
    full-embedded)
        MODE=embedded
        topology_up full-embedded || { result full-embedded fail "topology did not come up"; return; }
        if [ -f "$RECORD" ]; then log "an activation record of an earlier genesis is present: $(grep -m1 genesisId "$RECORD")"; fi
        g "$TOOL" status
        if [ $RC -eq 0 ] && has "state: EMPTY (next: me_genesis)"; then result empty-status-embedded ok "EMPTY over an old record";
        else result empty-status-embedded fail "exit $RC: $(tail -3 <<< "$OUT")"; fi
        full_genesis full-embedded ;;
    *) result "$1" fail "unknown topology" ;;
    esac
}

for t in "${TOPOLOGIES[@]}"; do run_topology "$t"; done
teardown
echo
echo "PASSED (${#PASSED[@]}): ${PASSED[*]}"
echo "FAILED (${#FAILED[@]}): ${FAILED[*]}"
[ ${#FAILED[@]} -eq 0 ]
