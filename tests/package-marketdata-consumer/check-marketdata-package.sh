#!/bin/bash
# package.marketdata_consumer: install this build as a package and link the market data components from a separate
# CMake project, as another repository does. Pass = every step below holds; each failure names its step.
#
#   1. `cmake --install <build> --prefix <work>/prefix` succeeds and the prefix holds the package config, the
#      forwarder domain headers and the two exported codec headers, and not the bridge codec's header.
#   2. tests/package-marketdata-consumer configures against the prefix ONLY, pinned to the build's source sha, builds,
#      and marketdata_consumer runs: a 22 through the codec into the ladder, record 1 through the record decode, the
#      stall verdict helpers.
#   3. Control: a package whose export lacks journal_codec must FAIL the consumer's configure, naming the component.
#
# usage: check-marketdata-package.sh <cmake> <build-dir> <work-dir>
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <cmake> <build-dir> <work-dir>" >&2
    exit 64
fi
CMAKE=$1
BUILD=$(cd "$2" && pwd)
WORK=$3
HERE=$(cd "$(dirname "$0")" && pwd)

fail() { echo "package.marketdata_consumer: FAIL: $*" >&2; exit 1; }
logHas() { tr '\n' ' ' < "$1" | tr -s ' ' | grep -qF -- "$2"; }
cache() { sed -n "s/^$1:[A-Z]*=//p" "$BUILD/CMakeCache.txt" | head -1; }

rm -rf "$WORK"
mkdir -p "$WORK"
PREFIX="$WORK/prefix"

# 1. install
"$CMAKE" --install "$BUILD" --prefix "$PREFIX" > "$WORK/install.log" 2>&1 || { cat "$WORK/install.log" >&2; fail "cmake --install failed"; }
CONFIG=$(find "$PREFIX" -name cluster_toolsConfig.cmake -print -quit)
[ -n "$CONFIG" ] || fail "no cluster_toolsConfig.cmake under $PREFIX"
SOURCE_SHA=$(sed -n 's/^set(cluster_tools_SOURCE_SHA "\(.*\)")$/\1/p' "$CONFIG")
BUILT=$(sed -n 's/^set(cluster_tools_COMPONENTS_BUILT "\(.*\)")$/\1/p' "$CONFIG")
echo "package.marketdata_consumer: installed to $PREFIX (source_sha $SOURCE_SHA, components $BUILT)"
for h in cluster_tools/domain/forwarder/BookLadder.h cluster_tools/domain/forwarder/MarketData.h \
    cluster_tools/adapters/sbe/ForwarderCodec.h cluster_tools/adapters/sbe/JournalTradeCodec.h \
    cluster_tools/archive/FollowPolicy.h cluster_tools/archive/ArchiveMetrics.h; do
    [ -f "$PREFIX/include/$h" ] || fail "the package lacks the header $h"
done
for h in cluster_tools/adapters/sbe/BridgeCodec.h cluster_tools/adapters/sbe/EngineConfigCodec.h; do
    [ ! -e "$PREFIX/include/$h" ] || fail "the package carries $h, a header of the binaries only"
done

COMMON=(-DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER="$(cache CMAKE_C_COMPILER)" -DCMAKE_CXX_COMPILER="$(cache CMAKE_CXX_COMPILER)")

# 2. the consumer
"$CMAKE" -S "$HERE" -B "$WORK/consumer" -DCMAKE_PREFIX_PATH="$PREFIX" "${COMMON[@]}" -DCT_EXPECT_SOURCE_SHA="$SOURCE_SHA" \
    > "$WORK/consumer-configure.log" 2>&1 || { cat "$WORK/consumer-configure.log" >&2; fail "the consumer did not configure against the package"; }
"$CMAKE" --build "$WORK/consumer" -j8 > "$WORK/consumer-build.log" 2>&1 \
    || { cat "$WORK/consumer-build.log" >&2; fail "the consumer did not build against the package"; }
"$WORK/consumer/marketdata_consumer" | tee "$WORK/consumer.out"
grep -q '^market data consumer OK$' "$WORK/consumer.out" || fail "marketdata_consumer did not pass"

# 3. control: journal_codec absent from the export
CONTROL_PREFIX="$WORK/prefix-without-journal-codec"
cp -a "$PREFIX" "$CONTROL_PREFIX"
python3 -I - "$CONTROL_PREFIX" <<'PY' || fail "control: the journal_codec target could not be removed from the copy"
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
removed = 0
for f in root.rglob("cluster_toolsTargets*.cmake"):
    text = f.read_text()
    # every blank-line separated stanza that names the target goes, and the name leaves the expected-target lists
    stanzas = re.split(r"\n\s*\n", text)
    kept = [s for s in stanzas if "cluster_tools::journal_codec" not in s or "foreach(_cmake_expected_target" in s]
    removed += len(stanzas) - len(kept)
    text = "\n\n".join(kept).replace("cluster_tools::journal_codec", "")
    f.write_text(text)
sys.exit(0 if removed > 0 else 1)
PY
if "$CMAKE" -S "$HERE" -B "$WORK/control-component" -DCMAKE_PREFIX_PATH="$CONTROL_PREFIX" "${COMMON[@]}" \
    > "$WORK/control-component.log" 2>&1; then
    fail "control: a package without journal_codec was accepted"
fi
logHas "$WORK/control-component.log" "cluster_tools component 'journal_codec' is not in this package" \
    || { cat "$WORK/control-component.log" >&2; fail "control: the missing journal_codec component was not named"; }
echo "package.marketdata_consumer: control: journal_codec missing from the export -> configure fails naming it"

echo "package.marketdata_consumer: PASS"
