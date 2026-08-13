#!/bin/bash
# Runs INSIDE the builder container.
# Mounts: /aeron = aeron-src (ro), /spike = spike dir, /build = named volume (cache).
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

AERONMD=$(find /build/aeron -name aeronmd -type f | head -1)
LIBAERON=$(find /build/aeron -name 'libaeron_static.a' -type f | head -1)
echo "aeronmd:  $AERONMD"
echo "libaeron: $LIBAERON"
cp "$AERONMD" /spike/dist/aeronmd
cp "$LIBAERON" /spike/dist/libaeron_static.a
file /spike/dist/aeronmd
echo "=== aeron artifacts staged in /spike/dist ==="
