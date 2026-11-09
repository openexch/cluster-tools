# C++ Aeron Cluster client: proof of concept

`poc.cpp` is the first C++ cluster client of this repo, kept as a single file for reference. It connects to a
match cluster (with follower REDIRECT handling), offers one `CreateOrder` through ingress, and decodes the
`OrderStatusBatch` ack off egress. The production client is in [`client/`](../client/).

## Build

It builds as the `ct_poc` target of the `-DWITH_AERON=ON` build (see the repo [README](../README.md)), against
the same generated codecs and Aeron tree as everything else. The send-config proof
(`e2e/scripts/run-proof.sh`) builds it in the `poc/Dockerfile.builder` image and uses it as its
CreateOrder probe.

`poc/scripts/` and `poc/docker-compose.yml` are the original standalone flow. They predate the pinned build
(they expect Aeron 1.52.2 and sbe-all 1.39.0) and are not maintained; use the main build instead.

## Protocol details

The cluster session protocol details below are referenced from `adapters/aeron` and `client/` comments.
Constants are from `poc.cpp`.

1. Session identity is the header fields plus the publication image. Every application message is
   `MessageHeader` (cluster schema) + `SessionMessageHeader{leadershipTermId, clusterSessionId,
   timestamp=0}` (the Java client leaves the timestamp at 0), and it must ride the same publication that
   carried the `SessionConnectRequest`: the leader links the connect's ingress image, so a correct header on
   a fresh publication is not your session.
2. Connect handshake. `version` is a semantic version int (0.3.0 = `768`; only a major mismatch is
   rejected). `responseStreamId` must equal the egress subscription's stream id (102; ingress is 101).
   `responseChannel` must be an address the cluster can route to, never `0.0.0.0` or `localhost`. The three
   variable-length fields are written in declared order: responseChannel, credentials, clientInfo.
3. Nested decode and a silent version gate. Egress application frames are a cluster-schema
   `SessionMessageHeader` wrapping an engine-schema message, each with its own blockLength and version. The
   engine gates ingress on an exact schemaId and version match and drops mismatches silently (a reject
   counter is the only trace). If you get silence, check your schema version first, then the node logs.

Build notes: `aeronmd` links `libaeron_driver.so` dynamically (ship it or set `LD_LIBRARY_PATH`); egress
polling needs a `FragmentAssembler` (large book snapshots fragment); Aeron inside docker needs a real
`--shm-size`.
