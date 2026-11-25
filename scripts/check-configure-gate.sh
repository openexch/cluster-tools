#!/bin/bash
# Proves the verified-dependency gate refuses every tampered or unpinned input: each case
# below must FAIL cmake configure with the expected message, the positive control must pass,
# and gen-codecs.sh must refuse a jar swapped after configure. Registered as the ctest
# build.configure_gate in the WITH_AERON build; runnable by hand too.
#
# Usage: scripts/check-configure-gate.sh <cmake> <aeron-tarball> <sbe-classpath> <money-schema> <match-schema> <me-journal-schema> [<aeron-all.jar>]
# The WITH_BRIDGE cases run in the same pass; the optional aeron-all jar adds
# the integration-fixture jar case.
set -euo pipefail

if [ $# -ne 6 ] && [ $# -ne 7 ]; then
    echo "usage: $0 <cmake> <aeron-tarball> <sbe-classpath> <money-schema> <match-schema> <me-journal-schema> [<aeron-all.jar>]" >&2
    exit 64
fi
CMAKE=$1
TARBALL=$2
SBE_CP=$3
MONEY=$4
MATCH=$5
JOURNAL=$6
AERON_ALL=${7:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d -t ct-configure-gate.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }

expect_refused_in() { # source-dir name needle cmake-args...
    local src=$1 name=$2 needle=$3
    shift 3
    local log="$WORK/$name.log"
    if "$CMAKE" -S "$src" -B "$WORK/build-$name" -DWITH_AERON=ON -DCT_BUILD_MEDIA_DRIVER=OFF "$@" > "$log" 2>&1; then
        fail "$name: configure succeeded, must be refused"
    elif tr -s ' \n' ' ' < "$log" | grep -q -- "$needle"; then # cmake wraps its messages
        pass "$name: refused ($needle)"
    else
        fail "$name: refused for another reason:"
        grep -A3 'CMake Error' "$log" | head -8
    fi
    rm -rf "$WORK/build-$name"
}
expect_refused() { # name needle cmake-args...
    expect_refused_in "$HERE" "$@"
}

# A copy of this source tree (tracked and untracked files, not the ignored build dirs) whose lock or patches a case edits.
copy_tree() { # dest
    mkdir -p "$1"
    git -C "$HERE" ls-files -z -co --exclude-standard | tar -C "$HERE" --null -T - -cf - | tar -C "$1" -xf -
}

# The money schema's root version is the lock's pin (money_schema_min_version); the cases below edit the ROOT
# attribute (the first version="N" of the file: sinceVersion attributes come after it) one below and one above it.
MONEY_V=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["money_schema_min_version"])' "$HERE/dependencies.lock")
MONEY_PREV=$((MONEY_V - 1))
MONEY_NEXT=$((MONEY_V + 1))
if ! grep -q "^[[:space:]]*version=\"$MONEY_V\"" "$MONEY"; then
    fail "money schema $MONEY does not carry the root version=\"$MONEY_V\" the lock pins"
fi

GOOD=(-DAERON_TARBALL="$TARBALL" -DSBE_CLASSPATH="$SBE_CP" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$JOURNAL")

# 1. A tampered Aeron tree, repacked: one C source changed, version.txt intact.
mkdir -p "$WORK/tamper"
tar -xzf "$TARBALL" -C "$WORK/tamper"
TOP=$(find "$WORK/tamper" -mindepth 1 -maxdepth 1 -type d | head -1)
echo "/* tampered */" >> "$TOP/aeron-client/src/main/c/aeron_client_conductor.c"
tar -czf "$WORK/aeron-tampered.tar.gz" -C "$WORK/tamper" "$(basename "$TOP")"
expect_refused tampered-tarball "has sha256" -DAERON_TARBALL="$WORK/aeron-tampered.tar.gz" \
    -DSBE_CLASSPATH="$SBE_CP" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH"

# 1b. The Aeron patches (dependencies.lock aeron_patches): a patch edited without a lock change, a missing patch, and a
# patch that no longer applies to the pinned tree (its lock sha256 updated, so only the apply check can refuse it).
PATCH_REL=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["aeron_patches"][0]["path"])' "$HERE/dependencies.lock")
copy_tree "$WORK/src-patch-sha"
printf '\n' >> "$WORK/src-patch-sha/$PATCH_REL"
expect_refused_in "$WORK/src-patch-sha" patch-sha-differs "Aeron patch $PATCH_REL has sha256" "${GOOD[@]}"
rm -rf "$WORK/src-patch-sha"
copy_tree "$WORK/src-patch-missing"
rm "$WORK/src-patch-missing/$PATCH_REL"
expect_refused_in "$WORK/src-patch-missing" patch-missing "aeron_patches) is missing" "${GOOD[@]}"
rm -rf "$WORK/src-patch-missing"
copy_tree "$WORK/src-patch-stale"
python3 -I - "$WORK/src-patch-stale" "$PATCH_REL" <<'PY'
import hashlib, json, sys
root, rel = sys.argv[1], sys.argv[2]
patch = open(f"{root}/{rel}").read()
needle = "\n         AERON_STATUS_MESSAGE_HEADER_SEND_SETUP_FLAG) < 0)\n"  # a context line of the hunk
assert patch.count(needle) == 1
patch = patch.replace(needle, "\n         AERON_STATUS_MESSAGE_HEADER_SEND_SETUP_FLAG) <= 0)\n")
open(f"{root}/{rel}", "w").write(patch)
lock_path = f"{root}/dependencies.lock"
lock = open(lock_path).read()
old = json.loads(lock)["aeron_patches"][0]["sha256"]
open(lock_path, "w").write(lock.replace(old, hashlib.sha256(patch.encode()).hexdigest()))
PY
expect_refused_in "$WORK/src-patch-stale" patch-does-not-apply "does not apply to the Aeron" "${GOOD[@]}"
rm -rf "$WORK/src-patch-stale"

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

