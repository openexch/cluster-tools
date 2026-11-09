#!/bin/bash
# The T1 forwarder integration run, one host, serial. Starts:
#   - the C media driver built from the verified Aeron tree (shared by the forwarders and the drivers),
#   - a 3-node AE (assets-cluster.jar at b05b8e8, the dispatch-anomaly amendments included),
#   - a 3-node ME (match-cluster.jar at 68a9375 + the epoch hello; config mode, behaviour 2),
#   - fwdA / fwdB (this build's ct-forwarder; the never-published ct_forwarder_nc_noresync for the negative control),
#   - ItCoreDriver.java / ItMeDriver.java (single-file sources, one per Aeron version).
# Then genesis (ME FRESH behaviour 2 with a random genesisId; core CoreConfig 1 -> 3 bound to it, OPEN GLOBAL, deposits)
# and the scenarios below, each ending at a two-sample quiescence barrier and the delivery oracle.
#
# NO BRIDGE: the Java bridge at b05b8e8 still speaks the v5 journal, so no ME
# outcome reaches the behaviour-3 core and every minted CREATE stays pending at the core. The oracle is therefore the
# delivery half of O-F1..O-F4: every CREATE pending at the core (the tap's RequestPendingDispatch scan) rests at the ME
# exactly once (ME RequestOpenOrdersSnapshot: one leg per omsOrderId, the two sets equal), and the ME's refusal counters
# stay 0 (O-F1). The load is non-crossing LIMIT GTC buys, so every applied CREATE rests. T1-A, T1-F, T1-G and the
# "nothing pending" rows need the C++ bridge (IT_BRIDGE) and are not run without it.
#
# Usage: app/forwarder/it/run-forwarder-it.sh <cmake-build-dir> <assets-cluster.jar> <match-cluster.jar> [scenario ...]
# Scenarios (default all, in this order): clean keepalive restart sigterm core-leader me-leader rolling nc-noresync
#   fatal-aeron epoch-fence
# Env: IT_WORK (work dir, default mktemp), IT_KEEP=1 keep it, IT_RATE (commands/s, default 200), IT_SECONDS (default 10).
#
# (the gate): IT_BRIDGE=<ct-bridge> adds the C++ bridge in external mode on the runner's
# driver, its sources the three ME nodes' own journal archives (localhost:ME_BASE + 100 i + 10, stream 4001 / control
# 4010 / replay 9101, MTU 1408), key 3:BRIDGE. The genesis changes: the ME's FRESH record reaches the core through the
# bridge (ItCoreDriver genesis ... via-bridge, the await-bound shape), the ME ledger capacity is 4096 (IT_ME_LEDGER).
# The oracle then also requires the "nothing pending" row (O-F4: the tap's 65 {0, 4096} answers 0), no
# duplicate leg at the ME, and the bridge lines: bridge_halted 0, no FeedHalt, bridge_core_cursor ==
# match_engine_next_journal_seq - 1 on the ME leader (every record fed). Scenarios added: t1g (ct_forwarder_tc_corrupt:
# the INVALID carrier resolved through the ME), of2 (ct-journal-tail over each ME archive), bridge-stop (T1-A's bridge
# SIGSTOP across a forwarder SIGKILL). IT_BRIDGE_MODE=embedded runs the bridge on its own embedded driver (one T1-K run).
# Also with the bridge: O-F4's backlog and ledger-size rows in every oracle, T1-A's split of the restarted fwdA's frames
# (suppressed + first records), T1-G's core ME_INVALID terminal count, and the REDs nc-refuse (ct_forwarder_nc_refuse_tc
# on T1-G: must FAIL O-F4) and nc-norelease (ct_forwarder_nc_norelease as fwdA and fwdB on T1-F: must FAIL the backlog and
# ledger-size rows; the ledger row cannot fail, see the scenario). Both REDs leave the core polluted: run them last,
# each on its own topology. The load's users hit the core's per-user open-order cap (reject reason 32) after about 12.7k
# resting orders: a run of every scenario on one topology saturates it before nc-noresync, so split long runs.
#
# (the gate rows; all need IT_BRIDGE): IT_HARNESS=<feed-harness.jar> (the
# assets harness) runs `t1-core dump --orders` / `invariants` / `probe-cancel` against this core
# (T1_CORE_INGRESS / T1_AERON_DIR set from the runner's ingress and driver). Rows:
#   t1f (extended)  fwdA = ct_forwarder_tc_capture (FWD_CAPTURE_DIR), fwdB normal; after the barrier one captured late
#                   release frame and one captured late CREATE of a released order are re-sent once on the ME FORWARDER
#                   key 8 (ItMeDriver resend): the release is an all-NOOP frame (noop frames +1, next journal seq
#                   unchanged); the CREATE journals one record 3 RELEASED 7 (no new leg, O-F3 unchanged, bridge 64 +1,
#                   order faults unchanged, nothing pending, release backlog 0 within 10 s); every captured release entry
#                   has exactly one APPLIED record 4 entry
#   t1g (extended)  + ItCoreDriver capture / events during the load: every corrupted CREATE (orderId % 50 == 0, the
#                   tc_corrupt rule, ids from the capture map) is absent from `dump --orders` (terminal), its last
#                   OrderEvent is REJECTED reason ME_INVALID by name, its users' rows of `invariants` OK; then
#                   `probe-cancel` of one live order: ACCEPTED, CANCELLED through the ME and the bridge, gone from the dump,
#                   `invariants` PASS. nc-refuse (the RED) must FAIL the per-order line "status LIVE, hold present"
#   of5             N = 2, capture from 2 s before the load to after it, ct-journal-tail --hex over the ME leader's
#                   archive, first-apply-check.py: 0 inversions, every captured template-9 id has a record 3, count >= sent -
#                   rejects
#   of5-meleader    the same under the me-leader fault: checked outside the declared window [SIGSTOP - 2 s, both
#                   forwarders ready again + 2 s], the inside count printed; RED half (of5-meleader-nc): without the
#                   window the check must find inversions > 0 (else INCONCLUSIVE: the election truncated nothing).
#                   Non-vacuity: the SIGSTOP lands 2 s after the load's first template-9 send with the load
#                   still running, T2 resync rises on both forwarders, and first-apply-check.py names "the window holds
#                   template-9 ids" and "some ids are checked"; any of these missing FAILs the row ("FAULT: ...")
#   of5-shuffle     RED: first-apply-check.py --shuffle over of5's files (run of5 first in the same invocation): inversions > 0
#   t1-names, no cluster: ItCoreDriver names (events prints UNKNOWN_<raw> for an unnamed enum value)
set -uo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <cmake-build-dir> <assets-cluster.jar> <match-cluster.jar> [scenario ...]" >&2
    exit 64
fi
BUILD=$(cd "$1" && pwd)
AE_JAR=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
ME_JAR=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
shift 3
SCENARIOS=("$@")
[ ${#SCENARIOS[@]} -eq 0 ] && SCENARIOS=(clean keepalive restart sigterm core-leader me-leader rolling nc-noresync fatal-aeron epoch-fence)
HERE=$(cd "$(dirname "$0")" && pwd)
FWD=$(find "$BUILD" -name ct-forwarder -type f -perm -u+x | head -1)
NC_NORESYNC=$(find "$BUILD" -name ct_forwarder_nc_noresync -type f | head -1)
AERONMD=$(find "$BUILD/aeron" -name aeronmd -type f | head -1)
[ -x "$FWD" ] || { echo "FATAL: ct-forwarder not built" >&2; exit 1; }
[ -x "$AERONMD" ] || { echo "FATAL: aeronmd not built" >&2; exit 1; }
RATE=${IT_RATE:-200}
# IT_ME_MARKETS (default 1) markets configured at the ME genesis (ItMeDriver T1_ME_MARKETS); the rows run
# 5 so the "markets 2..5" lines judge configured markets that no command touches
export T1_ME_MARKETS=${IT_ME_MARKETS:-1}
SECONDS_LOAD=${IT_SECONDS:-10}

W=${IT_WORK:-$(mktemp -d -t ct-forwarder-it.XXXXXX)}
mkdir -p "$W"
SHM=$(mktemp -d /dev/shm/ct-fwd-it.XXXXXX)
declare -A PID
declare -A FWD_T0 # Each forwarder's start (now_s); O-T1 counts a forwarder started after the row start from its start
MT_ROW_T0=0
cleanup() {
    for name in "${!PID[@]}"; do
        kill -9 "${PID[$name]}" 2>/dev/null || true
    done
    # every background child of this runner, also one whose PID[] slot was overwritten by a restart
    local orphan
    for orphan in $(jobs -p); do kill -9 "$orphan" 2>/dev/null || true; done
    wait 2>/dev/null || true
    declare -F lane_c_cleanup > /dev/null && lane_c_cleanup # The tmpfs-min mounts
    rm -rf "$SHM"
    if [ "${IT_KEEP:-0}" != "1" ]; then rm -rf "$W"; else echo "work dir kept: $W"; fi
}
trap cleanup EXIT
trap 'exit 143' TERM INT HUP # a timeout or ^C still runs the EXIT trap (the children die with the runner)

hex() { printf "$1%.0s" $(seq 1 32); }
# AE keys (assets TestAuth layout, users=* on every key): one key per role; fwdA, fwdB and the tap are three
# FORWARDER keys.
AE_KEYS="1:ADMIN:$(hex 11)::users=*;2:TOOL:$(hex 22)::users=*;3:BRIDGE:$(hex 33)::users=*;5:GATEWAY:$(hex 55)::users=*;6:FORWARDER:$(hex 66)::users=*;7:FORWARDER:$(hex 77)::users=*;8:FORWARDER:$(hex 88)::users=*;9:TREASURY:$(hex 99)::users=*"
ME_KEYS="4:ADMIN:$(hex 44)::users=*;6:FORWARDER:$(hex 6a)::users=*;7:FORWARDER:$(hex 7a)::users=*;8:FORWARDER:$(hex 8a)::users=*"
export T1_ADMIN_KEY="1:ADMIN:$(hex 11)" T1_TOOL_KEY="2:TOOL:$(hex 22)" T1_BRIDGE_KEY="3:BRIDGE:$(hex 33)"
export T1_GATEWAY_KEY="5:GATEWAY:$(hex 55)" T1_TAP_KEY="8:FORWARDER:$(hex 88)" T1_ME_ADMIN_KEY="4:ADMIN:$(hex 44)"
# ItMeDriver resend opens an ME FORWARDER session on key 8, which no forwarder uses
export T1_ME_FORWARDER_KEY="8:FORWARDER:$(hex 8a)"
FWDA_AE="6:FORWARDER:$(hex 66)"; FWDA_ME="6:FORWARDER:$(hex 6a)"
FWDB_AE="7:FORWARDER:$(hex 77)"; FWDB_ME="7:FORWARDER:$(hex 7a)"

AE_BASE=22000
ME_BASE=23000
AE_INGRESS="0=localhost:$((AE_BASE + 2)),1=localhost:$((AE_BASE + 102)),2=localhost:$((AE_BASE + 202))"
ME_INGRESS="0=localhost:$((ME_BASE + 2)),1=localhost:$((ME_BASE + 102)),2=localhost:$((ME_BASE + 202))"
JAVA_OPTS=(--add-opens java.base/jdk.internal.misc=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -Xms128m -Xmx384m -XX:+UseZGC)
# a constrained host: shared driver threads, backoff idle, small terms (/dev/shm)
NODE_ENV=(TRANSPORT_IDLE_MODE=backoff TRANSPORT_DRIVER_THREADING=shared TRANSPORT_TERM_LENGTH=4m
    TRANSPORT_LOG_TERM_LENGTH=16m TRANSPORT_SNAPSHOT_TERM_LENGTH=8m)
DRIVER_DIR="$SHM/client-driver"

log() { echo "[$(date +%H:%M:%S)] $*"; }

start_driver() { # dir logfile name
    env AERON_DIR="$1" AERON_DIR_DELETE_ON_START=1 AERON_DIR_DELETE_ON_SHUTDOWN=1 "$AERONMD" > "$2" 2>&1 &
    PID[$3]=$!
    for _ in $(seq 1 100); do [ -f "$1/cnc.dat" ] && return 0; sleep 0.1; done
    echo "FATAL: media driver $3 did not start" >&2
    exit 1
}

start_ae() { # node
    local i=$1
    mkdir -p "$W/ae$i"
    env "${NODE_ENV[@]}" CLUSTER_NODE="$i" CLUSTER_ADDRESSES=localhost,localhost,localhost CLUSTER_PORT_BASE=$AE_BASE \
        CLUSTER_PORT_STRIDE=100 BASE_DIR="$W/ae$i" ASSETS_AUTH_KEYS="$AE_KEYS" ASSETS_NODE_PORT=$((AE_BASE + 90 + i)) \
        java "${JAVA_OPTS[@]}" -Daeron.dir="$SHM/ae" -jar "$AE_JAR" >> "$W/ae$i.log" 2>&1 &
    PID[ae$i]=$!
}

start_me() { # node
    local i=$1
    mkdir -p "$W/me$i"
    # With the bridge the ME journals its settlement records (schema 4) into its journal archive
    local journalEnv=()
    [ -n "${IT_BRIDGE:-}" ] && journalEnv=(SETTLEMENT_JOURNAL_ENABLED=true SETTLEMENT_JOURNAL_DIR="$W/me-journal")
    env "${NODE_ENV[@]}" "${journalEnv[@]}" CLUSTER_NODE="$i" CLUSTER_ADDRESSES=localhost,localhost,localhost CLUSTER_PORT_BASE=$ME_BASE \
        CLUSTER_PORT_STRIDE=100 BASE_DIR="$W/me$i" ME_AUTH_KEYS="$ME_KEYS" METRICS_PORT=$((ME_BASE + 90 + i)) \
        MATCH_ENGINE_FROM_CONFIG=true java "${JAVA_OPTS[@]}" -Xmx768m -Daeron.dir="$SHM/me" -jar "$ME_JAR" >> "$W/me$i.log" 2>&1 &
    PID[me$i]=$!
}

# Driver mode: IT_FWD_MODE=external (default; the runner's aeronmd, or the dir given as the 6th argument)
# or embedded (each forwarder's own driver under FWD_EMBEDDED_PARENT, ct-forwarder-<id>); FWD_INSTANCE_ID is fixed per
# name (fwdA 101, fwdB 102) so a restart keeps its identity; FWD_DRIVER_TIMEOUT_MS 5000 (the drill value).
FWD_MODE=${IT_FWD_MODE:-external}
FWD_EMBEDDED_PARENT="$SHM/fwd-embedded"
fwd_instance_id() { case "$1" in fwdA) echo 101 ;; fwdB) echo 102 ;; *) echo 109 ;; esac; }
start_fwd() { # name binary aeKey meKey httpPort [aeronDir: forces external on that driver]
    local name=$1 bin=$2 ae=$3 me=$4 port=$5 driverEnv capEnv=()
    # ct_forwarder_tc_capture reads FWD_CAPTURE_DIR (set by the caller for that start only)
    [ -n "${FWD_CAPTURE_DIR:-}" ] && capEnv=(FWD_CAPTURE_DIR="$FWD_CAPTURE_DIR")
    if [ -n "${6:-}" ] || [ "$FWD_MODE" = external ]; then driverEnv=(FWD_DRIVER_MODE=external AERON_DIR="${6:-$DRIVER_DIR}")
    else mkdir -p "${FWD_EMBEDDED_DIR_OVERRIDE:-$FWD_EMBEDDED_PARENT}"
        driverEnv=(FWD_DRIVER_MODE=embedded FWD_DRIVER_DIR="${FWD_EMBEDDED_DIR_OVERRIDE:-$FWD_EMBEDDED_PARENT}"); fi
    env -i PATH="$PATH" FWD_CORE_INGRESS="$AE_INGRESS" FWD_CORE_RESPONSE_CHANNEL="aeron:udp?endpoint=127.0.0.1:0" \
        AE_AUTH_CREDENTIAL="$ae" FWD_ME_INGRESS="$ME_INGRESS" FWD_ME_RESPONSE_CHANNEL="aeron:udp?endpoint=127.0.0.1:0" \
        ME_AUTH_CREDENTIAL="$me" FWD_INGRESS_CHANNEL="aeron:udp?term-length=4m|mtu=8192" "${driverEnv[@]}" \
        FWD_DRIVER_TIMEOUT_MS=5000 FWD_INSTANCE_ID="$(fwd_instance_id "$name")" "${capEnv[@]}" \
        FWD_HTTP_BIND="127.0.0.1:$port" taskset -c 0-11 "$bin" >> "$W/$name.log" 2>&1 &
    PID[$name]=$!
    FWD_T0[$name]=$(date +%s.%N)
}

core_driver() { java "${JAVA_OPTS[@]}" -cp "$AE_JAR" "$HERE/ItCoreDriver.java" "$@"; }
me_driver() { java "${JAVA_OPTS[@]}" -cp "$ME_JAR" "$HERE/ItMeDriver.java" "$@"; }

