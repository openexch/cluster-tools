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
DRILLS=("$@")
[ "${DRILLS[0]}" = all ] && DRILLS=("${ALL[@]}")
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
bm() { metric "http://127.0.0.1:$BPORT/metrics" "$1"; }
bm0() { local v; v=$(bm "$1"); echo "${v:-0}"; }
bm_sum() { # family -> the sum over its labelled samples
    curl -s --max-time 2 "http://127.0.0.1:$BPORT/metrics" 2>/dev/null |
        awk -v f="$1" 'index($1, f "{") == 1 || $1 == f { s += $2 } END { print s + 0 }'
}
scrape() { curl -s --max-time 3 "http://127.0.0.1:$BPORT/metrics" > "$D/ev/metrics-$1.prom" 2>/dev/null || true
    curl -s --max-time 3 "http://127.0.0.1:$BPORT/health" > "$D/ev/health-$1.json" 2>/dev/null || true; }
health() { curl -s --max-time 2 "http://127.0.0.1:$BPORT/health" 2>/dev/null; }
http_code() { curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$BPORT/$1"; }
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

# start_bridge <name> <mode> [binary] [extra env...]
start_bridge() {
    local name=$1 mode=$2 binary=${3:-$BRIDGE}; shift 3 2>/dev/null || shift $#
    MODE=$mode
    local driverEnv
    if [ "$mode" = external ]; then
        BRDRV=${BRDRV_OVERRIDE:-$SHM/bridge-driver}
        if [ ! -f "$BRDRV/cnc.dat" ] || ! alive brdrv; then start_driver "$BRDRV" "$D/bridge-driver.log" brdrv || return 1; fi
        driverEnv="AERON_DIR=$BRDRV"
    else
        BRDRV=${BRDRV_OVERRIDE:-$SHM/bridge-embedded}
        mkdir -p "$BRDRV"
        driverEnv="BRIDGE_DRIVER_DIR=$BRDRV"
    fi
    env -i PATH="$PATH" BRIDGE_ME_JOURNAL_ARCHIVES="$(archives)" BRIDGE_JOURNAL_CONTROL_STREAM=4010 BRIDGE_JOURNAL_STREAM=4001 \
        BRIDGE_JOURNAL_CHANNEL=aeron:ipc BRIDGE_REPLAY_STREAM=9101 BRIDGE_JOURNAL_MTU=1408 BRIDGE_LOCAL_HOST=localhost \
        BRIDGE_DRIVER_MODE="$mode" "$driverEnv" BRIDGE_DRIVER_TIMEOUT_MS=5000 BRIDGE_AE_CLUSTER_ADDRESSES="$(ae_addresses)" \
        BRIDGE_AE_PORT_BASE=$AE_BASE CLUSTER_PORT_STRIDE=100 BRIDGE_AE_EGRESS_ENDPOINT=127.0.0.1:0 \
        AE_AUTH_CREDENTIAL="$BRIDGE_KEY" BRIDGE_INSTANCE_ID=1 BRIDGE_HTTP_BIND="127.0.0.1:$BPORT" "$@" \
        "$binary" >> "$D/ev/$name.log" 2>&1 &
    PID[bridge]=$!
    for _ in $(seq 1 100); do [ "$(http_code health)" != "000" ] && return 0; alive bridge || return 1; sleep 0.1; done
    return 1
}
wait_registered() { # timeout_s
    for _ in $(seq 1 $(($1 * 10))); do [ "$(bm0 bridge_registered)" = "1" ] && return 0; sleep 0.1; done; return 1
}

# The quiescence barrier: t1-core barrier N, then two /metrics samples 1 s apart at cursor N, staging and ring empty.
quiescent() { # seq timeout_ms
    t1core barrier "$1" "$2" > "$D/ev/barrier-$1.txt" 2>&1 || return 1
    # the cursor gauge and the ring move at the next report (periodic, 10 s): two consecutive good samples, 1 s apart
    local good=0
    for _ in $(seq 1 30); do
        sleep 1
        if [ "$(bm0 bridge_core_cursor)" = "$1" ] && [ "$(bm0 bridge_staged_trades)" = "0" ] &&
            [ "$(bm0 bridge_inflight_ring_depth)" = "0" ]; then
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
common_checks() { # appends to FAILS the shared lines of every drill
    [ "$(bm0 bridge_feed_halt_rejected_total)" = "0" ] || FAILS+=("feed_halt_rejected $(bm0 bridge_feed_halt_rejected_total)")
    [ "$(bm0 bridge_aeron_errors_total)" = "0" ] || FAILS+=("aeron_errors $(bm0 bridge_aeron_errors_total)")
    [ "$(bm0 bridge_log_dropped_total)" = "0" ] || FAILS+=("log_dropped $(bm0 bridge_log_dropped_total)")
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
    t1core gate > "$D/ev/gate-resumed.json" 2>&1; grep -q SUSPENDED "$D/ev/gate-resumed.json" || FAILS+=("gate opened by itself")
    t1core open > "$D/ev/open.txt" 2>&1; grep -q 'result=ACCEPTED' "$D/ev/open.txt" || FAILS+=("open: $(cat "$D/ev/open.txt")")
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
        *) echo "unknown drill $drill" >&2; exit 64 ;;
    esac
done
grep -cE ' (FAIL|INCONCLUSIVE) ' "$RESULTS" > /dev/null && exit 1 || exit 0
