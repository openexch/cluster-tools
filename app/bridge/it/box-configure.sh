#!/bin/bash
# Configure the WITH_AERON=ON WITH_BRIDGE=ON build of this worktree on a build box, with every test build and the
# integration inputs. Usage: app/bridge/it/box-configure.sh <build-dir> [<harness-jar>]
# Inputs (already on the box), each named by the environment:
#   CT_BOX_DEPS        the fetch-deps cache (Aeron tarball, sbe jars), verified offline
#   CT_BOX_JARS        a directory holding core-cluster.jar and core-feed-harness.jar
#   CT_CORE_TREE       a core checkout holding the lock's money_schema_ref
#   CT_MATCH_TREE      a match checkout holding the lock's schema refs, with match-cluster/target/match-cluster.jar
#   CT_AERON_ALL_JAR   aeron-all-1.53.1.jar (the archive IT)
#   CMAKE              the cmake binary (default: cmake on PATH; Aeron 1.53.1 needs >= 3.30)
#   the harness jar    $CT_BOX_JARS/core-feed-harness.jar unless a second argument names another
set -euo pipefail
BUILD=${1:?build dir}
if [ -n "${CT_ASSETS_TREE+set}" ]; then
    echo "FATAL: box-configure.sh refused to start: the environment variable CT_ASSETS_TREE is retired; it is now" \
        "CT_CORE_TREE (rename the setting; the old name is never read)" >&2
    exit 64
fi
JARS=${CT_BOX_JARS:?CT_BOX_JARS is required}
CORE_TREE=${CT_CORE_TREE:?CT_CORE_TREE is required}
MATCH_TREE=${CT_MATCH_TREE:?CT_MATCH_TREE is required}
HARNESS=${2:-$JARS/core-feed-harness.jar}
CMAKE=${CMAKE:-cmake}
DEPS=$(FETCH_OFFLINE=1 scripts/fetch-deps.sh "${CT_BOX_DEPS:?CT_BOX_DEPS is required}" | tr '\n' ' ')
SCHEMAS=$(scripts/fetch-schemas.sh "$BUILD-schemas" "$CORE_TREE" "$MATCH_TREE" | tr '\n' ' ')
AERON_ALL=$(ls "${CT_AERON_ALL_JAR:?CT_AERON_ALL_JAR is required}")
# shellcheck disable=SC2086
nice -n 10 "$CMAKE" -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_AERON=ON -DWITH_BRIDGE=ON \
    -DCT_INTEGRATION_TESTS=ON -DCT_BRIDGE_TEST_BUILDS=ON -DCT_FORWARDER_TEST_BUILDS=ON $DEPS $SCHEMAS \
    -DCT_FEED_HARNESS_JAR="$HARNESS" \
    -DCT_IT_CORE_JAR="$JARS/core-cluster.jar" \
    -DCT_IT_MATCH_JAR="$MATCH_TREE/match-cluster/target/match-cluster.jar" \
    -DCT_IT_FORWARDER_MATCH_JAR="$MATCH_TREE/match-cluster/target/match-cluster.jar" \
    -DCT_IT_AERON_ALL_JAR="$AERON_ALL" \
    -DCT_PARITY_CORE_TREE="$CORE_TREE"
