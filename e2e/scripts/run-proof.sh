#!/bin/bash
# The send-config end-to-end proof, against an isolated dockerized 3-node match
# cluster in ENGINE-FROM-CONFIG mode. Run from anywhere; everything happens in docker
# on this machine — no live host is touched.
#
# Prerequisites (refused loudly, never guessed):
#   e2e/match-cluster.jar   server jar built at the pinned match commit
#                           (mvn -pl match-cluster -am package -DskipTests)
#   e2e/order-schema.xml    the wire schema the jar embeds, extracted from the SAME
#                           match commit (git show <commit>:match-common/src/main/
#                           resources/sbe/order-schema.xml)
#   e2e/money-schema.xml    the core money-schema at dependencies.lock money_schema_ref
#                           (scripts/fetch-schemas.sh writes it; the build needs it
#                           since RC2 S11 even though the proof does not use it)
#   DEPS_DIR env            the verified dependency cache of scripts/fetch-deps.sh
#                           (aeron-1.53.1.tar.gz, sbe-tool-1.40.1.jar, agrona-2.5.0.jar)
#   docker                  with the poc builder image buildable (poc/Dockerfile.builder)
#
# The sequence (evidence goes to stdout — tee it):
#   1  fresh config-mode cluster: CreateOrder is REJECTED ENGINE_NOT_CONFIGURED(11)
#   2  send-config demo-config.json: offer ok; [ENGINE-CONFIG] ACCEPTED (fresh) x3
#   3  the same CreateOrder now: OrderStatus NEW (engines born from the log)
#   4  same config again: duplicate no-op; a DIFFERENT config: REJECTED
#   5  docker restart one node: it recovers and replays the config (no exit)
#
# KEEP_UP=1 leaves the stack running for inspection; default tears it down.
set -euo pipefail

E2E_DIR=$(cd "$(dirname "$0")/.." && pwd)
REPO_DIR=$(cd "$E2E_DIR/.." && pwd)
COMPOSE=(docker compose -f "$E2E_DIR/docker-compose.yml")

fail() {
    echo "PROOF FAIL: $1" >&2
    exit 1
}

banner() {
    echo
    echo "==================== $1 ===================="
}

client() {
    docker exec ccfg-client bash /e2e/scripts/with-md.sh "$@"
}

# grep one literal string in every node's log, printing the last N matches per node;
# fails the proof if any node has no match.
expect_on_all_nodes() {
    local needle=$1 last=$2 node
    for node in me0 me1 me2; do
        echo "--- ccfg-$node:"
        docker logs "ccfg-$node" 2>&1 | grep -F "$needle" | tail -n "$last" \
            || fail "node $node never logged: $needle"
    done
}

[ -f "$E2E_DIR/match-cluster.jar" ] || fail "e2e/match-cluster.jar missing (build it from the pinned match commit)"
[ -f "$E2E_DIR/order-schema.xml" ] || fail "e2e/order-schema.xml missing (extract it from the SAME match commit)"
[ -f "$E2E_DIR/money-schema.xml" ] || fail "e2e/money-schema.xml missing (scripts/fetch-schemas.sh)"
[ -n "${DEPS_DIR:-}" ] || fail "DEPS_DIR not set (run scripts/fetch-deps.sh <dir> and point DEPS_DIR at it)"
[ -f "$DEPS_DIR/aeron-1.53.1.tar.gz" ] || fail "no aeron-1.53.1.tar.gz under DEPS_DIR (configure verifies its sha256)"

banner "STEP 0a: build the client artifacts (cached docker volume)"
docker build -q -t ccpoc-builder -f "$REPO_DIR/poc/Dockerfile.builder" "$REPO_DIR/poc"
docker run --rm \
    -v "$DEPS_DIR":/deps:ro \
    -v ccpoc_aeronbuild:/build \
    -v "$REPO_DIR":/repo \
    -v "$E2E_DIR":/schema \
    ccpoc-builder bash /repo/e2e/scripts/build-client.sh

banner "STEP 0b: fresh cluster up (3 nodes, MATCH_ENGINE_FROM_CONFIG=true) + client"
"${COMPOSE[@]}" down -v >/dev/null 2>&1 || true
"${COMPOSE[@]}" up -d
echo "waiting for a leader (probe retries until a session is established)..."
for attempt in 1 2 3; do
    if client /e2e/dist/switch-behavior probe --frames 1 --timeout-ms 2000; then
        break
    fi
    [ "$attempt" = 3 ] && fail "no cluster session after 3 probe attempts"
    sleep 5
