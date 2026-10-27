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
- **[`archive/`](archive/)**: the archive side of the C++ settlement bridge (RC2 S9a): the
  schema 4 frame admission gate and common-head peek, the recording chain model, the follow
  state machine with the source-death detectors (pure, with negative controls), the per-source
  resume memo keyed on `journalSeq`, strict settings, and `archive/aeron` (`WITH_BRIDGE`): the
  Aeron C archive client built from the verified tarball, one `ArchiveSource` per archive, the
  `SourceSet` with one source attached at a time. `archive/it` runs it against a real Java
  Aeron Archive.
- **[`app/journal-tail/`](app/journal-tail/)**: `ct-journal-tail`, see below.
- **[`domain/forwarder/`](domain/forwarder/) + [`app/forwarder/`](app/forwarder/)**: `ct-forwarder`, the production
  forwarder (RC2 S12), see below. The pure core (classification and the INVALID carrier, the send ring, the resync
  machine with its 10,000-seed property test, the epoch binder, the admission watcher) builds in every configuration;
  the codec is in `adapters/sbe`, the agent and the binary need `WITH_AERON`.
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
| `-DWITH_CODECS=ON` (`codecs` job in `ci.yml`) | cmake >= 3.21, java 21, the pinned sbe-tool/agrona jars, the three wire schemas | SBE codegen for money-schema v7, order-schema v12 and ME journal schema 4, the EngineConfig codec, the codec layout goldens | the wire contracts without Aeron: codegen, schema pins, the Java-encoder goldens |
| `-DWITH_AERON=ON -DWITH_BRIDGE=ON` (`aeron.yml`) | as `WITH_AERON=ON`; the money and journal schemas must equal the lock's sha256 exactly | the Aeron C archive client + cpp_wrapper (archive codecs generated with the pinned sbe), `archive/aeron`, `ct-journal-tail`, the archive integration driver | the archive side against the generated schema 4 codecs; with `CT_INTEGRATION_TESTS=ON` and `-DCT_IT_AERON_ALL_JAR` the 9.3 scenarios against a real archive |
| `-DWITH_AERON=ON` (`aeron.yml`, `publish.yml`) | cmake >= 3.30, java 21, the verified inputs below | everything: aeron 1.53.1 client + aeronmd from source, codecs, the client and its tests, switch-behavior, the PoC | the client state machine (fake transport), the codec goldens against the engines' Java encoders, the transport against the real wire schema |

Every pinned input of the Aeron build is in `dependencies.lock` and nothing is trusted by
name or path: `scripts/fetch-deps.sh` downloads the Aeron 1.53.1 source tarball, sbe-tool
1.40.1 and agrona 2.5.0 and checks each sha256; `scripts/fetch-schemas.sh` extracts the
money-schema, the order-schema and the ME journal schema (schemaId 4) at the pinned commits and checks their sha256. Configure
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

# the bridge's archive side and its integration run against a real Java archive:
scripts/fetch-deps.sh ~/.cache/ct-deps --it > deps.args   # adds the pinned aeron-all jar
cmake -S . -B build-bridge -DWITH_AERON=ON -DWITH_BRIDGE=ON $(cat deps.args schemas.args)
cmake --build build-bridge -j
archive/it/run-it.sh build-bridge ~/.cache/ct-deps/aeron-all-1.53.1.jar
```

### ct-forwarder

A stateless C++20 process (RC2 S12): one resumable FORWARDER session on the core, one FORWARDER session on the ME.
Every pending core `MeDispatch` becomes the ME's `DurableOrderCommand` 9 or `ReleaseCommandIds` 10, byte for byte
the minted values (a dispatch the ME would drop unjournaled travels as the INVALID carrier, so only the ME ends an
order); it resyncs from the core (`RequestPendingDispatch`, a head probe every 10 s) after anything that may have lost a
frame: registration, either cluster's new leader, a live gap, a GLOBAL admission change, a shape violation, an
overflow, an operator `SIGHUP` / `POST /resync`. Nothing is written to disk; a restart is a cold start.

```bash
env FWD_CORE_INGRESS=0=10.0.0.1:9002,1=10.0.0.2:9002,2=10.0.0.3:9002 \
    FWD_CORE_RESPONSE_CHANNEL='aeron:udp?endpoint=10.0.0.9:0' AE_AUTH_CREDENTIAL=6:FORWARDER:<64 hex> \
    FWD_ME_INGRESS=0=10.0.1.1:9002,1=10.0.1.2:9002,2=10.0.1.3:9002 \
    FWD_ME_RESPONSE_CHANNEL='aeron:udp?endpoint=10.0.0.9:0' ME_AUTH_CREDENTIAL=6:FORWARDER:<64 hex> \
    FWD_INGRESS_CHANNEL='aeron:udp?term-length=4m|mtu=8192' AERON_DIR=/dev/shm/aeron FWD_HTTP_BIND=127.0.0.1:9600 \
    ct-forwarder
