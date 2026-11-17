#!/bin/bash
# package.archive_consumer: install this build as a package and link the archive follow library from a separate
# CMake project, as another repository does. Pass = every step below holds; each failure names its step.
#
#   1. `cmake --install <build> --prefix <work>/prefix` succeeds and the prefix holds the package config.
#   2. tests/package-archive-consumer configures against the prefix ONLY, pinned to the build's source sha, builds,
#      and archive_pure_consumer runs (archive_core, every configuration).
#   3. When the package has archive_aeron and an aeron-all jar is given: a real Java archive (archive/it's
#      JournalArchiveFixture) records a core order journal of a previous epoch, then one of the expected epoch,
#      and archive_tail_consumer tails it through the package: the expected epoch's records in order, the other
#      recording counted foreign, the archive id in the key. Without a jar the step says SKIP.
#   4. Control: a package whose export lacks archive_aeron must FAIL the consumer's configure, naming the component.
#      (A package built without WITH_BRIDGE lacks it already; otherwise the control removes it from a copy.)
#
# usage: check-archive-package.sh <cmake> <build-dir> <work-dir> [<aeron-all.jar>]
set -euo pipefail

if [ $# -lt 3 ] || [ $# -gt 4 ]; then
    echo "usage: $0 <cmake> <build-dir> <work-dir> [<aeron-all.jar>]" >&2
    exit 64
fi
CMAKE=$1
BUILD=$(cd "$2" && pwd)
WORK=$3
JAR=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

fail() { echo "package.archive_consumer: FAIL: $*" >&2; exit 1; }
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
echo "package.archive_consumer: installed to $PREFIX (source_sha $SOURCE_SHA, components $BUILT)"
HAS_AERON=0
case ";$BUILT;" in *";archive_aeron;"*) HAS_AERON=1 ;; esac

COMMON=(-DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER="$(cache CMAKE_C_COMPILER)" -DCMAKE_CXX_COMPILER="$(cache CMAKE_CXX_COMPILER)")

# 2. the consumer
"$CMAKE" -S "$HERE" -B "$WORK/consumer" "${COMMON[@]}" -DCT_EXPECT_SOURCE_SHA="$SOURCE_SHA" \
    > "$WORK/consumer-configure.log" 2>&1 || { cat "$WORK/consumer-configure.log" >&2; fail "the consumer did not configure against the package"; }
"$CMAKE" --build "$WORK/consumer" -j8 > "$WORK/consumer-build.log" 2>&1 \
    || { cat "$WORK/consumer-build.log" >&2; fail "the consumer did not build against the package"; }
"$WORK/consumer/archive_pure_consumer" | tee "$WORK/pure.out"
grep -q '^archive pure consumer OK$' "$WORK/pure.out" || fail "archive_pure_consumer did not pass"