done

banner "STEP 1: CreateOrder on the FRESH cluster -> REJECTED ENGINE_NOT_CONFIGURED"
STEP1=$(client /e2e/dist/poc) || true
echo "$STEP1" | grep -F "ORDER_STATUS" || true
echo "$STEP1" | grep -F "status=REJECTED(4)" | grep -F "rejectReason=11" | grep -qF "orderId=0" \
    || fail "step 1: expected REJECTED(4) rejectReason=11 orderId=0"
echo "STEP 1 PASS: fresh-cluster guard live (REJECTED, rejectReason=11, orderId=0)"

banner "STEP 2: send-config demo-config.json -> ACCEPTED (fresh) on all 3 nodes"
client /e2e/dist/switch-behavior send-config --file /e2e/demo-config.json \
    || fail "step 2: send-config exited non-zero"
sleep 3
expect_on_all_nodes "[ENGINE-CONFIG] ACCEPTED (fresh)" 1
echo "STEP 2 PASS: engines created from the replicated config on every node"

banner "STEP 3: the SAME CreateOrder -> OrderStatus NEW (engines born from the log)"
STEP3=$(client /e2e/dist/poc) || true
echo "$STEP3" | grep -F "ORDER_STATUS" || true
echo "$STEP3" | grep -F "omsOrderId=424242" | grep -qF "status=NEW(0)" \
    || fail "step 3: expected status=NEW(0) for omsOrderId=424242"
echo "STEP 3 PASS: order accepted post-config"

banner "STEP 4a: the SAME config again -> idempotent duplicate no-op"
client /e2e/dist/switch-behavior send-config --file /e2e/demo-config.json \
    || fail "step 4a: send-config exited non-zero"
sleep 3
expect_on_all_nodes "[ENGINE-CONFIG] duplicate ignored" 1
echo "STEP 4a PASS: duplicate was a counted no-op"

banner "STEP 4b: a DIFFERENT config -> deterministic REJECT"
client /e2e/dist/switch-behavior send-config --file /e2e/different-config.json \
    || fail "step 4b: send-config exited non-zero"
sleep 3
expect_on_all_nodes "[ENGINE-CONFIG] REJECTED" 1
echo "STEP 4b PASS: differing config rejected (runtime reconfig is a later slice)"

banner "STEP 5: docker restart ccfg-me1 -> recovers, config restored from the log"
RESTART_TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)
docker restart ccfg-me1
echo "waiting for ccfg-me1 to replay..."
RECOVERED=""
for _ in $(seq 1 24); do
    sleep 5
    if docker logs --since "$RESTART_TS" ccfg-me1 2>&1 | grep -qF "[ENGINE-CONFIG] ACCEPTED (fresh)"; then
        RECOVERED=yes
        break
    fi
done
[ -n "$RECOVERED" ] || fail "step 5: me1 never replayed the EngineConfig after restart"
echo "--- ccfg-me1 post-restart [ENGINE-CONFIG] lines:"
docker logs --since "$RESTART_TS" ccfg-me1 2>&1 | grep -F "[ENGINE-CONFIG]" || true
if docker logs --since "$RESTART_TS" ccfg-me1 2>&1 | grep -qF "CRITICAL"; then
    fail "step 5: me1 logged CRITICAL after restart (cross-check mismatch?)"
fi
[ "$(docker inspect -f '{{.State.Running}}' ccfg-me1)" = "true" ] \
    || fail "step 5: me1 is not running after restart"
# Final liveness must be deterministic evidence, not egress interpretation (the new
# leader re-delivers standby status events, so a poc order's ack is ambiguous here):
# a second duplicate send-config through the recovered cluster must move the
# duplicate counter to 2 on EVERY node — including the restarted one, which proves
# me1 is applying NEW logged commands after its replay, not just old ones.
echo "final liveness: a NEW logged command through the recovered cluster..."
client /e2e/dist/switch-behavior send-config --file /e2e/demo-config.json \
    || fail "step 5: post-restart send-config exited non-zero"
sleep 3
expect_on_all_nodes "duplicates=2" 1
echo "STEP 5 PASS: restarted node recovered from its own log (no exit, no re-adoption)"
echo "            and applies new logged commands (duplicates=2 on all 3 nodes)"

banner "ALL 5 STEPS PASSED"
if [ "${KEEP_UP:-0}" != "1" ]; then
    "${COMPOSE[@]}" down -v >/dev/null 2>&1 || true
    echo "(stack torn down; KEEP_UP=1 to keep it)"
fi
