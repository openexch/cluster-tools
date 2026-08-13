#!/bin/bash
# Runs INSIDE the ccpoc-client container.
set -uo pipefail
export AERON_DIR=/dev/shm/aeron-poc
export LD_LIBRARY_PATH=/spike/dist
rm -rf "$AERON_DIR"
/spike/dist/aeronmd > /tmp/aeronmd.log 2>&1 &
MD_PID=$!
for i in $(seq 1 50); do
    [ -f "$AERON_DIR/cnc.dat" ] && break
    sleep 0.2
done
if [ ! -f "$AERON_DIR/cnc.dat" ]; then
    echo "FATAL: aeronmd did not start"; cat /tmp/aeronmd.log; exit 3
fi
/spike/dist/poc
RC=$?
echo "POC_EXIT=$RC"
kill $MD_PID 2>/dev/null
exit $RC
