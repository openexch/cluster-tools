#!/bin/bash
# package.consumer: install this build as a package and link it from a separate CMake project, as another repository
# does. Pass = every step below holds; each failure names its step.
#
#   1. `cmake --install <build> --prefix <work>/prefix` succeeds; the prefix holds the package config, and no
#      executable (the binaries, the test builds and the session stub are never installed).
#   2. tests/package-consumer configures against the prefix ONLY (CMAKE_PREFIX_PATH), pinned to the build's source sha,
#      builds, and its programs run: pure_consumer always, aeron_consumer when the package has the Aeron half.
#   3. Controls, each must FAIL configure with its named text: a component the package does not have, and a pin to
#      another commit than the one the package was built from.
#
# usage: check-package.sh <cmake> <build-dir> <work-dir>
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <cmake> <build-dir> <work-dir>" >&2
    exit 64
fi
CMAKE=$1
BUILD=$(cd "$2" && pwd)
WORK=$3
HERE=$(cd "$(dirname "$0")" && pwd)

fail() { echo "package.consumer: FAIL: $*" >&2; exit 1; }
# CMake wraps message() text: join the log into one line before looking for a sentence in it
logHas() { tr '\n' ' ' < "$1" | tr -s ' ' | grep -qF -- "$2"; }
cache() { sed -n "s/^$1:[A-Z]*=//p" "$BUILD/CMakeCache.txt" | head -1; }

rm -rf "$WORK"
mkdir -p "$WORK"
PREFIX="$WORK/prefix"
mkdir -p "$PREFIX"

# 1. install
"$CMAKE" --install "$BUILD" --prefix "$PREFIX" > "$WORK/install.log" 2>&1 || { cat "$WORK/install.log" >&2; fail "cmake --install failed"; }
CONFIG=$(find "$PREFIX" -name cluster_toolsConfig.cmake -print -quit)
[ -n "$CONFIG" ] || fail "no cluster_toolsConfig.cmake under $PREFIX"
EXECUTABLES=$(find "$PREFIX" -type f -perm -u+x ! -name '*.a' ! -name '*.h' ! -name '*.cmake' ! -name '*.cpp' | sort)
[ -z "$EXECUTABLES" ] || fail "the package installs executables: $EXECUTABLES"
SOURCE_SHA=$(sed -n 's/^set(cluster_tools_SOURCE_SHA "\(.*\)")$/\1/p' "$CONFIG")
[ -n "$SOURCE_SHA" ] || fail "the package config names no source sha"
WITH_AERON=$(sed -n 's/^set(cluster_tools_WITH_AERON \(.*\))$/\1/p' "$CONFIG")
echo "package.consumer: installed to $PREFIX (source_sha $SOURCE_SHA, with_aeron $WITH_AERON)"

COMMON=(-DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER="$(cache CMAKE_C_COMPILER)" -DCMAKE_CXX_COMPILER="$(cache CMAKE_CXX_COMPILER)")

# 2. the consumer, pinned to the sha the package says it was built from
"$CMAKE" -S "$HERE" -B "$WORK/consumer" "${COMMON[@]}" -DCT_EXPECT_SOURCE_SHA="$SOURCE_SHA" \
    > "$WORK/consumer-configure.log" 2>&1 || { cat "$WORK/consumer-configure.log" >&2; fail "the consumer did not configure against the package"; }
"$CMAKE" --build "$WORK/consumer" -j8 > "$WORK/consumer-build.log" 2>&1 \
    || { cat "$WORK/consumer-build.log" >&2; fail "the consumer did not build against the package"; }
"$WORK/consumer/pure_consumer" | tee "$WORK/pure.out"
grep -q '^pure consumer OK$' "$WORK/pure.out" || fail "pure_consumer did not pass"
if [ "$WITH_AERON" = "TRUE" ]; then
    [ -x "$WORK/consumer/aeron_consumer" ] || fail "the package has the Aeron half but aeron_consumer was not built"
    mkdir -p "$WORK/driver"
    "$WORK/consumer/aeron_consumer" "$WORK/driver" | tee "$WORK/aeron.out"
    grep -q '^aeron consumer OK' "$WORK/aeron.out" || fail "aeron_consumer did not pass"
fi

# 3. controls: each must fail, with its text
if "$CMAKE" -S "$HERE" -B "$WORK/control-component" "${COMMON[@]}" -DCONSUMER_EXTRA_COMPONENTS=no_such_component \
    > "$WORK/control-component.log" 2>&1; then
    fail "control: a component the package does not have was found"
fi
logHas "$WORK/control-component.log" "cluster_tools component 'no_such_component' is not in this package" \
    || { cat "$WORK/control-component.log" >&2; fail "control: the missing component was not named"; }
if "$CMAKE" -S "$HERE" -B "$WORK/control-pin" "${COMMON[@]}" -DCT_EXPECT_SOURCE_SHA=0000000000000000000000000000000000000000 \
    > "$WORK/control-pin.log" 2>&1; then
    fail "control: a package built from another commit was accepted"
fi
logHas "$WORK/control-pin.log" "the consumer pins 0000000000000000000000000000000000000000" \
    || { cat "$WORK/control-pin.log" >&2; fail "control: the pin mismatch was not named"; }

echo "package.consumer: PASS"
