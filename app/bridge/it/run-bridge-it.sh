#!/bin/bash
# RC2 S9c W6: the T1-BR drills R-K0..R-K13 (contract 12.4) on real processes, one host, serial, each drill on a fresh
# topology so a failure is attributable:
#   - 1 or 3 PRODUCTION AE processes (assets-cluster.jar), the S9h T1 key table (`::users=*`),
#   - one `t1-journal` JVM (the S9h harness) hosting K scripted ME journal archives, controlled through a fifo that
#     `t1-core run --journal-ctl` writes to as well,
#   - `t1-core` one-shots (genesis, run, barrier, report, gate, oracle, latch-clear, open, deposit, withdraw),
#   - ct-bridge (or a never-published test build), the media driver per drill: `external` = a runner aeronmd from the
#     verified tree at a per-drill AERON_DIR on /dev/shm (the prod shape), `embedded` = BRIDGE_DRIVER_DIR on /dev/shm.
# Every drill writes evidence/<drill>/ (bridge logs, metrics before/after, oracle and report outputs) and one result
# line: PASS, FAIL, INCONCLUSIVE (a RED that did not fail, a precondition not met) or NOT RUN (no sudo, no input).
#
# Usage: app/bridge/it/run-bridge-it.sh <cmake-build-dir> <assets-cluster.jar> <harness.jar> <drill|all> [drill ...]
# Drills: rk0-external rk0-embedded tmpfs-min rk1 rk1-embedded rk2-external rk2-embedded rk2-nc rk3 rk4 rk5 rk6
#         rk8 rk9 rk9-nc rk10 rk11 rk11b rk12 rk12b rk12c rk12c-both rk13c rk13a rk13b rk13d-external rk13d-embedded
#         (`all`: this S9c batch). S10' T1-N2 (`n2`): rk14-smoke-external rk14-smoke-embedded rk14-smoke-red rk14-stop;
#         S10' halt (`halt`): rk2-stop-external rk2-stop-embedded rk2-stop-nc.
#         Three bridge slots: bridge0 key 3:BRIDGE id 1 port 26080, bridge1 10:BRIDGE id 2 26081, bridge_b 11:BRIDGE id 3
#         26082; IT_STAGE_A=1 turns an absent 4R core counter into a NOTE (the pre-<RC> core); IT_N2_ONLY0=1 never
#         starts bridge1 (the runner RED by hand; rk14-smoke-red does the same and checks the named failures).
# Env: IT_EVIDENCE (default app/bridge/it/evidence), IT_KEEP=1 keep work dirs, IT_ROUNDS (story rounds, default 12),
#      IT_LONG (records of the long round of R-K1/R-K7, default 1000000), IT_SEED (story seed base, default 9100).
set -uo pipefail

if [ $# -lt 4 ]; then
    echo "usage: $0 <cmake-build-dir> <assets-cluster.jar> <harness.jar> <drill|all> [drill ...]" >&2
    exit 64
fi
BUILD=$(cd "$1" && pwd)
AE_JAR=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
HARNESS=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
shift 3
ALL=(rk0-external rk0-embedded rk1 rk1-embedded rk2-external rk2-embedded rk2-nc rk3 rk4 rk5 rk6 rk8 rk9 rk9-nc
    rk10 rk11 rk11b rk12 rk12b rk12c rk12c-both rk13c rk13a rk13b rk13d-external rk13d-embedded tmpfs-min)
# S10' (contract 11): the N = 2 batch and the halt batch, each its own invocation
N2=(rk14-smoke-external rk14-smoke-embedded rk14-lag-external rk14-lag-embedded rk14-restart rk14-replace
    rk14-lanehole-ahead rk14-lanehole-behind rk14-lanehole-behind-dead rk14-lanehole-stale rk14-twoholes rk14-lag-failover
    rk14-stop rk14-smoke-red rk14-lanehole-ahead-nc rk14-lag-failover-nc)
HALT=(rk10b rk10c rk10-opsuspend rk11-regen rk12 rk10-fill rk10-restart rk10-fill-twice rk12-fill rk2-stop-external
    rk2-stop-embedded rk2-stop-nc rk11-regen-nc rk10b-negctl rk10-fill-nc rk12-fill-nc)
DRILLS=("$@")
case "${DRILLS[0]}" in
    all) DRILLS=("${ALL[@]}") ;;
    n2) DRILLS=("${N2[@]}") ;;
    halt) DRILLS=("${HALT[@]}") ;;
esac
HERE=$(cd "$(dirname "$0")" && pwd)
EVID=${IT_EVIDENCE:-$HERE/evidence}
mkdir -p "$EVID"
ROUNDS=${IT_ROUNDS:-12}
LONG=${IT_LONG:-1000000}
SEED_BASE=${IT_SEED:-9100}
bin() { find "$BUILD/app/bridge" -maxdepth 1 -name "$1" -type f -perm -u+x | head -1; }
BRIDGE=$(bin ct-bridge)
AERONMD=$(find "$BUILD/aeron" -name aeronmd -type f | head -1)
[ -x "$BRIDGE" ] || { echo "FATAL: ct-bridge not built" >&2; exit 1; }
[ -x "$AERONMD" ] || { echo "FATAL: aeronmd not built" >&2; exit 1; }
HEAD_SHA=$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo unknown)

hex() { printf "$1%.0s" $(seq 1 32); }
AE_KEYS="1:ADMIN:$(hex 11)::users=*;2:TOOL:$(hex 22)::users=*;3:BRIDGE:$(hex 33)::users=*;10:BRIDGE:$(hex aa)::users=*;11:BRIDGE:$(hex bb)::users=*;5:GATEWAY:$(hex 55)::users=*;6:FORWARDER:$(hex 66)::users=*;9:TREASURY:$(hex 99)::users=*"
BRIDGE_KEY="3:BRIDGE:$(hex 33)"
AE_BASE=24000
J_BASE=25000
BPORT=26080
JAVA_OPTS=(--add-opens java.base/jdk.internal.misc=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -Xms128m -Xmx1g -XX:+UseZGC)
NODE_ENV=(TRANSPORT_IDLE_MODE=backoff TRANSPORT_DRIVER_THREADING=shared TRANSPORT_TERM_LENGTH=4m
    TRANSPORT_LOG_TERM_LENGTH=16m TRANSPORT_SNAPSHOT_TERM_LENGTH=8m)

log() { echo "[$(date +%H:%M:%S)] $*"; }
RESULTS="$EVID/results.txt"
{
    echo "# run-bridge-it.sh $(date -u +%Y-%m-%dT%H:%M:%SZ) host $(hostname) $(uname -m) nproc $(nproc) head $HEAD_SHA"
    echo "# ct-bridge sha256 $(sha256sum "$BRIDGE" | cut -c1-64) assets $(sha256sum "$AE_JAR" | cut -c1-16) harness $(sha256sum "$HARNESS" | cut -c1-16)"
} >> "$RESULTS"

# ------------------------------------------------------------------ per-drill topology
declare -A PID
D=""; SHM=""; STORY=""; N=0; NAE=1; K=1; MODE=external; JOUT=""; TOOLDRV=""; BRDRV=""; JLINES=0
teardown() {
    exec 7>&- 2>/dev/null || true
    for name in "${!PID[@]}"; do
        kill -9 "${PID[$name]}" 2>/dev/null || true
    done
    # every background child of this runner, also one whose PID[] slot was overwritten by a restart
    local orphan
    for orphan in $(jobs -p); do kill -9 "$orphan" 2>/dev/null || true; done
    wait 2>/dev/null || true
    PID=()
    if [ -n "$SHM" ]; then
        if mountpoint -q "$SHM/tmpfs" 2>/dev/null; then sudo umount "$SHM/tmpfs" 2>/dev/null || true; fi
        rm -rf "$SHM"
    fi
    if [ -n "$D" ] && [ "${IT_KEEP:-0}" != "1" ]; then rm -rf "$D"; fi
    SHM=""; D=""
}
trap teardown EXIT
trap 'exit 143' TERM INT HUP # a timeout or ^C still runs the EXIT trap (the children die with the runner)

start_driver() { # dir log name
    env AERON_DIR="$1" AERON_DIR_DELETE_ON_START=1 AERON_DIR_DELETE_ON_SHUTDOWN=1 "$AERONMD" > "$2" 2>&1 &
    PID[$3]=$!
    for _ in $(seq 1 100); do [ -f "$1/cnc.dat" ] && return 0; sleep 0.1; done
    echo "FATAL: media driver $3 did not start" >&2
    return 1
}

ae_addresses() { local a=localhost; for _ in $(seq 2 "$NAE"); do a="$a,localhost"; done; echo "$a"; }
ae_ingress() { local s=""; for i in $(seq 0 $((NAE - 1))); do s="$s${s:+,}$i=localhost:$((AE_BASE + 100 * i + 2))"; done; echo "$s"; }

start_ae() { # node
    local i=$1
    mkdir -p "$D/ae$i"
    env "${NODE_ENV[@]}" CLUSTER_NODE="$i" CLUSTER_ADDRESSES="$(ae_addresses)" CLUSTER_PORT_BASE=$AE_BASE \
        CLUSTER_PORT_STRIDE=100 BASE_DIR="$D/ae$i" ASSETS_AUTH_KEYS="$AE_KEYS" ASSETS_NODE_PORT=$((AE_BASE + 90 + i)) \
        java "${JAVA_OPTS[@]}" -Daeron.dir="$SHM/ae$i" -jar "$AE_JAR" >> "$D/ae$i.log" 2>&1 &
    PID[ae$i]=$!
}

metric() { # url name -> value of the first sample line whose name (labels included) matches exactly
    curl -s --max-time 2 "$1" 2>/dev/null | awk -v n="$2" '$1 == n { print $2; exit }'
}
# S10' section 2 (T1-N2): three bridge slots, each with its own key, identity and HTTP port. The single-bridge drills of
# S9c run on slot bridge0 (port 26080) and keep their calls: every per-bridge helper below takes an optional leading
# slot name (bm bridge1 bridge_halted), the default is bridge0.
SLOTS=(bridge0 bridge1 bridge_b)
declare -A SLOT_PORT=([bridge0]=26080 [bridge1]=26081 [bridge_b]=26082)
declare -A SLOT_KEY=([bridge0]="3:BRIDGE:$(hex 33)" [bridge1]="10:BRIDGE:$(hex aa)" [bridge_b]="11:BRIDGE:$(hex bb)")
declare -A SLOT_ID=([bridge0]=1 [bridge1]=2 [bridge_b]=3)
is_slot() { [ -n "${1:-}" ] && [ -n "${SLOT_PORT[$1]:-}" ]; }
port_of() { echo "${SLOT_PORT[$1]}"; }
# usage: SL=bridge0; slot_arg "$@" && shift  -> sets SL to the named slot when the first argument is one
slot_arg() { if is_slot "${1:-}"; then SL=$1; return 0; fi; SL=bridge0; return 1; }
bm() { local SL; slot_arg "$@" && shift; metric "http://127.0.0.1:$(port_of "$SL")/metrics" "$1"; }
bm0() { local v; v=$(bm "$@"); echo "${v:-0}"; }
bm_sum() { # [slot] family -> the sum over its labelled samples
    local SL; slot_arg "$@" && shift
    curl -s --max-time 2 "http://127.0.0.1:$(port_of "$SL")/metrics" 2>/dev/null |
        awk -v f="$1" 'index($1, f "{") == 1 || $1 == f { s += $2 } END { print s + 0 }'
}
scrape() { # [slot] label (the default slot keeps the S9c file names)
    local SL tag=""; slot_arg "$@" && { shift; tag="$SL-"; }
    curl -s --max-time 3 "http://127.0.0.1:$(port_of "$SL")/metrics" > "$D/ev/metrics-$tag$1.prom" 2>/dev/null || true
    curl -s --max-time 3 "http://127.0.0.1:$(port_of "$SL")/health" > "$D/ev/health-$tag$1.json" 2>/dev/null || true
}
health() { local SL; slot_arg "$@" && shift; curl -s --max-time 2 "http://127.0.0.1:$(port_of "$SL")/health" 2>/dev/null; }
http_code() { local SL; slot_arg "$@" && shift; curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$(port_of "$SL")/$1"; }
# the live bridge slots (a single-bridge drill's PID[bridge] is slot bridge0)
live_slots() {
    local s; for s in "${SLOTS[@]}"; do
        if alive "$s" || { [ "$s" = bridge0 ] && alive bridge; }; then echo "$s"; fi
    done
}
leader_of() {
    for i in $(seq 0 $((NAE - 1))); do
        [ "$(metric "http://127.0.0.1:$((AE_BASE + 90 + i))/metrics" assets_cluster_role)" = "2" ] && { echo "$i"; return 0; }
    done
    return 1
}
ae_metric() { local l; l=$(leader_of) || { echo ""; return; }; metric "http://127.0.0.1:$((AE_BASE + 90 + l))/metrics" "$1"; }
wait_leader() { for _ in $(seq 1 600); do leader_of > /dev/null && return 0; sleep 0.1; done; return 1; }

T1ENV=()
# the oracle replays the story through the real core code in its own JVM: the 10^6 story needs a 4 GB heap
t1core() { env "${T1ENV[@]}" java "${JAVA_OPTS[@]}" -Xmx4g -jar "$HARNESS" t1-core "$@" | sed '/^feed-harness /d'; }

# An operator re-journals source k from scratch (the node's archive replaced: KILL, the catalog wiped, REVIVE, a complete
# recording 1..upto). The successor-recording shapes (FILL, RESTART) are not reached by the halted walk behind the old
# recording's hole or refusal (S9c README finding F-1); a replaced history is (the S9b E3 / property fill).
rejournal() { # k upto
    jctl 30 KILL "$1" >> "$D/ev/jctl.txt"
    rm -rf "$D/journal/src$1"
    jctl 60 REVIVE "$1" >> "$D/ev/jctl.txt"
    jctl 30 RECORD "$1" >> "$D/ev/jctl.txt"
    jctl 300 WRITE "$1" 1 "$2" >> "$D/ev/jctl.txt"
}

# t1-journal over a fifo; jctl writes a line and waits for its reply in the journal's stdout
jctl() { # timeout_s line...
    local t=$1; shift
    local before; before=$(wc -l < "$JOUT")
    echo "$*" >&7
    for _ in $(seq 1 $((t * 10))); do
        local now; now=$(wc -l < "$JOUT")
        if [ "$now" -gt "$before" ]; then sed -n "$((before + 1))p" "$JOUT"; return 0; fi
        sleep 0.1
    done
    echo "TIMEOUT $*"
    return 1
}

# Wait for a child to exit; sets WAIT_RC (255 = still running).
WAIT_RC=0
wait_exit() { # name timeout_s
    local p=${PID[$1]:-} n=$(($2 * 10)) st
    [ -z "$p" ] && { WAIT_RC=254; return 1; }
    for _ in $(seq 1 "$n"); do
        st=$(ps -o stat= -p "$p" 2>/dev/null)
        if [ -z "$st" ] || [[ "$st" == Z* ]]; then wait "$p"; WAIT_RC=$?; unset "PID[$1]"; return 0; fi
        sleep 0.1
    done
    WAIT_RC=255
    return 1
}
alive() { [ -n "${PID[$1]:-}" ] && kill -0 "${PID[$1]}" 2>/dev/null; }
now_s() { date +%s.%N; }
elapsed() { python3 -I -c "import sys; print(f'{float(sys.argv[2]) - float(sys.argv[1]):.2f}')" "$1" "$2"; }
fle() { python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) <= float(sys.argv[2]) else 1)" "$1" "$2"; }

# setup <drill> <K sources> <AE nodes> <story args...>
setup() {
    local drill=$1; K=$2; NAE=$3; shift 3
    D=$(mktemp -d -t "ct-bridge-it-$drill.XXXXXX")
    SHM=$(mktemp -d "/dev/shm/ct-bridge-it-$drill.XXXXXX")
    mkdir -p "$D/ev"
    STORY="$D/story"
    local seed=$1 rounds=$2; shift 2
    java -jar "$HARNESS" story "$seed" "$rounds" "$STORY" "$@" > "$D/story.log" 2>&1 || { log "story failed: $(tail -2 "$D/story.log")"; return 1; }
    N=$(sed -n 's/^records=//p' "$STORY/story.txt")
    TOOLDRV="$SHM/tool-driver"
    start_driver "$TOOLDRV" "$D/tool-driver.log" tooldrv || return 1
    T1ENV=(T1_CORE_INGRESS="$(ae_ingress)" T1_AERON_DIR="$TOOLDRV" T1_ADMIN_KEY="1:ADMIN:$(hex 11)" T1_TOOL_KEY="2:TOOL:$(hex 22)"
        T1_GATEWAY_KEY="5:GATEWAY:$(hex 55)" T1_TREASURY_KEY="9:TREASURY:$(hex 99)" T1_FORWARDER_KEY="6:FORWARDER:$(hex 66)"
        T1_BRIDGE_KEY="$BRIDGE_KEY" T1_JOURNAL_DIR="$D/journal")
    for i in $(seq 0 $((NAE - 1))); do start_ae "$i"; done
    wait_leader || { log "no AE leader"; return 1; }
    mkfifo "$D/jctl"
    JOUT="$D/journal.out"
    : > "$JOUT"
    env T1_JOURNAL_DIR="$D/journal" java "${JAVA_OPTS[@]}" -jar "$HARNESS" t1-journal < "$D/jctl" > "$JOUT" 2> "$D/journal.err" &
    PID[journal]=$!
    exec 7> "$D/jctl"
    jctl 60 SOURCES "$K" "$J_BASE" > "$D/ev/sources.txt" || return 1
    jctl 120 LOAD "$STORY/journal.fjr" >> "$D/ev/sources.txt" || return 1
    t1core genesis --story "$STORY" > "$D/ev/genesis.txt" 2>&1 || { log "genesis failed: $(tail -3 "$D/ev/genesis.txt")"; return 1; }
    log "setup $drill: K=$K AE=$NAE N=$N story $*"
}

archives() { local s=""; for k in $(seq 0 $((K - 1))); do s="$s${s:+,}localhost:$((J_BASE + 100 * k))"; done; echo "$s"; }

subset_archives() { # csv of source indexes -> their archive list (S10' 3.5: a drill and lag device, never the prod rule)
    local s="" k
    for k in ${1//,/ }; do s="$s${s:+,}localhost:$((J_BASE + 100 * k))"; done
    echo "$s"
}

# start_bridge <name|slot> <mode> [binary] [SOURCES_OF=<csv>] [DRIVER_TIMEOUT_MS=<ms>] [extra env...]
# A slot name (bridge0, bridge1, bridge_b; S10' section 2) derives the key, the identity and the HTTP port, the process
# is PID[<slot>] and logs to ev/<slot>.log (a restart appends, marked in ev/timeline.txt). Any other name is the S9c
# single-bridge shape: slot bridge0's key, identity and port, PID[bridge], ev/<name>.log. Extra env (e.g. a test build's
# BRIDGE_* override) comes last and wins.
start_bridge() {
    local name=$1 mode=$2 binary=${3:-$BRIDGE}; shift 3 2>/dev/null || shift $#
    local slot=bridge0 pidkey=bridge
    if is_slot "$name"; then slot=$name; pidkey=$name; fi
    MODE=$mode
    local arch; arch=$(archives)
    local extra=() kv driverEnv
    for kv in "$@"; do
        case $kv in
            SOURCES_OF=*) arch=$(subset_archives "${kv#SOURCES_OF=}") ;;
            DRIVER_TIMEOUT_MS=*) extra+=("BRIDGE_DRIVER_TIMEOUT_MS=${kv#*=}") ;;
            *) extra+=("$kv") ;;
        esac
    done
    if [ "$mode" = external ]; then
        BRDRV=${BRDRV_OVERRIDE:-$SHM/bridge-driver}
        if [ ! -f "$BRDRV/cnc.dat" ] || ! alive brdrv; then start_driver "$BRDRV" "$D/bridge-driver.log" brdrv || return 1; fi
        driverEnv="AERON_DIR=$BRDRV"
    else
        BRDRV=${BRDRV_OVERRIDE:-$SHM/bridge-embedded} # each instance in <dir>/ct-bridge-<id>
        mkdir -p "$BRDRV"
        driverEnv="BRIDGE_DRIVER_DIR=$BRDRV"
    fi
    env -i PATH="$PATH" BRIDGE_ME_JOURNAL_ARCHIVES="$arch" BRIDGE_JOURNAL_CONTROL_STREAM=4010 BRIDGE_JOURNAL_STREAM=4001 \
        BRIDGE_JOURNAL_CHANNEL=aeron:ipc BRIDGE_REPLAY_STREAM=9101 BRIDGE_JOURNAL_MTU=1408 BRIDGE_LOCAL_HOST=localhost \
        BRIDGE_DRIVER_MODE="$mode" "$driverEnv" BRIDGE_DRIVER_TIMEOUT_MS=5000 BRIDGE_AE_CLUSTER_ADDRESSES="$(ae_addresses)" \
        BRIDGE_AE_PORT_BASE=$AE_BASE CLUSTER_PORT_STRIDE=100 BRIDGE_AE_EGRESS_ENDPOINT=127.0.0.1:0 \
        AE_AUTH_CREDENTIAL="${SLOT_KEY[$slot]}" BRIDGE_INSTANCE_ID="${SLOT_ID[$slot]}" \
        BRIDGE_HTTP_BIND="127.0.0.1:$(port_of "$slot")" "${extra[@]}" "$binary" >> "$D/ev/$name.log" 2>&1 &
    PID[$pidkey]=$!
    tl "start $name pid ${PID[$pidkey]} mode $mode sources $arch ${extra[*]:-}"
    for _ in $(seq 1 100); do [ "$(http_code "$slot" health)" != "000" ] && return 0; alive "$pidkey" || return 1; sleep 0.1; done
    return 1
}
wait_registered() { # [slot] timeout_s
    local SL; slot_arg "$@" && shift
    for _ in $(seq 1 $(($1 * 10))); do [ "$(bm0 "$SL" bridge_registered)" = "1" ] && return 0; sleep 0.1; done; return 1
}
tl() { [ -n "$D" ] && echo "$(date +%s.%N) $*" >> "$D/ev/timeline.txt"; }