metric() { # url name -> value (first sample line, labels included in name match)
    curl -s --max-time 2 "$1" 2>/dev/null | awk -v n="$2" '$1 == n { print $2; exit }'
}
fwd_metric() { metric "http://127.0.0.1:$1/metrics" "$2"; }
ready() { [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$1/ready")" = "200" ]; }
wait_ready() { # port timeout_s
    for _ in $(seq 1 $(($2 * 5))); do ready "$1" && return 0; sleep 0.2; done
    return 1
}
leader_of() { # ae|me -> node index of the leader
    local kind=$1 base=$AE_BASE name=assets_cluster_role
    [ "$kind" = me ] && { base=$ME_BASE; name=match_cluster_role; }
    for i in 0 1 2; do
        if [ "$(metric "http://127.0.0.1:$((base + 90 + i))/metrics" "$name")" = "2" ]; then echo "$i"; return 0; fi
    done
    return 1
}
ae_counter() { # name -> value on the AE leader
    local l
    l=$(leader_of ae) || { echo 0; return; }
    metric "http://127.0.0.1:$((AE_BASE + 90 + l))/metrics" "$1"
}
me_counter() { # name -> value on the ME leader
    local l
    l=$(leader_of me) || { echo 0; return; }
    metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" "$1"
}
# Waits for a child to exit (a zombie counts: kill -0 still succeeds on one) and reaps it. Runs in THIS shell, never
# in $(...), so `wait` can collect the child's status. Sets WAIT_RC (255 = still running at the timeout).
WAIT_RC=0
wait_exit() { # name timeout_s
    local p=${PID[$1]:-} n=$(($2 * 10)) st
    [ -z "$p" ] && { WAIT_RC=254; return 1; }
    for _ in $(seq 1 "$n"); do
        st=$(ps -o stat= -p "$p" 2>/dev/null)
        if [ -z "$st" ] || [[ "$st" == Z* ]]; then
            wait "$p"; WAIT_RC=$?; unset "PID[$1]"; return 0
        fi
        sleep 0.1
    done
    WAIT_RC=255
    return 1
}
now_s() { date +%s.%N; }
elapsed() { python3 -I -c "import sys; print(f'{float(sys.argv[2]) - float(sys.argv[1]):.2f}')" "$1" "$2"; }

# Live ticks: IT_TICKS_LIVE=1 makes fatal-aeron and fatal-embedded run with MarketTick traffic live. Before the
# fault a 6 s load must have made fwdA offer market-1 ticks that the core applied; after the fatal exit (70) the process is
# gone and the core's applied counter stays flat for 2.5 s (the driver loss ends ticks with the process); the restarted fwdA
# offers ticks again and is ticks_ready. LIVE_OK / LIVE_TEXT carry the verdict of each step into the row's result.
TICKS_LIVE=${IT_TICKS_LIVE:-0}
LIVE_OK=1; LIVE_TEXT=""
live_note() { LIVE_TEXT="${LIVE_TEXT:+$LIVE_TEXT; }$*"; }
live_live() { # label: ticks must be live before the fault
    load_bg 6; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
    local o a tr
    o=$(fwd_sum 24001 forwarder_tick_offered_total 'market="1"'); a=$(ae_counter assets_ticks_applied_total); tr=$(fwd_metric 24001 forwarder_ticks_ready)
    LIVE_O0=${o:-0}; LIVE_A0=${a%.*}; LIVE_A0=${LIVE_A0:-0}
    if [ "$LIVE_O0" -gt 0 ] && [ "$LIVE_A0" -gt 0 ] && [ "$tr" = 1 ]; then live_note "ticks live before the fault (market-1 offered $LIVE_O0, core applied $LIVE_A0, ticks_ready 1)"
    else LIVE_OK=0; live_note "TICKS NOT LIVE before the fault (offered $LIVE_O0, core applied $LIVE_A0, ticks_ready ${tr:-?})"; fi
}
live_ended() { # the fwdA process is gone: no endpoint, core applied counter flat
    local a1 a2 up
    up=$(curl -s --max-time 1 http://127.0.0.1:24001/metrics | wc -l)
    sleep 0.5; a1=$(ae_counter assets_ticks_applied_total); sleep 2; a2=$(ae_counter assets_ticks_applied_total)
    a1=${a1%.*}; a2=${a2%.*}
    if [ "$up" = 0 ] && [ -n "$a1" ] && [ "$a1" = "$a2" ]; then live_note "ticks ended with the process (no /metrics, core applied flat $a1 -> $a2 over 2 s)"
    else LIVE_OK=0; live_note "TICKS DID NOT END (metrics lines $up, core applied $a1 -> $a2)"; fi
}
live_resumed() { # the restarted fwdA offers ticks again
    local o tr n=0
    for _ in $(seq 1 75); do
        o=$(fwd_sum 24001 forwarder_tick_offered_total 'market="1"'); tr=$(fwd_metric 24001 forwarder_ticks_ready)
        [ "${o:-0}" -gt 0 ] && [ "$tr" = 1 ] && break; sleep 0.2
    done
    if [ "${o:-0}" -gt 0 ] && [ "$tr" = 1 ]; then live_note "ticks resumed in the new process (market-1 offered $o, ticks_ready 1)"
    else LIVE_OK=0; live_note "TICKS DID NOT RESUME (offered ${o:-?}, ticks_ready ${tr:-?})"; fi
}

FAILED=()
PASSED=()
result() { # name ok detail
    if [ "$2" = ok ]; then PASSED+=("$1"); log "SCENARIO PASS $1: $3"; else FAILED+=("$1"); log "SCENARIO FAIL $1: $3"; fi
}

LOAD_TAG=0
load_bg() { # seconds -> starts the load in the background (LOAD_PID)
    LOAD_TAG=$((LOAD_TAG + 1))
    core_driver load "$DRIVER_DIR" "$AE_INGRESS" "$1" "$RATE" 50 "$LOAD_TAG" > "$W/load$LOAD_TAG.log" 2>&1 &
    LOAD_PID=$!
}

# The barrier and the delivery oracle. Prints the verdict line; returns 0 when every pending CREATE rests at the ME once.
ORACLE_DETAIL=""
oracle() { # label port...
    local label=$1; shift
    local deadline=$((SECONDS + 90))
    while [ $SECONDS -lt $deadline ]; do
        local all=1
        for p in "$@"; do
            ready "$p" || { all=0; break; }
            local sent head
            sent=$(fwd_metric "$p" 'forwarder_dispatch_seq{point="last_sent"}')
            head=$(fwd_metric "$p" 'forwarder_dispatch_seq{point="core_head"}')
            [ -n "$sent" ] && [ -n "$head" ] && [ "$sent" -ge "$head" ] || { all=0; break; }
        done
        [ $all -eq 1 ] && break
        sleep 1
    done
    local legs1 legs2
    me_driver open-orders "$DRIVER_DIR" "$ME_INGRESS" "$W/open-$label.1" > "$W/open-$label.1.log" 2>&1
    sleep 2
    me_driver open-orders "$DRIVER_DIR" "$ME_INGRESS" "$W/open-$label.txt" > "$W/open-$label.log" 2>&1
    legs1=$(wc -l < "$W/open-$label.1" 2>/dev/null || echo -1)
    legs2=$(wc -l < "$W/open-$label.txt" 2>/dev/null || echo -2)
    core_driver pending "$DRIVER_DIR" "$AE_INGRESS" "$W/pending-$label.txt" "$GENESIS" > "$W/pending-$label.log" 2>&1
    ORACLE_DETAIL=$(ORACLE_WITH_BRIDGE=$BRIDGE_BIN python3 -I - "$W/pending-$label.txt" "$W/open-$label.txt" "$legs1" "$legs2" <<'PY'
import sys
pending_path, open_path, legs1, legs2 = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
try:
    pending = [l.split() for l in open(pending_path) if l.strip()]
    legs = [l.split() for l in open(open_path) if l.strip()]
except OSError as e:
    print("FAIL oracle input missing: %s" % e); sys.exit(1)
creates = {int(p[1]) for p in pending if int(p[3]) == 0}
kinds = {int(p[3]) for p in pending}
oms = [int(l[1]) for l in legs]
dup = len(oms) - len(set(oms))
missing = sorted(creates - set(oms))
extra = sorted(set(oms) - creates)
import os
if os.environ.get("ORACLE_WITH_BRIDGE"):
    # The ME outcomes reach the core through the bridge: O-F4 nothing pending; every order rests once
    missing, extra = sorted(creates), []
ok = legs1 == legs2 and dup == 0 and not missing and not extra and kinds <= {0}
print("%s pending=%d creates=%d legs=%d stable=%s duplicateLegs=%d missing=%d%s extra=%d%s" % (
    "OK" if ok else "FAIL", len(pending), len(creates), len(oms), legs1 == legs2, dup, len(missing),
    (" e.g. %s" % missing[:5]) if missing else "", len(extra), (" e.g. %s" % extra[:5]) if extra else ""))
sys.exit(0 if ok else 1)
PY
)
    local rc=$?
    if [ -n "$BRIDGE_BIN" ]; then bridge_rows || rc=1; fi
    local refusals
    refusals="decode=$(me_counter match_ingress_decode_rejects_total) unauthorized=$(me_counter match_ingress_unauthorized_total) invalidTif=$(me_counter match_durable_invalid_tif_total) invalidSign=$(me_counter match_durable_invalid_sign_total) invalidOwner=$(me_counter match_durable_invalid_owner_id_total) capacity=$(me_counter match_durable_capacity_total) dupSuppressed=$(me_counter match_durable_duplicates_suppressed_total)"
    ORACLE_DETAIL="$ORACLE_DETAIL; ME $refusals"
    log "ORACLE $label: $ORACLE_DETAIL"
    if [ $rc -ne 0 ]; then return 1; fi
    # T1-G's corrupted commands are journalled INVALID on purpose: they are the declared exception (EXPECT_INVALID_TIF)
    case "$refusals" in
        *"decode=0 unauthorized=0 invalidTif=${EXPECT_INVALID_TIF:-0} invalidSign=0 invalidOwner=0 capacity=0 "*) return 0 ;;
        *) ORACLE_DETAIL="$ORACLE_DETAIL (O-F1 refusal counter non-zero)"; return 1 ;;
    esac
}

# ---------------------------------------------------------------- the C++ bridge
BRIDGE_BIN=${IT_BRIDGE:-}
BRIDGE_PORT=24009
JTAIL=$(find "$BUILD" -name ct-journal-tail -type f -perm -u+x | head -1)
bridge_metric() { metric "http://127.0.0.1:$BRIDGE_PORT/metrics" "$1"; }
bridge_sum() { curl -s --max-time 2 "http://127.0.0.1:$BRIDGE_PORT/metrics" 2>/dev/null |
    awk -v f="$1" 'index($1, f "{") == 1 || $1 == f { s += $2 } END { print s + 0 }'; }
