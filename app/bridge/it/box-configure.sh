#!/bin/bash
# RC2 S9c: configure the WITH_AERON=ON WITH_BRIDGE=ON build of this worktree on the build box (section 11), with every
# test build and the integration inputs. Usage: app/bridge/it/box-configure.sh <build-dir> [<harness-jar>]
# Inputs (already on the box, mirrored paths): s11-work/deps (Aeron tarball, sbe jars), the m2 (aeron-all for the
# archive IT), the schemas at the lock's pins from assets-rc2corewt / match-melanewt, and the jars:
#   assets-cluster.jar  assets-rc2corewt (6fcf66b, coordinator decision 2026-10-31)
#   match-cluster.jar   match-melanewt ed98477 (W9 inside)
#   the harness jar     the lock's (5ad2068, feed_harness_sha256) unless a second argument names another
set -euo pipefail
B=/home/emre/Apps/openexchange/.runtime-handoff/20260927-r00
WS=$B/workspace
BUILD=${1:?build dir}
HARNESS=${2:-$WS/assets-s9hvwt/assets-cluster/target/assets-feed-harness.jar}
CMAKE=$B/s11-work/tools/cmake-4.2.3-linux-x86_64/bin/cmake
DEPS=$(FETCH_OFFLINE=1 scripts/fetch-deps.sh "$B/s11-work/deps" | tr '\n' ' ')
SCHEMAS=$(scripts/fetch-schemas.sh "$BUILD-schemas" "$WS/assets-rc2corewt" "$WS/match-melanewt" | tr '\n' ' ')
AERON_ALL=$(ls "$B"/m2/io/aeron/aeron-all/1.53.1/aeron-all-1.53.1.jar)
# shellcheck disable=SC2086
nice -n 10 "$CMAKE" -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_AERON=ON -DWITH_BRIDGE=ON \
    -DCT_INTEGRATION_TESTS=ON -DCT_BRIDGE_TEST_BUILDS=ON -DCT_FORWARDER_TEST_BUILDS=ON $DEPS $SCHEMAS \
    -DCT_FEED_HARNESS_JAR="$HARNESS" \
    -DCT_IT_ASSETS_JAR="$WS/assets-rc2corewt/assets-cluster/target/assets-cluster.jar" \
    -DCT_IT_MATCH_JAR="$WS/match-melanewt/match-cluster/target/match-cluster.jar" \
    -DCT_IT_FORWARDER_MATCH_JAR="$WS/match-melanewt/match-cluster/target/match-cluster.jar" \
    -DCT_IT_AERON_ALL_JAR="$AERON_ALL" \
    -DCT_PARITY_ASSETS_TREE="$WS/assets-rc2corewt"