# The quiescence barrier: t1-core barrier N, then two /metrics samples 1 s apart at cursor N, staging and ring empty.
quiescent() { # seq timeout_ms
    t1core barrier "$1" "$2" > "$D/ev/barrier-$1.txt" 2>&1 || return 1
    # the cursor gauge and the ring move at the next report (periodic, 10 s): two consecutive good samples, 1 s apart,
    # on EVERY live bridge (S10' section 2)
    local good=0 s ok
    for _ in $(seq 1 30); do
        sleep 1
        ok=1
        for s in $(live_slots); do
            if [ "$(bm0 "$s" bridge_core_cursor)" != "$1" ] || [ "$(bm0 "$s" bridge_staged_trades)" != "0" ] ||
                [ "$(bm0 "$s" bridge_inflight_ring_depth)" != "0" ]; then
                ok=0
            fi
        done
        if [ $ok = 1 ]; then
            good=$((good + 1)); [ $good -ge 2 ] && return 0
        else
            good=0
        fi
    done
    return 1
}
run_story() { # args... (t1-core run with the journal fifo)
    t1core run "$STORY" --journal-ctl "$D/jctl" "$@"
}
barrier_of() { sed -n "$(($1 + 1))p" "$STORY/barriers.txt" | awk '{ print $NF }'; } # the last seq of round r
# a seq inside round r (not its first): the core already applied round r's command block when the feed stops there, so
# `oracle --upto s-1` compares a prefix the model can express (the model applies a round's commands with its records)
mid_round() { local a b; a=$(barrier_of $(($1 - 1))); b=$(barrier_of "$1"); echo $(( a + (b - a + 1) / 2 + 1 )); }
oracle_ok() { # label [args...] -> 0 when PASS with the expected latch line in ORACLE_LATCH (default "L false 0")
    local label=$1; shift
    t1core oracle "$STORY" "$@" > "$D/ev/oracle-$label.txt" 2>&1
    local rc=$?
    grep -q "^PASS " "$D/ev/oracle-$label.txt" && grep -q "^latch ${ORACLE_LATCH:-L false 0}\$" "$D/ev/oracle-$label.txt" && [ $rc -eq 0 ]
}
common_checks() { # appends to FAILS the shared lines of every drill, per live bridge, then the core's
    local s p
    for s in $(live_slots); do
        p=""; [ "$s" != bridge0 ] && p="$s "
        [ "$(bm0 "$s" bridge_feed_halt_rejected_total)" = "0" ] || FAILS+=("${p}feed_halt_rejected $(bm0 "$s" bridge_feed_halt_rejected_total)")
        [ "$(bm0 "$s" bridge_aeron_errors_total)" = "0" ] || FAILS+=("${p}aeron_errors $(bm0 "$s" bridge_aeron_errors_total)")
        [ "$(bm0 "$s" bridge_log_dropped_total)" = "0" ] || FAILS+=("${p}log_dropped $(bm0 "$s" bridge_log_dropped_total)")
    done
    # at N = 2 duplicates are recorded, never asserted zero (S9b 15c N-9); a mismatch is a defect in every drill
    local mm; mm=$(ae_metric assets_feed_duplicate_mismatch_total)
    [ "${mm:-0}" = "0" ] || FAILS+=("core duplicate mismatches $mm")
    echo "core feed_duplicate_mismatch ${mm:-?} unverified $(ae_metric assets_feed_duplicate_unverified_total)" >> "$D/ev/core.txt"
}
FAILS=()
NOTES=()
finish() { # drill verdict(PASS|FAIL|INCONCLUSIVE|NOT RUN) -> evidence + result line
    local drill=$1 verdict=$2
    scrape after 2>/dev/null || true
    cp "$D"/*.log "$D/ev/" 2>/dev/null || true
    cp "$D/journal.out" "$D/journal.err" "$D/ev/" 2>/dev/null || true
    rm -rf "${EVID:?}/$drill"
    mkdir -p "$EVID/$drill"
    cp -r "$D/ev/." "$EVID/$drill/"
    local detail; detail="$(IFS='; '; echo "${FAILS[*]:-} ${NOTES[*]:-}")"
    echo "$drill $verdict $detail" >> "$RESULTS"
    log "DRILL $verdict $drill: $detail"
    teardown
    FAILS=(); NOTES=()
}
verdict() { [ ${#FAILS[@]} -eq 0 ] && echo PASS || echo FAIL; }

# ------------------------------------------------------------------ the drills

clean_run() { # drill mode
    local drill=$1 mode=$2
    setup "$drill" 1 1 $((SEED_BASE)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge "$mode" || FAILS+=("bridge start")
    wait_registered 30 || FAILS+=("not registered in 30 s")
    scrape before
    if [ "$mode" = embedded ]; then du -sk "$BRDRV" > "$D/ev/driver-dir-size.txt" 2>&1; fi
    run_story --timeout 120000 > "$D/ev/run.txt" 2>&1 || FAILS+=("run: $(tail -1 "$D/ev/run.txt")")
    quiescent "$N" 60000 || FAILS+=("quiescence at $N")
    if [ "$mode" = embedded ]; then du -sk "$BRDRV" >> "$D/ev/driver-dir-size.txt" 2>&1; else du -sk "$BRDRV" > "$D/ev/driver-dir-size.txt" 2>&1; fi
    oracle_ok final || FAILS+=("oracle: $(head -2 "$D/ev/oracle-final.txt" | tr '\n' ' ')")
    local fwd; fwd=$(bm_sum bridge_forwarded_records_total)
    [ "$fwd" = "$N" ] || FAILS+=("forwarded records $fwd != N $N")
    [ "$(bm0 bridge_halted)" = "0" ] || FAILS+=("halted")
    [ "$(bm0 bridge_epochs_total)" = "1" ] || FAILS+=("epochs $(bm0 bridge_epochs_total)")
    [ "$(bm0 bridge_client_sessions_opened_total)" = "1" ] || FAILS+=("sessions $(bm0 bridge_client_sessions_opened_total)")
    [ "$(bm "bridge_driver_mode{mode=\"$mode\"}")" = "1" ] || FAILS+=("driver mode label")
    common_checks
    alive bridge || FAILS+=("bridge died")
}

drill_rk0() { clean_run "rk0-$1" "$1"; finish "rk0-$1" "$(verdict)"; }

drill_tmpfs_min() {
    # embedded mode on a tmpfs of decreasing size; the journal recorded with the production 64m term (a 192 MB replay
    # image). The smallest passing size is the README number (NOTE-5).
    if ! sudo -n true 2>/dev/null; then echo "tmpfs-min NOT RUN no sudo" >> "$RESULTS"; return; fi
    local sizes=(512 384 320 288 256 240 224 208) last_ok=""
    for mb in "${sizes[@]}"; do
        setup tmpfs-min 1 1 $((SEED_BASE + 1)) 8 || { FAILS+=("setup"); finish "tmpfs-min-$mb" FAIL; continue; }
        mkdir -p "$SHM/tmpfs"
        sudo mount -t tmpfs -o size=${mb}m tmpfs "$SHM/tmpfs" && sudo chmod 1777 "$SHM/tmpfs"
        jctl 60 RECORD 0 term=64m > /dev/null
        BRDRV_OVERRIDE="$SHM/tmpfs" start_bridge bridge embedded || FAILS+=("bridge start")
        if wait_registered 30 && run_story --timeout 60000 > "$D/ev/run.txt" 2>&1 && quiescent "$N" 30000 &&
            [ "$(bm_sum 'bridge_replay_open_failures_total')" = "0" ]; then
            last_ok=$mb
            NOTES+=("${mb} MB passes; used $(df -m --output=used "$SHM/tmpfs" | tail -1 | tr -d ' ') MB")
            finish "tmpfs-min-$mb" PASS
        else
            NOTES+=("${mb} MB fails: storage failures $(bm_sum bridge_replay_open_failures_total)")
            finish "tmpfs-min-$mb" FAIL
            break
        fi
    done
    echo "tmpfs-min RESULT smallest passing embedded tmpfs ${last_ok:-none} MB (64m-term journal)" >> "$RESULTS"
}

drill_rk1() {
    # the long story (S9h --long-round): kill -9 the bridge between 40 and 60 % of round 1; restart within 2 s. Then
    # R-K7: the cold restart behind every applied record, then the rest of the story.
    setup rk1 1 1 $((SEED_BASE + 2)) 8 --long-round "$LONG" || { FAILS+=("setup"); finish rk1 FAIL; return; }
    start_bridge bridge external || FAILS+=("bridge start")
    wait_registered 30 || FAILS+=("not registered")
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    local end1; end1=$(barrier_of 1)
    run_story --from 1 --to 1 --timeout 1800000 > "$D/ev/run1.txt" 2>&1 &
    local runner=$!
    local lo=$((end1 * 40 / 100)) hi=$((end1 * 60 / 100)) c=0
    # the offered seq moves at every offer (the cursor gauge only at a report, every 10 s)
    for _ in $(seq 1 6000); do c=$(bm0 bridge_last_offered_seq); [ "$c" -ge "$lo" ] && break; sleep 0.02; done
    NOTES+=("killed at offered seq $c of $end1 (window $lo..$hi)")
    [ "$c" -le "$hi" ] || FAILS+=("offered seq $c past 60 % before the kill")
    scrape killed
    kill -9 "${PID[bridge]}"; wait_exit bridge 5
    local t0; t0=$(now_s)
    sleep 1
    start_bridge bridge2 external || FAILS+=("restart")
    local reg=no
    for _ in $(seq 1 100); do [ "$(bm0 bridge_registered)" = "1" ] && [ "$(health | grep -c '"reportThisTerm":true')" = "1" ] && { reg=yes; break; }; sleep 0.1; done
    [ "$reg" = yes ] || FAILS+=("not registered and reported within 10 s")
    local walk=""
    for _ in $(seq 1 300); do
        [ "$(bm_sum bridge_forwarded_records_total)" != "0" ] && { walk=$(elapsed "$t0" "$(now_s)"); break; }; sleep 0.1
    done
    NOTES+=("cold walk to the first forward ${walk:-none} s")
    [ -n "$walk" ] && fle "$walk" 10 || FAILS+=("first forward ${walk:-none} s > 10 s")
    wait "$runner" || FAILS+=("round 1 run: $(tail -1 "$D/ev/run1.txt")")
    quiescent "$end1" 120000 || FAILS+=("quiescence at $end1")
    [ "$(bm_sum bridge_settle_rejected_total)" = "0" ] || FAILS+=("SettleRejected after the restart")
    oracle_ok upto-round1 --upto "$end1" || FAILS+=("oracle --upto $end1")
    NOTES+=("duplicates (recorded, N-9): $(ae_metric assets_feed_duplicate_unverified_total) unverified")
    common_checks
    scrape rk1-end
    local rk1fails=("${FAILS[@]}")
    # ---- R-K7: SIGTERM, a cold restart (an empty memo) behind every applied record, then the next records
    FAILS=()
    kill -TERM "${PID[bridge]}"; wait_exit bridge 10
    [ "$WAIT_RC" = "0" ] || FAILS+=("SIGTERM exit $WAIT_RC (expected 0)")
    local t1; t1=$(now_s)
    start_bridge bridge3 external || FAILS+=("cold restart")
    wait_registered 15 || FAILS+=("not registered")
    local gapmax=0
    for _ in $(seq 1 600); do
        local g; g=$(bm0 bridge_invoker_max_gap_ms); [ "${g:-0}" -gt "$gapmax" ] && gapmax=$g
        [ "$(bm_sum bridge_skipped_total)" -ge "$end1" ] && break
        sleep 0.1
    done
    local walkdone; walkdone=$(elapsed "$t1" "$(now_s)")
    local tw; tw=$(now_s)
    run_story --from 2 --timeout 600000 > "$D/ev/run2.txt" 2>&1 &
    runner=$!
    local first=""
    for _ in $(seq 1 300); do [ "$(bm_sum bridge_forwarded_records_total)" != "0" ] && { first=$(elapsed "$tw" "$(now_s)"); break; }; sleep 0.1; done
    wait "$runner" || FAILS+=("rounds 2..: $(tail -1 "$D/ev/run2.txt")")
    quiescent "$N" 60000 || FAILS+=("quiescence at $N")
    for _ in 1 2 3; do local g; g=$(bm0 bridge_invoker_max_gap_ms); [ "${g:-0}" -gt "$gapmax" ] && gapmax=$g; sleep 0.5; done
    [ "$gapmax" -le 1500 ] || FAILS+=("invoker max gap $gapmax ms")
    [ "$(bm0 bridge_client_keepalive_failures_total)" = "0" ] || FAILS+=("keepalive failures")
    [ "$(bm0 bridge_client_sessions_opened_total)" = "1" ] || FAILS+=("sessions $(bm0 bridge_client_sessions_opened_total)")
    [ -n "$first" ] && fle "$first" 10 || FAILS+=("first new forward ${first:-none} s after the write")
    local skipped; skipped=$(bm_sum bridge_skipped_total)
    [ "$skipped" = "$end1" ] || FAILS+=("skipped $skipped != $end1")
    oracle_ok final || FAILS+=("final oracle")
    common_checks
    NOTES+=("R-K7: walk over $end1 applied records took $walkdone s, invoker max gap $gapmax ms, first new forward $first s")
    local rk7=("${FAILS[@]}")
    FAILS=("${rk1fails[@]}")
    echo "rk7 $([ ${#rk7[@]} -eq 0 ] && echo PASS || echo FAIL) $(IFS='; '; echo "${rk7[*]:-}") walk $walkdone s gap $gapmax ms first $first s N $end1" >> "$RESULTS"
    finish rk1 "$(verdict)"
}

drill_rk1_embedded() {
    # three kill -9 + restart cycles in embedded mode on a tmpfs sized at the R-K0 minimum plus one replay image:
    # every restart reclaims the per-instance dir, no storage failure (FIX-1)
    if ! sudo -n true 2>/dev/null; then echo "rk1-embedded NOT RUN no sudo" >> "$RESULTS"; return; fi
    local size=${IT_TMPFS_MB:-512}
    setup rk1-embedded 1 1 $((SEED_BASE + 3)) 30 || { FAILS+=("setup"); finish rk1-embedded FAIL; return; }
    mkdir -p "$SHM/tmpfs"; sudo mount -t tmpfs -o size=${size}m tmpfs "$SHM/tmpfs" && sudo chmod 1777 "$SHM/tmpfs"
    jctl 60 RECORD 0 term=64m > /dev/null
    BRDRV_OVERRIDE="$SHM/tmpfs" start_bridge bridge embedded || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 4 --timeout 60000 > "$D/ev/run-0.txt" 2>&1 || FAILS+=("rounds 0..4")
    local from=5
    for cycle in 1 2 3; do
        kill -9 "${PID[bridge]}"; wait_exit bridge 5
        BRDRV_OVERRIDE="$SHM/tmpfs" start_bridge "bridge-$cycle" embedded || FAILS+=("restart $cycle")
        wait_registered 15 || FAILS+=("registered $cycle")
        [ "$(bm0 bridge_driver_dir_reclaimed_total)" = "1" ] || FAILS+=("cycle $cycle reclaimed $(bm0 bridge_driver_dir_reclaimed_total)")
        run_story --from $from --to $((from + 4)) --timeout 60000 > "$D/ev/run-$cycle.txt" 2>&1 || FAILS+=("rounds after restart $cycle")
        [ "$(bm "bridge_replay_open_failures_total{reason=\"storage\"}")" = "0" ] || FAILS+=("storage failures after $cycle")
        from=$((from + 5))
        NOTES+=("cycle $cycle tmpfs used $(df -m --output=used "$SHM/tmpfs" | tail -1 | tr -d ' ') MB")
    done
    run_story --from $from --timeout 60000 > "$D/ev/run-end.txt" 2>&1 || FAILS+=("the rest")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    finish rk1-embedded "$(verdict)"
}

kill_leader_under_load() { # drill mode binary -> sets KILLED_INFLIGHT
    local drill=$1 mode=$2 binary=$3
    KILLED_INFLIGHT=0
    start_bridge bridge "$mode" "$binary" || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    local end1; end1=$(barrier_of 1)
    run_story --from 1 --timeout 600000 > "$D/ev/run.txt" 2>&1 &
    local runner=$!
    for _ in $(seq 1 3000); do [ "$(bm0 bridge_core_cursor)" -ge $((end1 / 3)) ] && break; sleep 0.02; done
    local before; before=$(bm0 bridge_client_offers_in_ended_terms_total)
    local fences0; fences0=$(bm0 'bridge_fences_total{trigger="new_leader"}')
    scrape before-kill
    local l; l=$(leader_of)
    kill -9 "${PID[ae$l]}"; wait_exit "ae$l" 5
    local tk; tk=$(now_s)
    local fenced=""
    for _ in $(seq 1 100); do [ "$(bm0 'bridge_fences_total{trigger="new_leader"}')" -gt "$fences0" ] && { fenced=$(elapsed "$tk" "$(now_s)"); break; }; sleep 0.1; done
    wait_leader
    sleep 3
    start_ae "$l"
    wait "$runner" || NOTES+=("run: $(tail -1 "$D/ev/run.txt")")
    KILLED_INFLIGHT=$(( $(bm0 bridge_client_offers_in_ended_terms_total) - before ))
    FENCED_IN=${fenced:-none}
}

drill_rk2() { # mode
    local mode=$1 drill=rk2-$1 tries=0
    while :; do
        tries=$((tries + 1))
        setup "$drill" 1 3 $((SEED_BASE + 10 + tries)) 8 --long-round 200000 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
        kill_leader_under_load "$drill" "$mode" "$BRIDGE"
        quiescent "$N" 120000 || FAILS+=("quiescence at $N")
        NOTES+=("try $tries: offers in ended terms +$KILLED_INFLIGHT, fence after ${FENCED_IN} s")
        [ "$FENCED_IN" != none ] && fle "$FENCED_IN" 5 || FAILS+=("no NEW_LEADER fence within 5 s")
        [ "$(bm0 'bridge_settle_rejected_total{reason="6"}')" = "0" ] || FAILS+=("FEED_GAP rejects")
        t1core report > "$D/ev/report.json" 2>&1
        grep -q '"feedHalted":false' "$D/ev/report.json" || FAILS+=("core halted: $(cat "$D/ev/report.json")")
        oracle_ok final || FAILS+=("oracle")
        common_checks
        if [ "$KILLED_INFLIGHT" -gt 0 ] || [ $tries -ge 5 ] || [ ${#FAILS[@]} -gt 0 ]; then break; fi
        finish "$drill-try$tries" "$(verdict)"
    done
    if [ ${#FAILS[@]} -eq 0 ] && [ "$KILLED_INFLIGHT" -eq 0 ]; then finish "$drill" INCONCLUSIVE; else finish "$drill" "$(verdict)"; fi
}

drill_rk2_nc() {
    # RED ct_bridge_nc_nofence (FENCE_OFF + REGISTRATION_GATE_OFF): with an in-flight loss the core answers FEED_GAP or
    # latches GAP, and the oracle FAILs or stalls
    local nc; nc=$(bin ct_bridge_nc_nofence)
    local tries=0 red=no
    while [ $tries -lt 5 ] && [ $red = no ]; do
        tries=$((tries + 1))
        setup rk2-nc 1 3 $((SEED_BASE + 20 + tries)) 8 --long-round 200000 || { FAILS+=("setup"); finish rk2-nc FAIL; return; }
        kill_leader_under_load rk2-nc external "$nc"
        sleep 10
        local gaps; gaps=$(bm0 'bridge_settle_rejected_total{reason="6"}')
        t1core report > "$D/ev/report.json" 2>&1
        oracle_ok final; local orc=$?
        NOTES+=("try $tries: in-flight +$KILLED_INFLIGHT, FEED_GAP rejects $gaps, report $(tr -d '\n' < "$D/ev/report.json" | cut -c1-160), oracle rc $orc")
        if [ "$gaps" != "0" ] || grep -q '"haltReason":1' "$D/ev/report.json" || [ $orc -ne 0 ]; then red=yes; fi
        [ $red = no ] && finish "rk2-nc-try$tries" INCONCLUSIVE
    done
    [ $red = yes ] && finish rk2-nc PASS || finish rk2-nc INCONCLUSIVE
}

drill_rk3() {
    setup rk3 1 1 $((SEED_BASE + 30)) "$ROUNDS" || { FAILS+=("setup"); finish rk3 FAIL; return; }
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 5 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..5")
    local st0; st0=$(( $(bm0 'bridge_source_stalls_total{reason="image_lost"}') + $(bm0 'bridge_source_stalls_total{reason="archive_gone"}') ))
    local su0; su0=$(bm0 bridge_recording_successions_total)
    jctl 30 KILL 0 >> "$D/ev/jctl.txt"
    sleep 10
    jctl 60 REVIVE 0 >> "$D/ev/jctl.txt"
    local tr; tr=$(now_s)
    jctl 30 RECORD 0 >> "$D/ev/jctl.txt"
    local f0; f0=$(bm_sum bridge_forwarded_records_total)
    run_story --from 6 --timeout 120000 > "$D/ev/run1.txt" 2>&1 &
    local runner=$! first=""
    for _ in $(seq 1 600); do [ "$(bm_sum bridge_forwarded_records_total)" -gt "$f0" ] && { first=$(elapsed "$tr" "$(now_s)"); break; }; sleep 0.1; done
    wait "$runner" || FAILS+=("run after REVIVE: $(tail -1 "$D/ev/run1.txt")")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    local st1; st1=$(( $(bm0 'bridge_source_stalls_total{reason="image_lost"}') + $(bm0 'bridge_source_stalls_total{reason="archive_gone"}') ))
    [ "$st1" -gt "$st0" ] || FAILS+=("no image_lost / archive_gone stall")
    [ -n "$first" ] && fle "$first" 30 || FAILS+=("first new frame ${first:-none} s after REVIVE")
    # the successor is found by the re-list after the fault (KILL closed the archive under the follower), so it shows as
    # a second chain recording and a resume, not as a live succession (deviation)
    [ "$(bm0 bridge_recording_successions_total)" -gt "$su0" ] || [ "$(bm0 'bridge_chain_recordings{source="0"}')" -ge 2 ] ||
        FAILS+=("the new recording was not followed")
    NOTES+=("successions $(bm0 bridge_recording_successions_total), chain recordings $(bm0 'bridge_chain_recordings{source="0"}')")
    [ "$(bm_sum bridge_feed_halts_sent_total)" = "0" ] || FAILS+=("FeedHalt sent")
    [ "$(bm0 bridge_client_sessions_opened_total)" = "1" ] || FAILS+=("sessions $(bm0 bridge_client_sessions_opened_total)")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    NOTES+=("first new frame $first s after REVIVE")
    finish rk3 "$(verdict)"
}

drill_rk4() {
    # the 07-25 shape: the bridge's driver dir (the host aeronmd's, external mode) runs out of room for the replay image
    # WHILE the bridge streams; the journal is recorded with the production 64m term (a 192 MB replay image). The
    # storage precheck refuses each re-open without a request, the memo keeps its proofs (no rescan from the chain
    # start per retry), the process stays alive and not ready; when the room comes back the bridge resumes by itself.
    if ! sudo -n true 2>/dev/null; then echo "rk4 NOT RUN no sudo" >> "$RESULTS"; return; fi
    setup rk4 1 1 $((SEED_BASE + 40)) "$ROUNDS" || { FAILS+=("setup"); finish rk4 FAIL; return; }
    mkdir -p "$SHM/tmpfs"; sudo mount -t tmpfs -o size=512m tmpfs "$SHM/tmpfs" && sudo chmod 1777 "$SHM/tmpfs"
    jctl 60 RECORD 0 term=64m >> "$D/ev/jctl.txt"
    BRDRV_OVERRIDE="$SHM/tmpfs/aeron" start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 3 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..3")
    local q; q=$(bm0 bridge_report_queries_total)
    for _ in $(seq 1 200); do [ "$(bm0 bridge_report_queries_total)" -gt $((q + 1)) ] && break; sleep 0.1; done # proofs
    local cold0; cold0=$(bm0 'bridge_memo_resumes_total{kind="cold"}')
    local proof0; proof0=$(bm0 'bridge_memo_resumes_total{kind="proof"}')
    # precondition (FIX-9): C3 reached and the source attached at least once on this driver dir
    if [ "$(bm0 bridge_registered)" != "1" ] || [ "$(bm0 'bridge_source_attached{source="0"}')" != "1" ]; then
        NOTES+=("precondition not met"); finish rk4 INCONCLUSIVE; return
    fi
    local free; free=$(df -m --output=avail "$SHM/tmpfs" | tail -1 | tr -d ' ')
    fallocate -l $(( (free - 100) * 1024 * 1024 )) "$SHM/tmpfs/filler"
    NOTES+=("tmpfs 512 MB, $free MB free before the filler, $(df -m --output=avail "$SHM/tmpfs" | tail -1 | tr -d ' ') MB after")
    # a writer reconnect onto a new 64m-term recording (KILL, REVIVE, RECORD): the follower must open a replay of it
    jctl 30 KILL 0 >> "$D/ev/jctl.txt"; sleep 2; jctl 60 REVIVE 0 >> "$D/ev/jctl.txt"
    jctl 60 RECORD 0 term=64m >> "$D/ev/jctl.txt"
    run_story --from 4 --timeout 300000 > "$D/ev/run1.txt" 2>&1 &
    local runner=$!
    sleep 30
    scrape after30
    local storage; storage=$(bm0 'bridge_replay_open_failures_total{reason="storage"}')
    [ "$storage" -ge 3 ] || FAILS+=("storage failures $storage < 3")
    [ "$(bm0 'bridge_replay_open_failures_total{reason="archive_error"}')" = "0" ] || FAILS+=("archive_error failures")
    local cold; cold=$(( $(bm0 'bridge_memo_resumes_total{kind="cold"}') - cold0 ))
    [ "$cold" -le 1 ] || FAILS+=("cold resumes +$cold during the refusals (a rescan from the chain start per retry)")
    alive bridge || FAILS+=("bridge died")
    curl -s --max-time 2 "http://127.0.0.1:$BPORT/ready" | grep -q no_source_attached || FAILS+=("ready does not name no_source_attached")
    [ "$(http_code health)" = "200" ] && health | grep -q '"halted":false' || FAILS+=("health")
    NOTES+=("storage failures $storage in 30 s, cold +$cold, proof resumes +$(( $(bm0 'bridge_memo_resumes_total{kind="proof"}') - proof0 ))")
    # the room comes back: the next retry opens the replay; the story goes on
    rm -f "$SHM/tmpfs/filler"
    wait "$runner" || FAILS+=("rounds 4.. after the room came back: $(tail -1 "$D/ev/run1.txt")")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("oracle")
    [ "$(bm0 bridge_client_sessions_opened_total)" = "1" ] || FAILS+=("sessions $(bm0 bridge_client_sessions_opened_total)")
    common_checks
    finish rk4 "$(verdict)"
}

drill_rk5() {
    setup rk5 1 1 $((SEED_BASE + 50)) "$ROUNDS" || { FAILS+=("setup"); finish rk5 FAIL; return; }
    jctl 30 EMPTY 0 >> "$D/ev/jctl.txt"
    jctl 30 RECORD 0 >> "$D/ev/jctl.txt"
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --timeout 120000 > "$D/ev/run.txt" 2>&1 || FAILS+=("run")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    [ "$(bm0 bridge_empty_recordings_skipped_total)" = "1" ] || FAILS+=("empty skipped $(bm0 bridge_empty_recordings_skipped_total)")
    [ "$(bm_sum bridge_replay_open_failures_total)" = "0" ] || FAILS+=("replay open failures")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    finish rk5 "$(verdict)"
}

drill_rk6() { # binary drill
    local binary=$1 drill=$2
    F62=0; R62=0; KAF=0; LOSSES=0; SESS=0; ORC=1
    setup "$drill" 1 1 $((SEED_BASE + 60)) "$ROUNDS" --burst 129 || { FAILS+=("setup"); return 1; }
    local first last; first=$(sed -n 's/^special_first_seq=//p' "$STORY/story.txt"); last=$(sed -n 's/^special_last_seq=//p' "$STORY/story.txt")
    start_bridge bridge1 external || FAILS+=("bridge #1")
    wait_registered 30 || FAILS+=("registered #1")
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    kill -TERM "${PID[bridge]}"; wait_exit bridge 10
    [ "$WAIT_RC" = "0" ] || FAILS+=("SIGTERM exit $WAIT_RC (expected 0)")
    t1core round "$STORY" 1 > "$D/ev/round1.txt" 2>&1 || FAILS+=("round 1 core block")
    jctl 120 WRITE all "$first" "$last" > "$D/ev/burst-write.txt"
    grep -q "^OK wrote" "$D/ev/burst-write.txt" || FAILS+=("burst write: $(cat "$D/ev/burst-write.txt")")
    start_bridge bridge2 external "$binary" || FAILS+=("bridge #2")
    t1core barrier "$last" 60000 > "$D/ev/barrier-burst.txt" 2>&1 || FAILS+=("burst barrier")
    sleep 6
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    F62=$(bm0 'bridge_forwarded_total{kind="62"}'); R62=$(bm0 'bridge_forwarded_records_total{kind="62"}')
    KAF=$(bm0 bridge_client_keepalive_failures_total); LOSSES=$(bm_sum bridge_session_losses_total)
    SESS=$(bm0 bridge_client_sessions_opened_total)
    NOTES+=("62 frames $F62 records $R62 keepalive failures $KAF session losses $LOSSES sessions $SESS")
    oracle_ok final; ORC=$?
}
drill_rk6_clean() {
    drill_rk6 "$BRIDGE" rk6 || { finish rk6 FAIL; return; }
    [ "$F62" -ge 2 ] && [ "$R62" -ge 128 ] || FAILS+=("cap-64 62 frames: $F62 / $R62")
    [ "$SESS" = "1" ] && [ "$LOSSES" = "0" ] && [ "$KAF" = "0" ] || FAILS+=("the session did not survive")
    [ "$(bm0 'bridge_settle_rejected_total{reason="6"}')" = "0" ] || FAILS+=("FEED_GAP")
    [ $ORC -eq 0 ] || FAILS+=("oracle")
    common_checks
    finish rk6 "$(verdict)"
}
# rk6-nc (ct_bridge_nc_mtu1408, the ingress without mtu=8192) is RETIRED (review NOTE-2, contract 19.5): on this host
# Aeron fragments the cap-64 62 at 1408 and the consensus module reassembles it, so the session is never lost and the RED
# cannot fail. The ingress literal is pinned by the unit RED CONFIG_MTU_DEFAULT (BridgeConfigTest), the 2026-09-02 B
# replay-side MTU refusal by S9a and BridgeAgentTest.mtuRefusedEverywhereExits78; R-K6's pass lines stay.
drill_rk6_nc() {
    echo "rk6-nc is retired (review NOTE-2, contract 19.5)" >&2; finish rk6-nc FAIL
}

drill_rk8() {
    setup rk8 2 1 $((SEED_BASE + 80)) "$ROUNDS" || { FAILS+=("setup"); finish rk8 FAIL; return; }
    local s1; s1=$(( $(barrier_of 2) + 2 ))
    jctl 30 PAD 0 3000 >> "$D/ev/jctl.txt"; jctl 30 PAD 1 7000 >> "$D/ev/jctl.txt"
    jctl 30 HOLE 0 "$s1" >> "$D/ev/jctl.txt"
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 5 --timeout 120000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..5 (fail over to B)")
    jctl 60 FILL 0 "$s1" >> "$D/ev/jctl.txt"
    jctl 30 KILL 1 >> "$D/ev/jctl.txt"
    run_story --from 6 --timeout 120000 > "$D/ev/run1.txt" 2>&1 || FAILS+=("rounds 6.. (back to A)")
    jctl 60 REVIVE 1 >> "$D/ev/jctl.txt"
    quiescent "$N" 30000 || FAILS+=("quiescence")
    [ "$(bm0 bridge_source_failovers_total)" -ge 2 ] || FAILS+=("failovers $(bm0 bridge_source_failovers_total)")
    [ "$(bm0 'bridge_memo_invalidations_total{reason="position_rejected"}')" = "0" ] || FAILS+=("position_rejected")
    [ "$(bm_sum bridge_feed_halts_sent_total)" = "0" ] || FAILS+=("FeedHalt")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    NOTES+=("hole at $s1, failovers $(bm0 bridge_source_failovers_total)")
    finish rk8 "$(verdict)"
}

drill_rk9() { # binary drill
    local binary=$1 drill=$2
    setup "$drill" 2 1 $((SEED_BASE + 90)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    S9=$(mid_round 4)
    jctl 30 HOLE 0 "$S9" >> "$D/ev/jctl.txt"
    start_bridge bridge external "$binary" || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --timeout 120000 > "$D/ev/run.txt" 2>&1; RUNRC=$?
}
drill_rk9_clean() {
    drill_rk9 "$BRIDGE" rk9
    [ $RUNRC -eq 0 ] || FAILS+=("run")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    [ "$(bm0 bridge_source_failovers_total)" -ge 1 ] || FAILS+=("no failover")
    [ "$(bm_sum bridge_feed_halts_sent_total)" = "0" ] || FAILS+=("FeedHalt")
    t1core report > "$D/ev/report.json" 2>&1; grep -q '"feedHalted":false' "$D/ev/report.json" || FAILS+=("core halted")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    NOTES+=("hole on source 0 at $S9")
    finish rk9 "$(verdict)"
}
drill_rk9_nc() {
    drill_rk9 "$(bin ct_bridge_nc_norotation)" rk9-nc
    sleep 5
    local halts; halts=$(bm0 'bridge_feed_halts_sent_total{reason="1"}')
    t1core gate > "$D/ev/gate.json" 2>&1
    ORACLE_LATCH="L true 1" oracle_ok upto --upto $((S9 - 1)); local upto=$?
    oracle_ok final; local fin=$?
    NOTES+=("FeedHalts GAP $halts, gate $(tr -d '\n' < "$D/ev/gate.json"), oracle --upto rc $upto, final rc $fin")
    if [ "$halts" = "1" ] && grep -q SUSPENDED "$D/ev/gate.json" && [ $upto -eq 0 ] && [ $fin -ne 0 ]; then
        finish rk9-nc PASS; else finish rk9-nc INCONCLUSIVE; fi
}

drill_rk10() {
    setup rk10 2 1 $((SEED_BASE + 100)) "$ROUNDS" || { FAILS+=("setup"); finish rk10 FAIL; return; }
    local s; s=$(mid_round 4)
    jctl 30 HOLE all "$s" >> "$D/ev/jctl.txt"
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 4 --timeout 30000 > "$D/ev/run-halt.txt" 2>&1 # the barrier fails at the hole: expected
    for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
    sleep 3
    scrape halted
    [ "$(bm0 bridge_halted)" = "1" ] && [ "$(bm0 bridge_halt_reason)" = "1" ] || FAILS+=("halted $(bm0 bridge_halted) reason $(bm0 bridge_halt_reason)")
    [ "$(bm0 'bridge_feed_halts_sent_total{reason="1"}')" = "1" ] || FAILS+=("FeedHalts GAP $(bm0 'bridge_feed_halts_sent_total{reason="1"}')")
    local q0; q0=$(bm0 bridge_report_queries_total); sleep 15; local q1; q1=$(bm0 bridge_report_queries_total)
    [ $((q1 - q0)) -le 4 ] || FAILS+=("queries while halted: $((q1 - q0)) in 15 s")
    t1core report > "$D/ev/report-halted.json" 2>&1
    grep -q '"feedHalted":true' "$D/ev/report-halted.json" && grep -q '"haltReason":1' "$D/ev/report-halted.json" || FAILS+=("core report $(cat "$D/ev/report-halted.json")")
    t1core gate > "$D/ev/gate-halted.json" 2>&1
    grep -q SUSPENDED "$D/ev/gate-halted.json" && grep -q BRIDGE_HALT "$D/ev/gate-halted.json" || FAILS+=("gate $(cat "$D/ev/gate-halted.json")")
    ORACLE_LATCH="L true 1" oracle_ok upto-halted --upto $((s - 1)) || FAILS+=("oracle --upto $((s - 1)) L true 1")
    # deposits and withdrawals continue (TREASURY); deposit then withdraw the same amount per user keeps the model's
    # balances, so the final oracle needs no --extra (deviation: no flog writer for --extra)
    for u in 1 2; do
        t1core deposit "$u" 1 777 >> "$D/ev/treasury.txt" 2>&1 || FAILS+=("deposit $u")
        t1core withdraw "$u" 1 777 >> "$D/ev/treasury.txt" 2>&1 || FAILS+=("withdraw $u")
    done
    NOTES+=("treasury: $(tr '\n' ' ' < "$D/ev/treasury.txt")")
    local tf; tf=$(now_s)
    jctl 60 FILL all "$s" >> "$D/ev/jctl.txt"
    rejournal 0 "$(barrier_of 4)"
    local resumed=""
    for _ in $(seq 1 1500); do [ "$(bm0 bridge_halted)" = "0" ] && { resumed=$(elapsed "$tf" "$(now_s)"); break; }; sleep 0.1; done
    [ -n "$resumed" ] && fle "$resumed" 75 || FAILS+=("not resumed within 75 s")
    [ "$(bm0 'bridge_halt_recovered_total{reason="GAP"}')" = "1" ] || FAILS+=("recovered GAP $(bm0 'bridge_halt_recovered_total{reason="GAP"}')")
    t1core barrier "$(barrier_of 4)" 60000 > /dev/null 2>&1
    t1core report > "$D/ev/report-resumed.json" 2>&1; grep -q '"feedHalted":false' "$D/ev/report-resumed.json" || FAILS+=("core still halted")
    # S10' 11 (F-13 / RC-6): the S9c line inverts on the 4R core: the gate opens by itself at the GAP reach-clear, `open`
    # answers DUPLICATE (IT_RK10_PRE4R=1 keeps the S9c expectation for a pre-4R core)
    sleep 1
    t1core gate > "$D/ev/gate-resumed.json" 2>&1
    t1core open > "$D/ev/open.txt" 2>&1
    if [ "${IT_RK10_PRE4R:-0}" = 1 ]; then
        grep -q SUSPENDED "$D/ev/gate-resumed.json" || FAILS+=("gate opened by itself")
        grep -q 'result=ACCEPTED' "$D/ev/open.txt" || FAILS+=("open: $(cat "$D/ev/open.txt")")
    else
        grep -q '"gateState":"OPEN"' "$D/ev/gate-resumed.json" || FAILS+=("gate not OPEN by itself after the reach-clear: $(cat "$D/ev/gate-resumed.json")")
        grep -q 'result=DUPLICATE' "$D/ev/open.txt" || FAILS+=("open after the auto-reopen: $(cat "$D/ev/open.txt") (want DUPLICATE)")
    fi
    run_story --from 5 --timeout 120000 > "$D/ev/run-end.txt" 2>&1 || FAILS+=("rounds 5..")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("final oracle")
    common_checks
    NOTES+=("hole everywhere at $s, resumed $resumed s after FILL")
    finish rk10 "$(verdict)"
}

drill_rk11() { # variant (all|one)
    local variant=$1 drill=rk11; [ "$variant" = one ] && drill=rk11b
    setup "$drill" 2 1 $((SEED_BASE + 110)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 5 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..5")
    local nold; nold=$(barrier_of 5)
    if [ "$variant" = all ]; then
        jctl 120 REGENESIS all 777001 >> "$D/ev/jctl.txt"
        for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
        sleep 3
        [ "$(bm0 bridge_halt_reason)" = "2" ] || FAILS+=("halt reason $(bm0 bridge_halt_reason)")
        t1core report > "$D/ev/report.json" 2>&1
        grep -q '"haltReason":2' "$D/ev/report.json" || FAILS+=("core report $(cat "$D/ev/report.json")")
        ORACLE_LATCH="L true 2" oracle_ok upto --upto "$nold" || FAILS+=("oracle --upto $nold L true 2")
        local c0; c0=$(bm0 bridge_core_cursor); sleep "${IT_RK11_FLAT_S:-120}"
        [ "$(bm0 bridge_core_cursor)" = "$c0" ] || FAILS+=("cursor moved")
        [ "$(bm_sum bridge_halt_recovered_total)" = "0" ] || FAILS+=("recovered")
        t1core latch-clear 2 0 > "$D/ev/latch-clear.txt" 2>&1
        grep -q 'REJECTED' "$D/ev/latch-clear.txt" || FAILS+=("latch-clear: $(cat "$D/ev/latch-clear.txt")")
        [ "$(bm0 'bridge_settle_rejected_total{reason="7"}')" = "0" ] || FAILS+=("a foreign record was offered")
    else
        jctl 30 EPOCH 0 $((nold + 1)) 777002 >> "$D/ev/jctl.txt"
        run_story --from 6 --timeout 120000 > "$D/ev/run1.txt" 2>&1 || FAILS+=("rounds 6..")
        quiescent "$N" 30000 || FAILS+=("quiescence")
        [ "$(bm0 bridge_source_failovers_total)" -ge 1 ] || FAILS+=("no failover")
        [ "$(bm_sum bridge_feed_halts_sent_total)" = "0" ] || FAILS+=("FeedHalt")
        oracle_ok final || FAILS+=("oracle")
    fi
    common_checks
    finish "$drill" "$(verdict)"
}

trade_seq_after() { # seq -> the first Trade record at or after seq (the story's journal; a python reader of FJR1)
    python3 -I - "$STORY/journal.fjr" "$1" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
want = int(sys.argv[2])
# FJR1 (little endian): magic, u32 count, entries u8 type (1 RECORD: u16 length + bytes, 2 FLUSH)
(count,) = struct.unpack_from('<I', data, 4)
at = 8
for _ in range(count):
    kind = data[at]; at += 1
    if kind == 2:
        continue
    (n,) = struct.unpack_from('<H', data, at); at += 2
    rec = data[at:at + n]; at += n
    template = struct.unpack_from('<H', rec, 2)[0]
    seq = struct.unpack_from('<q', rec, 8)[0]
    if template == 1 and seq >= want:
        print(seq); sys.exit(0)
sys.exit(1)
PY
}

drill_rk12() { # variant: all one poison poison-both
    local variant=$1 drill
    case $variant in all) drill=rk12 ;; one) drill=rk12b ;; poison) drill=rk12c ;; poison-both) drill=rk12c-both ;; esac
    setup "$drill" 2 1 $((SEED_BASE + 120)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    local s; s=$(trade_seq_after "$(mid_round 4)") || { FAILS+=("no trade after round 3"); finish "$drill" FAIL; return; }
    local r=4; while [ "$(barrier_of $r)" -lt "$s" ]; do r=$((r + 1)); done
    case $variant in
        all) jctl 30 CORRUPT all "$s" schema3 >> "$D/ev/jctl.txt" ;;
        one) jctl 30 CORRUPT 0 "$s" schema3 >> "$D/ev/jctl.txt" ;;
        poison) jctl 30 CORRUPT 0 "$s" poison >> "$D/ev/jctl.txt" ;;
        poison-both) jctl 30 CORRUPT all "$s" poison >> "$D/ev/jctl.txt" ;;
    esac
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    local dr0; dr0=$(ae_metric assets_ingress_decode_rejects_total)
    if [ "$variant" = one ] || [ "$variant" = poison ]; then
        if [ "$variant" = poison ]; then
            # the core's SOURCE_SCHEMA latch suspends GLOBAL (BRIDGE_HALT) until ADMIN OPEN: round r runs to its barrier
            # (the storm guard recovers inside it), the gate is opened, then the rest
            run_story --to "$r" --timeout 180000 > "$D/ev/run.txt" 2>&1 || FAILS+=("rounds 0..$r")
            t1core open > "$D/ev/open.txt" 2>&1
            NOTES+=("open after the reach-clear: $(grep -o 'result=[A-Z]*' "$D/ev/open.txt")")
            run_story --from $((r + 1)) --timeout 180000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds $((r + 1))..")
        else
            run_story --timeout 180000 > "$D/ev/run.txt" 2>&1 || FAILS+=("run")
        fi
        quiescent "$N" 30000 || FAILS+=("quiescence")
        [ "$(bm_sum bridge_feed_halts_sent_total)" = "0" ] || FAILS+=("FeedHalt")
        if [ "$variant" = poison ]; then
            [ "$(bm0 bridge_storm_rotations_total)" = "1" ] || FAILS+=("storm rotations $(bm0 bridge_storm_rotations_total)")
            [ "$(bm0 bridge_core_refused_total)" = "0" ] || FAILS+=("core refused")
        else
            [ "$(bm0 bridge_source_failovers_total)" -ge 1 ] || FAILS+=("no failover")
        fi
        oracle_ok final || FAILS+=("oracle")
        NOTES+=("record $s; decode rejects at the core $(( $(ae_metric assets_ingress_decode_rejects_total) - ${dr0:-0} ))")
        common_checks
        finish "$drill" "$(verdict)"
        return
    fi
    run_story --to $((r - 1)) --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds before $r")
    run_story --from "$r" --to "$r" --timeout 20000 > "$D/ev/run-halt.txt" 2>&1 # stops at s: expected
    for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
    sleep 5
    scrape halted
    t1core report > "$D/ev/report-halted.json" 2>&1
    if [ "$variant" = all ]; then
        [ "$(bm0 bridge_halt_reason)" = "3" ] || FAILS+=("halt reason $(bm0 bridge_halt_reason)")
        [ "$(bm0 'bridge_source_bad_total{reason="schema"}')" = "2" ] || FAILS+=("bad schema $(bm0 'bridge_source_bad_total{reason="schema"}')")
        grep -q '"haltReason":3' "$D/ev/report-halted.json" || FAILS+=("core report $(cat "$D/ev/report-halted.json")")
        ORACLE_LATCH="L true 3" oracle_ok upto --upto $((s - 1)) || FAILS+=("oracle --upto $((s - 1)) L true 3")
        t1core latch-clear 3 0 > "$D/ev/latch-clear.txt" 2>&1
        grep -q 'ACCEPTED' "$D/ev/latch-clear.txt" || FAILS+=("latch-clear: $(cat "$D/ev/latch-clear.txt")")
        sleep 3
        [ "$(bm0 bridge_halted)" = "1" ] || FAILS+=("the bridge left the halt without a valid s")
    else
        [ "$(bm0 bridge_halt_reason)" = "4" ] || FAILS+=("halt reason $(bm0 bridge_halt_reason) (want 4 CORE_REFUSED)")
        local offers; offers=$(( $(ae_metric assets_ingress_decode_rejects_total) - ${dr0:-0} ))
        [ "$offers" -le 4 ] || FAILS+=("the refused seq offered $offers times (> stormFences 2 x 2 sources)")
        NOTES+=("refused seq $s offered $offers times")
    fi
    # recovery: valid copies in new recordings, a bridge restart
    jctl 30 CORRUPT all "$s" none >> "$D/ev/jctl.txt"
    for k in $(seq 0 $((K - 1))); do rejournal "$k" "$(barrier_of "$r")"; done
    kill -TERM "${PID[bridge]}"; wait_exit bridge 10
    [ "$WAIT_RC" = "0" ] || FAILS+=("SIGTERM while halted: exit $WAIT_RC (expected 0)")
    start_bridge bridge2 external || FAILS+=("restart")
    t1core barrier "$(barrier_of "$r")" 60000 > "$D/ev/barrier-s.txt" 2>&1 || FAILS+=("round $r not forwarded after the restart")
    t1core open > "$D/ev/open.txt" 2>&1; grep -q 'result=ACCEPTED' "$D/ev/open.txt" || NOTES+=("open: $(tr '\n' ' ' < "$D/ev/open.txt")")
    run_story --from $((r + 1)) --timeout 120000 > "$D/ev/run-end.txt" 2>&1 || FAILS+=("rounds $((r + 1))..")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("final oracle")
    common_checks
    NOTES+=("record $s (round $r)")
    finish "$drill" "$(verdict)"
}

drill_rk13c() {
    setup rk13c 1 1 $((SEED_BASE + 130)) 40 || { FAILS+=("setup"); finish rk13c FAIL; return; }
    start_bridge bridge external || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 10 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..10")
    run_story --from 11 --timeout 120000 > "$D/ev/run1.txt" 2>&1 &
    local runner=$!
    sleep 1
    kill -9 "${PID[brdrv]}"; wait_exit brdrv 5
    local tk; tk=$(now_s)
    wait_exit bridge 15
    local took; took=$(elapsed "$tk" "$(now_s)")
    [ "$WAIT_RC" = "70" ] || FAILS+=("exit $WAIT_RC (want 70)")
    fle "$took" 7 || FAILS+=("exit after $took s")
    grep -q "FATAL aeron client error" "$D/ev/bridge.log" && grep -qi "MediaDriver keepalive\|driver timeout" "$D/ev/bridge.log" || FAILS+=("no driver timeout line")
    NOTES+=("kill -9 aeronmd: exit $WAIT_RC after $took s")
    start_bridge bridge2 external || FAILS+=("restart on a fresh aeronmd")
    wait "$runner" || FAILS+=("the load run did not complete across the restart: $(tail -1 "$D/ev/run1.txt")")
    quiescent "$N" 60000 || FAILS+=("quiescence after the restart")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    finish rk13c "$(verdict)"
}

drill_rk13a() {
    setup rk13a 1 1 $((SEED_BASE + 131)) 40 || { FAILS+=("setup"); finish rk13a FAIL; return; }
    start_bridge bridge embedded || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 10 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..10")
    local pre; pre=$(bm0 bridge_core_cursor)
    kill -STOP "${PID[bridge]}"; sleep 15; kill -CONT "${PID[bridge]}"
    local tw; tw=$(now_s)
    run_story --from 11 --timeout 120000 > "$D/ev/run1.txt" 2>&1 &
    local runner=$! outcome=""
    for _ in $(seq 1 300); do
        if ! alive bridge; then wait_exit bridge 1; outcome="exit $WAIT_RC"; break; fi
        local c; c=$(bm0 bridge_core_cursor)
        if [ "$c" -gt "$pre" ]; then outcome="reconnected, cursor $pre -> $c, sessions $(bm0 bridge_client_sessions_opened_total)"; break; fi
        sleep 0.1
    done
    NOTES+=("after CONT: ${outcome:-alive with a flat cursor for 30 s}")
    case "$outcome" in
        "exit 70") start_bridge bridge2 embedded || FAILS+=("restart") ;;
        reconnected*) ;;
        *) FAILS+=("FORBIDDEN: alive with the cursor flat for 30 s after the write ($outcome)") ;;
    esac
    wait "$runner" || FAILS+=("the load run did not complete across the stop: $(tail -1 "$D/ev/run1.txt")")
    quiescent "$N" 60000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    finish rk13a "$(verdict)"
}

drill_rk13_tc() { # drill mode binary expected-line
    local drill=$1 mode=$2 binary=$3 want=$4
    setup "$drill" 1 1 $((SEED_BASE + 132)) 30 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge "$mode" "$binary" || FAILS+=("start")
    wait_registered 30 || FAILS+=("registered")
    run_story --to 8 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..8")
    local beat0; beat0=$(bm0 bridge_duty_max_cycle_us)
    kill -USR2 "${PID[bridge]}"
    local tk; tk=$(now_s)
    wait_exit bridge 15
    local took; took=$(elapsed "$tk" "$(now_s)")
    [ "$WAIT_RC" = "70" ] || FAILS+=("exit $WAIT_RC (want 70)")
    fle "$took" 7 || FAILS+=("exit after $took s")
    grep -q "$want" "$D/ev/bridge.log" || FAILS+=("no '$want' line")
    grep -q "termination hook" "$D/ev/bridge.log" && FAILS+=("the termination hook line (FIX-4: F1 is the path)")
    grep -q "duty heartbeat did not advance" "$D/ev/bridge.log" && FAILS+=("the duty watchdog fired (the duty heartbeat stopped)")
    NOTES+=("SIGUSR2: exit $WAIT_RC after $took s; $(grep -m1 -o "$want.\{0,80\}" "$D/ev/bridge.log")")
    start_bridge bridge2 "$mode" || FAILS+=("restart (normal build)")
    run_story --from 9 --timeout 120000 > "$D/ev/run1.txt" 2>&1 || FAILS+=("rounds 9..")
    quiescent "$N" 60000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("oracle")
    common_checks
    finish "$drill" "$(verdict)"
}

# ================================================================== S10' T1-N2 (contract sections 2, 3, 6)

# A core counter of 4R.1 rule 7 (RC-4). On a core without it (stage A, IT_STAGE_A=1) the line is a NOTE and the row's
# substitute decides; on the <RC> core an absent counter is a FAIL (it is part of the pass line).
core_counter() { # name -> value, or "absent"
    local v; v=$(ae_metric "$1"); echo "${v:-absent}"
}
need_counter() { # name -> value; an absent counter FAILs unless IT_STAGE_A=1
    local v; v=$(core_counter "$1")
    if [ "$v" = absent ]; then
        if [ "${IT_STAGE_A:-0}" = 1 ]; then NOTES+=("core counter $1 absent (stage A: the substitute decides)"); else FAILS+=("core counter $1 absent"); fi
    fi
    echo "$v"
}
core_cursor() { local v; v=$(ae_metric assets_feed_cursor); echo "${v:-0}"; }
identities() { # slots -> the bridge_info identity labels, space separated
    local s; for s in "$@"; do
        curl -s --max-time 2 "http://127.0.0.1:$(port_of "$s")/metrics" 2>/dev/null | grep -o '^bridge_info{identity="[0-9]*"' | head -1 | grep -o '[0-9]*'
    done | tr '\n' ' '
}
forwarded_sum() { # slots -> the sum of bridge_forwarded_records_total over them
    local s t=0; for s in "$@"; do t=$((t + $(bm_sum "$s" bridge_forwarded_records_total))); done; echo "$t"
}
round_parts() { # r -> "a_r b_r"
    local a; if [ "$1" -eq 0 ]; then a=1; else a=$(( $(barrier_of $(($1 - 1))) + 1 )); fi
    echo "$a $(barrier_of "$1")"
}
# The source-lag device (B-1, the primary lag device): round r composed by the runner, source <lagsrc> written only to
# b_r - <entries>; the other sources whole. Prints "a_r b_r lagto".
lag_round() { # r lagsrc entries
    local a b; read -r a b < <(round_parts "$1")
    local lagto=$((b - $3)) k
    t1core round "$STORY" "$1" > "$D/ev/round$1.txt" 2>&1 || FAILS+=("round $1 core block")
    for k in $(seq 0 $((K - 1))); do
        if [ "$k" = "$2" ]; then jctl 300 WRITE "$k" "$a" "$lagto" >> "$D/ev/jctl.txt"; else jctl 300 WRITE "$k" "$a" "$b" >> "$D/ev/jctl.txt"; fi
    done
    tl "lag_round $1: source $2 to $lagto, the others to $b"
    echo "$a $b $lagto"
}
# The short-pause device (B-1, second device): only on a lane started with DRIVER_TIMEOUT_MS=30000 (under the 10 s
# driver liveness); a pause past the driver timeout ends the process on CONT (S10' section 1), so it is refused here.
pause_lane() { # slot seconds
    grep -q "start $1 pid ${PID[$1]} .*BRIDGE_DRIVER_TIMEOUT_MS=30000" "$D/ev/timeline.txt" ||
        { FAILS+=("pause_lane $1: the lane was not started with DRIVER_TIMEOUT_MS=30000"); return 1; }
    kill -STOP "${PID[$1]}"; tl "STOP $1"
    sleep "$2"
    kill -CONT "${PID[$1]}"; tl "CONT $1"
}
n2_cursor_n() { # every live slot at cursor N within 30 s (the quiescent barrier covers it; this names the slot)
    local s; for s in $(live_slots); do
        local ok=0; for _ in $(seq 1 300); do [ "$(bm0 "$s" bridge_core_cursor)" = "$N" ] && { ok=1; break; }; sleep 0.1; done
        [ $ok = 1 ] || FAILS+=("$s cursor $(bm0 "$s" bridge_core_cursor) != N $N")
    done
}

drill_rk14_smoke() { # mode [red]
    local mode=$1 red=${2:-} drill=rk14-smoke-$1; [ -n "$red" ] && drill=rk14-smoke-red
    setup "$drill" 2 1 $((SEED_BASE + 1400)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge0 "$mode" || FAILS+=("bridge0 start")
    if [ -z "$red" ] && [ "${IT_N2_ONLY0:-0}" != 1 ]; then start_bridge bridge1 "$mode" || FAILS+=("bridge1 start"); fi
    wait_registered bridge0 30 || FAILS+=("bridge0 not registered in 30 s")
    alive bridge1 && { wait_registered bridge1 30 || FAILS+=("bridge1 not registered in 30 s"); }
    local ids; ids=$(identities bridge0 bridge1)
    [[ " $ids " == *" 1 "* ]] && [[ " $ids " == *" 2 "* ]] || FAILS+=("identities $(echo "$ids" | wc -w): '$ids'")
    local sess; sess=$(need_counter 'assets_sessions{class="BRIDGE"}')
    [ "$sess" = absent ] || [ "$sess" = 2 ] || FAILS+=("BRIDGE sessions $sess != 2")
    run_story --timeout 120000 > "$D/ev/run.txt" 2>&1 || FAILS+=("run: $(tail -1 "$D/ev/run.txt")")
    quiescent "$N" 60000 || FAILS+=("quiescence at $N")
    n2_cursor_n
    oracle_ok final || FAILS+=("oracle: $(head -2 "$D/ev/oracle-final.txt" | tr '\n' ' ')")
    local fwd dup; fwd=$(forwarded_sum $(live_slots)); dup=$(need_counter assets_feed_duplicate_total)
    [ $((fwd - N)) -ge 1 ] || FAILS+=("duplicates 0 (forwarded sum $fwd - N $N)")
    [ "$dup" = absent ] || [ "$dup" -ge 1 ] || FAILS+=("duplicates 0 (assets_feed_duplicate_total $dup)")
    [ "$dup" = absent ] || [ "$dup" = $((fwd - N)) ] || NOTES+=("duplicate counter $dup vs forwarded sum - N $((fwd - N)) differ")
    NOTES+=("identities '$ids'; BRIDGE sessions $sess; N $N forwarded sum $fwd (- N = $((fwd - N))); duplicate counter $dup")
    [ "$(bm0 bridge_halted)$(bm0 bridge1 bridge_halted)" = "00" ] || FAILS+=("halted")
    scrape bridge0 end; alive bridge1 && scrape bridge1 end
    common_checks
    if [ -n "$red" ]; then
        # the runner RED: bridge1 never started; the duplicates line and the identities line must both FAIL
        local d=no i=no f
        for f in "${FAILS[@]}"; do case $f in "duplicates 0"*) d=yes ;; identities*) i=yes ;; esac; done
        NOTES+=("RED expects 'duplicates 0' ($d) and 'identities 1' ($i): ${FAILS[*]}")
        FAILS=()
        if [ $d = yes ] && [ $i = yes ]; then finish "$drill" PASS; else finish "$drill" INCONCLUSIVE; fi
        return
    fi
    finish "$drill" "$(verdict)"
}

# ---- section 6: the loss-producing R-K2 fault (NOTE-1): SIGSTOP the AE leader for 15 s under bridge load
# member_back <node> -> 0 when the old member is a follower again within 60 s; MEMBER_BACK names how
member_back() {
    local l=$1 t0; t0=$(now_s); MEMBER_BACK=""
    for _ in $(seq 1 600); do
        if ! alive "ae$l"; then
            wait_exit "ae$l" 1; local rc=$WAIT_RC
            start_ae "$l"; tl "ae$l exited $rc after CONT: restarted by the runner"
            MEMBER_BACK="exited $rc after $(elapsed "$t0" "$(now_s)") s, restarted by the runner"
            break
        fi
        if [ "$(metric "http://127.0.0.1:$((AE_BASE + 90 + l))/metrics" assets_cluster_role)" = "0" ]; then
            MEMBER_BACK="follower by itself after $(elapsed "$t0" "$(now_s)") s"
            return 0
        fi
        sleep 0.1
    done
    if [ -z "$MEMBER_BACK" ]; then
        kill -9 "${PID[ae$l]}"; wait_exit "ae$l" 5; start_ae "$l"; tl "ae$l wedged 60 s: kill -9 and restart (the 08-12 shape)"
        MEMBER_BACK="alive but not a follower for 60 s: kill -9 and restarted (the 08-12 shape)"
    fi
    local left; left=$(python3 -I -c "import sys; print(max(1, int(60 - float(sys.argv[1]))))" "$(elapsed "$t0" "$(now_s)")")
    for _ in $(seq 1 $((left * 10 + 300))); do
        [ "$(metric "http://127.0.0.1:$((AE_BASE + 90 + l))/metrics" assets_cluster_role)" = "0" ] && { MEMBER_BACK="$MEMBER_BACK; follower after $(elapsed "$t0" "$(now_s)") s"; return 0; }
        sleep 0.1
    done
    MEMBER_BACK="$MEMBER_BACK; NOT a follower after $(elapsed "$t0" "$(now_s)") s"
    return 1
}
# stop_leader_under_load <mode> <binary> <slots...>: the story to a third of round 1, SIGSTOP the leader 15 s, CONT,
# the member's return. Sets STOP_INFLIGHT (offers in ended terms over the slots), FENCED_IN (s after the new leader
# serves, the slowest slot), MEMBER_BACK.
stop_leader_under_load() {
    local mode=$1 binary=$2; shift 2
    local slots=("$@") s
    STOP_INFLIGHT=0; FENCED_IN=none; MEMBER_BACK=""
    ROUNDS_OF_STORY=$(sed -n 's/^rounds=//p' "$STORY/story.txt")
    for s in "${slots[@]}"; do
        if [ "$s" = bridge0 ]; then start_bridge bridge0 "$mode" "$binary" || FAILS+=("start $s"); else start_bridge "$s" "$mode" || FAILS+=("start $s"); fi
        wait_registered "$s" 30 || FAILS+=("$s registered")
    done
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    local end1; end1=$(barrier_of 1)
    run_story --from 1 --timeout 900000 > "$D/ev/run.txt" 2>&1 &
    local runner=$!
    for _ in $(seq 1 3000); do [ "$(core_cursor)" -ge $((end1 / 3)) ] && break; sleep 0.02; done
    declare -A before fences0
    for s in "${slots[@]}"; do
        before[$s]=$(bm0 "$s" bridge_client_offers_in_ended_terms_total)
        fences0[$s]=$(bm0 "$s" 'bridge_fences_total{trigger="new_leader"}')
        scrape "$s" before-stop
    done
    local l; l=$(leader_of)
    kill -STOP "${PID[ae$l]}"; tl "STOP ae$l (leader) at core $(core_cursor)"
    local tk newl="" tnew="" i; tk=$(now_s)
    for _ in $(seq 1 300); do
        # the frozen member's /metrics does not answer: ask only the others
        for i in $(seq 0 $((NAE - 1))); do
            [ "$i" = "$l" ] && continue
            [ "$(metric "http://127.0.0.1:$((AE_BASE + 90 + i))/metrics" assets_cluster_role)" = "2" ] && { newl=$i; break; }
        done
        [ -n "$newl" ] && { tnew=$(now_s); break; }
        sleep 0.1
    done
    tl "new leader ${newl:-none} after $([ -n "$tnew" ] && elapsed "$tk" "$tnew")"
    NOTES+=("leader ae$l stopped; new leader ae${newl:-?} serving after $([ -n "$tnew" ] && elapsed "$tk" "$tnew" || echo none) s")
    local worst=0 fenced
    for s in "${slots[@]}"; do
        fenced=""
        for _ in $(seq 1 150); do [ "$(bm0 "$s" 'bridge_fences_total{trigger="new_leader"}')" -gt "${fences0[$s]}" ] && { fenced=$(now_s); break; }; sleep 0.1; done
        if [ -z "$fenced" ] || [ -z "$tnew" ]; then FENCED_IN=none; worst=-1; NOTES+=("$s: no new_leader fence"); continue; fi
        local d; d=$(python3 -I -c "import sys; print('%.2f' % max(0.0, float(sys.argv[1]) - float(sys.argv[2])))" "$fenced" "$tnew")
        NOTES+=("$s new_leader fence $d s after the new leader served")
        [ "$worst" != -1 ] && fle "$worst" "$d" && worst=$d
    done
    [ "$worst" != -1 ] && FENCED_IN=$worst
    local rest; rest=$(python3 -I -c "import sys; print('%.1f' % max(0.0, 15 - float(sys.argv[1])))" "$(elapsed "$tk" "$(now_s)")")
    sleep "$rest"
    kill -CONT "${PID[ae$l]}"; tl "CONT ae$l"
    member_back "$l" || FAILS+=("ae$l not back as a follower within 60 s")
    NOTES+=("ae$l after CONT: $MEMBER_BACK")
    if ! wait "$runner"; then
        # the story driver (t1-core run) waits 30 s per reply; a command in flight to the frozen leader is lost with its
        # reply (S10' T1 finding, not a bridge rule): re-drive from the round it was sending (its applied commands answer
        # DUPLICATE at the core)
        local lastr; lastr=$(grep -o '^ROUND [0-9]*' "$D/ev/run.txt" | tail -1 | awk '{print $2}')
        NOTES+=("story driver: $(grep -m1 -o 'no reply to correlationId [0-9]* within [0-9]* ms' "$D/ev/run.txt"); re-driven from round $((lastr + 1))")
        [ "$((lastr + 1))" -lt "$ROUNDS_OF_STORY" ] && run_story --from $((lastr + 1)) --timeout 900000 > "$D/ev/run-redrive.txt" 2>&1 ||
            NOTES+=("re-drive: $(tail -1 "$D/ev/run-redrive.txt" 2>/dev/null)")
    fi
    for s in "${slots[@]}"; do STOP_INFLIGHT=$((STOP_INFLIGHT + $(bm0 "$s" bridge_client_offers_in_ended_terms_total) - ${before[$s]})); done
}

drill_rk2_stop() { # mode
    local mode=$1 drill=rk2-stop-$1 tries=0
    while :; do
        tries=$((tries + 1))
        setup "$drill" 1 3 $((SEED_BASE + 200 + tries)) 8 --long-round 200000 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
        stop_leader_under_load "$mode" "$BRIDGE" bridge0
        quiescent "$N" 180000 || FAILS+=("quiescence at $N")
        NOTES+=("try $tries: offers in ended terms +$STOP_INFLIGHT, fence ${FENCED_IN} s after the new leader served")
        [ "$FENCED_IN" != none ] && fle "$FENCED_IN" 5 || FAILS+=("no NEW_LEADER fence within 5 s of the new leader serving")
        [ "$(bm0 'bridge_settle_rejected_total{reason="6"}')" = "0" ] || FAILS+=("FEED_GAP rejects at the bridge")
        local gr; gr=$(need_counter assets_feed_gap_rejects_total); [ "$gr" = absent ] || [ "$gr" = 0 ] || FAILS+=("core gap rejects $gr")
        t1core report > "$D/ev/report.json" 2>&1
        grep -q '"feedHalted":false' "$D/ev/report.json" || FAILS+=("core halted: $(cat "$D/ev/report.json")")
        oracle_ok final || FAILS+=("oracle")
        NOTES+=("duplicates (recorded): forwarded $(bm_sum bridge_forwarded_records_total) - N $N; counter $(core_counter assets_feed_duplicate_total)")
        common_checks
        if [ "$STOP_INFLIGHT" -gt 0 ] || [ $tries -ge 5 ] || [ ${#FAILS[@]} -gt 0 ]; then break; fi
        finish "$drill-try$tries" "$(verdict)"
    done
    if [ ${#FAILS[@]} -eq 0 ] && [ "$STOP_INFLIGHT" -eq 0 ]; then finish "$drill" INCONCLUSIVE; else finish "$drill" "$(verdict)"; fi
}

drill_rk2_stop_nc() {
    # RED ct_bridge_nc_nofence on the same fault: with an in-flight loss the core answers FEED_GAP or latches GAP, or the
    # oracle FAILs / stalls. PASS (RED) only when one named line fails; 5 seeds without a loss: INCONCLUSIVE
    local nc; nc=$(bin ct_bridge_nc_nofence) tries=0 red=no
    while [ $tries -lt 5 ] && [ $red = no ]; do
        tries=$((tries + 1))
        setup rk2-stop-nc 1 3 $((SEED_BASE + 220 + tries)) 8 --long-round 200000 || { FAILS+=("setup"); finish rk2-stop-nc FAIL; return; }
        stop_leader_under_load external "$nc" bridge0
        sleep 10
        local gaps; gaps=$(bm0 'bridge_settle_rejected_total{reason="6"}')
        local cgr; cgr=$(core_counter assets_feed_gap_rejects_total)
        t1core report > "$D/ev/report.json" 2>&1
        oracle_ok final; local orc=$?
        NOTES+=("try $tries: in-flight +$STOP_INFLIGHT, FEED_GAP rejects bridge $gaps core $cgr, report $(tr -d '\n' < "$D/ev/report.json" | cut -c1-160), oracle rc $orc")
        local named=""
        [ "$gaps" != "0" ] && named="FEED_GAP rejects $gaps"
        [ "$cgr" != absent ] && [ "$cgr" != 0 ] && named="${named:+$named; }core gap rejects $cgr"
        grep -q '"haltReason":1' "$D/ev/report.json" && named="${named:+$named; }report haltReason 1"
        [ $orc -ne 0 ] && named="${named:+$named; }oracle FAIL"
        if [ -n "$named" ]; then red=yes; NOTES+=("named failure: $named"); fi
        [ $red = no ] && finish "rk2-stop-nc-try$tries" INCONCLUSIVE
    done
    [ $red = yes ] && finish rk2-stop-nc PASS || finish rk2-stop-nc INCONCLUSIVE
}

drill_rk14_stop() {
    # section 6's leader freeze with two bridges (full lists): both fence, both resync from C', cursor N, oracle PASS
    local drill=rk14-stop
    setup "$drill" 2 3 $((SEED_BASE + 1490)) 8 --long-round 200000 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    stop_leader_under_load external "$BRIDGE" bridge0 bridge1
    quiescent "$N" 180000 || FAILS+=("quiescence at $N")
    n2_cursor_n
    [ "$FENCED_IN" != none ] || FAILS+=("a bridge did not fence new_leader")
    local s; for s in bridge0 bridge1; do
        [ "$(bm0 "$s" 'bridge_settle_rejected_total{reason="6"}')" = "0" ] || FAILS+=("$s FEED_GAP rejects")
        NOTES+=("$s epochs $(bm0 "$s" bridge_epochs_total) (a resync from C' per fence)")
    done
    t1core report > "$D/ev/report.json" 2>&1
    grep -q '"feedHalted":false' "$D/ev/report.json" || FAILS+=("core halted")
    oracle_ok final || FAILS+=("oracle")
    local fwd; fwd=$(forwarded_sum bridge0 bridge1)
    NOTES+=("offers in ended terms +$STOP_INFLIGHT; duplicates (recorded): forwarded sum - N $((fwd - N)), counter $(core_counter assets_feed_duplicate_total)")
    common_checks
    finish "$drill" "$(verdict)"
}

# ================================================================== S10' section 3.2: the N = 2 rows
# W0 setup fixes (2026-11-01, dry-run transcripts in N/S10p/evidence/lane-a/dry-run/): the lane-hole rows put the holes in
# round 1 of a --long-round 2000 story (the plain round 3 is 28 records); the hole lane runs from the start and its
# source's hole round is written after the other lane's barrier (a lane started after the core passed the hole syncs from
# C' and never meets it); restart / replace run on the 10^6 round and the second kill waits until the replacement lane
# offered past the cursor of its start (its cold walk takes seconds; /health "state" reads STREAMING during the walk).
cursor_sampler_start() { ( while :; do echo "$(date +%s.%N) $(core_cursor)"; sleep 2; done ) > "$D/ev/cursor-2s.txt" 2>/dev/null & PID[cursor2s]=$!; }
cursor_sampler_stop() { kill "${PID[cursor2s]}" 2>/dev/null; wait "${PID[cursor2s]}" 2>/dev/null; unset "PID[cursor2s]"; }
flat_samples() { python3 -I - "$D/ev/cursor-2s.txt" "$1" <<'PY2'
import sys
v = [int(l.split()[1]) for l in open(sys.argv[1]) if l.strip()]
print(sum(1 for a, b in zip(v, v[1:]) if a == b and b < int(sys.argv[2])))
PY2
}
caught_up() { # slot timeout_s: the lane offered past the core cursor of its start -> elapsed s
    local t0 c0; t0=$(now_s); c0=$(core_cursor)
    for _ in $(seq 1 $(($2 * 10))); do [ "$(bm0 "$1" bridge_last_offered_seq)" -ge "$c0" ] && { elapsed "$t0" "$(now_s)"; return 0; }; sleep 0.1; done
    echo none; return 1
}
dup_lines() { # the N = 2 duplicates line: the counter and the forwarded-sum substitute agree; mismatch 0
    local fwd dup; fwd=$(forwarded_sum "$@"); dup=$(need_counter assets_feed_duplicate_total)
    NOTES+=("duplicates: counter $dup, forwarded sum - N $((fwd - N))")
    [ "$dup" = absent ] || [ "$dup" -ge "${MIN_DUP:-1}" ] || FAILS+=("duplicates $dup < ${MIN_DUP:-1}")
    [ $((fwd - N)) -ge "${MIN_DUP:-1}" ] || FAILS+=("forwarded sum - N $((fwd - N)) < ${MIN_DUP:-1}")
}

drill_rk14_lag() { # mode
    local mode=$1 drill=rk14-lag-$1
    setup "$drill" 2 1 $((SEED_BASE + 1410)) "$ROUNDS" --long-round 50000 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge0 "$mode" || FAILS+=("bridge0 start")
    start_bridge bridge1 "$mode" "$BRIDGE" SOURCES_OF=1 || FAILS+=("bridge1 start")
    wait_registered bridge0 30 || FAILS+=("bridge0 registered"); wait_registered bridge1 30 || FAILS+=("bridge1 registered")
    local faults0; faults0=$(ae_metric assets_order_faults_total)
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    ( while :; do echo "$(ae_metric assets_feed_lanes_behind)"; sleep 1; done ) > "$D/ev/lanes-behind.txt" 2>&1 & PID[behind]=$!
    local a1 b1 lagto; read -r a1 b1 lagto < <(lag_round 1 1 10000)
    t1core barrier "$b1" 120000 > /dev/null 2>&1 || FAILS+=("barrier b_1")
    for _ in $(seq 1 600); do [ "$(bm0 bridge0 bridge_last_offered_seq)" = "$b1" ] && [ "$(bm0 bridge1 bridge_last_offered_seq)" -ge "$lagto" ] && break; sleep 0.1; done
    sleep 32 # S2-2 side observation: bridge1's /ready after 30 s of lag
    curl -s --max-time 2 "http://127.0.0.1:$(port_of bridge1)/ready" > "$D/ev/ready-bridge1-lagged.txt"
    local lag=$(( $(core_cursor) - $(bm0 bridge1 bridge_last_offered_seq) ))
    NOTES+=("lag at the catch-up write $lag; bridge1 /ready after 32 s: $(tr -d '\n' < "$D/ev/ready-bridge1-lagged.txt" | cut -c1-80)")
    [ "$lag" -ge 10000 ] || FAILS+=("lag $lag < 10000")
    jctl 300 WRITE 1 $((lagto + 1)) "$b1" >> "$D/ev/jctl.txt"; tl "catch-up WRITE 1 $((lagto + 1)) $b1"
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    quiescent "$N" 60000 || FAILS+=("quiescence")
    kill "${PID[behind]}" 2>/dev/null; wait "${PID[behind]}" 2>/dev/null; unset "PID[behind]"
    n2_cursor_n
    oracle_ok final || FAILS+=("oracle: $(head -1 "$D/ev/oracle-final.txt")")
    MIN_DUP=10000 dup_lines bridge0 bridge1
    [ "$(core_counter assets_feed_gap_rejects_total)" = 0 ] || FAILS+=("FEED_GAP rejects $(core_counter assets_feed_gap_rejects_total)")
    [ "$(ae_metric assets_order_faults_total)" = "$faults0" ] || FAILS+=("order faults changed")
    [ "$(bm_sum bridge0 bridge_feed_halts_sent_total)$(bm_sum bridge1 bridge_feed_halts_sent_total)" = 00 ] || FAILS+=("a FeedHalt was sent")
    [ "$(bm0 bridge0 bridge_halted)$(bm0 bridge1 bridge_halted)" = 00 ] || FAILS+=("halted")
    NOTES+=("lanes_behind > 0 samples $(grep -c '^[1-9]' "$D/ev/lanes-behind.txt") (recorded, node-local); bridge1 lane_ahead $(bm0 bridge1 'bridge_fences_total{trigger="lane_ahead"}'); core log position $(ae_metric assets_log_position)")
    common_checks
    finish "$drill" "$(verdict)"
}

drill_rk14_restart() { # restart|replace
    local v=$1 drill=rk14-$1
    setup "$drill" 2 1 $((SEED_BASE + 1430)) "$ROUNDS" --long-round "${IT_RESTART_LONG:-1000000}" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge0 external || FAILS+=("bridge0 start"); start_bridge bridge1 external || FAILS+=("bridge1 start")
    wait_registered bridge0 30 || FAILS+=("bridge0 registered"); wait_registered bridge1 30 || FAILS+=("bridge1 registered")
    [ "$v" = replace ] && { gate_watch_start 1800 || true; APPLIED0=$(core_counter assets_feed_halt_applied_total | sed 's/absent/0/'); }
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    sleep 1; local gmark; gmark=$(mono_ms)
    local a1 b1; read -r a1 b1 < <(round_parts 1)
    cursor_sampler_start
    run_story --from 1 --to 1 --timeout 1800000 > "$D/ev/run1.txt" 2>&1 & local runner=$!
    local p1=30 p2=60; [ "$v" = replace ] && { p1=40; p2=70; }
    local k1=$((a1 + (b1 - a1) * p1 / 100)) k2=$((a1 + (b1 - a1) * p2 / 100))
    for _ in $(seq 1 6000); do [ "$(core_cursor)" -ge "$k1" ] && break; sleep 0.02; done
    if [ "$v" = restart ]; then
        tl "kill -9 bridge0 at core $(core_cursor)"; kill -9 "${PID[bridge0]}"; wait_exit bridge0 5
        sleep 3; start_bridge bridge0 external || FAILS+=("bridge0 restart")
        local t0; t0=$(now_s); local reg=""
        for _ in $(seq 1 300); do [ "$(bm0 bridge0 bridge_registered)" = 1 ] && health bridge0 | grep -q '"reportThisTerm":true' && { reg=$(elapsed "$t0" "$(now_s)"); break; }; sleep 0.1; done
        NOTES+=("bridge0 restarted after kill -9: registered and reported after ${reg:-none} s; duplicate-identity retries $(bm0 bridge0 bridge_registration_duplicate_identity_retries_total); caught up after $(caught_up bridge0 60) s")
        [ -n "$reg" ] && fle "$reg" 30 || FAILS+=("restarted bridge0 not registered within 30 s")
    else
        tl "kill -TERM bridge0 at core $(core_cursor)"; kill -TERM "${PID[bridge0]}"; wait_exit bridge0 7
        [ "$WAIT_RC" = 0 ] || FAILS+=("bridge0 SIGTERM exit $WAIT_RC")
        start_bridge bridge_b external || FAILS+=("bridge_b start"); wait_registered bridge_b 10 || FAILS+=("bridge_b registered")
        NOTES+=("bridge_b caught up after $(caught_up bridge_b 60) s")
    fi
    for _ in $(seq 1 6000); do [ "$(core_cursor)" -ge "$k2" ] && break; sleep 0.02; done
    tl "kill -TERM bridge1 at core $(core_cursor)"; kill -TERM "${PID[bridge1]}"; wait_exit bridge1 7
    [ "$WAIT_RC" = 0 ] || FAILS+=("bridge1 SIGTERM exit $WAIT_RC")
    sleep "$([ "$v" = restart ] && echo 3 || echo 0)"
    start_bridge bridge1 external || FAILS+=("bridge1 restart"); wait_registered bridge1 15 || FAILS+=("bridge1 registered")
    wait "$runner" || FAILS+=("round 1: $(tail -1 "$D/ev/run1.txt")")
    cursor_sampler_stop
    local flat; flat=$(flat_samples "$b1")
    NOTES+=("flat 2 s cursor samples below b_1: $flat of $(wc -l < "$D/ev/cursor-2s.txt"); rates/2s: $(awk 'NR>1{printf "%d ", ($2-p)/($1-t)} {p=$2; t=$1}' "$D/ev/cursor-2s.txt")")
    [ "$flat" = 0 ] || FAILS+=("outage during restart: $flat flat samples")
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    quiescent "$N" 60000 || FAILS+=("quiescence")
    oracle_ok final || FAILS+=("oracle: $(head -1 "$D/ev/oracle-final.txt")")
    local sl; for sl in $(live_slots); do
        [ "$(bm_sum "$sl" bridge_settle_rejected_total)" = 0 ] || FAILS+=("$sl SettleRejected")
        [ "$(bm0 "$sl" bridge_client_sessions_opened_total)" = 1 ] || FAILS+=("$sl sessions $(bm0 "$sl" bridge_client_sessions_opened_total)")
    done
    if [ "$v" = replace ]; then
        gate_watch_stop
        [ -z "$(gate_after "$gmark")" ] || FAILS+=("gate transition during the replace: $(gate_after "$gmark" | head -2 | tr '\n' ';')")
        [ "$(counter_delta assets_feed_halt_applied_total "$APPLIED0")" = 0 ] || FAILS+=("halt applied during the replace")
        NOTES+=("BRIDGE sessions now $(core_counter 'assets_sessions{class="BRIDGE"}'); identities seen: 1 2 3 (bridge0 / bridge1 / bridge_b keys)")
    fi
    common_checks
    finish "$drill" "$(verdict)"
}

lh_base() { # drill seed -> S1, A1, B1 (the hole round 1 of a --long-round 2000 story)
    setup "$1" 2 1 $((SEED_BASE + $2)) "$ROUNDS" --long-round "${IT_LH_LONG:-2000}" || return 1
    S1=$(mid_round 1); read -r A1 B1 < <(round_parts 1)
}
drill_rk14_lanehole_ahead() { # binary drill
    local binary=$1 drill=$2
    lh_base "$drill" 1440 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    jctl 30 HOLE 0 "$S1" $((S1 + 98)) >> "$D/ev/jctl.txt" # the range verb (4.5 item 3)
    start_bridge bridge1 external "$BRIDGE" SOURCES_OF=1 || FAILS+=("bridge1 start")
    start_bridge bridge0 external "$binary" SOURCES_OF=0 || FAILS+=("bridge0 start")
    wait_registered bridge0 30 || FAILS+=("bridge0 registered"); wait_registered bridge1 30 || FAILS+=("bridge1 registered")
    gate_watch_start 1800 || true
    APPLIED0=$(core_counter assets_feed_halt_applied_total | sed 's/absent/0/'); STALE0=$(core_counter assets_feed_halt_stale_total | sed 's/absent/0/')
    ( while :; do echo "$(date +%s) $(http_code bridge0 ready) $(http_code bridge1 ready) $(bm0 bridge0 bridge_halted)"; sleep 1; done ) > "$D/ev/ready-1s.txt" 2>&1 & PID[ready]=$!
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    sleep 1; local gmark; gmark=$(mono_ms) # round 0 opens the gate: only transitions after the story's start count
    local tfirst; tfirst=$(date +%s)
    t1core round "$STORY" 1 > "$D/ev/round1.txt" 2>&1
    jctl 120 WRITE 1 "$A1" "$B1" >> "$D/ev/jctl.txt"
    t1core barrier "$B1" 60000 > /dev/null 2>&1 || FAILS+=("barrier b_1 by bridge1")
    local la0; la0=$(bm0 bridge0 'bridge_fences_total{trigger="lane_ahead"}')
    jctl 120 WRITE 0 "$A1" "$B1" >> "$D/ev/jctl.txt"
    sleep 8
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    quiescent "$N" 60000 || FAILS+=("quiescence")
    kill "${PID[ready]}" 2>/dev/null; wait "${PID[ready]}" 2>/dev/null; unset "PID[ready]"
    gate_watch_stop
    local sent; sent=$(bm0 bridge0 'bridge_feed_halts_sent_total{reason="1"}'); local stale; stale=$(counter_delta assets_feed_halt_stale_total "$STALE0")
    if [ "$binary" != "$BRIDGE" ]; then
        # RED ct_bridge_nc_noconfirm (S2-7 discriminator (b)): the nc build sends a FeedHalt the core acks stale
        NOTES+=("RED: FeedHalts sent $sent, halt_stale +$stale")
        FAILS=(); if [ "$sent" -ge 1 ] && [ "$stale" -ge 1 ]; then finish "$drill" PASS; else finish "$drill" INCONCLUSIVE; fi
        return
    fi
    grep -q "verdict source 0 e $S1 HOLED seq $((S1 + 99))" "$D/ev/bridge0.log" || FAILS+=("no HOLED verdict at s1 with seen s1 + 99")
    [ "$(( $(bm0 bridge0 'bridge_fences_total{trigger="lane_ahead"}') - la0 ))" -ge 1 ] || FAILS+=("no lane_ahead fence")
    [ "$sent" = 0 ] || FAILS+=("a FeedHalt was sent ($sent)")
    awk '$4 == 1' "$D/ev/ready-1s.txt" | grep -q . && FAILS+=("bridge0 halted at some sample")
    [ -z "$(gate_after "$gmark")" ] || FAILS+=("gate transition: $(gate_after "$gmark" | head -1)")
    [ "$(counter_delta assets_feed_halt_applied_total "$APPLIED0")" = 0 ] || FAILS+=("halt applied")
    local not200; not200=$(awk -v t="$tfirst" '$1 > t + 2 && ($2 != 200 || $3 != 200)' "$D/ev/ready-1s.txt" | wc -l)
    NOTES+=("/ready non-200 samples after the first forward (3.6): $not200")
    [ "$not200" = 0 ] || FAILS+=("/ready 503 during a confirm cycle ($not200 samples)")
    oracle_ok final || FAILS+=("oracle: $(head -1 "$D/ev/oracle-final.txt")")
    common_checks
    finish "$drill" "$(verdict)"
}
drill_rk14_lanehole_behind() { # variant behind|dead|stale
    local v=$1 drill=rk14-lanehole-$1 b0bin=$BRIDGE
    [ "$v" = stale ] && b0bin=$(bin ct_bridge_tc_slowhalt)
    lh_base "$drill" 1441 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    jctl 30 HOLE 0 "$S1" $((S1 + 98)) >> "$D/ev/jctl.txt"
    start_bridge bridge1 external "$BRIDGE" SOURCES_OF=1 || FAILS+=("bridge1 start")
    start_bridge bridge0 external "$b0bin" SOURCES_OF=0 || FAILS+=("bridge0 start")
    wait_registered bridge0 30 || FAILS+=("bridge0 registered"); wait_registered bridge1 30 || FAILS+=("bridge1 registered")
    gate_watch_start 1800 || true
    APPLIED0=$(core_counter assets_feed_halt_applied_total | sed 's/absent/0/'); STALE0=$(core_counter assets_feed_halt_stale_total | sed 's/absent/0/')
    REOPEN0=$(core_counter assets_gate_auto_reopen_total | sed 's/absent/0/'); AUTOCLR0=$(core_counter assets_feed_latch_auto_clear_total | sed 's/absent/0/')
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    sleep 1; local gmark; gmark=$(mono_ms)
    t1core round "$STORY" 1 > "$D/ev/round1.txt" 2>&1
    jctl 120 WRITE 1 "$A1" $((S1 + 49)) >> "$D/ev/jctl.txt"
    t1core barrier $((S1 + 49)) 60000 > /dev/null 2>&1 || FAILS+=("barrier s1 + 49")
    jctl 120 WRITE 0 "$A1" "$B1" >> "$D/ev/jctl.txt"
    for _ in $(seq 1 300); do [ "$(bm0 bridge0 bridge_halted)" = 1 ] && break; sleep 0.1; done
    [ "$(bm0 bridge0 bridge_halted)" = 1 ] || FAILS+=("bridge0 not halted")
    case $v in
        dead) for _ in $(seq 1 100); do grep -q 'FeedHalt AdminAck GAP' "$D/ev/bridge0.log" && break; sleep 0.1; done
              local sentd; sentd=$(bm0 bridge0 'bridge_feed_halts_sent_total{reason="1"}')
              kill -9 "${PID[bridge0]}"; wait_exit bridge0 5; tl "kill -9 bridge0 after its FeedHalt's AdminAck"
              [ "$sentd" = 1 ] || FAILS+=("FeedHalts before the kill $sentd"); sleep 5 ;;
        stale) sleep 1 ;;
        *) sleep 5 ;;
    esac
    tl "release WRITE 1 $((S1 + 50)) $B1"; jctl 120 WRITE 1 $((S1 + 50)) "$B1" >> "$D/ev/jctl.txt"
    t1core barrier "$B1" 60000 > /dev/null 2>&1 || FAILS+=("barrier b_1 after the release")
    if [ "$v" != dead ]; then
        local rec; rec=""; for _ in $(seq 1 300); do [ "$(bm0 bridge0 'bridge_halt_recovered_total{reason="GAP"}')" -ge 1 ] && { rec=yes; break; }; sleep 0.1; done
        [ -n "$rec" ] || FAILS+=("bridge0 no exit (a) in 30 s")
        [ "$(bm0 bridge0 bridge_client_sessions_opened_total)" = 1 ] || FAILS+=("bridge0 restarted / reconnected")
    fi
    sleep 2
    local sent; sent=$( [ "$v" = dead ] && echo 1 || bm0 bridge0 'bridge_feed_halts_sent_total{reason="1"}')
    [ "$sent" = 1 ] || FAILS+=("FeedHalts GAP $sent (want 1)")
    if [ "$v" = stale ]; then
        [ "$(counter_delta assets_feed_halt_stale_total "$STALE0")" -ge 1 ] || FAILS+=("halt stale +0 (the tc build's FeedHalt was not stale)")
        [ "$(counter_delta assets_feed_halt_applied_total "$APPLIED0")" = 0 ] || FAILS+=("halt applied +$(counter_delta assets_feed_halt_applied_total "$APPLIED0")")
        [ -z "$(gate_after "$gmark")" ] || FAILS+=("gate transition: $(gate_after "$gmark" | head -1)")
    else
        [ "$(counter_delta assets_feed_halt_applied_total "$APPLIED0")" = 1 ] || FAILS+=("halt applied +$(counter_delta assets_feed_halt_applied_total "$APPLIED0") (want +1)")
        local ack sus; ack=$(log_ms "$D/ev/bridge0.log" "FeedHalt AdminAck GAP"); sus=$(first_gate SUSPENDED BRIDGE_HALT 0)
        [ -n "$ack" ] && [ -n "$sus" ] && [ $(( sus > ack ? sus - ack : ack - sus )) -le 1000 ] || FAILS+=("SUSPENDED/BRIDGE_HALT not within 1 s of the AdminAck (${ack:-none} / ${sus:-none})")
        [ "$(counter_delta assets_feed_latch_auto_clear_total "$AUTOCLR0")" -ge 1 ] || FAILS+=("latch auto-clear +0")
        [ "$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")" = 1 ] || FAILS+=("auto reopen +$(counter_delta assets_gate_auto_reopen_total "$REOPEN0") (want +1)")
        gate_after "$gmark" | awk '$2=="OPEN"' | grep -q . || FAILS+=("no automatic OPEN in the gate stream")
        t1core report > "$D/ev/report.json" 2>&1; grep -q '"feedHalted":false' "$D/ev/report.json" || FAILS+=("core still halted")
    fi
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    t1core barrier "$N" 60000 > /dev/null 2>&1 || FAILS+=("barrier N")
    gate_watch_stop
    oracle_ok final || FAILS+=("oracle: $(head -1 "$D/ev/oracle-final.txt")")
    if [ "$v" = dead ]; then # the dead lane's session ends at the core's 10 s session timeout
        for _ in $(seq 1 300); do [ "$(core_counter 'assets_sessions{class="BRIDGE"}')" = 1 ] && break; sleep 0.1; done
        [ "$(core_counter 'assets_sessions{class="BRIDGE"}')" = 1 ] || FAILS+=("BRIDGE sessions $(core_counter 'assets_sessions{class="BRIDGE"}') (want 1 after the kill)")
    fi
    NOTES+=("gate: $(gate_transitions | tr '\n' ';' | cut -c1-200)")
    common_checks
    finish "$drill" "$(verdict)"
}
drill_rk14_twoholes() {
    local drill=rk14-twoholes
    lh_base "$drill" 1450 || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    local s2=$((S1 + 30))
    jctl 30 HOLE 0 "$S1" $((s2 + 5)) >> "$D/ev/jctl.txt"; jctl 30 HOLE 1 "$s2" "$s2" >> "$D/ev/jctl.txt"
    start_bridge bridge0 external "$BRIDGE" SOURCES_OF=0 || FAILS+=("bridge0 start")
    start_bridge bridge1 external "$BRIDGE" SOURCES_OF=1 || FAILS+=("bridge1 start")
    wait_registered bridge0 30 || FAILS+=("bridge0 registered"); wait_registered bridge1 30 || FAILS+=("bridge1 registered")
    gate_watch_start 1800 || true
    APPLIED0=$(core_counter assets_feed_halt_applied_total | sed 's/absent/0/'); REOPEN0=$(core_counter assets_gate_auto_reopen_total | sed 's/absent/0/')
    AUTOCLR0=$(core_counter assets_feed_latch_auto_clear_total | sed 's/absent/0/')
    run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
    sleep 1; GMARK=$(mono_ms)
    t1core round "$STORY" 1 > "$D/ev/round1.txt" 2>&1
    jctl 60 WRITE 0 "$A1" "$B1" >> "$D/ev/jctl.txt"; jctl 60 WRITE 1 "$A1" $((S1 - 1)) >> "$D/ev/jctl.txt"
    for _ in $(seq 1 300); do [ "$(bm0 bridge0 bridge_halted)" = 1 ] && break; sleep 0.1; done
    sleep 2
    [ "$(core_counter assets_feed_lane_halts)" = 1 ] || FAILS+=("step 1: lane halts $(core_counter assets_feed_lane_halts)")
    first_gate SUSPENDED BRIDGE_HALT 0 | grep -q . || FAILS+=("step 1: no SUSPENDED/BRIDGE_HALT")
    # step 2 (W0 setup fix: write through s2 + 1 so source 1 is HOLED at s2 with seen s2 + 1, not BEHIND)
    jctl 60 WRITE 1 "$S1" $((s2 + 1)) >> "$D/ev/jctl.txt"
    for _ in $(seq 1 300); do [ "$(bm0 bridge1 bridge_halted)" = 1 ] && break; sleep 0.1; done
    sleep 2
    [ "$(core_counter assets_feed_lane_halts)" = 2 ] || FAILS+=("step 2: lane halts $(core_counter assets_feed_lane_halts) (want 2)")
    [ "$(counter_delta assets_feed_halt_applied_total "$APPLIED0")" = 2 ] || FAILS+=("step 2: halt applied +$(counter_delta assets_feed_halt_applied_total "$APPLIED0") (want +2)")
    # step 3: FILL 1 s2, WRITE 1 s2+1 s2+2: record B clears, record A stays; NO transition for at least 5 s
    local tB; tB=$(gate_transitions | tail -1 | awk '{print $1}')
    jctl 30 FILL 1 "$s2" >> "$D/ev/jctl.txt"; jctl 30 WRITE 1 $((s2 + 1)) $((s2 + 2)) >> "$D/ev/jctl.txt"
    for _ in $(seq 1 1200); do [ "$(core_cursor)" -ge $((s2 + 2)) ] && break; sleep 0.1; done
    [ "$(core_cursor)" -ge $((s2 + 2)) ] || FAILS+=("step 3: cursor $(core_cursor) < s2 + 2 (bridge1 never reached the FILL successor)")
    sleep 5
    [ "$(core_counter assets_feed_lane_halts)" = 1 ] || FAILS+=("step 3: lane halts $(core_counter assets_feed_lane_halts) (want 1)")
    local after3; after3=$(gate_transitions | awk -v a="${tB:-0}" '$1 > a')
    [ -z "$after3" ] || FAILS+=("transition during the 5 s hold: $(echo "$after3" | head -1)")
    # step 4
    jctl 60 WRITE 1 $((s2 + 3)) "$B1" >> "$D/ev/jctl.txt"
    t1core barrier "$B1" 60000 > /dev/null 2>&1 || FAILS+=("step 4: barrier b_1")
    sleep 3
    [ "$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")" = 1 ] || FAILS+=("step 4: auto reopen +$(counter_delta assets_gate_auto_reopen_total "$REOPEN0") (want exactly 1)")
    [ "$(counter_delta assets_feed_latch_auto_clear_total "$AUTOCLR0")" = 2 ] || FAILS+=("step 4: latch auto-clear +$(counter_delta assets_feed_latch_auto_clear_total "$AUTOCLR0") (want +2)")
    [ "$(gate_after "$GMARK" | awk '$2=="OPEN"' | wc -l)" = 1 ] || FAILS+=("OPEN lines $(gate_after "$GMARK" | awk '$2=="OPEN"' | wc -l) (want 1)")
    [ "$(bm0 bridge0 'bridge_halt_recovered_total{reason="GAP"}')" -ge 1 ] || FAILS+=("bridge0 no exit (a)")
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    t1core barrier "$N" 60000 > /dev/null 2>&1 || FAILS+=("barrier N")
    gate_watch_stop
    oracle_ok final || FAILS+=("oracle: $(head -1 "$D/ev/oracle-final.txt")")
    NOTES+=("s1 $S1 s2 $s2; gate: $(gate_transitions | tr '\n' ';' | cut -c1-240)")
    common_checks
    finish "$drill" "$(verdict)"
}
drill_rk14_lag_failover() { # binary drill
    local binary=$1 drill=$2 tries=0
    while :; do
        tries=$((tries + 1))
        setup "$drill" 2 1 $((SEED_BASE + 1460 + tries)) "$ROUNDS" --long-round "${IT_LF_LONG:-1000000}" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
        local A B Sx; read -r A B < <(round_parts 1); Sx=$((A + (B - A) * 60 / 100))
        jctl 10 HOLE 0 "$Sx" "$Sx" >> "$D/ev/jctl.txt"
        start_bridge bridge0 external "$binary" || FAILS+=("bridge0 start"); start_bridge bridge1 external "$BRIDGE" SOURCES_OF=1 || FAILS+=("bridge1 start")
        wait_registered bridge0 30 || FAILS+=("bridge0 registered"); wait_registered bridge1 30 || FAILS+=("bridge1 registered")
        run_story --to 0 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("round 0")
        t1core round "$STORY" 1 > "$D/ev/round1.txt" 2>&1
        local fo0 la0 cold0 sk0; fo0=$(bm0 bridge0 bridge_source_failovers_total); la0=$(bm0 bridge0 'bridge_fences_total{trigger="lane_ahead"}')
        cold0=$(bm0 bridge0 'bridge_memo_resumes_total{kind="cold"}'); sk0=$(bm_sum bridge0 bridge_skipped_total)
        jctl 300 WRITE 1 "$A" "$B" >> "$D/ev/jctl.txt"; jctl 300 WRITE 0 "$A" $((Sx - 1)) >> "$D/ev/jctl.txt"
        local tc="" tb=""; for _ in $(seq 1 1200); do
            # W0 finding (dr-rk14-lag-failover, stage A): the core's cursor is not enough, bridge0's own last report must
            # show C' > s + 10000 (its periodic report, <= 10 s) or the hole is a plain failover (C = 0 at the write)
            [ -z "$tc" ] && [ "$(bm0 bridge0 bridge_core_cursor)" -gt $((Sx + 10000)) ] && tc=$(now_s)
            [ -z "$tb" ] && [ "$(bm0 bridge0 bridge_last_offered_seq)" = $((Sx - 1)) ] && tb=$(now_s)
            [ -n "$tc" ] && [ -n "$tb" ] && break; sleep 0.05; done
        local tw; tw=$(now_s); local gap; gap=$(python3 -I -c "import sys; print('%.2f' % (float(sys.argv[1]) - float(sys.argv[2])))" "$tw" "${tb:-$tw}")
        local lagAt=$(( $(core_cursor) - $(bm0 bridge0 bridge_last_offered_seq) ))
        jctl 300 WRITE 0 $((Sx + 1)) "$B" >> "$D/ev/jctl.txt"
        local tnext=""; for _ in $(seq 1 300); do [ "$(bm0 bridge0 bridge_last_offered_seq)" -gt $((Sx - 1)) ] && { tnext=$(elapsed "$tw" "$(now_s)"); break; }; sleep 0.1; done
        t1core barrier "$B" 300000 > /dev/null 2>&1 || FAILS+=("barrier b_1")
        sleep 3
        local holed; holed=$(grep -c "verdict source 0 e $Sx HOLED" "$D/ev/bridge0.log")
        local la=$(( $(bm0 bridge0 'bridge_fences_total{trigger="lane_ahead"}') - la0 )) fo=$(( $(bm0 bridge0 bridge_source_failovers_total) - fo0 ))
        local cold=$(( $(bm0 bridge0 'bridge_memo_resumes_total{kind="cold"}') - cold0 )) sk=$(( $(bm_sum bridge0 bridge_skipped_total) - sk0 ))
        NOTES+=("try $tries: hole $Sx; bridge0 report cursor at the write $(bm0 bridge0 bridge_core_cursor); s-1 -> catch-up write $gap s; HOLED lines $holed; lag at the hole $lagAt; lane_ahead +$la failovers +$fo cold +$cold skipped +$sk; next forward ${tnext:-none} s")
        if [ "$holed" -ge 1 ] || [ $tries -ge 3 ]; then break; fi
        finish "$drill-try$tries" INCONCLUSIVE
    done
    run_story --from 2 --timeout 120000 > "$D/ev/run2.txt" 2>&1 || FAILS+=("rounds 2..")
    t1core barrier "$N" 120000 > /dev/null 2>&1 || FAILS+=("barrier N")
    oracle_ok final || FAILS+=("oracle: $(head -1 "$D/ev/oracle-final.txt")")
    NOTES+=("forwarded bridge0 $(bm_sum bridge0 bridge_forwarded_records_total) bridge1 $(bm_sum bridge1 bridge_forwarded_records_total); core log position $(ae_metric assets_log_position) (3.7)")
    if [ "$binary" != "$BRIDGE" ]; then
        FAILS=(); if [ "$fo" -ge 1 ] || [ -z "$tnext" ] || ! fle "$tnext" 10; then finish "$drill" PASS; else finish "$drill" INCONCLUSIVE; fi; return
    fi
    [ "$holed" -ge 1 ] || { NOTES+=("precondition not met in 3 tries"); finish "$drill" INCONCLUSIVE; return; }
    [ "$la" -ge 1 ] || FAILS+=("lane_ahead +$la")
    [ "$fo" = 0 ] || FAILS+=("failovers +$fo")
    [ "$cold" -le 1 ] || FAILS+=("cold walks +$cold > 1")
    [ -n "$tnext" ] && fle "$tnext" 10 || FAILS+=("next forward ${tnext:-none} s > 10 s")
    common_checks
    finish "$drill" "$(verdict)"
}

# ================================================================== S10' section 4 / 5: the halt drills (stage B, <RC>)

# The gate stream (4.5 item 7): one GATEWAY session printing every AdmissionState of GLOBAL, the ONLY source of gate
# lines in these rows. Started straight on java (no sed in the pipe: the lines are read while it runs).
gate_watch_start() { # until_s
    env "${T1ENV[@]}" java "${JAVA_OPTS[@]}" -jar "$HARNESS" t1-core gate --watch 50 --until "$1" > "$D/ev/gate-watch.txt" 2>&1 &
    PID[gatewatch]=$!
    for _ in $(seq 1 300); do grep -q '^INITIAL ' "$D/ev/gate-watch.txt" 2>/dev/null && return 0; sleep 0.1; done
    FAILS+=("gate --watch never printed its INITIAL line"); return 1
}
gate_watch_stop() { kill -TERM "${PID[gatewatch]}" 2>/dev/null; wait "${PID[gatewatch]}" 2>/dev/null; unset "PID[gatewatch]"; }
# transitions after the INITIAL line: "t state names"
gate_transitions() { awk '/^t=/{ for (i=1;i<=NF;i++) { split($i,kv,"="); v[kv[1]]=kv[2] } print v["t"], v["state"], v["names"] }' "$D/ev/gate-watch.txt"; }
mono_ms() { python3 -I -c 'import time; print(int(time.monotonic() * 1000))'; } # CLOCK_MONOTONIC, as the watch's t
gate_after() { gate_transitions | awk -v a="$1" '$1 > a'; } # transitions after a mono_ms mark
first_gate() { # state name-substring after_ms -> the t of the first matching transition (empty: none)
    gate_transitions | awk -v s="$1" -v n="$2" -v a="$3" '$1 >= a && $2 == s && index($3, n) { print $1; exit }'
}
log_ms() { # logfile pattern -> the [ms] of the first matching line (the bridge's steady clock, as the watch's t)
    grep -m1 -- "$2" "$1" 2>/dev/null | sed -n 's/^[A-Z]* \[\([0-9]*\)\].*/\1/p'
}
counter_delta() { echo $(( $(core_counter "$1" | sed 's/absent/0/') - $2 )); }
inv_ok() { # label -> invariants PASS (4.5 item 2, C10)
    t1core invariants > "$D/ev/invariants-$1.txt" 2>&1
    grep -q '^INVARIANTS PASS' "$D/ev/invariants-$1.txt" || FAILS+=("invariants $1: $(grep -m1 'INVARIANTS' "$D/ev/invariants-$1.txt")")
}
trade_fields() { # seq -> "maker market takerIsBuy makerOmsOrderId" from the story's FJR (schema 4 Trade offsets)
    python3 -I - "$STORY/journal.fjr" "$1" <<'PY2'
import struct, sys
data = open(sys.argv[1], 'rb').read(); want = int(sys.argv[2])
(count,) = struct.unpack_from('<I', data, 4); at = 8
for _ in range(count):
    kind = data[at]; at += 1
    if kind == 2: continue
    (n,) = struct.unpack_from('<H', data, at); at += 2
    rec = data[at:at + n]; at += n
    if struct.unpack_from('<q', rec, 8)[0] != want: continue
    if struct.unpack_from('<H', rec, 2)[0] != 1: sys.exit(1)
    print(struct.unpack_from('<q', rec, 68)[0], struct.unpack_from('<i', rec, 40)[0], rec[92], struct.unpack_from('<q', rec, 109)[0])
    sys.exit(0)
sys.exit(1)
PY2
}
halt_setup() { # drill seedOffset trade(yes|no) -> S, HR; HOLE all S, bridge0 external, run to the hole, halted
    local drill=$1
    setup "$drill" 2 1 $((SEED_BASE + $2)) "$ROUNDS" || return 1
    # a Trade (its maker holds the hold asset): the first at or after mid_round 4, else after mid_round 2 (W0: the plain
    # story of a seed may carry no Trade in its later rounds)
    if [ "$3" = yes ]; then S=$(trade_seq_after "$(mid_round 4)") || S=$(trade_seq_after "$(mid_round 2)") || { FAILS+=("no trade after mid_round 2"); return 1; }
    else S=$(mid_round 4); fi
    HR=0; while [ "$(barrier_of $HR)" -lt "$S" ]; do HR=$((HR + 1)); done
    jctl 30 HOLE all "$S" >> "$D/ev/jctl.txt"
    start_bridge bridge0 external || FAILS+=("bridge0 start")
    wait_registered bridge0 30 || FAILS+=("registered")
    gate_watch_start 1800 || return 1
    APPLIED0=$(core_counter assets_feed_halt_applied_total | sed 's/absent/0/')
    REOPEN0=$(core_counter assets_gate_auto_reopen_total | sed 's/absent/0/')
    AUTOCLR0=$(core_counter assets_feed_latch_auto_clear_total | sed 's/absent/0/')
    run_story --to "$HR" --timeout 20000 > "$D/ev/run-halt.txt" 2>&1 # stops at the hole: expected
    for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
    sleep 2
    GMARK=$(mono_ms) # the story's round 0 OPEN is before this mark
    scrape bridge0 halted
    [ "$(bm0 bridge_halted)" = "1" ] && [ "$(bm0 bridge_halt_reason)" = "1" ] || FAILS+=("halted $(bm0 bridge_halted) reason $(bm0 bridge_halt_reason)")
}
halt_bound_check() { # the R-B6-1 bound: SUSPENDED/BRIDGE_HALT within 1 s of the FeedHalt's AdminAck
    local ack; ack=$(log_ms "$D/ev/bridge0.log" "FeedHalt AdminAck GAP")
    local sus; sus=$(first_gate SUSPENDED BRIDGE_HALT 0)
    if [ -z "$ack" ] || [ -z "$sus" ]; then FAILS+=("R-B6-1: AdminAck ${ack:-none} / SUSPENDED ${sus:-none}"); return; fi
    local d=$(( sus > ack ? sus - ack : ack - sus ))
    NOTES+=("R-B6-1: SUSPENDED/BRIDGE_HALT ${d} ms from the FeedHalt AdminAck")
    [ "$d" -le 1000 ] || FAILS+=("SUSPENDED $d ms from the AdminAck (> 1000)")
    [ "$(counter_delta assets_feed_halt_applied_total "$APPLIED0")" = 1 ] || FAILS+=("halt applied +$(counter_delta assets_feed_halt_applied_total "$APPLIED0") (want +1)")
}
reopen_check() { # 4R: OPEN by itself within 1 s of the bridge's "halt left", no `open` issued
    local left; left=$(log_ms "$D/ev/bridge0.log" "halt left")
    local open; open=$(first_gate OPEN "" "${left:-0}")
    [ -z "$open" ] && open=$(gate_transitions | awk '$2=="OPEN"{print $1; exit}')
    if [ -z "$left" ] || [ -z "$open" ]; then FAILS+=("4R reopen: halt left ${left:-none} / OPEN ${open:-none}"); return; fi
    local d=$(( open > left ? open - left : left - open ))
    NOTES+=("4R: OPEN ${d} ms from the reach-clear (bridge 'halt left')")
    [ "$d" -le 1000 ] || FAILS+=("auto OPEN $d ms from the reach-clear (> 1000)")
    [ "$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")" = "${1:-1}" ] || FAILS+=("auto reopen +$(counter_delta assets_gate_auto_reopen_total "$REOPEN0") (want +${1:-1})")
}