start_bridge() { # [binary]
    local bin=${1:-$BRIDGE_BIN} mode=${IT_BRIDGE_MODE:-external} driverEnv
    if [ "$mode" = external ]; then driverEnv="AERON_DIR=$DRIVER_DIR"; else mkdir -p "$SHM/bridge-embedded"; driverEnv="BRIDGE_DRIVER_DIR=$SHM/bridge-embedded"; fi
    env -i PATH="$PATH" BRIDGE_ME_JOURNAL_ARCHIVES="localhost:$((ME_BASE + 10)),localhost:$((ME_BASE + 110)),localhost:$((ME_BASE + 210))" \
        BRIDGE_JOURNAL_CONTROL_STREAM=4010 BRIDGE_JOURNAL_STREAM=4001 BRIDGE_JOURNAL_CHANNEL=aeron:ipc BRIDGE_REPLAY_STREAM=9101 \
        BRIDGE_JOURNAL_MTU=1408 BRIDGE_LOCAL_HOST=localhost BRIDGE_DRIVER_MODE="$mode" "$driverEnv" BRIDGE_DRIVER_TIMEOUT_MS=5000 \
        BRIDGE_AE_CLUSTER_ADDRESSES=localhost,localhost,localhost BRIDGE_AE_PORT_BASE=$AE_BASE CLUSTER_PORT_STRIDE=100 \
        BRIDGE_AE_EGRESS_ENDPOINT=127.0.0.1:0 AE_AUTH_CREDENTIAL="$T1_BRIDGE_KEY" BRIDGE_INSTANCE_ID=1 \
        BRIDGE_HTTP_BIND="127.0.0.1:$BRIDGE_PORT" taskset -c 0-11 "$bin" >> "$W/bridge.log" 2>&1 &
    PID[bridge]=$!
}
# The bridge rows (appended to ORACLE_DETAIL; 1 when one fails).
bridge_rows() {
    [ -z "$BRIDGE_BIN" ] && return 0
    local l next cursor halts halted
    for _ in $(seq 1 60); do
        l=$(leader_of me) || { sleep 1; continue; }
        next=$(metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" match_engine_next_journal_seq)
        cursor=$(bridge_metric bridge_core_cursor)
        [ -n "$next" ] && [ -n "$cursor" ] && [ "$cursor" = "$((next - 1))" ] && break
        sleep 1
    done
    halts=$(bridge_sum bridge_feed_halts_sent_total); halted=$(bridge_metric bridge_halted)
    # O-F4: the core's release backlog drains and its model of the ME ledger equals the ME's own gauge
    local backlog aeLedger meLedger
    for _ in $(seq 1 30); do
        backlog=$(ae_counter assets_release_backlog); aeLedger=$(ae_counter assets_me_ledger_size)
        meLedger=$(me_counter match_command_ledger_size)
        [ "$backlog" = 0 ] && [ -n "$aeLedger" ] && [ "$aeLedger" = "$meLedger" ] && break
        sleep 1
    done
    ORACLE_DETAIL="$ORACLE_DETAIL; bridge cursor $cursor ME next_journal_seq $next halted $halted feedHalts $halts settleRejected $(bridge_sum bridge_settle_rejected_total) 62s $(bridge_metric 'bridge_forwarded_total{kind="62"}') 64s $(bridge_metric 'bridge_forwarded_records_total{kind="64"}') 67s $(bridge_metric 'bridge_forwarded_records_total{kind="67"}'); O-F4 release backlog $backlog ledger core $aeLedger ME $meLedger"
    [ -n "$next" ] && [ "$cursor" = "$((next - 1))" ] && [ "$halted" = 0 ] && [ "$halts" = 0 ] &&
        [ "$backlog" = 0 ] && [ -n "$aeLedger" ] && [ "$aeLedger" = "$meLedger" ]
}

# ---------------------------------------------------------------- bring-up
log "work dir $W"
start_driver "$DRIVER_DIR" "$W/aeronmd.log" driver
for i in 0 1 2; do start_ae "$i"; start_me "$i"; done
for _ in $(seq 1 120); do leader_of ae > /dev/null && leader_of me > /dev/null && break; sleep 1; done
log "AE leader $(leader_of ae), ME leader $(leader_of me)"
GENESIS=$(python3 -I -c 'import secrets; print(hex(secrets.randbits(62) | 1))')
log "genesis $GENESIS"
ME_LEDGER=100000
[ -n "$BRIDGE_BIN" ] && ME_LEDGER=${IT_ME_LEDGER:-4096}
me_driver genesis "$DRIVER_DIR" "$ME_INGRESS" "$GENESIS" "$ME_LEDGER" > "$W/me-genesis.log" 2>&1 || { cat "$W/me-genesis.log"; echo "FATAL: ME genesis"; exit 1; }
if [ -n "$BRIDGE_BIN" ]; then
    # First: one ct-journal-tail against ME node 0's archive on the T1-FB port and stream (a wrong value costs the run)
    "$JTAIL" --aeron-dir "$DRIVER_DIR" --archive "localhost:$((ME_BASE + 10))" --control-stream 4010 --journal-stream 4001 \
        --channel aeron:ipc --replay-stream 9102 --local-host localhost --journal-mtu 1408 --from-seq 1 --to-seq 1 \
        --timeout-ms 20000 > "$W/note6-journal-tail.txt" 2> "$W/note6-journal-tail.err"
    rc=$?; log "ct-journal-tail localhost:$((ME_BASE + 10)) stream 4010: exit $rc, $(head -c 200 "$W/note6-journal-tail.txt")"
    [ $rc -eq 0 ] || { tail -5 "$W/note6-journal-tail.err"; echo "FATAL: the ME journal archive is not where T1-FB expects it"; exit 1; }
    start_bridge
    core_driver genesis "$DRIVER_DIR" "$AE_INGRESS" "$GENESIS" "$ME_LEDGER" 50 via-bridge > "$W/core-genesis.log" 2>&1 || { cat "$W/core-genesis.log"; tail -20 "$W/bridge.log"; echo "FATAL: core genesis through the bridge"; exit 1; }
else
core_driver genesis "$DRIVER_DIR" "$AE_INGRESS" "$GENESIS" 100000 50 > "$W/core-genesis.log" 2>&1 || { cat "$W/core-genesis.log"; echo "FATAL: core genesis"; exit 1; }
fi
cat "$W/me-genesis.log" "$W/core-genesis.log" | grep -E "EPOCH|OPEN|DEPOSITED|CoreConfig"
start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
wait_ready 24001 60 || { tail -50 "$W/fwdA.log"; echo "FATAL: fwdA never ready"; exit 1; }
log "fwdA ready: $(curl -s http://127.0.0.1:24001/health)"

# ---------------------------------------------------------------- scenarios
run_scenario() {
    local s=$1
    log "=== $s"
    case "$s" in
    clean) # T1-K: no fault, O-F1 with zero declared exceptions
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        if oracle clean 24001; then
            local invalid; invalid=$(curl -s http://127.0.0.1:24001/metrics | awk '$1 ~ /^forwarder_invalid_dispatch_total\{/ { s += $2 } END { print s + 0 }')
            [ "$invalid" = 0 ] && result clean ok "$ORACLE_DETAIL; forwarder_invalid_dispatch_total 0" || result clean fail "invalid dispatches $invalid"
        else result clean fail "$ORACLE_DETAIL"; fi ;;
    keepalive) # T1-H: 15 s with no load: both sessions kept (no reconnect)
        local c0 m0 c1 m1
        c0=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="core"}'); m0=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="me"}')
        sleep 15
        c1=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="core"}'); m1=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="me"}')
        if [ "$c0" = "$c1" ] && [ "$m0" = "$m1" ] && ready 24001; then result keepalive ok "reconnects core $c1 me $m1 unchanged over 15 s, ready";
        else result keepalive fail "reconnects core $c0->$c1 me $m0->$m1"; fi ;;
    restart) # T1-A: SIGKILL fwdA at 5 s of load, restart; RESYNC(0) re-sends every pending command. With IT_BRIDGE the
        # bridge is SIGSTOPped first and SIGCONTed after the load (its session was closed: it reconnects or exits 70)
        local d0; d0=$(me_counter match_durable_duplicates_suppressed_total)
        [ -n "$BRIDGE_BIN" ] && kill -STOP "${PID[bridge]}"
        load_bg "$SECONDS_LOAD"; sleep 5
        kill -9 "${PID[fwdA]}"; wait_exit fwdA 5
        sleep 1
        # T1-A: nothing reaches the ME while fwdA is down (N = 1), so the journal and the suppression counter at
        # this point split what the restarted fwdA sends into first records (new ids) and suppressed duplicates
        local seqR dR; seqR=$(me_counter match_engine_next_journal_seq); dR=$(me_counter match_durable_duplicates_suppressed_total)
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
        wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        if [ -n "$BRIDGE_BIN" ]; then
            kill -CONT "${PID[bridge]}"; sleep 12
            if ! kill -0 "${PID[bridge]}" 2>/dev/null; then wait "${PID[bridge]}"; log "bridge exited $? after SIGCONT: restarted"; start_bridge;
            else log "bridge alive after SIGCONT: sessions $(bridge_metric bridge_client_sessions_opened_total)"; fi
        fi
        if oracle restart 24001; then
            local d1; d1=$(me_counter match_durable_duplicates_suppressed_total)
            local split="" splitOk=0
            if [ -n "$BRIDGE_BIN" ]; then
                local l next sent first
                l=$(leader_of me); next=$(metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" match_engine_next_journal_seq)
                sent=$(fwd_metric 24001 'forwarder_dispatch_sent_total{template="9"}') # commands; template 10 carries release entries (record 4)
                "$JTAIL" --aeron-dir "$DRIVER_DIR" --archive "localhost:$((ME_BASE + 100 * l + 10))" --control-stream 4010 \
                    --journal-stream 4001 --channel aeron:ipc --replay-stream 9130 --local-host localhost --journal-mtu 1408 \
                    --from-seq "$seqR" --to-seq $((next - 1)) --timeout-ms 120000 > "$W/t1a-me$l.txt" 2> "$W/t1a-me$l.err"
                first=$(awk '/^FRAME / && / template=3 / { n++ } END { print n + 0 }' "$W/t1a-me$l.txt")
                split="; T1-A: restarted fwdA sent $sent commands (template 9) = suppressed $((d1 - dR)) + first records $first (journal seq $seqR..$((next - 1)))"
                [ "$sent" -gt 0 ] && [ "$sent" = $((d1 - dR + first)) ] || splitOk=1
            fi
            [ "$d1" -gt "$d0" ] && [ $splitOk -eq 0 ] && result restart ok "$ORACLE_DETAIL; duplicates suppressed $d0 -> $d1 (the restart's resend)$split" \
                || result restart fail "suppressed $d0 -> $d1$split"
        else result restart fail "$ORACLE_DETAIL"; fi ;;
    sigterm) # T1-D: SIGTERM fwdA mid-load: exit 0 within 1 s; restart
        load_bg "$SECONDS_LOAD"; sleep 5
        local t0 rc took; t0=$(now_s); kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3; rc=$WAIT_RC; took=$(elapsed "$t0" "$(now_s)")
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
        wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        if [ "$rc" = 0 ] && python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) <= 1.0 else 1)" "$took" && oracle sigterm 24001; then
            result sigterm ok "exit 0 in ${took} s; $ORACLE_DETAIL";
        else result sigterm fail "exit $rc in ${took} s; $ORACLE_DETAIL"; fi ;;
    core-leader) # T1-B: SIGKILL the AE leader at 5 s; restart it after the new leader serves
        local t1a l; t1a=$(fwd_metric 24001 'forwarder_resync_total{reason="T1"}')
        load_bg "$SECONDS_LOAD"; sleep 5
        l=$(leader_of ae); log "SIGKILL AE leader $l"; kill -9 "${PID[ae$l]}"; unset "PID[ae$l]"
        for _ in $(seq 1 60); do nl=$(leader_of ae) && [ "$nl" != "$l" ] && break; sleep 0.5; done
        log "new AE leader ${nl:-none}"; sleep 2; start_ae "$l"
        wait "$LOAD_PID"; grep -E "LOAD|FAIL" "$W/load$LOAD_TAG.log"
        if oracle core-leader 24001; then
            local t1b; t1b=$(fwd_metric 24001 'forwarder_resync_total{reason="T1"}')
            [ "$t1b" -gt "$t1a" ] && result core-leader ok "T1 $t1a -> $t1b; $ORACLE_DETAIL" || result core-leader fail "no T1 ($t1a -> $t1b)"
        else result core-leader fail "$ORACLE_DETAIL"; fi ;;
    me-leader) # T1-C: SIGSTOP the ME leader 3 s under load, then SIGKILL; restart it later
        local t2a l; t2a=$(fwd_metric 24001 'forwarder_resync_total{reason="T2"}')
        load_bg "$SECONDS_LOAD"; sleep 3
        l=$(leader_of me); log "SIGSTOP ME leader $l"; kill -STOP "${PID[me$l]}"; sleep 3
        log "SIGKILL ME leader $l"; kill -9 "${PID[me$l]}"; unset "PID[me$l]"
        for _ in $(seq 1 60); do nl=$(leader_of me) && [ "$nl" != "$l" ] && break; sleep 0.5; done
        log "new ME leader ${nl:-none}"
        wait "$LOAD_PID"; grep -E "LOAD|FAIL" "$W/load$LOAD_TAG.log"; start_me "$l"
        if oracle me-leader 24001; then
            local t2b; t2b=$(fwd_metric 24001 'forwarder_resync_total{reason="T2"}')
            [ "$t2b" -gt "$t2a" ] && result me-leader ok "T2 $t2a -> $t2b; $ORACLE_DETAIL" || result me-leader fail "no T2 ($t2a -> $t2b)"
        else result me-leader fail "$ORACLE_DETAIL"; fi ;;
    rolling) # T1-E: N = 2; SIGKILL fwdA at 3 s, restart; SIGTERM fwdB at 7 s, restart
        start_fwd fwdB "$FWD" "$FWDB_AE" "$FWDB_ME" 24002; wait_ready 24002 60 || log "fwdB not ready"
        local d0; d0=$(me_counter match_durable_duplicates_suppressed_total)
        load_bg "$SECONDS_LOAD"; sleep 3
        kill -9 "${PID[fwdA]}"; wait_exit fwdA 5; start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
        sleep 4; kill -TERM "${PID[fwdB]}"; local rcb; wait_exit fwdB 3; rcb=$WAIT_RC; start_fwd fwdB "$FWD" "$FWDB_AE" "$FWDB_ME" 24002
        wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        if oracle rolling 24001 24002; then
            local d1; d1=$(me_counter match_durable_duplicates_suppressed_total)
            [ "$rcb" = 0 ] && [ "$d1" -gt "$d0" ] && result rolling ok "fwdB SIGTERM exit 0; duplicates suppressed $d0 -> $d1 (two copies of every command, one applies); $ORACLE_DETAIL" \
                || result rolling fail "fwdB exit $rcb, duplicates $d0 -> $d1"
        else result rolling fail "$ORACLE_DETAIL"; fi
        kill -TERM "${PID[fwdB]}"; wait_exit fwdB 3 ;;
    nc-noresync) # negative control: the naive relay (T1-T9 off) must FAIL the oracle on the restart scenario
        [ -x "$NC_NORESYNC" ] || { result nc-noresync fail "ct_forwarder_nc_noresync not built (-DCT_FORWARDER_TEST_BUILDS=ON)"; return; }
        load_bg "$SECONDS_LOAD"; sleep 4
        kill -9 "${PID[fwdA]}"; wait_exit fwdA 5
        sleep 2; start_fwd fwdA "$NC_NORESYNC" "$FWDA_AE" "$FWDA_ME" 24001
        wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        if oracle nc-noresync 24001; then result nc-noresync fail "the oracle did not see the loss: $ORACLE_DETAIL";
        else result nc-noresync ok "RED as required (commands minted while fwdA was down were never sent): $ORACLE_DETAIL"; fi
        # back to the real forwarder: its T1 RESYNC(0) heals the ME
        kill -9 "${PID[fwdA]}"; wait_exit fwdA 5
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
        oracle nc-healed 24001 && log "healed by the real forwarder: $ORACLE_DETAIL" || log "NOT healed: $ORACLE_DETAIL" ;;
    fatal-aeron) # T1-J: fwdA on its own driver; SIGKILL the driver: exit 70 within the 5 s driver timeout + 2 s
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3
        local dir="$SHM/fwdA-driver"; start_driver "$dir" "$W/aeronmd-fwdA.log" driverA
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001 "$dir"; wait_ready 24001 60
        LIVE_OK=1; LIVE_TEXT=""
        [ "$TICKS_LIVE" = 1 ] && { live_live fatal-aeron; load_bg 40; }
        local t0 rc took; t0=$(now_s); kill -9 "${PID[driverA]}"; wait "${PID[driverA]}" 2>/dev/null; unset "PID[driverA]"
        wait_exit fwdA 10; rc=$WAIT_RC; took=$(elapsed "$t0" "$(now_s)")
        local verdict=fail vtext="exit $rc after ${took} s"
        if [ "$rc" = 70 ] && python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) <= 7.0 else 1)" "$took"; then
            verdict=ok; vtext="driver SIGKILL -> exit 70 after ${took} s"
        fi
        [ "$TICKS_LIVE" = 1 ] && live_ended
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        if [ "$TICKS_LIVE" = 1 ]; then
            live_resumed; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
            oracle fatal-aeron 24001 && live_note "delivery oracle: $ORACLE_DETAIL" || { LIVE_OK=0; live_note "delivery oracle FAILED: $ORACLE_DETAIL"; }
            [ "$LIVE_OK" = 1 ] || verdict=fail
            vtext="$vtext; $LIVE_TEXT"
        fi
        result fatal-aeron "$verdict" "$vtext" ;;
    epoch-fence) # T1-I: re-genesis the ME under a running fwdA: 78; the restarted fwdA is REJECTED by the core: 78
        for i in 0 1 2; do kill -9 "${PID[me$i]}" 2>/dev/null; unset "PID[me$i]"; done
        sleep 1; for i in 0 1 2; do rm -rf "$W/me$i"; done; rm -rf "$SHM"/me-* "$W/me-journal" # The journal too
        for i in 0 1 2; do start_me "$i"; done
        for _ in $(seq 1 120); do leader_of me > /dev/null && break; sleep 1; done
        local g2; g2=$(python3 -I -c 'import secrets; print(hex(secrets.randbits(62) | 1))')
        me_driver genesis "$DRIVER_DIR" "$ME_INGRESS" "$g2" 100000 > "$W/me-genesis-2.log" 2>&1
        local rc1 rc2; wait_exit fwdA 30; rc1=$WAIT_RC
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
        wait_exit fwdA 60; rc2=$WAIT_RC
        grep -h "STOP\|stopped" "$W/fwdA.log" | tail -3
        if [ "$rc1" = 78 ] && [ "$rc2" = 78 ] && grep -q "F7_REJECTED" "$W/fwdA.log" && grep -q "F8_EPOCH" "$W/fwdA.log"; then
            result epoch-fence ok "re-genesis -> exit 78 (F8); restart with the new epoch -> core REJECTED -> exit 78 (F7)"
        else result epoch-fence fail "first exit $rc1, second exit $rc2"; fi ;;
    t1g) # T1-G: fwdA = ct_forwarder_tc_corrupt (N = 1): every corrupted command is journalled INVALID by the ME and
        # resolved at the core through the bridge (nothing pending); the counts agree
        local tc; tc=$(find "$BUILD" -name ct_forwarder_tc_corrupt -type f | head -1)
        [ -x "$tc" ] || { result t1g fail "ct_forwarder_tc_corrupt not built"; return; }
        local i0 b0 c0; i0=$(me_counter match_durable_invalid_tif_total); b0=$(bridge_metric 'bridge_forwarded_records_total{kind="64"}')
        c0=$(ae_counter assets_me_invalid_total)
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3
        start_fwd fwdA "$tc" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        # T1-G: the core's OrderEvents (the reject reason by name) and the dispatch map (the corrupted ids)
        gate_events_bg t1g; gate_capture_bg t1g $((SECONDS_LOAD + 10))
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        gate_capture_wait t1g # before the oracle: its `pending` tap uses the same FORWARDER key
        local inv i1 b1; inv=$(curl -s http://127.0.0.1:24001/metrics | awk '$1 ~ /^forwarder_invalid_dispatch_total\{/ { s += $2 } END { print s + 0 }')
        EXPECT_INVALID_TIF=$(( ${i0:-0} + inv ))
        local ok=0; oracle t1g 24001 || ok=1
        i1=$(me_counter match_durable_invalid_tif_total); b1=$(bridge_metric 'bridge_forwarded_records_total{kind="64"}')
        local c1; c1=$(ae_counter assets_me_invalid_total)
        local po=0; gate_per_order t1g "$W/capture-t1g/dispatch.map" "$W/events-t1g.txt" 1 || po=1
        gate_events_stop
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3; start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        if [ $ok -eq 0 ] && [ "$inv" -gt 0 ] && [ $((i1 - i0)) = "$inv" ] && [ $((b1 - b0)) -ge "$inv" ] && [ $((c1 - c0)) = "$inv" ] && [ $po -eq 0 ]; then
            result t1g ok "invalid dispatches $inv = ME invalid_tif +$((i1 - i0)) = core ME_INVALID terminals +$((c1 - c0)); bridge 64 records +$((b1 - b0)); per-order: $PO_DETAIL; $ORACLE_DETAIL"
        else result t1g fail "invalid $inv ME +$((i1 - i0)) core ME_INVALID +$((c1 - c0)) bridge 64s +$((b1 - b0)); per-order (rc $po): $PO_DETAIL; $ORACLE_DETAIL"; fi ;;
    nc-refuse) # Negative control on T1-G: the refusal preset (classes I and C discarded) over the test corruption
        # must FAIL O-F4: exactly the corrupted CREATEs stay pending at the core, nothing reaches the ME as INVALID
        local tc; tc=$(find "$BUILD" -name ct_forwarder_nc_refuse_tc -type f | head -1)
        [ -x "$tc" ] || { result nc-refuse fail "ct_forwarder_nc_refuse_tc not built"; return; }
        local i0; i0=$(me_counter match_durable_invalid_tif_total)
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3
        start_fwd fwdA "$tc" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        gate_events_bg nc-refuse; gate_capture_bg nc-refuse $((SECONDS_LOAD + 10)) # The per-order RED
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        gate_capture_wait nc-refuse
        local inv; inv=$(curl -s http://127.0.0.1:24001/metrics | awk '$1 ~ /^forwarder_invalid_dispatch_total\{/ { s += $2 } END { print s + 0 }')
        local ok=0; oracle nc-refuse 24001 || ok=1
        local i1 pc; i1=$(me_counter match_durable_invalid_tif_total)
        pc=$(awk '$4 == 0 { n++ } END { print n + 0 }' "$W/pending-nc-refuse.txt" 2>/dev/null)
        # the T1-G per-order row must FAIL on its named line (the refused CREATEs stay live, their holds kept)
        local po=0; gate_per_order nc-refuse "$W/capture-nc-refuse/dispatch.map" "$W/events-nc-refuse.txt" 0 || po=1
        gate_events_stop
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3; start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        if [ $ok -eq 1 ] && [ "$inv" -gt 0 ] && [ "$pc" = "$inv" ] && [ "$i1" = "$i0" ] && [ $po -eq 1 ] &&
            [[ "$PO_DETAIL" == *"status LIVE, hold present"* ]]; then
            result nc-refuse ok "RED as required: O-F4 FAIL, pending CREATEs $pc = refused corrupted dispatches $inv, ME invalid_tif unchanged; the T1-G per-order line FAILS: $PO_DETAIL; $ORACLE_DETAIL"
        else result nc-refuse fail "oracle rc $ok pending CREATEs $pc refused $inv ME invalid $i0 -> $i1; per-order rc $po (must FAIL on 'status LIVE, hold present'): $PO_DETAIL; $ORACLE_DETAIL"; fi ;;
    nc-norelease) # Negative control on T1-F: both forwarders discard every RELEASE_ID entry; must FAIL O-F4's
        # backlog row and leave the releases pending at the core. The ledger-size row is recorded, not required to fail
        # (measured 2026-10-31: a release that never reaches the ME is applied on neither side, so the core's model and
        # the ME's ledger grow together, 2000 = 2000). Then the real forwarders relay the backlog (healed)
        local nc; nc=$(find "$BUILD" -name ct_forwarder_nc_norelease -type f | head -1)
        [ -x "$nc" ] || { result nc-norelease fail "ct_forwarder_nc_norelease not built"; return; }
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3
        start_fwd fwdA "$nc" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        start_fwd fwdB "$nc" "$FWDB_AE" "$FWDB_ME" 24002; wait_ready 24002 60
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        local ok=0; oracle nc-norelease 24001 24002 || ok=1
        local backlog aeL meL; backlog=$(ae_counter assets_release_backlog); aeL=$(ae_counter assets_me_ledger_size); meL=$(me_counter match_command_ledger_size)
        kill -TERM "${PID[fwdA]}" "${PID[fwdB]}"; wait_exit fwdA 3; wait_exit fwdB 3
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        local pr; pr=$(awk '$4 != 0 { n++ } END { print n + 0 }' "$W/pending-nc-norelease.txt" 2>/dev/null)
        if [ $ok -eq 1 ] && [ "${backlog:-0}" -gt 0 ] && [ "${pr:-0}" -gt 0 ]; then
            result nc-norelease ok "RED as required: the T1-F line 'assets_release_backlog 0 within 10 s' FAILS: release backlog $backlog, releases pending at the core $pr (ledger core $aeL, ME $meL: both keep the unreleased entries); $ORACLE_DETAIL"
        else result nc-norelease fail "oracle rc $ok backlog $backlog pending releases $pr ledger core $aeL ME $meL; $ORACLE_DETAIL"; fi
        oracle nc-norelease-healed 24001 && log "healed by the real forwarder: $ORACLE_DETAIL" || log "NOT healed: $ORACLE_DETAIL" ;;
    t1f) # T1-F releaseRelayedAtN2: N = 2 forwarders relay every release; the ME applies each release once (exactly
        # one APPLIED entry per release in record 4, read by ct-journal-tail), the second copy is a no-op frame, the core's
        # release backlog drains, the bridge forwarded the 67s. fwdA is ct_forwarder_tc_capture, and after the
        # barrier one captured late release frame and one captured late CREATE of a released order are re-sent (t1f_resend)
        local tcc; tcc=$(find "$BUILD" -name ct_forwarder_tc_capture -type f | head -1)
        [ -x "$tcc" ] || { result t1f fail "ct_forwarder_tc_capture not built"; return; }
        local cap="$W/capture-fwdA-t1f"; rm -rf "$cap"; mkdir -p "$cap"
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3
        FWD_CAPTURE_DIR="$cap" start_fwd fwdA "$tcc" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60 || log "fwdA (tc_capture) not ready"
        start_fwd fwdB "$FWD" "$FWDB_AE" "$FWDB_ME" 24002; wait_ready 24002 60 || log "fwdB not ready"
        local n0; n0=$(me_counter match_release_noop_frames_total)
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        local ok=0; oracle t1f 24001 24002 || ok=1
        local backlog=""
        for _ in $(seq 1 20); do backlog=$(metric "http://127.0.0.1:$((AE_BASE + 90 + $(leader_of ae)))/metrics" assets_release_backlog); [ "$backlog" = 0 ] && break; sleep 0.5; done
        local n1; n1=$(me_counter match_release_noop_frames_total)
        local l next; l=$(leader_of me); next=$(metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" match_engine_next_journal_seq)
        "$JTAIL" --aeron-dir "$DRIVER_DIR" --archive "localhost:$((ME_BASE + 100 * l + 10))" --control-stream 4010 \
            --journal-stream 4001 --channel aeron:ipc --replay-stream 9120 --local-host localhost --journal-mtu 1408 \
            --from-seq 1 --to-seq $((next - 1)) --hex --timeout-ms 120000 > "$W/t1f-me$l.txt" 2> "$W/t1f-me$l.err"
        local applied; applied=$(python3 -I - "$W/t1f-me$l.txt" <<'PY'
import sys, collections
applied = collections.Counter()
entries = 0
for line in open(sys.argv[1]):
    if not line.startswith("FRAME "):
        continue
    kv = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
    if kv.get("template") != "4" or "hex" not in kv:
        continue
    b = bytes.fromhex(kv["hex"])
    g = 8 + 44
    block = int.from_bytes(b[g:g + 2], 'little'); n = int.from_bytes(b[g + 2:g + 4], 'little')
    for i in range(n):
        e = b[g + 4 + block * i:g + 4 + block * (i + 1)]
        entries += 1
        if e[17] == 0:  # result APPLIED (after commandIdHigh 8, idLowFloor 8, scope 1)
            applied[e[0:17]] += 1
twice = sum(1 for c in applied.values() if c > 1)
print("release entries %d applied %d applied-twice %d" % (entries, len(applied), twice))
sys.exit(1 if twice or not applied else 0)
PY
)
        local arc=$?
        local b67; b67=$(bridge_metric 'bridge_forwarded_records_total{kind="67"}')
        local rs=0; t1f_resend "$cap" "$W/t1f-me$l.txt" || rs=1
        kill -TERM "${PID[fwdB]}"; wait_exit fwdB 3
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3; start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        if [ $ok -eq 0 ] && [ $arc -eq 0 ] && [ "$backlog" = 0 ] && [ "$n1" -gt "$n0" ] && [ "${b67:-0}" -ge 1 ] && [ $rs -eq 0 ]; then
            result t1f ok "$applied; noop frames $n0 -> $n1; release backlog 0; bridge 67 records $b67; re-sends: $T1F_RESEND_DETAIL; $ORACLE_DETAIL"
        else result t1f fail "$applied (rc $arc); noop $n0 -> $n1; backlog $backlog; bridge 67 $b67; re-sends (rc $rs): $T1F_RESEND_DETAIL; $ORACLE_DETAIL"; fi ;;
    of2) # O-F2: the ME journal replayed through the C++ reader (ct-journal-tail --hex) over each ME archive: one
        # record 3 per command id; the ME's suppressed duplicates are not journalled twice
        local l next; l=$(leader_of me); next=$(metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" match_engine_next_journal_seq)
        local all_ok=0 detail=""
        for i in 0 1 2; do
            "$JTAIL" --aeron-dir "$DRIVER_DIR" --archive "localhost:$((ME_BASE + 100 * i + 10))" --control-stream 4010 \
                --journal-stream 4001 --channel aeron:ipc --replay-stream $((9110 + i)) --local-host localhost --journal-mtu 1408 \
                --from-seq 1 --to-seq $((next - 1)) --hex --timeout-ms 120000 > "$W/of2-me$i.txt" 2> "$W/of2-me$i.err"
            local rc=$?
            local v; v=$(python3 -I - "$W/of2-me$i.txt" <<'PY'
import sys, collections
ids = collections.Counter()
n = 0
for line in open(sys.argv[1]):
    if not line.startswith("FRAME "):
        continue
    n += 1
    kv = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
    if kv.get("template") != "3" or "hex" not in kv:
        continue
    b = bytes.fromhex(kv["hex"])
    ids[b[32:48]] += 1    # commandId (high, low) after the header and journalSeq, meEpoch, egressSeq
dups = sum(1 for c in ids.values() if c > 1)
print("records %d outcomes %d duplicated-ids %d" % (n, len(ids), dups))
sys.exit(1 if dups else 0)
PY
)
            [ $rc -eq 0 ] && [ $? -eq 0 ] || all_ok=1
            detail="$detail me$i($rc): $v;"
        done
        [ $all_ok -eq 0 ] && result of2 ok "$detail suppressed $(me_counter match_durable_duplicates_suppressed_total)" || result of2 fail "$detail" ;;
    *) { declare -F gate_scenario > /dev/null && gate_scenario "$s"; } || { declare -F lane_c_scenario > /dev/null && lane_c_scenario "$s"; } ||
        { declare -F mt_scenario > /dev/null && mt_scenario "$s"; } ||
        result "$s" fail "unknown scenario" ;;
    esac
}

