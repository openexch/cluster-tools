#!/bin/bash
# Generates C++ SBE codecs into a build directory. Nothing generated is ever committed;
# CMake runs this (adapters/sbe/CMakeLists.txt) and wires the include path.
# Adapted from poc/scripts/gen-codecs.sh.
#
# Usage: gen-codecs.sh <output-dir> <schema.xml> [<schema.xml>...]
# Env:   SBE_JAR — path to the sbe-all fat jar; downloaded to <output-dir>/.cache when
#                  unset or missing. NOTE: the runnable artifact is sbe-all, not
#                  sbe-tool-*-all (see poc/README.md).
#        JAVA    — java executable (default: java).
set -euo pipefail

SBE_VERSION=1.39.0

OUT_DIR=$1
shift

JAVA_BIN=${JAVA:-java}
JAR=${SBE_JAR:-}
if [ -z "$JAR" ] || [ ! -f "$JAR" ]; then
    JAR="$OUT_DIR/.cache/sbe-all-$SBE_VERSION.jar"
    if [ ! -f "$JAR" ]; then
        mkdir -p "$OUT_DIR/.cache"
        curl -sfL \
            "https://repo1.maven.org/maven2/uk/co/real-logic/sbe-all/$SBE_VERSION/sbe-all-$SBE_VERSION.jar" \
            -o "$JAR.tmp"
        mv "$JAR.tmp" "$JAR"
    fi
fi

# Only the cluster-protocol schema is generated today; drop stale output before regen.
rm -rf "$OUT_DIR/io_aeron_cluster_codecs"

"$JAVA_BIN" -Dsbe.target.language=Cpp -Dsbe.output.dir="$OUT_DIR" -jar "$JAR" "$@"