# 4b. Cache variables retired by the rename of the engine to core: refused naming the new name (the positive
# control below configures without them).
expect_refused retired-it-jar "the cache variable CT_IT_ASSETS_JAR is retired; it is now CT_IT_CORE_JAR" \
    "${GOOD[@]}" -DCT_IT_ASSETS_JAR="$MONEY"

# 5-7. Schemas: missing, below the floor, required template removed. The negatives the
# pin rules name: the v11 order schema, the previous money-schema version, an ME journal whose hash differs,
# a v12 order schema whose template 9 lost timeInForce.
SCHEMAS=(-DAERON_TARBALL="$TARBALL" -DSBE_CLASSPATH="$SBE_CP")
expect_refused no-money-schema "is required" "${SCHEMAS[@]}" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$JOURNAL"
sed '0,/version="12"/s//version="11"/' "$MATCH" > "$WORK/order-v11.xml"
expect_refused match-v11-below-floor "below the floor" "${SCHEMAS[@]}" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$WORK/order-v11.xml" -DME_JOURNAL_SCHEMA="$JOURNAL"
sed "0,/version=\"$MONEY_V\"/s//version=\"$MONEY_PREV\"/" "$MONEY" > "$WORK/money-prev.xml"
expect_refused money-prev-below-floor "below the floor" "${SCHEMAS[@]}" \
    -DMONEY_SCHEMA="$WORK/money-prev.xml" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$JOURNAL"
python3 -I - "$MONEY" "$WORK/money-no117.xml" <<'PY'
import sys
xml = open(sys.argv[1]).read()
needle = '<sbe:message name="UserBalances" id="117"'
assert xml.count(needle) == 1
open(sys.argv[2], 'w').write(xml.replace(needle, '<sbe:message name="UserBalancesX" id="199"'))
PY
expect_refused money-no-template-117 "template 117 is missing" "${SCHEMAS[@]}" \
    -DMONEY_SCHEMA="$WORK/money-no117.xml" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$JOURNAL"
sed 's/<sbe:message name="DurableOrderCommand" id="9"/<sbe:message name="DurableOrderCommandX" id="99"/' "$MATCH" \
    > "$WORK/order-no9.xml"
expect_refused match-no-template-9 "template 9 is missing" "${SCHEMAS[@]}" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$WORK/order-no9.xml" -DME_JOURNAL_SCHEMA="$JOURNAL"
python3 -I - "$MATCH" "$WORK/order-no-tif.xml" <<'PY'
import re, sys
xml = open(sys.argv[1]).read()
m = re.search(r'<sbe:message[^>]*\bid="9".*?</sbe:message>', xml, re.S)
body = m.group(0)
assert 'name="timeInForce"' in body
open(sys.argv[2], 'w').write(xml.replace(body, body.replace('name="timeInForce"', 'name="timeInForceX"')))
PY
expect_refused match-no-timeInForce "has no timeInForce" "${SCHEMAS[@]}" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$WORK/order-no-tif.xml" -DME_JOURNAL_SCHEMA="$JOURNAL"
sed '0,/version="1"/s//version="1" /' "$JOURNAL" > "$WORK/journal-edited.xml"
expect_refused journal-hash-differs "dependencies.lock pins" "${SCHEMAS[@]}" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$WORK/journal-edited.xml"
expect_refused no-journal-schema "is required" "${SCHEMAS[@]}" -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH"

