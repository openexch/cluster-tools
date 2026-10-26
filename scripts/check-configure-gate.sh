#!/bin/bash
# Proves the verified-dependency gate refuses every tampered or unpinned input: each case
# below must FAIL cmake configure with the expected message, the positive control must pass,
# and gen-codecs.sh must refuse a jar swapped after configure. Registered as the ctest
# build.configure_gate in the WITH_AERON build; runnable by hand too.
#
# Usage: scripts/check-configure-gate.sh <cmake> <aeron-tarball> <sbe-classpath> <money-schema> <match-schema>
set -euo pipefail

if [ $# -ne 5 ]; then
    echo "usage: $0 <cmake> <aeron-tarball> <sbe-classpath> <money-schema> <match-schema>" >&2
    exit 64
fi
CMAKE=$1
TARBALL=$2
SBE_CP=$3
MONEY=$4
MATCH=$5
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d -t ct-configure-gate.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }

expect_refused() { # name needle cmake-args...
    local name=$1 needle=$2
    shift 2
    local log="$WORK/$name.log"
    if "$CMAKE" -S "$HERE" -B "$WORK/build-$name" -DWITH_AERON=ON -DCT_BUILD_MEDIA_DRIVER=OFF "$@" > "$log" 2>&1; then
        fail "$name: configure succeeded, must be refused"
    elif tr -s ' \n' ' ' < "$log" | grep -q -- "$needle"; then # cmake wraps its messages
        pass "$name: refused ($needle)"
    else
        fail "$name: refused for another reason:"
        grep -A3 'CMake Error' "$log" | head -8
    fi
    rm -rf "$WORK/build-$name"
}

GOOD=(-DAERON_TARBALL="$TARBALL" -DSBE_CLASSPATH="$SBE_CP" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH")

# 1. A tampered Aeron tree, repacked: one C source changed, version.txt intact.
mkdir -p "$WORK/tamper"
tar -xzf "$TARBALL" -C "$WORK/tamper"
TOP=$(find "$WORK/tamper" -mindepth 1 -maxdepth 1 -type d | head -1)
echo "/* tampered */" >> "$TOP/aeron-client/src/main/c/aeron_client_conductor.c"
tar -czf "$WORK/aeron-tampered.tar.gz" -C "$WORK/tamper" "$(basename "$TOP")"
expect_refused tampered-tarball "has sha256" -DAERON_TARBALL="$WORK/aeron-tampered.tar.gz" \
    -DSBE_CLASSPATH="$SBE_CP" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH"

# 2. A source tree as input (the tampered one, version.txt says 1.53.1): not accepted at all.
expect_refused aeron-tree-input "AERON_SRC is not an input" -DAERON_SRC="$TOP" "${GOOD[@]}"

# 3. No tarball.
expect_refused no-tarball "needs -DAERON_TARBALL" -DSBE_CLASSPATH="$SBE_CP" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH"

# 4. A jar the lock does not pin on the SBE classpath.
IFS=: read -r SBE_JAR AGRONA_JAR <<< "$SBE_CP"
cp "$AGRONA_JAR" "$WORK/agrona-other.jar"
printf 'x' >> "$WORK/agrona-other.jar"
expect_refused unpinned-jar "is not a jar dependencies.lock pins" -DAERON_TARBALL="$TARBALL" \
    -DSBE_CLASSPATH="$SBE_JAR:$WORK/agrona-other.jar" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH"

# 5-7. Schemas: missing, below the floor, required template removed.
expect_refused no-money-schema "is required" -DAERON_TARBALL="$TARBALL" -DSBE_CLASSPATH="$SBE_CP" -DMATCH_SCHEMA="$MATCH"
sed '0,/version="11"/s//version="10"/' "$MATCH" > "$WORK/order-v10.xml"
expect_refused match-below-floor "below the floor" -DAERON_TARBALL="$TARBALL" -DSBE_CLASSPATH="$SBE_CP" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$WORK/order-v10.xml"
sed 's/<sbe:message name="DurableOrderCommand" id="9"/<sbe:message name="DurableOrderCommandX" id="99"/' "$MATCH" \
    > "$WORK/order-no9.xml"
expect_refused match-no-template-9 "template 9 is missing" -DAERON_TARBALL="$TARBALL" -DSBE_CLASSPATH="$SBE_CP" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$WORK/order-no9.xml"

# 8. Positive control: the pinned inputs configure.
if "$CMAKE" -S "$HERE" -B "$WORK/build-good" -DWITH_AERON=ON -DCT_BUILD_MEDIA_DRIVER=OFF "${GOOD[@]}" \
    > "$WORK/good.log" 2>&1; then
    pass "pinned inputs configure"
else
    fail "pinned inputs were refused:"
    grep -A3 'CMake Error' "$WORK/good.log" | head -8
fi

# 9. A jar swapped AFTER configure is refused by gen-codecs.sh at build time.
COPY=$(find "$WORK/build-good/_deps/sbe" -name 'agrona*.jar' | head -1)
SBE_COPY=$(find "$WORK/build-good/_deps/sbe" -name 'sbe-tool*.jar' | head -1)
SHAS=$(python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["sbe_sha256"], d["agrona_sha256"])' \
    "$HERE/dependencies.lock")
printf 'x' >> "$COPY"
if SBE_CLASSPATH="$SBE_COPY:$COPY" SBE_JAR_SHA256S="$SHAS" bash "$HERE/adapters/sbe/gen-codecs.sh" \
    "$WORK/gen" "$WORK/gen.stamp" "$MONEY" > "$WORK/gen.log" 2>&1; then
    fail "gen-codecs.sh ran a jar swapped after configure"
elif grep -q "refusing to run it" "$WORK/gen.log"; then
    pass "gen-codecs.sh refuses a jar swapped after configure"
else
    fail "gen-codecs.sh failed for another reason: $(cat "$WORK/gen.log")"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "CONFIGURE GATE PASS"
else
    echo "CONFIGURE GATE FAIL: $FAILED"
    exit 1
fi