# ---------------------------------------------------------------- the forwarder's driver modes
# Scenarios (dispatched from run_scenario's default arm; IT_FWD_MODE picks the mode of the whole run):
#   mode-check      8.2 rule 6: /health driverMode, forwarder_driver_mode{mode}, forwarder_info identity = FWD_INSTANCE_ID;
#                   embedded: the per-instance dir ct-forwarder-101 and its lock exist (its size recorded); external: none
#   identity        8.2 rule 3: three restarts of fwdA (SIGTERM, SIGTERM, kill -9) keep identity 101; embedded: the
#                   kill -9 leaves the dir, the restart reclaims it (forwarder_driver_dir_reclaimed_total 1)
#   fatal-embedded  8.3 (IT_FWD_MODE=embedded): kill -STOP fwdA 15 s, CONT, a load: exit 70, or a reconnect with progress
#                   (ready and last_sent advancing) within 30 s; alive without progress for 30 s = FAIL
#   tmpfs-min (IT_FWD_MODE=embedded, sudo): fwdA restarted on a tmpfs of each size in
#                   IT_TMPFS_SIZES (MB, default "128 96 64 48 32 24 16"), load + oracle per size; the minimum is the
#                   smallest size that passes with every larger size passing; a failing size records its exit code
#   storage-starved (IT_FWD_MODE=embedded, sudo): fwdA on a 48 MB tmpfs (below the 64 MB minimum: the ME ingress
#                   publication is refused for storage space): /ready names storage_starved and
#                   forwarder_storage_starved_seconds_total grows, then exit 70 within 30 s + one check of that onset, the
#                   reason in the log. storage-starved-nc: ct_forwarder_nc_storagekeep (STORAGE_STARVED_EXIT_OFF) must
#                   FAIL that rule's line "alive 45 s after the storage_starved onset"
# The external kill (`fatal-aeron`, kill -9 of fwdA's own aeronmd: exit 70 within the timeout + 2 s) is the row above.
LANE_C_MOUNT=""
lane_c_cleanup() {
    if [ -n "$LANE_C_MOUNT" ]; then sudo -n umount "$LANE_C_MOUNT" 2> /dev/null || sudo -n umount -l "$LANE_C_MOUNT" 2> /dev/null; LANE_C_MOUNT=""; fi
}
fwd_identity() { curl -s --max-time 2 "http://127.0.0.1:$1/metrics" 2> /dev/null | sed -n 's/^forwarder_info{identity="\([0-9]*\)".*/\1/p' | head -1; }
fwd_alive() { local p=${PID[$1]:-} st; [ -n "$p" ] || return 1; st=$(ps -o stat= -p "$p" 2> /dev/null); [ -n "$st" ] && [[ "$st" != Z* ]]; }
lane_c_scenario() {
    local s=$1
    case "$s" in
    mode-check)
        local m id h dirs=""
        m=$(fwd_metric 24001 "forwarder_driver_mode{mode=\"$FWD_MODE\"}"); id=$(fwd_identity 24001)
        h=$(curl -s --max-time 2 http://127.0.0.1:24001/health)
        local ok=0
        [ "$m" = 1 ] && [ "$id" = 101 ] && [[ "$h" == *"\"driverMode\":\"$FWD_MODE\""* ]] || ok=1
        if [ "$FWD_MODE" = embedded ]; then
            [ -f "$FWD_EMBEDDED_PARENT/ct-forwarder-101/cnc.dat" ] && [ -f "$FWD_EMBEDDED_PARENT/ct-forwarder-101.lock" ] || ok=1
            dirs="; embedded dir $(du -sk "$FWD_EMBEDDED_PARENT/ct-forwarder-101" 2> /dev/null | cut -f1) KB (du), files: $(ls "$FWD_EMBEDDED_PARENT/ct-forwarder-101" 2> /dev/null | tr '\n' ' ')"
        else
            [ ! -e "$FWD_EMBEDDED_PARENT/ct-forwarder-101" ] || ok=1
            dirs="; runner driver dir $(du -sk "$DRIVER_DIR" 2> /dev/null | cut -f1) KB (du, shared by every client)"
        fi
        if [ $ok -eq 0 ]; then result mode-check ok "forwarder_driver_mode{mode=\"$FWD_MODE\"} $m, identity $id, /health driverMode $FWD_MODE$dirs"
        else result mode-check fail "mode $FWD_MODE: metric '$m' identity '$id' health $h$dirs"; fi ;;
    identity)
        local i ids="" ok=0 rc reclaimed
        for i in 1 2 3; do
            if [ $i -lt 3 ]; then kill -TERM "${PID[fwdA]}"; wait_exit fwdA 5; rc=$WAIT_RC; [ "$rc" = 0 ] || ok=1
            else kill -9 "${PID[fwdA]}"; wait_exit fwdA 5; rc=kill9; fi
            [ "$FWD_MODE" = embedded ] && [ $i -eq 3 ] && { [ -f "$FWD_EMBEDDED_PARENT/ct-forwarder-101/cnc.dat" ] || ok=1; }
            start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
            wait_ready 24001 60 || ok=1
            local id; id=$(fwd_identity 24001); [ "$id" = 101 ] || ok=1
            reclaimed=$(fwd_metric 24001 forwarder_driver_dir_reclaimed_total)
            ids="$ids restart $i (stop $rc): identity $id reclaimed $reclaimed;"
        done
        local want=0; [ "$FWD_MODE" = embedded ] && want=1
        [ "$reclaimed" = "$want" ] || ok=1
        load_bg 5; wait "$LOAD_PID"
        oracle identity 24001 || ok=1
        [ $ok -eq 0 ] && result identity ok "$ids $ORACLE_DETAIL" || result identity fail "$ids (reclaimed after the kill -9 must be $want) $ORACLE_DETAIL" ;;
    fatal-embedded)
        [ "$FWD_MODE" = embedded ] || { result fatal-embedded fail "needs IT_FWD_MODE=embedded"; return 0; }
        local sent0 c0 m0 t0 outcome="" rc="" took sent c1 m1
        LIVE_OK=1; LIVE_TEXT=""; [ "$TICKS_LIVE" = 1 ] && live_live fatal-embedded
        sent0=$(fwd_metric 24001 'forwarder_dispatch_seq{point="last_sent"}')
        c0=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="core"}'); m0=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="me"}')
        log "SIGSTOP fwdA (driver and client) 15 s"
        kill -STOP "${PID[fwdA]}"; sleep 15; t0=$(now_s); kill -CONT "${PID[fwdA]}"
        load_bg "$SECONDS_LOAD"
        for _ in $(seq 1 60); do
            if ! fwd_alive fwdA; then wait_exit fwdA 2; rc=$WAIT_RC; outcome=exit; break; fi
            sent=$(fwd_metric 24001 'forwarder_dispatch_seq{point="last_sent"}')
            if ready 24001 && [ -n "$sent" ] && [ -n "$sent0" ] && [ "$sent" -gt "$sent0" ]; then outcome=progress; break; fi
            sleep 0.5
        done
        took=$(elapsed "$t0" "$(now_s)")
        wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        case "$outcome" in
        exit)
            [ "$TICKS_LIVE" = 1 ] && live_ended
            start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
            [ "$TICKS_LIVE" = 1 ] && live_resumed
            if [ "$rc" = 70 ] && [ "$LIVE_OK" = 1 ] && oracle fatal-embedded 24001; then result fatal-embedded ok "exit 70 ${took} s after CONT ($(grep -h 'exit 70\|stale\|timeout' "$W/fwdA.log" | tail -1)); restarted; $ORACLE_DETAIL${LIVE_TEXT:+; $LIVE_TEXT}"
            else result fatal-embedded fail "exit $rc ${took} s after CONT; $ORACLE_DETAIL${LIVE_TEXT:+; $LIVE_TEXT}"; fi ;;
        progress)
            c1=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="core"}'); m1=$(fwd_metric 24001 'forwarder_session_reconnect_total{cluster="me"}')
            if oracle fatal-embedded 24001; then result fatal-embedded ok "alive and progressing ${took} s after CONT (last_sent $sent0 -> $sent, reconnects core $c0 -> $c1, me $m0 -> $m1); $ORACLE_DETAIL${LIVE_TEXT:+; $LIVE_TEXT}"
            else result fatal-embedded fail "progressing but the oracle failed: $ORACLE_DETAIL"; fi ;;
        *)
            result fatal-embedded fail "FORBIDDEN: alive without progress for 30 s after CONT (ready $(ready 24001 && echo 1 || echo 0), last_sent $sent0 -> $sent)" ;;
        esac ;;
    tmpfs-min)
        [ "$FWD_MODE" = embedded ] || { result tmpfs-min fail "needs IT_FWD_MODE=embedded"; return 0; }
        sudo -n true 2> /dev/null || { result tmpfs-min fail "NOT RUN: no passwordless sudo for the tmpfs mount"; return 0; }
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 5
        local mb rows="" passing=() failing=()
        for mb in ${IT_TMPFS_SIZES:-128 96 64 48 32 24 16}; do
            local mnt="$SHM/fwd-tmpfs-$mb" ok=0 peak=0 used why=""
            mkdir -p "$mnt"
            sudo -n mount -t tmpfs -o "size=${mb}m,uid=$(id -u),gid=$(id -g),mode=0755" tmpfs "$mnt" || { rows="$rows ${mb}MB: mount failed;"; continue; }
            LANE_C_MOUNT=$mnt
            FWD_EMBEDDED_DIR_OVERRIDE="$mnt" start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001
            if wait_ready 24001 60; then
                load_bg 5
                while kill -0 "$LOAD_PID" 2> /dev/null; do
                    used=$(df -k --output=used "$mnt" | tail -1 | tr -d ' '); [ "$used" -gt "$peak" ] && peak=$used; sleep 0.5
                done
                wait "$LOAD_PID"
                oracle "tmpfs-$mb" 24001 || { ok=1; why="oracle: $ORACLE_DETAIL"; }
                used=$(df -k --output=used "$mnt" | tail -1 | tr -d ' '); [ "$used" -gt "$peak" ] && peak=$used
                fwd_alive fwdA || { ok=1; why="$why exited"; }
            else
                ok=1; why="never ready: $(grep -h -i 'error\|exit\|insufficient\|space' "$W/fwdA.log" | tail -2 | tr '\n' ' ')"
            fi
            if fwd_alive fwdA; then kill -TERM "${PID[fwdA]}"; wait_exit fwdA 5 || { kill -9 "${PID[fwdA]}"; wait_exit fwdA 5; }
            else wait_exit fwdA 1; why="$why (exited $WAIT_RC)"; fi
            sudo -n umount "$mnt" 2> /dev/null || sudo -n umount -l "$mnt"; LANE_C_MOUNT=""
            if [ $ok -eq 0 ]; then passing+=("$mb"); rows="$rows ${mb}MB PASS peak ${peak} KB;"
            else failing+=("$mb"); rows="$rows ${mb}MB FAIL ($why) peak ${peak} KB;"; fi
            log "tmpfs ${mb} MB: $([ $ok -eq 0 ] && echo PASS || echo FAIL) peak ${peak} KB $why"
        done
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        # the minimum: the smallest passing size with no failing size above it
        local min="" f highestFail=0
        for f in "${failing[@]}"; do [ "$f" -gt "$highestFail" ] && highestFail=$f; done
        for mb in "${passing[@]}"; do [ "$mb" -gt "$highestFail" ] && { [ -z "$min" ] || [ "$mb" -lt "$min" ]; } && min=$mb; done
        if [ -n "$min" ]; then result tmpfs-min ok "minimum ${min} MB (largest failing ${highestFail} MB);$rows"
        else result tmpfs-min fail "no size passed;$rows"; fi ;;
    storage-starved|storage-starved-nc)
        [ "$FWD_MODE" = embedded ] || { result "$s" fail "needs IT_FWD_MODE=embedded"; return 0; }
        sudo -n true 2> /dev/null || { result "$s" fail "NOT RUN: no passwordless sudo for the tmpfs mount"; return 0; }
        local bin=$FWD
        if [ "$s" = storage-starved-nc ]; then bin=$(find "$BUILD" -name ct_forwarder_nc_storagekeep -type f | head -1)
            [ -x "$bin" ] || { result "$s" fail "ct_forwarder_nc_storagekeep not built"; return 0; }; fi
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 5
        local mnt="$SHM/fwd-tmpfs-starved" onset="" seconds="" took rc ok=0 detail
        mkdir -p "$mnt"
        sudo -n mount -t tmpfs -o "size=48m,uid=$(id -u),gid=$(id -g),mode=0755" tmpfs "$mnt" || { result "$s" fail "mount failed"; return 0; }
        LANE_C_MOUNT=$mnt
        FWD_EMBEDDED_DIR_OVERRIDE="$mnt" start_fwd fwdA "$bin" "$FWDA_AE" "$FWDA_ME" 24001
        for _ in $(seq 1 600); do
            if curl -s --max-time 1 http://127.0.0.1:24001/ready 2> /dev/null | grep -- storage_starved > /dev/null; then onset=$(now_s); break; fi
            fwd_alive fwdA || break
            sleep 0.1
        done
        if [ -z "$onset" ]; then
            ok=1; detail="no storage_starved on /ready within 60 s"
        elif [ "$s" = storage-starved ]; then
            sleep 2; seconds=$(fwd_metric 24001 forwarder_storage_starved_seconds_total)
            wait_exit fwdA 40; rc=$WAIT_RC; took=$(elapsed "$onset" "$(now_s)")
            [ "$rc" = 70 ] || ok=1
            python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) <= 31.0 else 1)" "$took" || ok=1
            [ -n "$seconds" ] && [ "$seconds" -ge 1 ] && [ "$seconds" -le 3 ] || ok=1 # wall seconds, 2 s after the onset
            grep -q F_STORAGE_STARVED "$W/fwdA.log" && grep -q "insufficient storage space" "$W/fwdA.log" || ok=1
            detail="48 MB: exit $rc ${took} s after the storage_starved onset (bound 30 s + one check); forwarder_storage_starved_seconds_total $seconds 2 s in; $(grep -h -o 'STOP F_STORAGE_STARVED.\{0,170\}' "$W/fwdA.log" | tail -1)"
        else
            sleep 45
            if fwd_alive fwdA; then detail="RED as required: the rule's line FAILS: alive 45 s after the storage_starved onset (forwarder_storage_starved_seconds_total $(fwd_metric 24001 forwarder_storage_starved_seconds_total))"
                kill -TERM "${PID[fwdA]}"; wait_exit fwdA 5 || { kill -9 "${PID[fwdA]}"; wait_exit fwdA 5; }
            else wait_exit fwdA 1; ok=1; detail="the nc build exited $WAIT_RC: the seam did not hold the process (INCONCLUSIVE RED)"; fi
        fi
        fwd_alive fwdA && { kill -9 "${PID[fwdA]}"; wait_exit fwdA 5; }
        sudo -n umount "$mnt" 2> /dev/null || sudo -n umount -l "$mnt"; LANE_C_MOUNT=""
        start_fwd fwdA "$FWD" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60
        [ $ok -eq 0 ] && result "$s" ok "$detail" || result "$s" fail "$detail" ;;
    *) return 1 ;;
    esac
    return 0
}

# ---------------------------------------------------------------- the gate rows
# Helpers for the extended t1f / t1g / nc-refuse arms above and the of5 rows below (dispatched from run_scenario's default
# arm). The harness (IT_HARNESS) runs against this core with the runner's own keys.
HARNESS=${IT_HARNESS:-}
[ -n "$HARNESS" ] && HARNESS=$(cd "$(dirname "$HARNESS")" && pwd)/$(basename "$HARNESS")
T1ENV=(T1_CORE_INGRESS="$AE_INGRESS" T1_AERON_DIR="$DRIVER_DIR" T1_ADMIN_KEY="$T1_ADMIN_KEY" T1_TOOL_KEY="$T1_TOOL_KEY"
    T1_GATEWAY_KEY="$T1_GATEWAY_KEY" T1_TREASURY_KEY="9:TREASURY:$(hex 99)" T1_FORWARDER_KEY="$T1_TAP_KEY")
t1core() { env "${T1ENV[@]}" java "${JAVA_OPTS[@]}" -Xmx1g -jar "$HARNESS" t1-core "$@" < /dev/null | sed '/^feed-harness /d'; }
now_ms() { date +%s%3N; }

