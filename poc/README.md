# C++ Aeron Cluster client — proof of concept

A ~470-line C++ client (built on Aeron's official `cpp_wrapper`) that performs a full
round-trip against a 3-node [match](https://github.com/openexch/match) cluster:

1. **Connect**: `SessionConnectRequest` to every member's ingress; decodes follower
   `SessionEvent code=REDIRECT` and adopts the leader's `code=OK`
   (clusterSessionId + leadershipTermId).
2. **Ingress**: one schema-v9 `CreateOrder`, framed with the cluster
   `SessionMessageHeader`.
3. **Egress**: decodes the `OrderStatusBatch` ack (nested cluster-schema over
   match-schema frames), plus book snapshots/heartbeats as they stream by.

## Running it

Everything runs in docker; nothing is installed on the host.

1. Shallow-clone Aeron at the tag the engines embed (`1.52.2`) next to this dir.
2. Build the server jar from [match](https://github.com/openexch/match):
   `mvn -pl match-cluster -am package` → drop `match-cluster.jar` here.
3. Fetch the SBE fat jar (note: the runnable artifact is **`sbe-all`**, not
   `sbe-tool-*-all`): `uk.co.real-logic:sbe-all:1.39.0` → `tools/`.
4. `scripts/build-aeron.sh` (cmake, cached in a docker volume) →
   `scripts/gen-codecs.sh` (C++ codecs for the cluster protocol + the match wire
   schema) → `scripts/compile-poc.sh`.
5. `docker compose up -d me0 me1 me2`, wait for the election, then run the client
   container (`scripts/run-client.sh` starts `aeronmd` + the poc; see the compose
   file for the network layout).

Success output ends with:

```
PASS: connect + ingress CreateOrder + egress OrderStatus round-trip on schema v9
```

## Protocol details worth knowing (learned the hard way)

1. **Session identity is (header fields + publication image).** Every app message is
   `MessageHeader(cluster schema) + SessionMessageHeader{leadershipTermId, clusterSessionId,
   timestamp=0}` — the Java client leaves timestamp at 0 — AND must ride the **same
   publication that carried the `SessionConnectRequest`**: the leader links the connect's
   ingress image, so a correct header on a fresh publication is not your session.
2. **Connect handshake exactness.** `version` is a semantic version int
   (0.3.0 → `768`; only a major mismatch is rejected); `responseStreamId` must equal
   the egress subscription's streamId (102); `responseChannel` must be a routable IP
   (a container IP — never `0.0.0.0`/`localhost`); the three variable-length fields are
   written in declared order (responseChannel, credentials, clientInfo).
3. **Nested double-decode and a silent version gate.** Egress app frames are a
   cluster-schema `SessionMessageHeader` wrapping an engine-schema message, each with
   its own acting blockLength/version. The engine gates ingress on an exact
   schemaId/version match and **drops mismatches silently** (a reject counter is the
   only trace) — if you get silence, suspect your schema version first, then read the
   node logs.

Build footnotes: aeron 1.52.2 requires cmake ≥ 3.30 (ubuntu 24.04 ships 3.28 — the
builder image installs 3.31); `aeronmd` dynamically links `libaeron_driver.so` (ship it
or set `LD_LIBRARY_PATH`); the poc links `libaeron_static.a` + pthread/dl/m; egress
polling needs a `FragmentAssembler` (large snapshots fragment); anything Aeron inside
docker wants a real `--shm-size`.