drill_rk10b() { # variant: b (R-B6-1, -2, -4) | c (rule 4, withdraw between two engine events) | negctl (RED)
    local v=$1 drill=rk10${1}; [ "$v" = negctl ] && drill=rk10b-negctl
    halt_setup "$drill" 1500 "$([ "$v" = c ] && echo yes || echo no)" || { finish "$drill" FAIL; return; }
    [ "$v" = negctl ] && { kill -9 "${PID[bridge0]}"; wait_exit bridge0 5; }
    halt_bound_check
    inv_ok halted-before-probes
    ORACLE_LATCH="L true 1" oracle_ok upto-halted --upto $((S - 1)) || FAILS+=("oracle --upto $((S - 1)) L true 1")
    t1core probe-create 1 1 > "$D/ev/probe-create.txt" 2>&1
    grep -q 'OrderReject GATE_CLOSED' "$D/ev/probe-create.txt" || FAILS+=("probe-create: $(tr '\n' ' ' < "$D/ev/probe-create.txt" | cut -c1-120)")
    if [ "$v" = c ]; then
        local maker market tib morder hold
        read -r maker market tib morder < <(trade_fields "$S")
        hold=0; [ "$tib" = 1 ] && hold=$market
        t1core invariants > "$D/ev/invariants-maker.txt" 2>&1
        local line avail locked; line=$(grep "^I $maker $hold " "$D/ev/invariants-maker.txt")
        avail=$(echo "$line" | sed -n 's/.*available=\([0-9-]*\).*/\1/p'); locked=$(echo "$line" | sed -n 's/.*locked=\([0-9-]*\).*/\1/p')
        NOTES+=("s $S Trade: maker $maker order $morder hold asset $hold: available $avail locked $locked")
        MAKER=$maker; HOLDA=$hold; AVAIL=$avail
        t1core probe-amend "$morder" > "$D/ev/probe-amend.txt" 2>&1
        grep -q 'OrderReject GATE_CLOSED' "$D/ev/probe-amend.txt" || FAILS+=("probe-amend: $(tr '\n' ' ' < "$D/ev/probe-amend.txt" | cut -c1-120)")
        t1core withdraw "$maker" "$hold" "$avail" --record "$D/ev/extra.flog" > "$D/ev/w-avail.txt" 2>&1
        grep -q 'result=ACCEPTED' "$D/ev/w-avail.txt" || FAILS+=("withdraw available: $(cat "$D/ev/w-avail.txt")")
        t1core withdraw "$maker" "$hold" 1 > "$D/ev/w-plus1.txt" 2>&1
        grep -q 'result=REJECTED' "$D/ev/w-plus1.txt" && grep -qi 'insufficient' "$D/ev/w-plus1.txt" || FAILS+=("withdraw 1 more: $(cat "$D/ev/w-plus1.txt")")
        NOTES+=("withdraw available: $(tr '\n' ' ' < "$D/ev/w-avail.txt"); 1 more: $(tr '\n' ' ' < "$D/ev/w-plus1.txt")")
        inv_ok after-withdraw
        grep -q "^I $maker $hold .*locked=$locked " "$D/ev/invariants-after-withdraw.txt" || FAILS+=("locked changed by the withdraw")
    else
        for u in 1 2; do
            t1core deposit "$u" 1 777 >> "$D/ev/treasury.txt" 2>&1; t1core withdraw "$u" 1 777 >> "$D/ev/treasury.txt" 2>&1
        done
        [ "$(grep -c 'result=ACCEPTED' "$D/ev/treasury.txt")" = 4 ] || FAILS+=("treasury during the halt: $(tr '\n' ' ' < "$D/ev/treasury.txt")")
        # the invariants RED (4.3): the seam breaks one locked balance; the check must FAIL naming user and asset
        t1core invariants --seam BREAK_LOCKED > "$D/ev/invariants-red.txt" 2>&1
        grep -q '^INVARIANTS FAIL user=' "$D/ev/invariants-red.txt" && NOTES+=("invariants RED: $(grep -m1 '^INVARIANTS FAIL' "$D/ev/invariants-red.txt")") ||
            FAILS+=("invariants --seam BREAK_LOCKED did not fail: $(tail -1 "$D/ev/invariants-red.txt")")
    fi
    inv_ok halted-after-probes
    if [ "$v" = negctl ]; then
        start_bridge bridge0 external "$(bin ct_bridge_negctl)" || FAILS+=("negctl start")
    fi
    jctl 60 FILL all "$S" >> "$D/ev/jctl.txt"
    rejournal 0 "$(barrier_of "$HR")"
    local tf; tf=$(now_s) resumed=""
    for _ in $(seq 1 1500); do [ "$(bm0 bridge_halted)" = "0" ] && { resumed=$(elapsed "$tf" "$(now_s)"); break; }; sleep 0.1; done
    t1core barrier "$(barrier_of "$HR")" 60000 > /dev/null 2>&1
    sleep 2
    [ -n "$resumed" ] || FAILS+=("not resumed within 150 s")
    [ "$v" = negctl ] || reopen_check 1
    [ "$(counter_delta assets_feed_latch_auto_clear_total "$AUTOCLR0")" -ge 1 ] || FAILS+=("latch auto-clear +0")
    if [ "$v" = c ]; then
        oracle_ok after-fill --upto "$(barrier_of "$HR")" --extra "$D/ev/extra.flog" || FAILS+=("oracle --upto b_$HR --extra the withdraw")
        inv_ok after-fill
        t1core deposit "$MAKER" "$HOLDA" "$AVAIL" > "$D/ev/redeposit.txt" 2>&1
        grep -q 'result=ACCEPTED' "$D/ev/redeposit.txt" || FAILS+=("re-deposit: $(cat "$D/ev/redeposit.txt")")
    fi
    run_story --from $((HR + 1)) --timeout 120000 > "$D/ev/run-end.txt" 2>&1 || FAILS+=("rounds $((HR + 1))..")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    gate_watch_stop
    inv_ok final
    if [ "$v" = negctl ]; then
        oracle_ok final; local orc=$?
        NOTES+=("negctl: final oracle rc $orc ($(head -1 "$D/ev/oracle-final.txt"))")
        if [ $orc -ne 0 ] && grep -q 'FAIL at line' "$D/ev/oracle-final.txt" && grep -q '^INVARIANTS PASS' "$D/ev/invariants-final.txt"; then
            FAILS=(); finish "$drill" PASS; else FAILS=(); finish "$drill" INCONCLUSIVE; fi
        return
    fi
    oracle_ok final || FAILS+=("final oracle: $(head -1 "$D/ev/oracle-final.txt")")
    NOTES+=("hole at $S (round $HR), resumed $resumed s after FILL; gate transitions: $(gate_transitions | tr '\n' ';' | cut -c1-300)")
    common_checks
    finish "$drill" "$(verdict)"
}

