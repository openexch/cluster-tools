# cluster-tools

C++ tools that speak the Aeron Cluster protocol used by the openexchange engines.

Aeron ships no cluster *client* outside Java — the official C/C++ APIs cover transport
and archive only. The cluster session protocol itself, however, is SBE-defined and
language-neutral, so a client can be written against the C++ wrapper directly. This
repo is where those tools live.

## Contents

- **[`client/`](client/)** — the production C++ cluster client (RC2 S11) the forwarder and
  the C++ bridge share: a non-blocking state machine mirroring
  `io.aeron.cluster.client.AeronCluster` 1.53.1 (async connect with credentials, follower
  REDIRECT, bounded retries with backoff, leader-change follow with the new term stamped,
  egress-image loss, 1 s keepalive from the duty cycle, typed close reasons), a C++20
  `ClusterTransport` concept with the Aeron implementation, the core `RegisterSession`
  registrar (AdminAck re-send, re-declare on every new leader), two clusters on one Aeron
  client, and exit 70 on a fatal Aeron client error.
- **[`poc/`](poc/)** — a minimal (~470 LOC) C++ cluster client, proven against a
  3-node dockerized match cluster: session connect (with follower REDIRECT handling),
  an order submitted through ingress, and the OrderStatus ack decoded off egress.
  Start here; its README documents the protocol details the hard way taught us.

## Architecture

The PoC is deliberately a flat single file — it is evidence, not a tool. Real tools
in this repo follow a ports-and-adapters (hexagonal) layout with the domain kept
pure: `domain/` holds logic that knows nothing about Aeron or SBE, `ports/` defines
the seams, `adapters/` implements them (Aeron transport, SBE codecs), and each tool
is a thin `app/` composing the three. On hot paths ports are compile-time
(templates/concepts); on operator-tool cold paths a small virtual interface is fine.

The tree:

```
domain/               pure decision logic — no Aeron, no SBE, no I/O
  SwitchDecision      the behaviour-versioning switch decision table, + doctest tests
  EngineConfig        the engine-creation config value type + validation mirroring the
                      engine's own rules (match EngineConfigState), + doctest tests
client/               the shared runtime cluster client (see Contents); pure parts
                      (endpoints, credentials, backoff, settings) build without aeron,
                      client/aeron holds the transport + fatal-error handling, client/it
                      the real-cluster integration scenarios (run-it.sh)
ports/                the seams (header-only)
  ClusterSession      connect / offer / poll / close — virtual, cold-path (the header
                      documents why, and why hot-path ports would be templates/concepts)
  Result              tiny expected-style carrier; no exceptions cross a port boundary
adapters/aeron/       AeronClusterSession — the cluster session protocol over the
                      official aeron cpp_wrapper (the only code in the repo that
                      includes an aeron header)
adapters/json/        the engine-config file input adapter: a strict, hand-rolled,
                      schema-directed parser (no dependencies, no transport — builds
                      and tests in the WITH_AERON=OFF configuration)
adapters/sbe/         codegen wiring + the EngineConfig encoder: cluster-protocol and
                      match order-schema C++ codecs are generated into the build dir
                      (nothing generated is committed); the encoder is the only TU
                      outside the generated tree that includes a match codec header
app/switch-behavior/  the operator CLI; subcommands `probe` and `send-config` work today
third_party/doctest/  vendored single-header test framework (MIT)
poc/                  the original flat proof of concept, kept untouched as reference
e2e/                  dockerized end-to-end proof harnesses (config-mode cluster)
```

The build matrix:

| configuration | needs | builds | proves |
|---|---|---|---|
| `-DWITH_AERON=OFF` (default, `ci.yml`) | cmake >= 3.16 + a C++20 compiler | domain, ports, the json adapter, the pure client parts, their tests | the hexagon: domain logic, input parsing and the client's pure rules build and pass with zero infrastructure |
| `-DWITH_AERON=ON` (`aeron.yml`, `publish.yml`) | cmake >= 3.30, java 21, the verified inputs below | everything: aeron 1.53.1 client + aeronmd from source, codecs, the client and its tests, switch-behavior, the PoC | the client state machine (fake transport), the codec goldens against the engines' Java encoders, the transport against the real wire schema |

Every pinned input of the Aeron build is in `dependencies.lock` and nothing is trusted by
name or path: `scripts/fetch-deps.sh` downloads the Aeron 1.53.1 source tarball, sbe-tool
1.40.1 and agrona 2.5.0 and checks each sha256; `scripts/fetch-schemas.sh` extracts the
money-schema and the order-schema at the pinned commits and checks their sha256. Configure
then re-hashes the Aeron tarball (`-DAERON_TARBALL`) and extracts it itself (a source tree is
not accepted as input), re-hashes the SBE jars and runs codegen only from hashed copies in the
build tree (re-hashed before every run), and checks each schema's id, version floor and
required templates. `scripts/check-configure-gate.sh` proves each refusal.

```bash
# domain + ports + json adapter + pure client + tests: no aeron, no java:
cmake -S . -B build -DWITH_AERON=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# full build:
scripts/fetch-deps.sh ~/.cache/ct-deps > deps.args
scripts/fetch-schemas.sh ~/.cache/ct-schemas /path/to/assets /path/to/match > schemas.args
cmake -S . -B build-aeron -DWITH_AERON=ON $(cat deps.args schemas.args)
cmake --build build-aeron -j
ctest --test-dir build-aeron --output-on-failure

# real-cluster integration (3-node core + 1-node ME on this host, kills included):
client/it/run-it.sh build-aeron /path/to/assets-cluster.jar /path/to/match-cluster.jar
```

A newer wire schema (money v7, order v12) drops in by pointing `-DMONEY_SCHEMA` /
`-DMATCH_SCHEMA` at it (or bumping the `*_ref`/`*_sha256` pins); the codec goldens then fail
until `adapters/sbe/tests/golden/regen.sh` is re-run with engine jars at the new commits,
which is the drift alarm working. `-DME_JOURNAL_SCHEMA=` adds the ME journal codecs (RC2 S1).

## License

Apache-2.0.

## Artifacts

Every push to main publishes arm64 Linux binaries to the org's artifact pipeline
(`cluster-tools/{sha}/` — see `.github/workflows/publish.yml`): `switch-behavior`,
`aeronmd`, `libaeron_driver.so`, `libaeron.so`, plus a `manifest.json` with per-file
sha256. Run them with `LD_LIBRARY_PATH` pointing at the artifact directory; nothing
is ever compiled on a deployment box.