# ItCoreDriver capture in the background (the JVM itself, so the PID is the JVM's); returns once it is registered.
gate_capture_bg() { # label seconds
    local dir="$W/capture-$1"
    rm -rf "$dir"; mkdir -p "$dir"
    java "${JAVA_OPTS[@]}" -cp "$AE_JAR" "$HERE/ItCoreDriver.java" capture "$DRIVER_DIR" "$AE_INGRESS" "$dir" "$2" "$GENESIS" \
        > "$dir/capture.log" 2>&1 &
    PID[capture-$1]=$!
    for _ in $(seq 1 600); do
        grep -q "^CAPTURE armed" "$dir/capture.log" 2> /dev/null && return 0
        kill -0 "${PID[capture-$1]}" 2> /dev/null || break
        sleep 0.1
    done
    log "capture $1 not armed: $(tail -3 "$dir/capture.log" | tr '\n' ' ')"
    return 1
}
GATE_CAPTURE_RC=0
GATE_CAPTURE_LINE=""
gate_capture_wait() { # label -> GATE_CAPTURE_RC (0 dense, 1 gap or failure), GATE_CAPTURE_LINE
    wait_exit "capture-$1" 180; GATE_CAPTURE_RC=$WAIT_RC
    GATE_CAPTURE_LINE=$(grep -h -E "^CAPTURE (count|GAP)|^DRIVER FAIL" "$W/capture-$1/capture.log" 2> /dev/null | tr '\n' ' ')
    log "capture $1: exit $GATE_CAPTURE_RC $GATE_CAPTURE_LINE"
}
# ItCoreDriver events (GATEWAY OrderEvent recorder) until gate_events_stop.
gate_events_bg() { # label
    rm -f "$W/events-$1.txt"
    java "${JAVA_OPTS[@]}" -cp "$AE_JAR" "$HERE/ItCoreDriver.java" events "$DRIVER_DIR" "$AE_INGRESS" "$W/events-$1.txt" 0 \
        > "$W/events-$1.log" 2>&1 &
    PID[events]=$!
    for _ in $(seq 1 600); do
        grep -q "^EVENTS armed" "$W/events-$1.log" 2> /dev/null && return 0
        kill -0 "${PID[events]}" 2> /dev/null || break
        sleep 0.1
    done
    log "events $1 not armed: $(tail -3 "$W/events-$1.log" | tr '\n' ' ')"
    return 1
}
gate_events_stop() {
    [ -n "${PID[events]:-}" ] || return 0
    kill -TERM "${PID[events]}" 2> /dev/null; wait_exit events 10 || { kill -9 "${PID[events]}" 2> /dev/null; wait_exit events 3; }
}
# ct-journal-tail --hex over the ME leader's archive, seq from..next-1 (GATE_JNEXT = the leader's next journal seq).
GATE_JNEXT=""
gate_journal() { # outFile replayStream fromSeq
    local l; l=$(leader_of me) || { GATE_JNEXT=""; return 1; }
    GATE_JNEXT=$(metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" match_engine_next_journal_seq)
    [ -n "$GATE_JNEXT" ] && [ "$GATE_JNEXT" -gt "$3" ] || { : > "$1"; return 0; }
    "$JTAIL" --aeron-dir "$DRIVER_DIR" --archive "localhost:$((ME_BASE + 100 * l + 10))" --control-stream 4010 \
        --journal-stream 4001 --channel aeron:ipc --replay-stream "$2" --local-host localhost --journal-mtu 1408 \
        --from-seq "$3" --to-seq $((GATE_JNEXT - 1)) --hex --timeout-ms 120000 > "$1" 2> "$1.err"
}
# the bridge fed every ME record: bridge_core_cursor == next journal seq - 1 (and the seq moved past `from` when given)
gate_bridge_caught_up() { # [fromSeq]
    local l next cursor
    for _ in $(seq 1 30); do
        l=$(leader_of me) && next=$(metric "http://127.0.0.1:$((ME_BASE + 90 + l))/metrics" match_engine_next_journal_seq)
        cursor=$(bridge_metric bridge_core_cursor)
        [ -n "${next:-}" ] && [ -n "$cursor" ] && [ "$cursor" = "$((next - 1))" ] && [ "$next" -gt "${1:-0}" ] && return 0
        sleep 1
    done
    return 1
}

# 7.2 T1-F re-sends (after the t1f barrier): the picker over fwdA's capture and the ME journal, then the late release and
# the late CREATE on the ME FORWARDER key. Sets T1F_RESEND_DETAIL; 0 when every line passes.
T1F_RESEND_DETAIL=""
t1f_resend() { # captureDir journalHex(seq 1..)
    local cap=$1 jr=$2 picks prc
    picks=$(python3 -I - "$cap" "$jr" <<'PY'
import collections, hashlib, os, sys
cap, jr = sys.argv[1], sys.argv[2]
le = lambda b, o, w: int.from_bytes(b[o:o + w], "little", signed=True)
side = []
try:
    for line in open(os.path.join(cap, "sidecar.txt")):
        p = line.split()
        if len(p) != 7:
            print("SIDECAR malformed line %r" % line.strip()); sys.exit(1)
        side.append((int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5]), p[6]))
except OSError as e:
    print("SIDECAR unreadable: %s" % e); sys.exit(1)
frames, bad = {}, []
for n, t, h, l, k, seq, sha in side:
    b = open(os.path.join(cap, "%d.bin" % n), "rb").read()
    frames[n] = b
    if hashlib.sha256(b).hexdigest() != sha:
        bad.append(n)
first3 = {}                                  # (high, low) -> result of the first record 3
applied4 = collections.Counter()             # (high, floor, scope) -> APPLIED entries in record 4
by_high = collections.defaultdict(list)
for line in open(jr):
    if not line.startswith("FRAME "):
        continue
    kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
    if "hex" not in kv:
        continue
    b = bytes.fromhex(kv["hex"])
    if kv.get("template") == "3" and len(b) >= 161:
        first3.setdefault((le(b, 32, 8), le(b, 40, 8)), le(b, 136, 4))
    elif kv.get("template") == "4":
        g = 8 + 44
        block = int.from_bytes(b[g:g + 2], "little"); cnt = int.from_bytes(b[g + 2:g + 4], "little")
        for i in range(cnt):
            e = b[g + 4 + block * i:g + 4 + block * (i + 1)]
            if e[17] == 0:  # APPLIED (after commandIdHigh 8, idLowFloor 8, scope 1)
                key = (le(e, 0, 8), le(e, 8, 8), e[16])
                applied4[key] += 1
                by_high[key[0]].append((key[1], key[2]))
def entries10(b):  # header 8, releaseSeq 8, group header 4 at 16, entries of the group block from 20
    block = int.from_bytes(b[16:18], "little"); cnt = int.from_bytes(b[18:20], "little")
    return [(le(b, 20 + block * i, 8), le(b, 28 + block * i, 8), b[36 + block * i]) for i in range(cnt)]
def released(h, l):  # an APPLIED release covers (h, l): UP_TO with floor >= l, or ORDER
    return any((sc == 0 and fl >= l) or sc == 1 for fl, sc in by_high.get(h, []))
create, rel, entries = None, None, set()
t9 = t10 = 0
for n, t, h, l, k, seq, sha in side:
    if t == 9:
        t9 += 1
        if create is None and k == 0 and first3.get((h, l)) == 0 and released(h, l):
            create = (n, h, l, le(frames[n], 8 + 24, 8))
    elif t == 10:
        t10 += 1
        es = entries10(frames[n])
        entries.update(es)
        if es and all(applied4[e] >= 1 for e in es):
            rel = (n, seq, len(es))  # the last fully applied one: its re-send is all NOOP
once = sum(1 for e in entries if applied4[e] == 1)
none = [e for e in entries if applied4[e] == 0]
twice = [e for e in entries if applied4[e] > 1]
print("CAPTURE fwdA frames template9=%d template10=%d sha256-mismatch=%d" % (t9, t10, len(bad)))
print("RELEASES captured entries=%d with-one-APPLIED=%d none=%d twice=%d%s" % (len(entries), once, len(none), len(twice),
      (" e.g. none %s" % none[:3]) if none else ""))
if create:
    print("PICK create n=%d id=%d/%d oms=%d" % create)
if rel:
    print("PICK release n=%d releaseSeq=%d entries=%d" % rel)
sys.exit(0 if not bad and create and rel and entries and not none and not twice else 1)
PY
)
    prc=$?
    T1F_RESEND_DETAIL=$(tr '\n' ' ' <<< "$picks")
    local cn ch cl crel
    cn=$(sed -n 's/^PICK create n=\([0-9]*\) .*/\1/p' <<< "$picks")
    ch=$(sed -n 's/^PICK create n=[0-9]* id=\(-\{0,1\}[0-9]*\)\/.*/\1/p' <<< "$picks")
    cl=$(sed -n 's/^PICK create n=[0-9]* id=[-0-9]*\/\(-\{0,1\}[0-9]*\) .*/\1/p' <<< "$picks")
    crel=$(sed -n 's/^PICK release n=\([0-9]*\) .*/\1/p' <<< "$picks")
    if [ $prc -ne 0 ] || [ -z "$cn" ] || [ -z "$crel" ]; then T1F_RESEND_DETAIL="picker FAIL: $T1F_RESEND_DETAIL"; return 1; fi
    local ok=0 rc sha want
    # the late release: an all-NOOP frame journals nothing and consumes no journalSeq (the NOOP journal rule)
    local s0 s1 n0 n1
    s0=$(me_counter match_engine_next_journal_seq); n0=$(me_counter match_release_noop_frames_total)
    me_driver resend "$DRIVER_DIR" "$ME_INGRESS" "$cap/$crel.bin" 1 > "$W/t1f-resend-release.log" 2>&1; rc=$?
    sha=$(sed -n 's/^FRAME .* sha256=\([0-9a-f]*\)$/\1/p' "$W/t1f-resend-release.log"); want=$(awk -v n="$crel" '$1 == n { print $7 }' "$cap/sidecar.txt")
    sleep 1
    s1=$(me_counter match_engine_next_journal_seq); n1=$(me_counter match_release_noop_frames_total)
    local relLine="late release $crel.bin (exit $rc, $(grep -h '^HELLO\|^REPLY\|^HELLO REFUSED' "$W/t1f-resend-release.log" | tr '\n' ' ')): match_release_noop_frames_total $n0 -> $n1 (+1), match_engine_next_journal_seq $s0 -> $s1 (unchanged), sha256 $([ -n "$sha" ] && [ "$sha" = "$want" ] && echo pinned || echo "MISMATCH $sha != $want")"
    [ "$rc" = 0 ] && [ -n "$sha" ] && [ "$sha" = "$want" ] && [ -n "$n0" ] && [ "$n1" = $((n0 + 1)) ] && [ -n "$s0" ] && [ "$s1" = "$s0" ] || ok=1
    # the late CREATE of a released order: B2 RELEASED, one record 3 with result 7, no leg
    me_driver open-orders "$DRIVER_DIR" "$ME_INGRESS" "$W/t1f-open-before.txt" > /dev/null 2>&1
    local b0 b1 f0 f1 pend backlog=""
    b0=$(bridge_metric 'bridge_forwarded_records_total{kind="64"}'); f0=$(ae_counter assets_order_faults_total)
    s0=$(me_counter match_engine_next_journal_seq)
    me_driver resend "$DRIVER_DIR" "$ME_INGRESS" "$cap/$cn.bin" 1 > "$W/t1f-resend-create.log" 2>&1; rc=$?
    sha=$(sed -n 's/^FRAME .* sha256=\([0-9a-f]*\)$/\1/p' "$W/t1f-resend-create.log"); want=$(awk -v n="$cn" '$1 == n { print $7 }' "$cap/sidecar.txt")
    gate_bridge_caught_up "$s0" || log "t1f: the bridge did not feed the late CREATE's record within 30 s"
    gate_journal "$W/t1f-late-create.txt" 9140 "$s0"
    local late; late=$(python3 -I - "$W/t1f-late-create.txt" "$ch" "$cl" <<'PY'
import sys
path, h, l = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
le = lambda b, o, w: int.from_bytes(b[o:o + w], "little", signed=True)
mine, other = [], 0
for line in open(path):
    if not line.startswith("FRAME "):
        continue
    kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
    if "hex" not in kv:
        continue
    b = bytes.fromhex(kv["hex"])
    if kv.get("template") == "3" and (le(b, 32, 8), le(b, 40, 8)) == (h, l):
        mine.append(le(b, 136, 4))
    else:
        other += 1
print("records 3 for %d/%d after the re-send: %d results %s (other records %d)" % (h, l, len(mine), mine, other))
sys.exit(0 if mine == [7] else 1)
PY
)
    local lrc=$?
    me_driver open-orders "$DRIVER_DIR" "$ME_INGRESS" "$W/t1f-open-after.txt" > /dev/null 2>&1
    local legs="O-F3 legs unchanged"
    if ! [ -s "$W/t1f-open-before.txt" ] || ! cmp -s <(sort "$W/t1f-open-before.txt") <(sort "$W/t1f-open-after.txt"); then legs="O-F3 legs CHANGED"; ok=1; fi
    for _ in $(seq 1 20); do backlog=$(ae_counter assets_release_backlog); [ "$backlog" = 0 ] && break; sleep 0.5; done
    b1=$(bridge_metric 'bridge_forwarded_records_total{kind="64"}'); f1=$(ae_counter assets_order_faults_total)
    pend=$(ae_counter assets_pending_commands)
    local b67; b67=$(bridge_metric 'bridge_forwarded_records_total{kind="67"}')
    T1F_RESEND_DETAIL="$T1F_RESEND_DETAIL| $relLine | late CREATE $cn.bin (exit $rc, sha256 $([ -n "$sha" ] && [ "$sha" = "$want" ] && echo pinned || echo "MISMATCH $sha != $want")): $late; $legs; assets_release_backlog $backlog within 10 s; bridge {kind=\"67\"} $b67 (>= 1), {kind=\"64\"} $b0 -> $b1 (+1); assets_order_faults_total $f0 -> $f1 (unchanged); assets_pending_commands $pend"
    [ "$rc" = 0 ] && [ -n "$sha" ] && [ "$sha" = "$want" ] && [ $lrc -eq 0 ] && [ "$backlog" = 0 ] && [ "${b67:-0}" -ge 1 ] &&
        [ -n "$b0" ] && [ "$b1" = $((b0 + 1)) ] && [ -n "$f0" ] && [ "$f1" = "$f0" ] && [ "$pend" = 0 ] || ok=1
    return $ok
}

# 7.2 T1-G per-order: the corrupted CREATEs are the tc_corrupt rule's ids (kind 0, orderId % 50 == 0) in the capture map
# (fwdA's log names only the first and every 1000th per rule). Sets PO_DETAIL; 0 when every per-order line passes.
PO_DETAIL=""
gate_per_order() { # label mapFile eventsFile withCancel(0|1)
    local label=$1 map=$2 ev=$3 cancel=$4
    PO_DETAIL=""
    [ -n "$HARNESS" ] || { PO_DETAIL="NOT RUN: IT_HARNESS (the assets feed-harness jar) is not set"; return 1; }
    [ -s "$map" ] || { PO_DETAIL="no dispatch map ($map)"; return 1; }
    local ids="$W/po-$label.ids" one="$W/po-$label.one"
    awk '$4 == 0 && $2 > 0 && $2 % 50 == 0 { print $2 }' "$map" | sort -n -u > "$ids"
    local n; n=$(wc -l < "$ids")
    [ "$n" -gt 0 ] || { PO_DETAIL="no corrupted CREATE (kind 0, orderId % 50 == 0) in the capture map"; return 1; }
    # one call (lane R, <RC> 7bd21f4): `ABSENT <id>` for an id not in the store, the rest continue, then
    # `DUMP orders=<n> absent=<k> lines=<m>`
    local ok=0 id out absent=0 live=0 firstLive="" all
    all=$(t1core dump --orders "$ids" 2>&1); echo "$all" > "$W/po-$label-dump.txt"
    local summary; summary=$(grep -m1 '^DUMP orders=' <<< "$all")
    [ -n "$summary" ] || { PO_DETAIL="dump --orders gave no DUMP line: $(head -c 200 <<< "$all" | tr '\n' ' ')"; return 1; }
    [ "$(sed -n 's/^DUMP orders=\([0-9]*\) .*/\1/p' <<< "$summary")" = "$n" ] || { PO_DETAIL="dump --orders: '$summary' for $n ids"; return 1; }
    while read -r id; do
        if grep -q "^ABSENT $id\$" <<< "$all"; then
            absent=$((absent + 1))
            grep -q "^H $id " <<< "$all" && { ok=1; [ -z "$firstLive" ] && firstLive="order $id: ABSENT but an H line"; }
            continue
        fi
        out=$(grep "^[OH] $id " <<< "$all")
        live=$((live + 1)); ok=1
        if [ -z "$firstLive" ]; then
            local o h; o=$(grep "^O $id " <<< "$out" | head -1); h=$(grep "^H $id " <<< "$out" | head -1)
            if [ -n "$h" ]; then firstLive="order $id: status LIVE, hold present ($o; $h)"
            elif [ -n "$o" ]; then firstLive="order $id: status LIVE, no hold ($o)"
            else firstLive="order $id: dump --orders unreadable: $(head -c 200 <<< "$out" | tr '\n' ' ')"; fi
        fi
    done < "$ids"
    local why; why=$(python3 -I - "$ids" "$ev" <<'PY'
import sys
ids = [int(l) for l in open(sys.argv[1]) if l.strip()]
last = {}
try:
    for line in open(sys.argv[2]):
        p = line.split()
        if len(p) == 7:
            last[int(p[0])] = p
except OSError as e:
    print("events unreadable: %s" % e); sys.exit(1)
good = [i for i in ids if i in last and last[i][3] == "REJECTED" and last[i][4] == "ME_INVALID"]
bad = [i for i in ids if i not in good]
users = sorted({int(last[i][1]) for i in ids if i in last})
print("last OrderEvent REJECTED reason ME_INVALID: %d of %d%s" % (len(good), len(ids),
      (" (e.g. %s)" % ", ".join("%d %s" % (i, "/".join(last[i][2:5]) if i in last else "no event") for i in bad[:3])) if bad else ""))
print("USERS " + " ".join(str(u) for u in users))
sys.exit(0 if not bad else 1)
PY
)
    local wrc=$?; [ $wrc -eq 0 ] || ok=1
    local users; users=$(sed -n 's/^USERS //p' <<< "$why")
    why=$(grep -v '^USERS' <<< "$why")
    t1core invariants > "$W/po-$label-invariants.txt" 2>&1; local irc=$?
    local u bad=""
    for u in $users; do
        grep -q "^I $u " "$W/po-$label-invariants.txt" || bad="$bad $u(no rows)"
        grep -q "^I $u .* FAIL$" "$W/po-$label-invariants.txt" && bad="$bad $u(FAIL)"
    done
    [ $irc -eq 0 ] && [ -z "$bad" ] || ok=1
    PO_DETAIL="corrupted CREATEs $n ($summary): ABSENT from dump --orders (terminal, no O / H line) $absent, live $live${firstLive:+ (first: $firstLive)}; $why; invariants exit $irc ($(grep -h '^INVARIANTS' "$W/po-$label-invariants.txt" | head -1)) users of those orders $(wc -w <<< "$users")${bad:+ FAIL:$bad}"
    [ "$cancel" = 1 ] || return $ok
    # At T1-FB: probe-cancel of one live order, its terminal through the ME and the bridge
    local victim; victim=$(python3 -I - "$ids" "$ev" <<'PY'
import sys
ids = {int(l) for l in open(sys.argv[1]) if l.strip()}
last = {}
for line in open(sys.argv[2]):
    p = line.split()
    if len(p) == 7:
        last[int(p[0])] = p
live = [o for o, p in last.items() if o not in ids and p[3] in ("NEW", "PARTIALLY_FILLED")]
print(max(live) if live else "")
PY
)
    [ -n "$victim" ] || { PO_DETAIL="$PO_DETAIL; probe-cancel: no live order in the events"; return 1; }
    echo "$victim" > "$one"
    local before after pc r0 r1 term=""
    before=$(t1core dump --orders "$one" 2>&1)
    r0=$(bridge_sum bridge_forwarded_records_total)
    pc=$(t1core probe-cancel "$victim" 2>&1 | grep -E '^Order(Ack|Reject)' | head -1)
    for _ in $(seq 1 30); do
        term=$(awk -v o="$victim" '$1 == o { s = $4 } END { print s }' "$ev")
        [ "$term" = CANCELLED ] && break
        sleep 1
    done
    after=$(t1core dump --orders "$one" 2>&1)
    r1=$(bridge_sum bridge_forwarded_records_total)
    t1core invariants > "$W/po-$label-invariants-2.txt" 2>&1; irc=$?
    local hb ha="gone"; hb=$(grep -c "^H $victim " <<< "$before"); grep -q "^ABSENT $victim\$" <<< "$after" || ha="STILL THERE"
    PO_DETAIL="$PO_DETAIL; probe-cancel $victim: ${pc:-no reply} (H lines before $hb), last OrderEvent ${term:-none}, its O / H lines $ha, bridge records $r0 -> $r1, invariants exit $irc"
    [[ "$pc" == OrderAck* ]] && [ "$hb" -ge 1 ] && [ "$term" = CANCELLED ] && [ "$ha" = gone ] && [ "$r1" -gt "$r0" ] && [ $irc -eq 0 ] || ok=1
    return $ok
}

