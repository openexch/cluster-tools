# syntax=docker/dockerfile:1
# SPDX-License-Identifier: Apache-2.0
#
# The cluster-tools runtime image: the published binary set (aeronmd and its two libraries, the driver properties,
# ct-bridge, ct-forwarder, ct-genesis, ...) on the distribution the set is built for, with the launchers from
# deploy/bin. Nothing is compiled here: the image wraps the published artifacts, as a host install does.
#
# Build context: a directory holding exactly the published set (the files of the publish workflow's FILES list).
# The deploy/ directory of this repo is a named context:
#
#   docker build -f deploy/oci/Dockerfile --build-context deploy=deploy -t openexchange/cluster-tools <set-dir>
#
# Run (read-only root, non-root; the driver directory on shared memory):
#   driver:  docker run --read-only --tmpfs /tmp --shm-size 1g -e AERON_DIR=/dev/shm/aeron-core \
#                -e AERON_DRIVER_PROFILE=prod <image> /opt/openexchange/cluster-tools/bin/aeronmd-launch run
#   worker:  docker run --read-only --tmpfs /tmp --env-file ct-bridge.env <image> \
#                /opt/openexchange/cluster-tools/bin/worker-launch external ct-bridge
# A process and its external driver share the network namespace (--network container:<driver>) and the driver
# directory (a shared tmpfs volume), and run as the same uid.

# ubuntu:24.04: the binaries need glibc 2.38 and GLIBCXX 3.4.32; libuuid, libstdc++ and taskset are in the base.
ARG BASE_IMAGE=ubuntu:24.04@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b
FROM ${BASE_IMAGE}
ARG OE_UID=10001
RUN groupadd --system --gid "${OE_UID}" openexchange \
    && useradd --system --uid "${OE_UID}" --gid "${OE_UID}" --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin openexchange
COPY --chmod=0555 . /opt/openexchange/cluster-tools/
COPY --from=deploy --chmod=0555 bin/aeronmd-launch bin/worker-launch /opt/openexchange/cluster-tools/bin/
ENV CT_HOME=/opt/openexchange/cluster-tools
USER openexchange
# No default process: the command names it (aeronmd-launch run, worker-launch <mode> <worker>). Without one the
# launcher refuses with its usage (78).
CMD ["/opt/openexchange/cluster-tools/bin/worker-launch"]
