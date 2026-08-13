#!/bin/bash
# Runs inside any container with java 21. Mounts: /aeron (ro), /spike.
set -euo pipefail
rm -rf /spike/gen/io_aeron_cluster_codecs /spike/gen/com_match_infrastructure_generated
java -Dsbe.target.language=Cpp -Dsbe.output.dir=/spike/gen \
    -jar /spike/tools/sbe-all-1.39.0.jar \
    /aeron/aeron-cluster/src/main/resources/cluster/aeron-cluster-codecs.xml \
    /spike/order-schema.xml
echo "=== generated: ==="
ls /spike/gen/