# 7b. WITH_BRIDGE: money and journal pinned by EXACT sha256 and version.
# WITH_BRIDGE also requires the pinned harness jar (CT_GATE_FEED_HARNESS_JAR from the ctest
# registration); without one the bridge cases cannot reach the schema checks, so they are reported as skipped.
HARNESS=${CT_GATE_FEED_HARNESS_JAR:-}
BRIDGE=(-DWITH_BRIDGE=ON)
if [ -n "$HARNESS" ]; then
    BRIDGE+=(-DCT_FEED_HARNESS_JAR="$HARNESS")
    expect_refused bridge-without-harness "CT_FEED_HARNESS_JAR is required" "${GOOD[@]}" -DWITH_BRIDGE=ON
    cp "$HARNESS" "$WORK/harness-other.jar"
    printf 'x' >> "$WORK/harness-other.jar"
    expect_refused bridge-harness-sha "feed_harness_sha256" "${GOOD[@]}" -DWITH_BRIDGE=ON \
        -DCT_FEED_HARNESS_JAR="$WORK/harness-other.jar"
else
    echo "SKIP the WITH_BRIDGE cases: no CT_GATE_FEED_HARNESS_JAR"
fi
expect_refused bridge-without-aeron "WITH_BRIDGE=ON requires WITH_AERON=ON" "${GOOD[@]}" -DWITH_BRIDGE=ON -DWITH_AERON=OFF
expect_refused bridge-no-journal-schema "is required" "${SCHEMAS[@]}" "${BRIDGE[@]}" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH"
# the cases below get past the harness check only with a harness: without one they are the skipped ones
if [ -n "$HARNESS" ]; then
sed "0,/version=\"$MONEY_V\"/s//version=\"$MONEY_V\" /" "$MONEY" > "$WORK/money-edited.xml"
expect_refused bridge-money-hash-differs "the bridge needs the exact core wire" "${SCHEMAS[@]}" "${BRIDGE[@]}" \
    -DMONEY_SCHEMA="$WORK/money-edited.xml" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$JOURNAL"
sed "0,/version=\"$MONEY_V\"/s//version=\"$MONEY_NEXT\"/" "$MONEY" > "$WORK/money-next.xml"
expect_refused bridge-money-next "WITH_BRIDGE=ON: MONEY_SCHEMA" "${SCHEMAS[@]}" "${BRIDGE[@]}" \
    -DMONEY_SCHEMA="$WORK/money-next.xml" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$JOURNAL"
sed '0,/version="1"/s//version="2"/' "$JOURNAL" > "$WORK/journal-v2.xml"
expect_refused bridge-journal-v2 "dependencies.lock pins" "${SCHEMAS[@]}" "${BRIDGE[@]}" \
    -DMONEY_SCHEMA="$MONEY" -DMATCH_SCHEMA="$MATCH" -DME_JOURNAL_SCHEMA="$WORK/journal-v2.xml"
if [ -n "$AERON_ALL" ]; then
    cp "$AERON_ALL" "$WORK/aeron-all-other.jar"
    printf 'x' >> "$WORK/aeron-all-other.jar"
    # the driver is built here (the later -D wins over expect_refused's OFF): the jar check, not the driver rule, must refuse
    expect_refused bridge-it-jar-sha "CT_IT_AERON_ALL_JAR" "${GOOD[@]}" "${BRIDGE[@]}" -DCT_BUILD_MEDIA_DRIVER=ON \
        -DCT_INTEGRATION_TESTS=ON -DCT_IT_CORE_JAR="$MONEY" -DCT_IT_MATCH_JAR="$MATCH" \
        -DCT_IT_AERON_ALL_JAR="$WORK/aeron-all-other.jar"
fi
# ct-bridge links aeron_driver_static (the embedded driver mode), so WITH_BRIDGE needs the driver built
expect_refused bridge-without-driver "needs CT_BUILD_MEDIA_DRIVER=ON" "${GOOD[@]}" "${BRIDGE[@]}"
if "$CMAKE" -S "$HERE" -B "$WORK/build-bridge" -DWITH_AERON=ON "${BRIDGE[@]}" -DCT_BUILD_MEDIA_DRIVER=ON "${GOOD[@]}" \
    > "$WORK/bridge.log" 2>&1; then
    pass "pinned inputs configure with WITH_BRIDGE=ON"
else
    fail "pinned inputs were refused with WITH_BRIDGE=ON:"
    grep -A3 'CMake Error' "$WORK/bridge.log" | head -8
fi
rm -rf "$WORK/build-bridge"
fi

# ct-forwarder links aeron_driver_static too (its embedded driver mode), so WITH_AERON needs the driver
expect_refused forwarder-without-driver "ct-forwarder (WITH_AERON) needs CT_BUILD_MEDIA_DRIVER=ON" "${GOOD[@]}"

# 8. Positive control: the pinned inputs configure.
if "$CMAKE" -S "$HERE" -B "$WORK/build-good" -DWITH_AERON=ON -DCT_BUILD_MEDIA_DRIVER=ON "${GOOD[@]}" \
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