drill_rk10_opsuspend() {
    local drill=rk10-opsuspend
    halt_setup "$drill" 1510 no || { finish "$drill" FAIL; return; }
    t1core suspend > "$D/ev/suspend.txt" 2>&1
    grep -q 'result=ACCEPTED' "$D/ev/suspend.txt" || FAILS+=("suspend: $(cat "$D/ev/suspend.txt")")
    sleep 1
    local t0; t0=$(gate_transitions | tail -1 | awk '{print $1}')
    jctl 60 FILL all "$S" >> "$D/ev/jctl.txt"; rejournal 0 "$(barrier_of "$HR")"
    for _ in $(seq 1 1500); do [ "$(bm0 bridge_halted)" = "0" ] && break; sleep 0.1; done
    t1core barrier "$(barrier_of "$HR")" 60000 > /dev/null 2>&1
    sleep 2
    local after; after=$(gate_transitions | awk -v a="$t0" '$1 > a')
    NOTES+=("transitions after the fill: $(echo "$after" | tr '\n' ';')")
    echo "$after" | grep -q ' OPEN ' && FAILS+=("OPEN while ADMIN holds")
    echo "$after" | awk '{print $3}' | grep -q 'ADMIN' || FAILS+=("no SUSPENDED/ADMIN line after the BRIDGE_HALT clear")
    echo "$after" | tail -1 | grep -q 'BRIDGE_HALT' && FAILS+=("BRIDGE_HALT still set after the reach-clear")
    [ "$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")" = 0 ] || FAILS+=("auto reopen counted while ADMIN holds")
    t1core probe-create 1 1 > "$D/ev/probe-create-after.txt" 2>&1
    grep -q 'OrderReject GATE_CLOSED' "$D/ev/probe-create-after.txt" || FAILS+=("probe-create after: $(cat "$D/ev/probe-create-after.txt")")
    t1core open > "$D/ev/open.txt" 2>&1; grep -q 'result=ACCEPTED' "$D/ev/open.txt" || FAILS+=("open: $(cat "$D/ev/open.txt")")
    run_story --from $((HR + 1)) --timeout 120000 > "$D/ev/run-end.txt" 2>&1 || FAILS+=("rounds $((HR + 1))..")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    gate_watch_stop
    oracle_ok final || FAILS+=("final oracle: $(head -1 "$D/ev/oracle-final.txt")")
    common_checks
    finish "$drill" "$(verdict)"
}