# 7.2 O-F5: capture across the load (fault: none | me-leader), the barrier, the ME leader's journal, first-apply-check.py.
GATE_FIRST_APPLY_OUT=""
GATE_FIRST_APPLY_RC=1
GATE_FIRST_APPLY_WINDOW=()
gate_first_apply() { # label fault
    local label=$1 fault=$2
    GATE_FIRST_APPLY_OUT=""; GATE_FIRST_APPLY_RC=1; GATE_FIRST_APPLY_WINDOW=()
    start_fwd fwdB "$FWD" "$FWDB_AE" "$FWDB_ME" 24002; wait_ready 24002 60 || log "fwdB not ready"
    gate_capture_bg "$label" $((SECONDS_LOAD + 15)) || { GATE_FIRST_APPLY_OUT="capture never armed"; kill -TERM "${PID[fwdB]}"; wait_exit fwdB 3; return 1; }
    sleep 2 # the capture runs from 2 s before the load
    load_bg "$SECONDS_LOAD"
    local ml="" wstart wend faultFails=()
    if [ "$fault" = me-leader ]; then # T1-C's fault: SIGSTOP the ME leader 3 s under load, then SIGKILL
        local a0 b0; a0=$(fwd_metric 24001 'forwarder_resync_total{reason="T2"}'); b0=$(fwd_metric 24002 'forwarder_resync_total{reason="T2"}')
        # The load JVM compiles ItCoreDriver.java first (seconds), so "3 s after load_bg" could
        # stop the ME before the first command. The fault is placed 2 s after the load's first template-9 send instead,
        # and the load must still be running when the ME leader stops (else no dispatch crosses the leader change).
        local s0 s9 loadUp=""; s0=$(fwd_metric 24001 'forwarder_dispatch_sent_total{template="9"}')
        for _ in $(seq 1 300); do
            s9=$(fwd_metric 24001 'forwarder_dispatch_sent_total{template="9"}')
            [ -n "$s9" ] && [ "$s9" -gt "${s0:-0}" ] 2> /dev/null && { loadUp=yes; break; }
            sleep 0.1
        done
        [ -n "$loadUp" ] || faultFails+=("the load never reached the ME (fwdA template-9 sent ${s0:-?} -> ${s9:-?} in 30 s)")
        sleep 2; ml=$(leader_of me); wstart=$(( $(now_ms) - 2000 ))
        kill -0 "$LOAD_PID" 2> /dev/null || faultFails+=("the load ended before the ME leader stopped (IT_SECONDS $SECONDS_LOAD)")
        log "SIGSTOP ME leader $ml"; kill -STOP "${PID[me$ml]}"; sleep 3
        log "SIGKILL ME leader $ml"; kill -9 "${PID[me$ml]}"; unset "PID[me$ml]"
        local nl="" t2="" a1 b1
        for _ in $(seq 1 60); do nl=$(leader_of me) && [ "$nl" != "$ml" ] && break; sleep 0.5; done
        for _ in $(seq 1 120); do
            a1=$(fwd_metric 24001 'forwarder_resync_total{reason="T2"}'); b1=$(fwd_metric 24002 'forwarder_resync_total{reason="T2"}')
            [ "$a1" -gt "${a0:-0}" ] 2> /dev/null && [ "$b1" -gt "${b0:-0}" ] 2> /dev/null && { t2=yes; break; }
            sleep 0.5
        done
        # A fault that no forwarder resynced from is not the O-F5 me-leader case (the row was vacuous)
        [ -n "$t2" ] || faultFails+=("T2 resync did not rise on both forwarders in 60 s (fwdA ${a0:-?} -> ${a1:-?}, fwdB ${b0:-?} -> ${b1:-?})")
        wait_ready 24001 60; wait_ready 24002 60
        wend=$(( $(now_ms) + 2000 ))
        GATE_FIRST_APPLY_WINDOW=(--window "$wstart" "$wend")
        log "new ME leader ${nl:-none}; declared window $wstart..$wend ms"
    fi
    wait "$LOAD_PID"; local loadLine; loadLine=$(grep -h "^LOAD " "$W/load$LOAD_TAG.log" | tail -1); log "$loadLine"
    [ -n "$ml" ] && start_me "$ml"
    gate_capture_wait "$label"
    local orc=0; oracle "$label" 24001 24002 || orc=1
    gate_journal "$W/$label-journal.txt" $([ "$fault" = me-leader ] && echo 9142 || echo 9141) 1; local jrc=$?
    GATE_FIRST_APPLY_OUT=$(python3 -I "$HERE/first-apply-check.py" "$W/capture-$label/dispatch.map" "$W/$label-journal.txt" --load "$loadLine" \
        "${GATE_FIRST_APPLY_WINDOW[@]}" 2>&1)
    GATE_FIRST_APPLY_RC=$?
    GATE_FIRST_APPLY_OUT="capture exit $GATE_CAPTURE_RC ($GATE_CAPTURE_LINE); journal exit $jrc (seq 1..$((GATE_JNEXT - 1))); oracle rc $orc; $(tr '\n' ' ' <<< "$GATE_FIRST_APPLY_OUT"); $ORACLE_DETAIL"
    [ ${#faultFails[@]} -eq 0 ] || GATE_FIRST_APPLY_OUT="FAULT: $(IFS=';'; echo "${faultFails[*]}"); $GATE_FIRST_APPLY_OUT"
    kill -TERM "${PID[fwdB]}"; wait_exit fwdB 3
    [ "$GATE_CAPTURE_RC" = 0 ] && [ $jrc -eq 0 ] && [ $orc -eq 0 ] && [ $GATE_FIRST_APPLY_RC -eq 0 ] && [ ${#faultFails[@]} -eq 0 ]
}

gate_scenario() {
    local s=$1
    case "$s" in
    of5)
        [ -n "$BRIDGE_BIN" ] || { result of5 fail "needs IT_BRIDGE (no outcome reaches the core without it)"; return 0; }
        if gate_first_apply of5 none; then result of5 ok "$GATE_FIRST_APPLY_OUT"; else result of5 fail "$GATE_FIRST_APPLY_OUT"; fi ;;
    of5-meleader)
        [ -n "$BRIDGE_BIN" ] || { result of5-meleader fail "needs IT_BRIDGE"; return 0; }
        if gate_first_apply of5-meleader me-leader; then result of5-meleader ok "$GATE_FIRST_APPLY_OUT"; else result of5-meleader fail "$GATE_FIRST_APPLY_OUT"; fi
        # the RED half: the same files without the declared window must FAIL "0 inversions" (the window is needed)
        local red rrc
        red=$(python3 -I "$HERE/first-apply-check.py" "$W/capture-of5-meleader/dispatch.map" "$W/of5-meleader-journal.txt" 2>&1); rrc=$?
        red=$(tr '\n' ' ' <<< "$red")
        if [ $rrc -eq 1 ] && [[ "$red" == *"OF5 FAIL: "*"0 inversions"* ]]; then result of5-meleader-nc ok "RED as required: without the window: $red"
        elif [ $rrc -eq 0 ]; then result of5-meleader-nc fail "INCONCLUSIVE: no inversion inside the window (the ME leader change truncated nothing): $red"
        else result of5-meleader-nc fail "failed for another reason (rc $rrc): $red"; fi ;;
    t1-names) # ItCoreDriver events prints UNKNOWN_<raw> for an unnamed enum value, no cluster
        local out rc; out=$(core_driver names 2>&1); rc=$?; out=$(tr '\n' ' ' <<< "$out")
        if [ $rc -eq 0 ] && [[ "$out" == *"NAMES PASS"* ]]; then result t1-names ok "$out"
        else result t1-names fail "rc $rc: $out"; fi ;;
    of5-shuffle)
        local map="$W/capture-of5/dispatch.map" jr="$W/of5-journal.txt" out rc inv
        [ -s "$map" ] && [ -s "$jr" ] || { result of5-shuffle fail "NOT RUN: run of5 first in the same invocation (no $map / $jr)"; return 0; }
        out=$(python3 -I "$HERE/first-apply-check.py" "$map" "$jr" --shuffle 2>&1); rc=$?
        out=$(tr '\n' ' ' <<< "$out"); inv=$(sed -n 's/.* inversions=\([0-9]*\) .*/\1/p' <<< "$out")
        if [ $rc -eq 1 ] && [[ "$out" == *"OF5 FAIL: "*"0 inversions"* ]] && [ "${inv:-0}" -gt 0 ]; then
            result of5-shuffle ok "RED as required: the '0 inversions' line FAILS ($inv inversions): $out"
        else result of5-shuffle fail "the shuffled map did not fail the '0 inversions' line (rc $rc): $out"; fi ;;
    *) return 1 ;;
    esac
    return 0
}

# ---------------------------------------------------------------- the MarketTick rows
# Rows (dispatched from run_scenario's default arm; one row per invocation = one fresh topology; run with
# IT_ME_MARKETS=5 so markets 2..5 are configured and never traded):
#   tk-a / tk-h   tick-clean (tk-h with IT_FWD_MODE=embedded, C13): 10 s load; O-T1; the deeper-level line (no market-1
#                 offer between second 5 and the load's end while template 25 still arrives); chain breaks 0
#   tk-b          tick-n2: fwdA + fwdB; O-T1 for both; conservation (core applied + stale == A + B offered), stale > 0,
#                 applied - max(A, B) recorded, not judged (interleaved applies)
#   tk-c          tickResumesAfterMeFailoverWithinOneFlush (IT_RATE=50 IT_SECONDS=25): SIGSTOP the ME leader 3 s at 3 s,
#                 SIGKILL, restart after the load; fwdA's "ME new leader" line -> market 1 OK with template 22 +1 <= 1.0 s;
#                 leading/trailing offers after the line; core best bid 14.99 at the end; O-T1
#   tk-c-nc       the RED half on the ed98477 ME jar: STOP L0 3 s right after a periodic 22 (so its 10 s clock is fresh),
#                 CONT; when L1 leads STOP it 3 s, CONT; RED when L0 leads again and the TK-C line FAILS, else INCONCLUSIVE
#   tk-d          tick-broken: fwdA = ct_forwarder_tc_bookdrop, 25 s load at IT_RATE=50: breaks (from_mismatch) > 0,
#                 recoveries == breaks, max sampled broken age <= 10,600 ms, no tick offered while BROKEN (suppressed{broken} printed, not judged), O-T1
#   tk-e / tk-e-nc tick-empty (IT_BRIDGE + IT_HARNESS): 3 probe-creates at 10.00, tick seen, 3 probe-cancels; core best
#                 bid and ask 0 within 1.0 s of the last cancel's return; tk-e-nc (ed98477 ME jar) must FAIL "core bestBid
#                 still <p> 15 s after the book emptied"
#   tk-f          tick-core-leader: SIGKILL the AE leader at 5 s of a 10 s load, restart; a re-offer after the re-registration;
#                 O-T1 with the core counters of a surviving follower
#   tk-g          epoch-fence extended: a 3 s load and an applied tick before the epoch-fence row; epoch drops 0 over
#                 the row; unbound 0 from the first applied tick on
#   tk-i          tick-trade (IT_BRIDGE + IT_HARNESS, IT_RATE=50 IT_SECONDS=3): one crossing sell; the trade mark equals the
#                 ME's last tradeId (a ItMeDriver listen session opened before the sell), one offer and a
#                 version rise after the fill, core last trade id equal, invariants PASS, O-T1
#   tk-r / tk-r-nc tick-me-restored: 5 s load then quiet, ClusterTool snapshot on the ME leader, each follower SIGKILLed and
#                 restarted 12 s later (the archive mark file), the leader SIGKILLed: within 1.0 s of fwdA's
#                 "ME new leader" line market 1 template 22 +1, state 1, version > 0; tk-r-nc (the resnapshot rule off) must FAIL "no
#                 BookSnapshot for market 1 within 1.0 s of the new leader". The version regression counter is printed,
#                 not judged (a restored leader handing over to a replayed node lowers a quiet
#                 market's version, which rule 1 and E5 absorb)
# Rule: every ME session open resnapshots every session, so no ItMeDriver book / listen session and no
# tick_oracle is opened inside a judged "template 22 +1" window (TK-C, TK-C-nc, TK-R: from the fault to the line's end).
TICKPY="$HERE/market-ticks.py"
mono_ms() { python3 -I "$TICKPY" mono; }
MT_LABEL=""; MT_FAILS=(); MT_INFO=(); MT_SAMPLER=""; MT_AE_MEMBER=0
mt_line() { # ok|fail text
    if [ "$1" = ok ]; then log "LINE PASS $2"; MT_INFO+=("$2"); else log "LINE FAIL $2"; MT_FAILS+=("$2"); fi
}
mt_info() { log "INFO $*"; MT_INFO+=("$*"); }
tick_sample() { # file: the 100 ms sampler of fwdA, fwdB, the ME and AE nodes (one thread per target, 0.3 s timeouts)
    python3 -I "$TICKPY" sample "$1" 0.1 fwdA=http://127.0.0.1:24001/metrics fwdB=http://127.0.0.1:24002/metrics \
        me0=http://127.0.0.1:$((ME_BASE + 90))/metrics me1=http://127.0.0.1:$((ME_BASE + 91))/metrics \
        me2=http://127.0.0.1:$((ME_BASE + 92))/metrics ae0=http://127.0.0.1:$((AE_BASE + 90))/metrics \
        ae1=http://127.0.0.1:$((AE_BASE + 91))/metrics ae2=http://127.0.0.1:$((AE_BASE + 92))/metrics > "$1.err" 2>&1 &
    PID[sampler]=$!
}
scrape() { curl -s --max-time 2 "$1" 2> /dev/null > "$2"; }
fwd_sum() { # port family [label filter regex]: the sum of a forwarder family's series
    curl -s --max-time 2 "http://127.0.0.1:$1/metrics" 2> /dev/null |
        awk -v f="$2" -v r="${3:-}" 'index($1, f "{") == 1 && (r == "" || $1 ~ r) { s += $2 } END { printf "%d\n", s + 0 }'
}
prom_val() { awk -v n="$2" '$1 == n { printf "%d\n", $2; f = 1; exit } END { if (!f) print "" }' "$1"; }
mt_row_start() { # label port...
    MT_LABEL=$1; shift; MT_FAILS=(); MT_INFO=(); MT_ROW_T0=$(date +%s.%N) # O-T1 "row seconds" start (fix)
    local i impl bad=""
    for i in 0 1 2; do
        impl=$(metric "http://127.0.0.1:$((ME_BASE + 90 + i))/metrics" match_engine_effective_impl)
        [ -n "$impl" ] && [ "${impl%.*}" = 0 ] || bad="$bad me$i=${impl:-?}"
    done
    [ -z "$bad" ] && mt_line ok "match_engine_effective_impl 0 (ARRAY) on me0..me2 at row start" ||
        mt_line fail "match_engine_effective_impl not 0 at row start:$bad"
    local l; l=$(leader_of ae) || l=0
    MT_AE_MEMBER=$(( (l + 1) % 3 )) # a follower: no row restarts it (TK-F kills the leader)
    scrape "http://127.0.0.1:$((AE_BASE + 90 + MT_AE_MEMBER))/metrics" "$W/mt-$MT_LABEL-ae$MT_AE_MEMBER-start.prom"
    for p in "$@"; do scrape "http://127.0.0.1:$p/metrics" "$W/mt-$MT_LABEL-fwd$p-start.prom"; done
    MT_SAMPLER="$W/mt-$MT_LABEL-sampler.txt"; rm -f "$MT_SAMPLER"; tick_sample "$MT_SAMPLER"
    mt_info "row $MT_LABEL: AE leader $l, counter member ae$MT_AE_MEMBER; ME leader $(leader_of me); ME markets $T1_ME_MARKETS"
}
mt_row_end() { # stops the sampler, writes the change-only evidence copy
    [ -n "${PID[sampler]:-}" ] && { kill "${PID[sampler]}" 2> /dev/null; wait_exit sampler 3; }
    python3 -I "$TICKPY" changes "$MT_SAMPLER" > "$W/mt-$MT_LABEL-sampler-changes.txt" 2> /dev/null
}
mt_result() { # name [extra]
    mt_row_end
    if [ ${#MT_FAILS[@]} -eq 0 ]; then result "$1" ok "${2:+$2; }$(IFS=';'; echo "${MT_INFO[*]}")"
    else result "$1" fail "$(IFS=';'; echo "${MT_FAILS[*]}")${2:+; $2}"; fi
}
# O-T1: the barrier, then 350 ms, then the BOOK lines and the judge. TICK_ORACLE_LINE = its verdict.
TICK_ORACLE_LINE=""
tick_oracle() { # label port...
    local label=$1; shift
    local t0 ok=0 detail p body tr pend brk now
    t0=$(date +%s%N)
    while :; do
        local all=1; detail=""
        for p in "$@"; do
            body=$(curl -s --max-time 1 "http://127.0.0.1:$p/metrics" 2> /dev/null)
            tr=$(awk '$1 == "forwarder_ticks_ready" { print $2 }' <<< "$body")
            pend=$(awk '$1 ~ /^forwarder_tick_pending_age_ms\{/ && $2 != 0 { n++ } END { print n + 0 }' <<< "$body")
            brk=$(awk '$1 ~ /^forwarder_book_state\{/ && $2 == 2 { n++ } END { print n + 0 }' <<< "$body")
            ready "$p" || { all=0; detail="$detail $p not ready;"; }
            [ "$tr" = 1 ] && [ "$pend" = 0 ] && [ "$brk" = 0 ] || { all=0; detail="$detail $p ticks_ready=${tr:-?} pending>0=$pend broken=$brk;"; }
        done
        now=$(date +%s%N)
        [ $all -eq 1 ] && { ok=1; break; }
        [ $(( now - t0 )) -ge 12000000000 ] && break
        sleep 0.2
    done
    local bs; bs=$(python3 -I -c "import sys; print('%.2f' % ((int(sys.argv[2]) - int(sys.argv[1])) / 1e9))" "$t0" "$now")
    [ $ok -eq 1 ] && detail="every forwarder ready, ticks_ready 1, pending age 0, no BROKEN market"
    sleep 0.35
    local spec="$W/tick-oracle-$label.json" fwds="" names="" lead
    for p in "$@"; do
        scrape "http://127.0.0.1:$p/metrics" "$W/mt-$label-fwd$p-end.prom"
        local n; n=$([ "$p" = 24001 ] && echo fwdA || echo fwdB)
        # O-T1 offered line (fix): row seconds = row start (or this forwarder's start, if later) to now; a
        # forwarder started after the row start counts from 0 (its start prom belongs to an older process)
        local rs fresh; read -r rs fresh < <(python3 -I -c "import sys; r, f, n = map(float, sys.argv[1:4]); print('%.2f %s' % (n - max(r, f), 'true' if f > r else 'false'))" "$MT_ROW_T0" "${FWD_T0[$n]}" "$(date +%s.%N)")
        fwds="$fwds${fwds:+,}\"$p\": {\"start\": \"$W/mt-$MT_LABEL-fwd$p-start.prom\", \"end\": \"$W/mt-$label-fwd$p-end.prom\", \"row_s\": $rs, \"fresh\": $fresh}"
        names="$names${names:+,}\"$p\": \"$n\""
    done
    lead=$(leader_of ae) || lead=$MT_AE_MEMBER
    scrape "http://127.0.0.1:$((AE_BASE + 90 + lead))/metrics" "$W/mt-$label-ae-leader-end.prom"
    scrape "http://127.0.0.1:$((AE_BASE + 90 + MT_AE_MEMBER))/metrics" "$W/mt-$label-ae$MT_AE_MEMBER-end.prom"
    me_driver book "$DRIVER_DIR" "$ME_INGRESS" > "$W/book-$label.txt" 2>&1
    cat > "$spec" <<JSON
{"barrier_ok": $([ $ok -eq 1 ] && echo true || echo false), "barrier_s": $bs, "barrier_detail": "$detail",
 "book": "$W/book-$label.txt", "ae_leader_end": "$W/mt-$label-ae-leader-end.prom", "ae_member": "ae$MT_AE_MEMBER",
 "ae_member_name": "ae$MT_AE_MEMBER", "ae_member_start": "$W/mt-$MT_LABEL-ae$MT_AE_MEMBER-start.prom",
 "ae_member_end": "$W/mt-$label-ae$MT_AE_MEMBER-end.prom", "sampler": "$MT_SAMPLER",
 "forwarders": {$fwds}, "fwd_names": {$names}}
JSON
    python3 -I "$TICKPY" oracle "$spec" > "$W/tick-oracle-$label.txt" 2>&1
    local rc=$?
    sed 's/^/  O-T1 /' "$W/tick-oracle-$label.txt"
    TICK_ORACLE_LINE=$(tail -1 "$W/tick-oracle-$label.txt")
    [ $rc -eq 0 ] && mt_line ok "O-T1 $label (core counters ae$MT_AE_MEMBER)" || mt_line fail "$TICK_ORACLE_LINE"
    return $rc
}
mt_delivery_oracle() { # label port...: the delivery oracle (ticks must not disturb dispatch)
    local label=$1; shift
    oracle "$label" "$@" && mt_line ok "delivery oracle $label: $ORACLE_DETAIL" || mt_line fail "delivery oracle $label: $ORACLE_DETAIL"
}
mt_breaks() { # port -> chain breaks of market 1 by cause, and the ME's dropped market messages (carry X3)
    local c out=""
    for c in from_mismatch level_op depth crossed malformed; do
        out="$out $c=$(fwd_sum "$1" forwarder_book_chain_breaks_total "market=\"1\",cause=\"$c\"")"
    done
    local i d=""; for i in 0 1 2; do d="$d me$i=$(metric "http://127.0.0.1:$((ME_BASE + 90 + i))/metrics" match_dropped_market_msgs_total)"; done
    echo "market 1 chain breaks:$out; match_dropped_market_msgs_total$d"
}
mt_drain_capped() { # the max sampled match_egress_drain_capped_total per ME node over the row (8.1)
    local i out=""
    for i in 0 1 2; do
        out="$out me$i=$(python3 -I "$TICKPY" series "$MT_SAMPLER" "me$i" match_egress_drain_capped_total | awk '{ if ($2 > m) m = $2 } END { printf "%d", m + 0 }')"
    done
    echo "match_egress_drain_capped_total max sampled:$out"
}
leader_wait_except() { # node seconds -> the first node != $1 whose match_cluster_role is 2 (0.5 s probes, wall-clock bound)
    local deadline i v; deadline=$(( $(date +%s%N) + $2 * 1000000000 ))
    while [ "$(date +%s%N)" -lt "$deadline" ]; do
        for i in 0 1 2; do
            [ "$i" = "$1" ] && continue
            v=$(curl -s --max-time 0.5 "http://127.0.0.1:$((ME_BASE + 90 + i))/metrics" 2> /dev/null | awk '$1 == "match_cluster_role" { print $2; exit }')
            [ "$v" = 2 ] && { echo "$i"; return 0; }
        done
        sleep 0.1
    done
    return 1
}
core_quote() { # aeIndex market -> "bid ask"
    curl -s --max-time 0.5 "http://127.0.0.1:$((AE_BASE + 90 + $1))/metrics" 2> /dev/null |
        awk -v m="$2" '$1 == "assets_market_best_bid{market=\"" m "\"}" { b = $2 } $1 == "assets_market_best_ask{market=\"" m "\"}" { a = $2 }
            END { printf "%d %d\n", b, a }'
}
mt_tick_clean() { # label (tk-a, tk-h)
    local label=$1
    mt_row_start "$label" 24001
    load_bg "$SECONDS_LOAD"; local lp=$LOAD_PID; sleep 5
    local o5 f5 o1 f1
    o5=$(fwd_sum 24001 forwarder_tick_offered_total 'market="1"'); f5=$(fwd_sum 24001 forwarder_book_frames_total 'market="1",template="25"')
    wait "$lp"; grep LOAD "$W/load$LOAD_TAG.log"
    o1=$(fwd_sum 24001 forwarder_tick_offered_total 'market="1"'); f1=$(fwd_sum 24001 forwarder_book_frames_total 'market="1",template="25"')
    [ "$o1" = "$o5" ] && [ "$f1" -gt "$f5" ] &&
        mt_line ok "deeper-level line: market-1 offers from second 5 to the load's end $o5 -> $o1 (0) while template 25 rose $f5 -> $f1" ||
        mt_line fail "deeper-level line: market-1 offers from second 5 to the load's end $o5 -> $o1, template 25 $f5 -> $f1"
    tick_oracle "$label" 24001
    local br; br=$(fwd_sum 24001 forwarder_book_chain_breaks_total)
    [ "$br" = 0 ] && mt_line ok "chain breaks 0 (every market, every cause)" || mt_line fail "chain breaks $br: $(mt_breaks 24001)"
    mt_info "offered market 1 by edge: $(for e in leading trailing reoffer retry; do printf '%s=%s ' $e "$(fwd_sum 24001 forwarder_tick_offered_total "market=\"1\",edge=\"$e\"")"; done)"
    mt_delivery_oracle "$label" 24001
    mt_result "$label"
}
mt_scenario() {
    local s=$1
    case "$s" in
    tk-a) mt_tick_clean tk-a ;;
    tk-h) [ "$FWD_MODE" = embedded ] || { result tk-h fail "needs IT_FWD_MODE=embedded"; return 0; }; mt_tick_clean tk-h ;;
    tk-b)
        start_fwd fwdB "$FWD" "$FWDB_AE" "$FWDB_ME" 24002; wait_ready 24002 60 || log "fwdB not ready"
        mt_row_start tk-b 24001 24002
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        tick_oracle tk-b 24001 24002
        python3 -I - "$W"/mt-tk-b-fwd24001-start.prom "$W"/mt-tk-b-fwd24001-end.prom "$W"/mt-tk-b-fwd24002-start.prom \
            "$W"/mt-tk-b-fwd24002-end.prom "$W/mt-tk-b-ae$MT_AE_MEMBER-start.prom" "$W/mt-tk-b-ae$MT_AE_MEMBER-end.prom" \
            > "$W/tk-b-conservation.txt" <<'PY'
