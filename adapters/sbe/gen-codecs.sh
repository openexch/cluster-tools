#!/bin/bash
# Generates C++ SBE codecs for ONE schema into its own directory. Nothing generated is ever
# committed; CMake runs this (adapters/sbe/CMakeLists.txt) once per schema and wires the
# include paths. The sbe-tool + agrona jars are the dependencies.lock pins, verified at
# configure time (cmake/VerifiedDependencies.cmake); this script never downloads anything.
#
# Usage: gen-codecs.sh <output-dir> <stamp-file> <schema.xml> [<language> [<namespace>]]
#        language defaults to Cpp; the Aeron archive C client codecs (cmake/AeronArchive.cmake)
#        are generated as C under the namespace aeron.archive.client
# Env:   SBE_CLASSPATH   - sbe-tool.jar:agrona.jar, the hashed copies in the build tree (required)
#        SBE_JAR_SHA256S - space-separated sha256 values every classpath entry must match
#                          (the lock's); checked before EVERY run, not only at configure
#        JAVA            - java executable (default: java)
set -euo pipefail

OUT_DIR=$1
STAMP=$2
SCHEMA=$3
LANGUAGE=${4:-Cpp}
NAMESPACE=${5:-}
: "${SBE_CLASSPATH:?SBE_CLASSPATH is required}"
: "${SBE_JAR_SHA256S:?SBE_JAR_SHA256S is required}"
JAVA_BIN=${JAVA:-java}

IFS=: read -r -a JARS <<< "$SBE_CLASSPATH"
for jar in "${JARS[@]}"; do
    got=$(sha256sum "$jar" | cut -d' ' -f1)
    case " $SBE_JAR_SHA256S " in
        *" $got "*) ;;
        *) echo "FATAL: $jar (sha256 $got) is not a jar dependencies.lock pins; refusing to run it" >&2; exit 1 ;;
    esac
done

# Drop stale output before regen: a template removed from the schema must not survive as a
# header in the build tree.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
NS_ARGS=()
if [ -n "$NAMESPACE" ]; then NS_ARGS=(-Dsbe.target.namespace="$NAMESPACE"); fi
"$JAVA_BIN" --add-opens java.base/jdk.internal.misc=ALL-UNNAMED \
    -Dsbe.target.language="$LANGUAGE" "${NS_ARGS[@]}" \
    -Dsbe.output.dir="$OUT_DIR" \
    -Dsbe.validation.stop.on.error=true \
    -cp "$SBE_CLASSPATH" uk.co.real_logic.sbe.SbeTool "$SCHEMA"
touch "$STAMP"
