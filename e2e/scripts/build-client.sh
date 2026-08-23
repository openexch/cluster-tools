#!/bin/bash
# Runs INSIDE the builder container (see poc/Dockerfile.builder). Builds everything the
# proof's client box runs, staging it into /repo/e2e/dist:
#   - aeron: aeronmd + libaeron_static.a (cmake, cached in the /build volume)
#   - switch-behavior: the repo's WITH_AERON=ON cmake build (codecs generated from the
#     aeron checkout + the match order-schema extract at /schema/order-schema.xml)
#   - poc: the flat PoC client compiled UNCHANGED against the same generated codecs —
#     the proof's CreateOrder/OrderStatus probe
# Mounts: /aeron = aeron 1.52.2 checkout (ro), /repo = this repo, /build = named
# volume (cache), /schema = dir holding order-schema.xml (and optionally the sbe jar).
set -euo pipefail

if [ ! -f /build/aeron/.done ]; then
    echo "=== configuring aeron ==="
    cmake -S /aeron -B /build/aeron \
        -DCMAKE_BUILD_TYPE=Release \
        -DAERON_TESTS=OFF \
        -DAERON_SYSTEM_TESTS=OFF \
        -DAERON_SLOW_SYSTEM_TESTS=OFF \
        -DBUILD_AERON_ARCHIVE_API=OFF \
        -DAERON_BUILD_SAMPLES=OFF \
        -DAERON_BUILD_DOCUMENTATION=OFF
    echo "=== building aeronmd + aeron_static ==="
    cmake --build /build/aeron --target aeronmd aeron_static -j"$(nproc)"
    touch /build/aeron/.done
else
    echo "=== aeron build cached, skipping ==="
fi

AERON_LIB=$(find /build/aeron -name 'libaeron_static.a' -type f | head -1)
AERONMD=$(find /build/aeron -name aeronmd -type f | head -1)
# aeronmd links BOTH shared libs dynamically; ship them next to it (LD_LIBRARY_PATH
# in the compose file points at dist/).
AERON_DRIVER_SO=$(find /build/aeron -name 'libaeron_driver.so' -type f | head -1)
AERON_SO=$(find /build/aeron -name 'libaeron.so' -type f | head -1)

SBE_JAR_ARG=""
if [ -f /schema/sbe-all-1.39.0.jar ]; then
    SBE_JAR_ARG="-DSBE_JAR=/schema/sbe-all-1.39.0.jar"
fi

echo "=== building switch-behavior (WITH_AERON=ON) ==="
# shellcheck disable=SC2086 # SBE_JAR_ARG is one optional -D flag
cmake -S /repo -B /build/ct \
    -DWITH_AERON=ON \
    -DAERON_SRC=/aeron \
    -DAERON_LIB="$AERON_LIB" \
    -DMATCH_SCHEMA=/schema/order-schema.xml \
    $SBE_JAR_ARG
cmake --build /build/ct -j"$(nproc)"

echo "=== compiling the poc probe against the same generated codecs ==="
g++ -O2 -std=c++17 -Wall -o /build/ct/poc /repo/poc/poc.cpp \
    -I /aeron/aeron-client/src/main/cpp_wrapper \
    -I /aeron/aeron-client/src/main/c \
    -I /build/ct/generated/sbe \
    "$AERON_LIB" \
    -lpthread -ldl -lm

echo "=== staging into /repo/e2e/dist ==="
mkdir -p /repo/e2e/dist
cp /build/ct/app/switch-behavior/switch-behavior /repo/e2e/dist/
cp /build/ct/poc /repo/e2e/dist/
cp "$AERONMD" /repo/e2e/dist/
cp "$AERON_DRIVER_SO" /repo/e2e/dist/
cp "$AERON_SO" /repo/e2e/dist/
ls -la /repo/e2e/dist/
echo "=== client artifacts staged ==="