import sys
def prom(p):
    d = {}
    for l in open(p):
        if l.strip() and l[0] != "#":
            k, v = l.rsplit(" ", 1); d[k] = float(v)
    return d
def fam(d, n, sub=""):
    return sum(v for k, v in d.items() if k.startswith(n + "{") and sub in k)
a0, a1, b0, b1, c0, c1 = map(prom, sys.argv[1:7])
A = fam(a1, "forwarder_tick_offered_total") - fam(a0, "forwarder_tick_offered_total")
B = fam(b1, "forwarder_tick_offered_total") - fam(b0, "forwarder_tick_offered_total")
RA = fam(a1, "forwarder_tick_offered_total", 'edge="reoffer"') - fam(a0, "forwarder_tick_offered_total", 'edge="reoffer"')
RB = fam(b1, "forwarder_tick_offered_total", 'edge="reoffer"') - fam(b0, "forwarder_tick_offered_total", 'edge="reoffer"')
d = lambda n: c1.get(n, 0) - c0.get(n, 0)
ap, st = d("assets_ticks_applied_total"), d("assets_ticks_stale_total")
ok1 = ap + st == A + B
ok2 = st > 0
print("%s conservation: core applied %d + stale %d = %d == A offered %d + B offered %d = %d" % ("PASS" if ok1 else "FAIL", ap, st, ap + st, A, B, A + B))
print("%s core stale delta > 0: %d" % ("PASS" if ok2 else "FAIL", st))
print("INFO core applied %d - max(A %d, B %d) = %d, re-offers %d (recorded, not judged: D-1, the two coalescing phases "
      "interleave; applied <= A + B follows from the conservation line)" % (ap, A, B, ap - max(A, B), RA + RB))