drill_rk11_regen() { # binary drill
    local binary=$1 drill=$2
    setup "$drill" 2 1 $((SEED_BASE + 1520)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    start_bridge bridge0 external "$binary" || FAILS+=("start")
    wait_registered bridge0 30 || FAILS+=("registered")
    run_story --to 5 --timeout 60000 > "$D/ev/run0.txt" 2>&1 || FAILS+=("rounds 0..5")
    jctl 120 REGENESIS all 777001 >> "$D/ev/jctl.txt"
    for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
    sleep 3
    [ "$(bm0 bridge_halt_reason)" = "2" ] || FAILS+=("halt reason $(bm0 bridge_halt_reason) (want 2)")
    local c0; c0=$(bm0 bridge_core_cursor); sleep "${IT_RK11_FLAT_S:-120}"
    [ "$(bm0 bridge_core_cursor)" = "$c0" ] || FAILS+=("cursor moved while EPOCH_MISMATCH")
    ( while :; do echo "$(date +%s.%N) latch=$(ae_metric assets_feed_latch_reason)"; sleep 0.5; done ) > "$D/ev/latch-0.5s.txt" 2>&1 &
    PID[latchw]=$!
    local f0; f0=$(( $(bm0 'bridge_fences_total{trigger="report_epoch_changed"}') + $(bm0 'bridge_fences_total{trigger="report_cursor_regressed"}') ))
    local k68; k68=$(bm0 'bridge_forwarded_records_total{kind="68"}')
    # the runbook (4.1 rule 6): the ME archives first (whole catalogs: KILL, remove, REVIVE), FRESH-only re-genesis
    jctl 30 KILL all >> "$D/ev/jctl.txt"; rm -rf "$D"/journal/src*; jctl 60 REVIVE all >> "$D/ev/jctl.txt"
    jctl 120 REGENESIS all 777001 --fresh-only >> "$D/ev/jctl.txt"
    jctl 30 LIST all > "$D/ev/list-after-regen.txt"
    NOTES+=("LIST after the fresh-only re-genesis: $(cat "$D/ev/list-after-regen.txt")")
    # then the core
    kill -TERM "${PID[ae0]}"; wait_exit ae0 30; rm -rf "$D/ae0" "$SHM/ae0"; start_ae 0; wait_leader || FAILS+=("no leader")
    local tl0; tl0=$(now_s)
    t1core genesis --story "$STORY" --genesis-id 777001 > "$D/ev/genesis2.txt" 2>&1 || FAILS+=("genesis 2: $(tail -1 "$D/ev/genesis2.txt")")
    t1core await-bound 30000 --genesis-id 777001 > "$D/ev/await-bound.txt" 2>&1 || FAILS+=("await-bound: $(cat "$D/ev/await-bound.txt")")
    NOTES+=("bound $(elapsed "$tl0" "$(now_s)") s after the core genesis")
    sleep 2
    kill "${PID[latchw]}" 2>/dev/null; wait "${PID[latchw]}" 2>/dev/null; unset "PID[latchw]"
    local f1; f1=$(( $(bm0 'bridge_fences_total{trigger="report_epoch_changed"}') + $(bm0 'bridge_fences_total{trigger="report_cursor_regressed"}') ))
    [ "$f1" -gt "$f0" ] || FAILS+=("no report_epoch_changed / report_cursor_regressed fence")
    [ "$(bm0 bridge_halted)" = "0" ] || FAILS+=("the bridge stays HALTED after the dual genesis")
    [ "$(bm0 'bridge_halt_recovered_total{reason="EPOCH_MISMATCH"}')" = 1 ] || FAILS+=("recovered EPOCH_MISMATCH $(bm0 'bridge_halt_recovered_total{reason="EPOCH_MISMATCH"}')")
    [ "$(bm0 'bridge_forwarded_records_total{kind="68"}')" -gt "$k68" ] || FAILS+=("FRESH 68 not forwarded")
    grep -q 'latch=4' "$D/ev/latch-0.5s.txt" && FAILS+=("core latch_reason 4 (SOURCE_UNBOUND)")
    t1core open > "$D/ev/open.txt" 2>&1; grep -q 'result=ACCEPTED' "$D/ev/open.txt" || FAILS+=("open: $(cat "$D/ev/open.txt")")
    run_story --from 0 --genesis 777001 --timeout 120000 > "$D/ev/run-epoch2.txt" 2>&1 || FAILS+=("epoch 2 rounds: $(tail -1 "$D/ev/run-epoch2.txt")")
    oracle_ok epoch2 --genesis 777001 || FAILS+=("oracle --genesis 777001: $(head -1 "$D/ev/oracle-epoch2.txt")")
    [ "$(bm0 bridge_client_sessions_opened_total)" = 2 ] || NOTES+=("sessions opened $(bm0 bridge_client_sessions_opened_total) (want 2)")
    NOTES+=("fault lines during the wipe (expected, NOTE S2-1): $(grep -c 'ARCHIVE_GONE\|UNREACHABLE' "$D/ev/bridge0.log")")
    common_checks
    if [ "$drill" = rk11-regen-nc ]; then
        # RED ct_bridge_nc_haltedparks: the bridge stays HALTED after the dual genesis
        local named=no f; for f in "${FAILS[@]}"; do case $f in "the bridge stays HALTED"*|"await-bound"*) named=yes ;; esac; done
        NOTES+=("RED expects 'stays HALTED' / await-bound timeout: ${FAILS[*]}"); FAILS=()
        [ $named = yes ] && finish "$drill" PASS || finish "$drill" INCONCLUSIVE; return
    fi
    finish "$drill" "$(verdict)"
}

