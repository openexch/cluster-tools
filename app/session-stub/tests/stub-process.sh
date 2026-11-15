#!/bin/bash
# session_stub.process: the ct_session_stub binary, no cluster needed.
#   1. no arguments: exit 78, every required option named in one pass
#   2. a credential of another class than GATEWAY: exit 78, the class named
#   3. external mode with no media driver in --aeron-dir: exit 70 inside the driver timeout
#   4. embedded mode driven by stdin (status, throttle pause, a bad line, quit): exit 0 and the events in order, the
#      embedded driver dir removed on the way out
#   5. embedded mode stopped by SIGTERM: exit 0
#   6. external mode against a live embedded stub's driver: exit 0; a second embedded stub with the same instance id: 78
# usage: stub-process.sh <ct_session_stub> <work-dir>
set -uo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <ct_session_stub> <work-dir>" >&2
    exit 64
fi
STUB=$1
WORK=$2
rm -rf "$WORK"
mkdir -p "$WORK/drivers"
FAILED=0
fail() { echo "FAIL $*"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS $*"; }

GATEWAY_KEY="7:gateway:$(printf 'ab%.0s' $(seq 1 32))"
BRIDGE_KEY="7:bridge:$(printf 'ab%.0s' $(seq 1 32))"
COMMON=(--instance-id 9 --ingress-endpoints 0=127.0.0.1:1 --ingress-channel 'aeron:udp?term-length=64k|mtu=8192'
    --response-channel 'aeron:udp?endpoint=127.0.0.1:0|term-length=64k|mtu=8192' --channels acks,orders,control
    --status-interval-ms 0)

# 1
env -i "$STUB" > "$WORK/1.out" 2>&1
rc=$?
if [ "$rc" -eq 78 ] && grep -q -- '--driver-mode is required' "$WORK/1.out" && grep -q -- '--channels is required' "$WORK/1.out" \
    && grep -q 'CORE_AUTH_CREDENTIAL' "$WORK/1.out"; then
    pass "no arguments: 78 with every problem named"
else
    fail "no arguments: exit $rc"; cat "$WORK/1.out"
fi

# 2
env -i CORE_AUTH_CREDENTIAL="$BRIDGE_KEY" "$STUB" --driver-mode embedded --driver-dir "$WORK/drivers" "${COMMON[@]}" \
    > "$WORK/2.out" 2>&1
rc=$?
if [ "$rc" -eq 78 ] && grep -qi 'GATEWAY' "$WORK/2.out"; then
    pass "a BRIDGE key: 78, the class named"
else
    fail "a BRIDGE key: exit $rc"; cat "$WORK/2.out"
fi

# 3
start=$(date +%s)
env -i CORE_AUTH_CREDENTIAL="$GATEWAY_KEY" timeout 30 "$STUB" --driver-mode external --aeron-dir "$WORK/no-driver" \
    --driver-timeout-ms 3000 "${COMMON[@]}" < /dev/null > "$WORK/3.out" 2>&1
rc=$?
took=$(( $(date +%s) - start ))
if [ "$rc" -eq 70 ] && [ "$took" -le 15 ]; then
    pass "external without a driver: 70 after ${took} s"
else
    fail "external without a driver: exit $rc after ${took} s"; cat "$WORK/3.out"
fi

# 4
printf 'status\nthrottle pause\nthrottle sideways\nsleep 200\nstatus\nquit\n' | env -i CORE_AUTH_CREDENTIAL="$GATEWAY_KEY" \
    timeout 60 "$STUB" --driver-mode embedded --driver-dir "$WORK/drivers" "${COMMON[@]}" > "$WORK/4.out" 2> "$WORK/4.err"
rc=$?
expected=('event=start channels=acks,orders,control mask=0x51' 'event=status state=' 'event=throttle mode=pause'
    'event=command_error line="throttle sideways"' 'throttle=pause' 'event=quit')
ok=1
line=0
for e in "${expected[@]}"; do
    n=$(grep -n -F -- "$e" "$WORK/4.out" | head -1 | cut -d: -f1)
    if [ -z "$n" ] || [ "$n" -lt "$line" ]; then ok=0; echo "missing or out of order: $e"; break; fi
    line=$n
done
if [ "$rc" -eq 0 ] && [ "$ok" -eq 1 ] && [ ! -e "$WORK/drivers/ct-session-stub-9" ]; then
    pass "embedded, scripted: 0, events in order, driver dir removed"
else
    fail "embedded, scripted: exit $rc"; cat "$WORK/4.out" "$WORK/4.err"; ls -la "$WORK/drivers"
fi

# 5
mkfifo "$WORK/stdin5"
env -i CORE_AUTH_CREDENTIAL="$GATEWAY_KEY" "$STUB" --driver-mode embedded --driver-dir "$WORK/drivers" "${COMMON[@]}" \
    < "$WORK/stdin5" > "$WORK/5.out" 2>&1 &
pid=$!
exec 9> "$WORK/stdin5" # keep the stub's stdin open
for _ in $(seq 1 100); do grep -q 'event=start' "$WORK/5.out" && break; sleep 0.1; done
kill -TERM "$pid"
wait "$pid"
rc=$?
exec 9>&-
if [ "$rc" -eq 0 ] && grep -q 'event=start' "$WORK/5.out"; then
    pass "SIGTERM: 0"
else
    fail "SIGTERM: exit $rc"; cat "$WORK/5.out"
fi

# 6: external mode against a live embedded driver, and the live-instance refusal
mkfifo "$WORK/stdin6"
env -i CORE_AUTH_CREDENTIAL="$GATEWAY_KEY" "$STUB" --driver-mode embedded --driver-dir "$WORK/drivers" "${COMMON[@]}" \
    < "$WORK/stdin6" > "$WORK/6a.out" 2>&1 &
pid6=$!
exec 8> "$WORK/stdin6" # keep the first stub's stdin open
for _ in $(seq 1 100); do grep -q 'event=start' "$WORK/6a.out" && break; sleep 0.1; done
DRIVER_DIR="$WORK/drivers/ct-session-stub-9"
EXT_COMMON=("${COMMON[@]}")
EXT_COMMON[1]=10 # --instance-id 10: a different stub
printf 'status\nquit\n' | env -i CORE_AUTH_CREDENTIAL="$GATEWAY_KEY" timeout 60 "$STUB" --driver-mode external \
    --aeron-dir "$DRIVER_DIR" "${EXT_COMMON[@]}" > "$WORK/6b.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && grep -q 'event=start' "$WORK/6b.out" && grep -q 'event=quit' "$WORK/6b.out"; then
    pass "external against a live driver: 0, start and quit"
else
    fail "external against a live driver: exit $rc"; cat "$WORK/6b.out"
fi
env -i CORE_AUTH_CREDENTIAL="$GATEWAY_KEY" timeout 30 "$STUB" --driver-mode embedded --driver-dir "$WORK/drivers" \
    "${COMMON[@]}" < /dev/null > "$WORK/6c.out" 2>&1
rc=$?
if [ "$rc" -eq 78 ]; then
    pass "a second embedded stub with the same instance id while the first is live: 78"
else
    fail "same instance id while live: exit $rc"; cat "$WORK/6c.out"
fi
printf 'quit\n' >&8
wait "$pid6"
rc=$?
exec 8>&-
if [ "$rc" -eq 0 ] && grep -q 'event=quit' "$WORK/6a.out"; then
    pass "first stub stopped cleanly: 0"
else
    fail "first stub stop: exit $rc"; cat "$WORK/6a.out"
fi

if [ "$FAILED" -ne 0 ]; then
    echo "session_stub.process: $FAILED failed"
    exit 1
fi
echo "session_stub.process: PASS"