```

Every variable is required (a missing one, a non-FORWARDER credential or two equal fixed-port response channels exit
78). GET `/health` (200 UP / 503 STALLED), `/ready` (200, or 503 naming the failing conjuncts), `/metrics`
(Prometheus; alert rules in `app/forwarder/alerts.yml`), POST `/resync`. Exit codes: 0 stopped by SIGTERM/SIGINT;
70 process health (a fatal Aeron error, an escaped exception or broken invariant, the 5 s duty watchdog, the 2 s join
bound); 75 an inferred refusal (restart allowed); 78 configuration or an observed refusal: the unit sets
`RestartPreventExitStatus=78`, `RestartSec >= 2s` and a start limit. The T1 integration run is
`app/forwarder/it/run-forwarder-it.sh <build> <assets-cluster.jar> <match-cluster.jar>` (3 AE + 3 ME + 2 forwarders,
the negative-control binaries need `-DCT_FORWARDER_TEST_BUILDS=ON`; they are never published).

### ct-journal-tail

Reads the ME journal (schema 4) from the configured archives through `archive/`, without a core
session, and prints it. It reads only: no file is written and no core is contacted. It is the
tool the core's SOURCE_SCHEMA runbook uses to look at journal entry `s`:

```bash
ct-journal-tail --aeron-dir /dev/shm/aeron --archive 10.0.0.1:9010 --control-stream 4010 \
    --journal-stream 4001 --channel aeron:ipc --replay-stream 9101 --local-host 10.0.0.9 \
    --journal-mtu 1408 --from-seq 1234 --to-seq 1234 --hex
```

Every admitted frame in `[--from-seq, --to-seq]` prints as `FRAME seq=... template=... epoch=...
egress=... recording=... end=... length=...` (with `hex=` under `--hex`); every refused frame
prints as `REFUSED <reason> <position> <length> <hex>` and the tail goes on with the next frame
(a refused frame without a seq is selected by its position). `--cursor C --epoch E` seeds the
resume memo as a FeedPositionReport would. Exit 0 after `--to-seq`, 75 on `--timeout-ms`, 78 on
a usage or settings error, 70 on any fatal error (never 134).

The pins today are money-schema v7 (assets `b05b8e8`), order-schema v12 and ME journal schema 4 v1
(both match `68a9375`), the six ME journal golden fixtures of match's `MeJournalFixtureDump`
(`archive/tests/fixtures/mejournal`, source commit and `SHA256SUMS` hash in the lock, verified at
every configure) and, for the archive integration fixture only, aeron-all 1.53.1. A newer engine wire schema drops in by pointing `-DMONEY_SCHEMA` /
`-DMATCH_SCHEMA` at it (or bumping the `*_ref`/`*_sha256` pins); the codec goldens then fail
until `adapters/sbe/tests/golden/regen.sh` is re-run with engine jars at the new commits,
which is the drift alarm working. The ME journal schema is pinned by exact sha256 (a fixed
on-disk contract), and `-DME_JOURNAL_SCHEMA=` is required with `WITH_CODECS` / `WITH_AERON`.

## License

Apache-2.0.

## Artifacts

Every push to main publishes arm64 Linux binaries to the org's artifact pipeline
(`cluster-tools/{sha}/` — see `.github/workflows/publish.yml`): `switch-behavior`,
`aeronmd`, `libaeron_driver.so`, `libaeron.so`, plus a `manifest.json` with per-file
sha256. Run them with `LD_LIBRARY_PATH` pointing at the artifact directory; nothing
is ever compiled on a deployment box.