drill_rk10_fill() { # variant fill|restart|twice ; binary
    local v=$1 binary=${2:-$BRIDGE} drill=rk10-$1; [ "$binary" != "$BRIDGE" ] && drill=rk10-$1-nc
    setup "$drill" 2 1 $((SEED_BASE + 1530)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    local s1; s1=$(mid_round 4); local s2=0; S=$s1
    [ "$v" = twice ] && s2=$(mid_round 7)
    HR=4
    jctl 30 HOLE all "$s1" >> "$D/ev/jctl.txt"; [ "$s2" -gt 0 ] && jctl 30 HOLE all "$s2" >> "$D/ev/jctl.txt"
    start_bridge bridge0 external "$binary" || FAILS+=("start"); wait_registered bridge0 30 || FAILS+=("registered")
    gate_watch_start 1800 || { finish "$drill" FAIL; return; }
    REOPEN0=$(core_counter assets_gate_auto_reopen_total | sed 's/absent/0/')
    run_story --to 4 --timeout 20000 > "$D/ev/run-halt.txt" 2>&1
    for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
    GMARK=$(mono_ms)
    sleep 20 # C2: the successor comes 20 s after the halt
    local cold0; cold0=$(bm0 'bridge_memo_resumes_total{kind="cold"}')
    case $v in
        fill|twice) jctl 30 FILL 0 "$s1" >> "$D/ev/jctl.txt" ;;
        restart) jctl 30 FILL 0 "$s1" >> "$D/ev/jctl.txt"; jctl 30 RESTART 0 "$s1" >> "$D/ev/jctl.txt" ;; # W0: RESTART skips held seqs
    esac
    local tf; tf=$(now_s) resumed=""
    for _ in $(seq 1 1200); do [ "$(bm0 bridge_halted)" = "0" ] && { resumed=$(elapsed "$tf" "$(now_s)"); break; }; sleep 0.1; done
    NOTES+=("resumed ${resumed:-NOT within 120 s} after the successor; chain recordings src0 $(bm 'bridge_chain_recordings{source="0"}')")
    if [ "$binary" != "$BRIDGE" ]; then
        # RED ct_bridge_nc_nosuccessor: not resumed within 90 s, the old recording only; --upto still PASSes
        ORACLE_LATCH="L true 1" oracle_ok upto --upto $((s1 - 1)); local up=$?
        gate_watch_stop
        NOTES+=("RED: halted $(bm0 bridge_halted), --upto rc $up")
        FAILS=(); if [ -z "$resumed" ] && [ "$(bm0 bridge_halted)" = 1 ] && [ $up -eq 0 ]; then finish "$drill" PASS; else finish "$drill" INCONCLUSIVE; fi
        return
    fi
    [ -n "$resumed" ] && fle "$resumed" 90 || FAILS+=("not resumed within 90 s")
    [ "$(bm0 'bridge_halt_recovered_total{reason="GAP"}')" -ge 1 ] || FAILS+=("recovered GAP 0")
    [ "$(bm0 bridge_halt_successor_attaches_total)" -ge 1 ] || FAILS+=("no successor attach")
    run_story --from 5 --to $([ "$v" = twice ] && echo 7 || echo $((ROUNDS - 1))) --timeout 120000 > "$D/ev/run-5.txt" 2>&1
    if [ "$v" = twice ]; then
        for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
        sleep 20; jctl 30 FILL 1 "$s2" >> "$D/ev/jctl.txt"
        for _ in $(seq 1 1200); do [ "$(bm0 bridge_halted)" = "0" ] && break; sleep 0.1; done
        run_story --from 8 --timeout 120000 > "$D/ev/run-8.txt" 2>&1
    fi
    quiescent "$N" 60000 || FAILS+=("quiescence")
    gate_watch_stop
    local stepbacks; stepbacks=$(bm0 bridge_halt_stepbacks_total)
    local cold; cold=$(( $(bm0 'bridge_memo_resumes_total{kind="cold"}') - cold0 ))
    NOTES+=("stepbacks $stepbacks, cold +$cold, relists src0 $(bm 'bridge_halt_relists_total{source="0"}'), opens $(counter_delta assets_gate_auto_reopen_total "$REOPEN0"), duplicates $(core_counter assets_feed_duplicate_total)")
    case $v in
        fill) [ "$stepbacks" = 1 ] || FAILS+=("stepbacks $stepbacks (want 1)") ;;
        restart) [ "$stepbacks" = 0 ] || FAILS+=("stepbacks $stepbacks (want 0)") ;;
        twice) [ "$(bm0 'bridge_halt_recovered_total{reason="GAP"}')" = 2 ] || FAILS+=("recovered GAP $(bm0 'bridge_halt_recovered_total{reason="GAP"}') (want 2)")
               [ "$(bm0 'bridge_feed_halts_sent_total{reason="1"}')" -ge 2 ] || FAILS+=("FeedHalts GAP < 2") ;;
    esac
    local opens; opens=$(gate_after "$GMARK" | awk '$2=="OPEN"' | wc -l)
    [ "$opens" = "$([ "$v" = twice ] && echo 2 || echo 1)" ] || FAILS+=("automatic OPEN lines $opens")
    [ "$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")" = "$([ "$v" = twice ] && echo 2 || echo 1)" ] || FAILS+=("auto reopen counter +$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")")
    oracle_ok final || FAILS+=("final oracle: $(head -1 "$D/ev/oracle-final.txt")")
    common_checks
    finish "$drill" "$(verdict)"
}