PY
        while read -r v rest; do case "$v" in PASS) mt_line ok "$rest" ;; INFO) mt_info "$rest" ;; *) mt_line fail "$rest" ;; esac
        done < "$W/tk-b-conservation.txt"
        mt_delivery_oracle tk-b 24001 24002
        kill -TERM "${PID[fwdB]}"; wait_exit fwdB 3
        mt_result tk-b ;;
    tk-c)
        mt_row_start tk-c 24001
        load_bg "$SECONDS_LOAD"; local lp=$LOAD_PID; sleep 3
        local l nl m0; l=$(leader_of me); m0=$(mono_ms)
        log "SIGSTOP ME leader $l (mono $m0)"; kill -STOP "${PID[me$l]}"; sleep 3
        log "SIGKILL ME leader $l"; kill -9 "${PID[me$l]}"; unset "PID[me$l]"
        nl=$(leader_wait_except "$l" 30) || nl=""
        log "new ME leader ${nl:-none}: $(grep -h 'onRoleChange: LEADER' "$W/me${nl:-x}.log" 2> /dev/null | wc -l) LEADER lines in its log"
        sleep 3 # the judged window (1.0 s from the line) plus margin, no ME session opened in it
        local dl; dl=$(python3 -I "$TICKPY" delay "$W/fwdA.log" "$MT_SAMPLER" fwdA 1 "$m0" 1000); local drc=$?
        [ $drc -eq 0 ] && mt_line ok "tickResumesAfterMeFailoverWithinOneFlush: $dl" || mt_line fail "tickResumesAfterMeFailoverWithinOneFlush: $dl"
        local lm; lm=$(sed -n 's/.*line_mono=\([0-9]*\).*/\1/p' <<< "$dl")
        wait "$lp"; grep -E "LOAD|FAIL" "$W/load$LOAD_TAG.log"; start_me "$l"
        local r; r=$(python3 -I "$TICKPY" rise "$MT_SAMPLER" fwdA "${lm:-$m0}" 'forwarder_tick_offered_total{market="1",edge="leading"}' \
            'forwarder_tick_offered_total{market="1",edge="trailing"}' | tr '\n' ' ')
        local rb re; rb=$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^before=/) { sub("before=", "", $i); s += $i } } END { print s + 0 }' <<< "$r")
        re=$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^end=/) { sub("end=", "", $i); s += $i } } END { print s + 0 }' <<< "$r")
        [ "$re" -gt "$rb" ] && mt_line ok "leading+trailing offers of market 1 rise after the line: $rb -> $re" ||
            mt_line fail "leading+trailing offers of market 1 do not rise after the line: $rb -> $re"
        tick_oracle tk-c 24001
        local bid; bid=$(prom_val "$W/mt-tk-c-ae-leader-end.prom" 'assets_market_best_bid{market="1"}')
        [ "$bid" = 1499000000 ] && mt_line ok "core best bid at the end 14.99 ($bid)" || mt_line fail "core best bid at the end $bid, not 14.99 (1499000000)"
        local climb; climb=$(python3 -I "$TICKPY" series "$MT_SAMPLER" "ae$MT_AE_MEMBER" 'assets_market_best_bid{market="1"}' | awk '$2 >= 1499000000 { print $1; exit }')
        mt_info "recorded: climb end (core member best bid 14.99) at mono ${climb:-never} vs the new-leader line at mono ${lm:-?} ($([ -n "$climb" ] && [ -n "$lm" ] && echo "$((climb - lm)) ms after the line"))"
        mt_info "recorded: $(mt_breaks 24001); $(mt_drain_capped)"
        mt_delivery_oracle tk-c 24001
        mt_result tk-c ;;
    tk-c-nc)
        mt_row_start tk-c-nc 24001
        load_bg "$SECONDS_LOAD"; local lp=$LOAD_PID; sleep 3
        # STOP L0 right after a periodic 22 reached fwdA, so its 10 s periodic clock is fresh when it may lead again
        local k='forwarder_book_frames_total{market="1",template="22"}' c0 c1 waited=0
        c0=$(fwd_metric 24001 "$k")
        while [ $waited -lt 120 ]; do c1=$(fwd_metric 24001 "$k"); [ "${c1:-0}" -gt "${c0:-0}" ] && break; sleep 0.1; waited=$((waited + 1)); done
        local l0 l1="" l2="" m0 m1 t22
        t22=$(mono_ms); l0=$(leader_of me); m0=$(mono_ms)
        log "periodic 22 seen at fwdA ($c0 -> $c1, mono $t22): SIGSTOP L0 = me$l0"; kill -STOP "${PID[me$l0]}"; sleep 3
        kill -CONT "${PID[me$l0]}"; log "SIGCONT me$l0"
        l1=$(leader_wait_except "$l0" 8) || l1=""
        if [ -n "$l1" ]; then
            # IT_NC_L1_HOLD_S (default 2, the row's method): let the CONTed L0 catch up as a follower before L1
            # stops, so the election can pick it (attempts 1-3 with hold 0 always elected the third node); still
            # inside L0's 10 s clock. The contract's "within 8 s of L0's last leader flush" is asserted below.
            sleep "${IT_NC_L1_HOLD_S:-2}"
            m1=$(mono_ms); log "L1 = me$l1 leads ($((m1 - t22)) ms after the 22): SIGSTOP it"; kill -STOP "${PID[me$l1]}"; sleep 3
            kill -CONT "${PID[me$l1]}"; log "SIGCONT me$l1"
            l2=$(leader_wait_except "$l1" 8) || l2=""
        fi
        local m2; m2=$(mono_ms); log "L2 = me${l2:-none} ($((m2 - t22)) ms after L0's last observed periodic 22)"
        sleep 3
        local dl drc; dl=$(python3 -I "$TICKPY" delay "$W/fwdA.log" "$MT_SAMPLER" fwdA 1 "${m1:-$m0}" 1000 --which first); drc=$?
        wait "$lp"; grep -E "LOAD|FAIL" "$W/load$LOAD_TAG.log"
        for i in 0 1 2; do mt_info "me$i alive $(kill -0 "${PID[me$i]}" 2> /dev/null && echo yes || echo no), LEADER lines $(grep -c 'onRoleChange: LEADER' "$W/me$i.log")"; done
        mt_info "leader history L0=me$l0 L1=me${l1:-none} L2=me${l2:-none}; L0's last observed periodic 22 at mono $t22"
        tick_oracle tk-c-nc 24001 > /dev/null; mt_info "O-T1 (printed, not judged in the RED half): $TICK_ORACLE_LINE"
        oracle tk-c-nc 24001; mt_info "delivery oracle: $ORACLE_DETAIL"
        mt_row_end
        local hist="L0=me$l0 L1=me${l1:-none} L2=me${l2:-none}" pre=""
        # preconditions: L1 stopped within 8 s of L0's last observed periodic 22, and L0 leads again
        # within 10 s of it ("the new leader led within the last 10 s"); otherwise a slow election is INCONCLUSIVE
        [ -n "$l1" ] && [ $((m1 - t22)) -gt 8000 ] && pre="L1 me$l1 stopped $((m1 - t22)) ms after L0's last periodic 22 (> 8000)"
        [ -z "$pre" ] && [ -n "$l2" ] && [ "$l2" = "$l0" ] && [ $((m2 - t22)) -gt 10000 ] &&
            pre="L0 me$l0 led again $((m2 - t22)) ms after its last periodic 22 (> 10000)"
        [ -n "$pre" ] && mt_info "precondition not met: $pre"
        if [ -n "$pre" ]; then
            result tk-c-nc fail "INCONCLUSIVE: precondition not met: $pre ($hist); the TK-C line: $dl; $(IFS=';'; echo "${MT_INFO[*]}")"
        elif [ -n "$l2" ] && [ "$l2" = "$l0" ]; then
            if [ $drc -ne 0 ]; then result tk-c-nc ok "RED as required: the recent leader me$l0 returned ($hist, $((m2 - t22)) ms after its last periodic 22) and the TK-C line FAILS: $dl; $(IFS=';'; echo "${MT_INFO[*]}")"
            else result tk-c-nc fail "INCONCLUSIVE: the recent leader me$l0 returned ($hist) but the TK-C line held: $dl; $(IFS=';'; echo "${MT_INFO[*]}")"; fi
        else result tk-c-nc fail "INCONCLUSIVE: no recent leader returned ($hist); the TK-C line on the second change: $dl; $(IFS=';'; echo "${MT_INFO[*]}")"; fi ;;
    tk-d)
        local tc; tc=$(find "$BUILD" -name ct_forwarder_tc_bookdrop -type f | head -1)
        [ -x "$tc" ] || { result tk-d fail "ct_forwarder_tc_bookdrop not built (-DCT_FORWARDER_TEST_BUILDS=ON)"; return 0; }
        kill -TERM "${PID[fwdA]}"; wait_exit fwdA 3
        start_fwd fwdA "$tc" "$FWDA_AE" "$FWDA_ME" 24001; wait_ready 24001 60 || log "fwdA (tc_bookdrop) not ready"
        mt_row_start tk-d 24001
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        tick_oracle tk-d 24001
        local br rc sup mx drops
        br=$(fwd_sum 24001 forwarder_book_chain_breaks_total 'market="1",cause="from_mismatch"')
        rc=$(fwd_sum 24001 forwarder_book_recoveries_total 'market="1"')
        sup=$(fwd_sum 24001 forwarder_tick_suppressed_total 'market="1",reason="broken"')
        mx=$(python3 -I "$TICKPY" series "$MT_SAMPLER" fwdA 'forwarder_book_broken_age_ms{market="1"}' | awk '{ if ($2 > m) m = $2 } END { printf "%d", m + 0 }')
        drops=$(grep -c 'tc_bookdrop: BookDelta' "$W/fwdA.log")
        [ "$br" -gt 0 ] && mt_line ok "chain breaks{from_mismatch} market 1 $br > 0 (tc_bookdrop drops $drops)" || mt_line fail "no from_mismatch break (drops $drops)"
        [ "$rc" = "$br" ] && mt_line ok "recoveries $rc == breaks{from_mismatch} $br at the end" || mt_line fail "recoveries $rc != breaks{from_mismatch} $br at the end"
        [ "$mx" -le 10600 ] && mt_line ok "max sampled forwarder_book_broken_age_ms{market=\"1\"} $mx <= 10600" || mt_line fail "max sampled broken age $mx > 10600"
        # Suppressed{broken} is counted only when an edge was already due at a BROKEN step
        # (TickEmitter.cpp step()), so "> 0" passed 5 of 7 runs: printed, not judged. The judged property is E1 itself.
        local no norc; no=$(python3 -I "$TICKPY" nooffer "$MT_SAMPLER" fwdA 1); norc=$?
        [ $norc -eq 0 ] && mt_line ok "no MarketTick offered for market 1 while its ladder is BROKEN: ${no#NOOFFER PASS }" || mt_line fail "${no#NOOFFER FAIL }"
        mt_info "forwarder_tick_suppressed_total{market=\"1\",reason=\"broken\"} $sup (printed, not judged)"
        mt_info "recorded: $(mt_breaks 24001)"
        mt_delivery_oracle tk-d 24001
        mt_result tk-d ;;
    tk-e|tk-e-nc)
        [ -n "$HARNESS" ] && [ -n "$BRIDGE_BIN" ] || { result "$s" fail "needs IT_HARNESS and IT_BRIDGE"; return 0; }
        mt_row_start "$s" 24001
        local ids=() r id i px=1000000000
        for i in 1 2 3; do
            r=$(t1core probe-create $i 1 --side buy --price $px --qty 100000000 2>&1 | tail -1); log "create user $i: $r"
            id=$(sed -n 's/^OrderAck order=\([0-9]*\) .*/\1/p' <<< "$r"); [ -n "$id" ] && ids+=("$id")
        done
        [ ${#ids[@]} -eq 3 ] && mt_line ok "three probe-creates at 10.00: order ids ${ids[*]}" || mt_line fail "probe-create ids: ${ids[*]:-none}"
        local al q seen=""; al=$(leader_of ae) || al=0
        for _ in $(seq 1 100); do q=$(core_quote "$al" 1); [ "${q%% *}" = "$px" ] && { seen=yes; break; }; sleep 0.1; done
        [ -n "$seen" ] && mt_line ok "tick seen: core best bid $px" || mt_line fail "no tick: core quote $q"
        for id in "${ids[@]}"; do r=$(t1core probe-cancel "$id" 2>&1 | tail -1); log "cancel $id: $r"; done
        local tc0; tc0=$(date +%s%N); log "last probe-cancel returned"
        local t="" n=0 qq
        while [ $(( $(date +%s%N) - tc0 )) -lt 15000000000 ]; do
            qq=$(core_quote "$al" 1); [ "$qq" = "0 0" ] && { t=$(( ($(date +%s%N) - tc0) / 1000000 )); break; }; sleep 0.05
        done
        local text
        if [ -n "$t" ] && [ "$t" -le 1000 ]; then text="PASS core best bid 0 and best ask 0 for market 1 ${t} ms after the last cancel's return (<= 1000)"
        elif [ -n "$t" ]; then text="FAIL core best bid 0 and best ask 0 only ${t} ms after the last cancel's return (> 1000)"
        else text="FAIL core bestBid still ${qq%% *} 15 s after the book emptied"; fi
        log "TK-E line: $text"
        sleep 1; me_driver book "$DRIVER_DIR" "$ME_INGRESS" > "$W/book-$s-after.txt" 2>&1; mt_info "after the window: $(grep '^BOOK market=1 ' "$W/book-$s-after.txt")"
        t1core dump --orders <(printf '%s\n' "${ids[@]}") 2>&1 | grep -E '^(ABSENT|O) ' > "$W/$s-dump.txt"; mt_info "dump: $(tr '\n' ' ' < "$W/$s-dump.txt")"
        local u; u=$(t1core invariants 2>&1 | grep -v '^feed-harness ' | tail -1)
        [[ "$u" == *PASS* ]] && mt_line ok "t1core invariants: $u" || mt_line fail "t1core invariants: $u"
        if [ "$s" = tk-e ]; then
            [[ "$text" == PASS* ]] && mt_line ok "${text#PASS }" || mt_line fail "${text#FAIL }"
            tick_oracle tk-e 24001; mt_delivery_oracle tk-e 24001; mt_result tk-e
        else
            tick_oracle tk-e-nc 24001 > /dev/null; mt_info "O-T1 (printed, not judged in the RED half): $TICK_ORACLE_LINE"
            oracle tk-e-nc 24001; mt_info "delivery oracle: $ORACLE_DETAIL"; mt_row_end
            [[ "$text" == "FAIL core bestBid still "*" 15 s after the book emptied" ]] &&
                result tk-e-nc ok "RED as required: FAILS \"${text#FAIL }\"; $(IFS=';'; echo "${MT_INFO[*]}")" ||
                result tk-e-nc fail "not RED: $text; $(IFS=';'; echo "${MT_INFO[*]}")"
        fi ;;
    tk-f)
        mt_row_start tk-f 24001
        load_bg "$SECONDS_LOAD"; local lp=$LOAD_PID; sleep 5
        local l nl m0; l=$(leader_of ae); m0=$(mono_ms)
        [ "$l" != "$MT_AE_MEMBER" ] || mt_line fail "the counter member ae$MT_AE_MEMBER is the AE leader"
        log "SIGKILL AE leader $l"; kill -9 "${PID[ae$l]}"; unset "PID[ae$l]"
        for _ in $(seq 1 60); do nl=$(leader_of ae) && [ "$nl" != "$l" ] && break; sleep 0.5; done
        log "new AE leader ${nl:-none}"; sleep 2; start_ae "$l"
        wait "$lp"; grep -E "LOAD|FAIL" "$W/load$LOAD_TAG.log"
        tick_oracle tk-f 24001
        local reg; reg=$(python3 -I "$TICKPY" logline "$W/fwdA.log" '\] registered \(term [1-9]' "$m0")
        local r; r=$(python3 -I "$TICKPY" rise "$MT_SAMPLER" fwdA "${reg/none/$m0}" 'forwarder_tick_offered_total{market="1",edge="reoffer"}')
        local rb re; rb=$(sed -n 's/.* before=\([0-9]*\).*/\1/p' <<< "$r"); re=$(sed -n 's/.* end=\([0-9]*\).*/\1/p' <<< "$r")
        [ "$reg" != none ] && [ "${re:-0}" -gt "${rb:-0}" ] && mt_line ok "re-offer after the re-registration (log mono $reg): reoffer $rb -> $re" ||
            mt_line fail "no re-offer after the re-registration (log mono $reg): reoffer ${rb:-?} -> ${re:-?}"
        mt_info "fwdA: $(grep -h 'core new leader\|registered (term' "$W/fwdA.log" | tr '\n' ' ')"
        mt_info "recorded: $(mt_breaks 24001)"
        mt_delivery_oracle tk-f 24001
        mt_result tk-f ;;
    tk-g)
        mt_row_start tk-g 24001
        load_bg 3; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        local mp="http://127.0.0.1:$((AE_BASE + 90 + MT_AE_MEMBER))/metrics" a0 a1
        a0=$(prom_val "$W/mt-tk-g-ae$MT_AE_MEMBER-start.prom" assets_ticks_applied_total)
        for _ in $(seq 1 50); do a1=$(metric "$mp" assets_ticks_applied_total); [ "${a1%.*}" -gt "${a0:-0}" ] 2> /dev/null && break; sleep 0.1; done
        [ "${a1%.*}" -gt "${a0:-0}" ] 2> /dev/null && mt_line ok "assets_ticks_applied_total delta > 0 before the re-genesis: $a0 -> ${a1%.*}" ||
            mt_line fail "no tick applied before the re-genesis: $a0 -> $a1"
        run_scenario epoch-fence
        local last=""; [ ${#PASSED[@]} -gt 0 ] && last=${PASSED[-1]}
        [ "$last" = epoch-fence ] && mt_line ok "the epoch-fence row PASSES" || mt_line fail "the epoch-fence row FAILED"
        sleep 2
        scrape "$mp" "$W/mt-tk-g-ae$MT_AE_MEMBER-end.prom"
        python3 -I - "$W/mt-tk-g-ae$MT_AE_MEMBER-start.prom" "$W/mt-tk-g-ae$MT_AE_MEMBER-end.prom" "$MT_SAMPLER" "ae$MT_AE_MEMBER" > "$W/tk-g-lines.txt" <<'PY'
import sys
def prom(p):
    d = {}
    for l in open(p):
        if l.strip() and l[0] != "#":
            k, v = l.rsplit(" ", 1); d[k] = float(v)
    return d
s, e = prom(sys.argv[1]), prom(sys.argv[2])
ed = e.get("assets_ticks_epoch_drop_total", 0) - s.get("assets_ticks_epoch_drop_total", 0)
print("%s assets_ticks_epoch_drop_total delta 0 across the whole row: %d" % ("PASS" if ed == 0 else "FAIL", ed))
a0, u0 = s.get("assets_ticks_applied_total", 0), s.get("assets_ticks_unbound_total", 0)
snaps = {}
for line in open(sys.argv[3]):
    p = line.split()
    if len(p) == 5 and p[2] == sys.argv[4]:
        snaps.setdefault((int(p[0]), int(p[1])), {})[p[3]] = float(p[4])
first = next((v for k, v in sorted(snaps.items()) if v.get("assets_ticks_applied_total", 0) > a0), None)
if first is None:
    print("FAIL no applied tick in the sampler (unbound line vacuous)")
else:
    u1 = first.get("assets_ticks_unbound_total", 0); ue = e.get("assets_ticks_unbound_total", 0)
    print("%s assets_ticks_unbound_total delta 0 from the first applied tick to the end: %d (before it: %d, printed)" % (
        "PASS" if ue - u1 == 0 else "FAIL", ue - u1, u1 - u0))
PY
        while read -r v rest; do [ "$v" = PASS ] && mt_line ok "$rest" || mt_line fail "$rest"; done < "$W/tk-g-lines.txt"
        mt_result tk-g ;;
    tk-i)
        [ -n "$HARNESS" ] && [ -n "$BRIDGE_BIN" ] || { result tk-i fail "needs IT_HARNESS and IT_BRIDGE"; return 0; }
        mt_row_start tk-i 24001
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"
        me_driver listen "$DRIVER_DIR" "$ME_INGRESS" 15 > "$W/tk-i-listen.txt" 2>&1 &
        PID[listen]=$!
        for _ in $(seq 1 100); do grep -q '^LISTEN open' "$W/tk-i-listen.txt" 2> /dev/null && break; sleep 0.1; done
        sleep 1.5 # the listener's own session-open resnapshot is applied before the baseline
        local o0 v0 o1 v1 d r
        o0=$(fwd_sum 24001 forwarder_tick_offered_total 'market="1"'); v0=$(fwd_metric 24001 'forwarder_tick_last_offered_version{market="1"}')
        d=$(t1core deposit 1 1 1000000000 2>&1 | tail -1); log "deposit BTC user 1: $d"
        r=$(t1core probe-create 1 1 --side sell --price 1000000000 --qty 100000000 2>&1 | tail -1); log "sell: $r"
        sleep 3
        o1=$(fwd_sum 24001 forwarder_tick_offered_total 'market="1"'); v1=$(fwd_metric 24001 'forwarder_tick_last_offered_version{market="1"}')
        wait_exit listen 20
        local meid fid cid
        meid=$(awk '/tpl=26 market=1 / { for (i = 1; i <= NF; i++) if ($i ~ /^maxTradeId=/) { sub("maxTradeId=", "", $i); if ($i + 0 > m) m = $i + 0 } } END { print m + 0 }' "$W/tk-i-listen.txt")
        fid=$(fwd_metric 24001 'forwarder_trade_last_id{market="1"}')
        mt_info "listener: $(grep -E 'tpl=(25|26) ' "$W/tk-i-listen.txt" | tail -3 | tr '\n' ' ')"
        [ "$meid" -gt 0 ] && [ "${fid%.*}" = "$meid" ] && mt_line ok "forwarder_trade_last_id{market=\"1\"} $fid == the ME's last tradeId $meid (26 maxTradeId)" ||
            mt_line fail "forwarder_trade_last_id $fid != the ME's last tradeId $meid"
        [ $((o1 - o0)) -ge 1 ] && [ "${v1%.*}" -gt "${v0%.*}" ] && mt_line ok "after the fill: offered market 1 +$((o1 - o0)) ($o0 -> $o1), last offered version $v0 -> $v1" ||
            mt_line fail "after the fill: offered market 1 +$((o1 - o0)), last offered version $v0 -> $v1"
        tick_oracle tk-i 24001
        cid=$(prom_val "$W/mt-tk-i-ae-leader-end.prom" 'assets_market_last_trade_id{market="1"}')
        [ "$cid" = "$meid" ] && mt_line ok "core assets_market_last_trade_id{market=\"1\"} $cid == the ME's $meid" || mt_line fail "core last trade id $cid != the ME's $meid"
        local u; u=$(t1core invariants 2>&1 | grep -v '^feed-harness ' | tail -1)
        [[ "$u" == *PASS* ]] && mt_line ok "t1core invariants: $u" || mt_line fail "t1core invariants: $u"
        mt_delivery_oracle tk-i 24001
        mt_result tk-i ;;
    tk-r|tk-r-nc)
        mt_row_start "$s" 24001
        load_bg "$SECONDS_LOAD"; wait "$LOAD_PID"; grep LOAD "$W/load$LOAD_TAG.log"; sleep 2
        local l f dir nl ok=1 restored=""; l=$(leader_of me); dir=$(dirname "$(find "$W/me$l" -name cluster-mark.dat | head -1)")
        log "ClusterTool snapshot on ME leader $l ($dir)"
        java "${JAVA_OPTS[@]}" -cp "$ME_JAR" io.aeron.cluster.ClusterTool "$dir" snapshot 2>&1 | tail -2 | sed 's/^/  ClusterTool /'; sleep 3
        for f in 0 1 2; do
            [ "$f" = "$l" ] && continue
            log "SIGKILL ME follower $f"; kill -9 "${PID[me$f]}"; wait_exit "me$f" 5; sleep 12; start_me "$f"
            local r=""; for _ in $(seq 1 120); do
                kill -0 "${PID[me$f]}" 2> /dev/null || { r=dead; break; }
                r=$(curl -s --max-time 0.5 "http://127.0.0.1:$((ME_BASE + 90 + f))/metrics" | awk '$1 == "match_cluster_role" { print $2; exit }')
                [ "$r" = 0 ] && break; sleep 0.5; done
            log "follower $f role ${r:-none}"; [ "$r" = 0 ] || ok=0
            grep -q 'loadSnapshot called' "$W/me$f.log" && restored="$restored me$f" || ok=0
        done
        [ $ok = 1 ] && mt_line ok "both followers restored from the snapshot ($restored; $(grep -h 'republished the restored' "$W"/me*.log | head -1))" ||
            mt_line fail "followers not both restored ($restored)"
        local m0; m0=$(mono_ms); log "SIGKILL ME leader $l (mono $m0)"; kill -9 "${PID[me$l]}"; unset "PID[me$l]"
        nl=$(leader_wait_except "$l" 30) || nl=""
        log "new ME leader ${nl:-none}"
        [ -n "$nl" ] && mt_line ok "a restored node leads: me$nl" || mt_line fail "no new ME leader in 30 s"
        sleep 3 # the judged window plus margin, no ME session opened in it
        local dl drc; dl=$(python3 -I "$TICKPY" delay "$W/fwdA.log" "$MT_SAMPLER" fwdA 1 "$m0" 1000 --version-positive); drc=$?
        local lm; lm=$(sed -n 's/.*line_mono=\([0-9]*\).*/\1/p' <<< "$dl")
        local mk m st sts=""
        for m in 2 3 4 5; do
            st=$(python3 -I "$TICKPY" series "$MT_SAMPLER" fwdA "forwarder_book_state{market=\"$m\"}" "${lm:-$m0}" | awk '{ if ($2 > x) x = $2 } END { printf "%d", x + 0 }')
            sts="$sts $m=$st"; [ "$st" -le 1 ] || mk=bad
        done
        local tr; tr=$(fwd_metric 24001 forwarder_ticks_ready)
        mt_info "recorded: forwarder_book_version_regressions_total market 1 $(fwd_metric 24001 'forwarder_book_version_regressions_total{market="1"}') (not judged); $(mt_drain_capped)"
        mt_info "fwdA after the line: book version market 1 $(fwd_metric 24001 'forwarder_book_version{market="1"}'), markets 2..5 versions $(for m in 2 3 4 5; do printf '%s ' "$(fwd_metric 24001 "forwarder_book_version{market=\"$m\"}")"; done)"
        if [ "$s" = tk-r ]; then
            [ $drc -eq 0 ] && mt_line ok "$dl" || mt_line fail "no BookSnapshot for market 1 within 1.0 s of the new leader ($dl)"
            [ -z "${mk:-}" ] && mt_line ok "markets 2..5 state 0 or 1 after the line (max sampled:$sts)" || mt_line fail "markets 2..5 state after the line:$sts"
            [ "$tr" = 1 ] && mt_line ok "forwarder_ticks_ready 1" || mt_line fail "forwarder_ticks_ready $tr"
            tick_oracle tk-r 24001; mt_delivery_oracle tk-r 24001; mt_result tk-r
        else
            tick_oracle tk-r-nc 24001 > /dev/null; mt_info "O-T1 (printed, not judged in the RED half): $TICK_ORACLE_LINE"
            oracle tk-r-nc 24001; mt_info "delivery oracle: $ORACLE_DETAIL"; mt_row_end
            [ $drc -ne 0 ] && result tk-r-nc ok "RED as required: FAILS \"no BookSnapshot for market 1 within 1.0 s of the new leader\" ($dl); $(IFS=';'; echo "${MT_INFO[*]}")" ||
                result tk-r-nc fail "not RED: $dl; $(IFS=';'; echo "${MT_INFO[*]}")"
        fi ;;
    *) return 1 ;;
    esac
    return 0
}

for s in "${SCENARIOS[@]}"; do run_scenario "$s"; done
# The bridge's own stop is asserted: SIGTERM = exit 0 (the documented code; a bridge that a
# scenario ended earlier, e.g. epoch-fence's re-genesis, is restarted by that scenario)
if [ -n "$BRIDGE_BIN" ] && [ -n "${PID[bridge]:-}" ]; then
    kill -TERM "${PID[bridge]}"; wait_exit bridge 10
    if [ "$WAIT_RC" = 0 ]; then result bridge-sigterm ok "ct-bridge SIGTERM: exit 0"
    else result bridge-sigterm fail "ct-bridge SIGTERM: exit $WAIT_RC (expected 0)"; fi
fi

# forwarder metrics at the end (evidence)
curl -s --max-time 2 http://127.0.0.1:24001/metrics > "$W/fwdA-metrics-final.prom" 2>/dev/null || true
echo
echo "PASSED: ${PASSED[*]:-none}"
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "FORWARDER T1 PASS: ${#PASSED[@]} scenarios"
else
    echo "FORWARDER T1 FAIL: ${FAILED[*]}"
    IT_KEEP=1
    exit 1
fi
