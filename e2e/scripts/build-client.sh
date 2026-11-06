#!/bin/bash
# Runs INSIDE the builder container (see poc/Dockerfile.builder). Builds everything the
# proof's client box runs, staging it into /repo/e2e/dist, in ONE cmake build of this repo:
#   - aeron 1.53.1 from the tarball configure verifies and extracts (aeronmd + its shared
#     libraries + the C client)
#   - switch-behavior (codecs generated from the aeron tree + the schema extracts)
#   - ct_poc: the flat PoC client compiled UNCHANGED against the same generated codecs -
#     the proof's CreateOrder/OrderStatus probe
# Mounts: /deps = scripts/fetch-deps.sh cache (ro), /repo = this repo, /build = named
# volume (cache), /schema = dir holding order-schema.xml, money-schema.xml and me-journal-schema.xml.
set -euo pipefail

echo "=== building (WITH_AERON=ON, verified pins) ==="
cmake -S /repo -B /build/ct \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_AERON=ON \
    -DAERON_TARBALL=/deps/aeron-1.53.1.tar.gz \
    -DSBE_CLASSPATH=/deps/sbe-tool-1.40.1.jar:/deps/agrona-2.5.0.jar \
    -DMONEY_SCHEMA=/schema/money-schema.xml \
    -DMATCH_SCHEMA=/schema/order-schema.xml \
    -DME_JOURNAL_SCHEMA=/schema/me-journal-schema.xml
cmake --build /build/ct -j"$(nproc)"

AERONMD=$(find /build/ct/aeron -name aeronmd -type f | head -1)
# aeronmd links BOTH shared libs dynamically; ship them next to it (LD_LIBRARY_PATH
# in the compose file points at dist/).
AERON_DRIVER_SO=$(find /build/ct/aeron -name 'libaeron_driver.so' -type f | head -1)
AERON_SO=$(find /build/ct/aeron -name 'libaeron.so' -type f | head -1)

echo "=== staging into /repo/e2e/dist ==="
mkdir -p /repo/e2e/dist
cp /build/ct/app/switch-behavior/switch-behavior /repo/e2e/dist/
cp /build/ct/poc/ct_poc /repo/e2e/dist/poc
cp "$AERONMD" /repo/e2e/dist/
cp "$AERON_DRIVER_SO" /repo/e2e/dist/
cp "$AERON_SO" /repo/e2e/dist/
ls -la /repo/e2e/dist/
echo "=== client artifacts staged ==="