drill_rk12_fill() { # binary
    local binary=${1:-$BRIDGE} drill=rk12-fill; [ "$binary" != "$BRIDGE" ] && drill=rk12-fill-nc
    setup "$drill" 2 1 $((SEED_BASE + 1540)) "$ROUNDS" || { FAILS+=("setup"); finish "$drill" FAIL; return; }
    local s; s=$(trade_seq_after "$(mid_round 4)") || { FAILS+=("no trade"); finish "$drill" FAIL; return; }
    local r=4; while [ "$(barrier_of $r)" -lt "$s" ]; do r=$((r + 1)); done
    jctl 30 CORRUPT all "$s" schema3 >> "$D/ev/jctl.txt"
    start_bridge bridge0 external "$binary" || FAILS+=("start"); wait_registered bridge0 30 || FAILS+=("registered")
    gate_watch_start 1800 || { finish "$drill" FAIL; return; }
    REOPEN0=$(core_counter assets_gate_auto_reopen_total | sed 's/absent/0/')
    AUTOCLR0=$(core_counter assets_feed_latch_auto_clear_total | sed 's/absent/0/')
    run_story --to $((r - 1)) --timeout 60000 > "$D/ev/run0.txt" 2>&1
    run_story --from "$r" --to "$r" --timeout 20000 > "$D/ev/run-halt.txt" 2>&1
    for _ in $(seq 1 900); do [ "$(bm0 bridge_halted)" = "1" ] && break; sleep 0.1; done
    sleep 5
    GMARK=$(mono_ms)
    [ "$(bm0 bridge_halt_reason)" = 3 ] || FAILS+=("halt reason $(bm0 bridge_halt_reason) (want 3)")
    # valid copies in a successor: the CORRUPT mark is per source and survives a RESTART (W0), so it is removed first
    jctl 30 CORRUPT 0 "$s" none >> "$D/ev/jctl.txt"; jctl 30 RESTART 0 "$s" >> "$D/ev/jctl.txt"
    local tf; tf=$(now_s) resumed=""
    for _ in $(seq 1 1200); do [ "$(bm0 bridge_halted)" = "0" ] && { resumed=$(elapsed "$tf" "$(now_s)"); break; }; sleep 0.1; done
    if [ "$binary" != "$BRIDGE" ]; then
        gate_watch_stop; NOTES+=("RED: halted $(bm0 bridge_halted) reason $(bm0 bridge_halt_reason) after 120 s")
        FAILS=(); if [ -z "$resumed" ] && [ "$(bm0 bridge_halt_reason)" = 3 ]; then finish "$drill" PASS; else finish "$drill" INCONCLUSIVE; fi
        return
    fi
    [ -n "$resumed" ] || FAILS+=("still halted SOURCE_SCHEMA after 120 s")
    [ "$(bm0 'bridge_halt_recovered_total{reason="SOURCE_SCHEMA"}')" = 1 ] || FAILS+=("recovered SOURCE_SCHEMA $(bm0 'bridge_halt_recovered_total{reason="SOURCE_SCHEMA"}')")
    t1core barrier "$(barrier_of "$r")" 60000 > /dev/null 2>&1; sleep 2
    [ "$(counter_delta assets_feed_latch_auto_clear_total "$AUTOCLR0")" -ge 1 ] || FAILS+=("latch auto-clear +0")
    [ "$(counter_delta assets_gate_auto_reopen_total "$REOPEN0")" = 0 ] || FAILS+=("an automatic OPEN after SOURCE_SCHEMA")
    gate_after "$GMARK" | awk '$2=="OPEN"' | grep -q . && FAILS+=("gate --watch shows an OPEN before the operator's open")
    t1core open > "$D/ev/open.txt" 2>&1; grep -q 'result=ACCEPTED' "$D/ev/open.txt" || FAILS+=("open: $(cat "$D/ev/open.txt")")
    run_story --from $((r + 1)) --timeout 120000 > "$D/ev/run-end.txt" 2>&1 || FAILS+=("rounds $((r + 1))..")
    quiescent "$N" 30000 || FAILS+=("quiescence")
    gate_watch_stop
    oracle_ok final || FAILS+=("final oracle: $(head -1 "$D/ev/oracle-final.txt")")
    NOTES+=("record $s (round $r), resumed $resumed s after the RESTART")
    common_checks
    finish "$drill" "$(verdict)"
}