# 3. the tail against a real archive
if [ "$HAS_AERON" = 1 ]; then
    [ -x "$WORK/consumer/archive_tail_consumer" ] || fail "the package has archive_aeron but archive_tail_consumer was not built"
    if [ -z "$JAR" ]; then
        echo "package.archive_consumer: SKIP the fixture-archive tail: no aeron-all jar given"
    else
        mkdir -p "$WORK/classes" "$WORK/driver"
        javac -proc:none -d "$WORK/classes" -cp "$JAR" "$ROOT/archive/it/JournalArchiveFixture.java" \
            > "$WORK/javac.log" 2>&1 || { cat "$WORK/javac.log" >&2; fail "the fixture does not compile"; }
        PORT=$((19600 + RANDOM % 300))
        SHM="/dev/shm/ct-pkg-archive-$$"
        mkfifo "$WORK/fixture.in"
        java --add-opens java.base/jdk.internal.misc=ALL-UNNAMED -Xmx256m -cp "$JAR:$WORK/classes" JournalArchiveFixture \
            "$SHM" "$WORK/archive" "$PORT" "$ROOT/archive/tests/fixtures/mejournal" 20 \
            < "$WORK/fixture.in" > "$WORK/fixture.out" 2> "$WORK/fixture.err" &
        JPID=$!
        exec 3> "$WORK/fixture.in"
        cleanup() { echo QUIT >&3 2>/dev/null || true; exec 3>&- || true; wait "$JPID" 2>/dev/null || true; rm -rf "$SHM"; }
        trap cleanup EXIT
        answers() { grep -c . "$WORK/fixture.out" 2>/dev/null || echo 0; }
        ask() { # command -> waits for its answer line
            local before
            before=$(answers)
            echo "$1" >&3
            for _ in $(seq 1 600); do
                [ "$(answers)" -gt "$before" ] && break
                sleep 0.1
            done
            local answer
            answer=$(tail -1 "$WORK/fixture.out")
            case "$answer" in ERROR*|"") cat "$WORK/fixture.err" >&2; fail "fixture: $1 -> ${answer:-no answer}" ;; esac
        }
        for _ in $(seq 1 600); do grep -q '^READY' "$WORK/fixture.out" 2>/dev/null && break; sleep 0.1; done
        grep -q '^READY' "$WORK/fixture.out" || { cat "$WORK/fixture.err" >&2; fail "the fixture archive did not start"; }
        ask "RECORD"                    # a previous genesis: epoch 3
        for seq in 1 2; do ask "HEX $("$WORK/consumer/archive_pure_consumer" --core-frame-hex "$seq" 3)"; done
        ask "STOP"
        ask "RECORD"                    # this genesis: epoch 9
        for seq in 1 2 3; do ask "HEX $("$WORK/consumer/archive_pure_consumer" --core-frame-hex "$seq" 9)"; done
        "$WORK/consumer/archive_tail_consumer" "$WORK/driver" "127.0.0.1:$PORT" 4001 3 9 2> "$WORK/tail.err" | tee "$WORK/tail.out" \
            || { cat "$WORK/tail.err" >&2; fail "archive_tail_consumer failed"; }
        grep -q '^archive tail consumer OK frames=3 archive=[0-9]* foreign=1$' "$WORK/tail.out" \
            || { cat "$WORK/tail.err" >&2; fail "archive_tail_consumer did not read the expected epoch only"; }
        cleanup
        trap - EXIT
    fi
fi

# 4. control: archive_aeron absent from the export
CONTROL_PREFIX="$PREFIX"
if [ "$HAS_AERON" = 1 ]; then
    CONTROL_PREFIX="$WORK/prefix-without-archive-aeron"
    cp -a "$PREFIX" "$CONTROL_PREFIX"
    python3 -I - "$CONTROL_PREFIX" <<'PY' || fail "control: the archive_aeron target could not be removed from the copy"
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
removed = 0
for f in root.rglob("cluster_toolsTargets*.cmake"):
    text = f.read_text()
    # every blank-line separated stanza that names the target goes, and the name leaves the expected-target lists
    stanzas = re.split(r"\n\s*\n", text)
    kept = [s for s in stanzas if "cluster_tools::archive_aeron" not in s or "foreach(_cmake_expected_target" in s]
    removed += len(stanzas) - len(kept)
    text = "\n\n".join(kept).replace("cluster_tools::archive_aeron", "")
    f.write_text(text)
sys.exit(0 if removed > 0 else 1)
PY
fi
if "$CMAKE" -S "$HERE" -B "$WORK/control-component" -DCMAKE_PREFIX_PATH="$CONTROL_PREFIX" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$(cache CMAKE_C_COMPILER)" -DCMAKE_CXX_COMPILER="$(cache CMAKE_CXX_COMPILER)" -DREQUIRE_ARCHIVE_AERON=ON \
    > "$WORK/control-component.log" 2>&1; then
    fail "control: a package without archive_aeron was accepted"
fi
logHas "$WORK/control-component.log" "cluster_tools component 'archive_aeron' is not in this package" \
    || { cat "$WORK/control-component.log" >&2; fail "control: the missing archive_aeron component was not named"; }
echo "package.archive_consumer: control: archive_aeron missing from the export -> configure fails naming it"

echo "package.archive_consumer: PASS"