for drill in "${DRILLS[@]}"; do
    log "=== $drill"
    case $drill in
        rk0-external) drill_rk0 external ;;
        rk0-embedded) drill_rk0 embedded ;;
        tmpfs-min) drill_tmpfs_min ;;
        rk1) drill_rk1 ;;
        rk1-embedded) drill_rk1_embedded ;;
        rk2-external) drill_rk2 external ;;
        rk2-embedded) drill_rk2 embedded ;;
        rk2-nc) drill_rk2_nc ;;
        rk3) drill_rk3 ;;
        rk4) drill_rk4 ;;
        rk5) drill_rk5 ;;
        rk6) drill_rk6_clean ;;
        rk6-nc) drill_rk6_nc ;;
        rk8) drill_rk8 ;;
        rk9) drill_rk9_clean ;;
        rk9-nc) drill_rk9_nc ;;
        rk10) drill_rk10 ;;
        rk11) drill_rk11 all ;;
        rk11b) drill_rk11 one ;;
        rk12) drill_rk12 all ;;
        rk12b) drill_rk12 one ;;
        rk12c) drill_rk12 poison ;;
        rk12c-both) drill_rk12 poison-both ;;
        rk13c) drill_rk13c ;;
        rk13a) drill_rk13a ;;
        rk13b) drill_rk13_tc rk13b embedded "$(bin ct_bridge_tc_driverstall)" "MediaDriver keepalive" ;;
        rk13d-external) drill_rk13_tc rk13d-external external "$(bin ct_bridge_tc_conductorstall)" "conductor heartbeat stale" ;;
        rk13d-embedded) drill_rk13_tc rk13d-embedded embedded "$(bin ct_bridge_tc_conductorstall)" "conductor heartbeat stale" ;;
        rk14-smoke-external) drill_rk14_smoke external ;;
        rk14-smoke-embedded) drill_rk14_smoke embedded ;;
        rk14-smoke-red) drill_rk14_smoke external red ;;
        rk2-stop-external) drill_rk2_stop external ;;
        rk2-stop-embedded) drill_rk2_stop embedded ;;
        rk2-stop-nc) drill_rk2_stop_nc ;;
        rk14-stop) drill_rk14_stop ;;
        rk14-lag-external) drill_rk14_lag external ;;
        rk14-lag-embedded) drill_rk14_lag embedded ;;
        rk14-restart) drill_rk14_restart restart ;;
        rk14-replace) drill_rk14_restart replace ;;
        rk14-lanehole-ahead) drill_rk14_lanehole_ahead "$BRIDGE" rk14-lanehole-ahead ;;
        rk14-lanehole-ahead-nc) drill_rk14_lanehole_ahead "$(bin ct_bridge_nc_noconfirm)" rk14-lanehole-ahead-nc ;;
        rk14-lanehole-behind) drill_rk14_lanehole_behind behind ;;
        rk14-lanehole-behind-dead) drill_rk14_lanehole_behind dead ;;
        rk14-lanehole-stale) drill_rk14_lanehole_behind stale ;;
        rk14-twoholes) drill_rk14_twoholes ;;
        rk14-lag-failover) drill_rk14_lag_failover "$BRIDGE" rk14-lag-failover ;;
        rk14-lag-failover-nc) drill_rk14_lag_failover "$(bin ct_bridge_nc_rotatepast)" rk14-lag-failover-nc ;;
        rk10b) drill_rk10b b ;;
        rk10c) drill_rk10b c ;;
        rk10b-negctl) drill_rk10b negctl ;;
        rk10-opsuspend) drill_rk10_opsuspend ;;
        rk11-regen) drill_rk11_regen "$BRIDGE" rk11-regen ;;
        rk11-regen-nc) drill_rk11_regen "$(bin ct_bridge_nc_haltedparks)" rk11-regen-nc ;;
        rk10-fill) drill_rk10_fill fill ;;
        rk10-restart) drill_rk10_fill restart ;;
        rk10-fill-twice) drill_rk10_fill twice ;;
        rk10-fill-nc) drill_rk10_fill fill "$(bin ct_bridge_nc_nosuccessor)" ;;
        rk12-fill) drill_rk12_fill ;;
        rk12-fill-nc) drill_rk12_fill "$(bin ct_bridge_nc_noschemarescan)" ;;
        *) echo "unknown drill $drill" >&2; exit 64 ;;
    esac
done
grep -cE ' (FAIL|INCONCLUSIVE) ' "$RESULTS" > /dev/null && exit 1 || exit 0
