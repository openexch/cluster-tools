# cluster-tools

C++20 processes and tools that speak the Aeron Cluster protocol of the Open Exchange engines.

Status: nightly prerelease (v1.0.0-nightly.20261105). Not for production.

User documentation (architecture, configuration, operations): https://openexchange.dev/docs

## Where it fits in v1

v1 is two Java Aeron clusters plus the processes around them:

- [match](https://github.com/openexch/match): the matching engine (ME).
- [core](https://github.com/openexch/core): the order and ledger core (order lifecycle, risk, holds,
  ledger). Called "the core" below.
- [cluster-kit](https://github.com/openexch/cluster-kit): cluster machinery shared by both engines.
- cluster-tools (this repo): the C++ cluster client and the processes that connect the two clusters.
- [tools](https://github.com/openexch/tools): probe and test tooling.

Aeron ships a cluster client only for Java. The cluster session protocol is SBE-defined, so this repo
implements it in C++ on top of the official Aeron C/C++ client.

| Binary | Role |
|---|---|
| `ct-forwarder` | Sends every pending core order dispatch to the ME (`DurableOrderCommand` 9, `ReleaseCommandIds` 10) and turns the ME's book egress into `MarketTick` 60 messages for the core. Stateless. |
| `ct-bridge` | The settlement bridge: reads the ME journal (schema 4) from the ME archives and feeds every record to the core exactly once and in order, or halts. Stateless. |
| `ct-journal-tail` | Read-only operator tool: prints ME journal records from an archive. |
| `switch-behavior` | Operator CLI: `probe` (connect and print egress frames) and `send-config` (send a logged `EngineConfig` to match from a JSON file). |

## Layout

Ports and adapters. The domain is pure (no Aeron, no SBE, no I/O) and builds and tests with no
infrastructure.

```
domain/              switch decision table and EngineConfig validation
domain/forwarder/    forwarder core: classification, send ring, resync machine, book ladder, tick emitter
domain/bridge/       bridge core: cursor table, rotation, halt machine, staging, translation
archive/             bridge archive side: frame admission gate, recording chain model, resume memo
archive/aeron/       Aeron C archive client sources (WITH_BRIDGE)
client/              cluster client: async connect, follower redirect, leader-change follow, keepalive
client/aeron/        Aeron transport for the client
ports/               seams (header-only)
adapters/aeron/      cluster session over the Aeron cpp_wrapper (switch-behavior)
adapters/json/       strict EngineConfig JSON parser
adapters/sbe/        SBE codegen wiring, codecs, Java-encoder goldens
app/common/          process machinery shared by ct-forwarder and ct-bridge (HTTP, supervisor, embedded driver)
app/forwarder/       ct-forwarder
app/bridge/          ct-bridge
app/journal-tail/    ct-journal-tail
app/switch-behavior/ switch-behavior
poc/                 the original single-file cluster client proof of concept (see poc/README.md)
e2e/                 dockerized send-config proof against a config-mode match cluster
third_party/doctest/ vendored test framework (MIT)
```

## Build and test

Every pinned input is in `dependencies.lock` (Aeron 1.53.1, sbe-tool 1.40.1, agrona 2.5.0, the three wire
schemas by commit and sha256). `scripts/fetch-deps.sh` downloads and verifies the jars and the Aeron
tarball; `scripts/fetch-schemas.sh` extracts the schemas from core and match checkouts and verifies their
sha256, so the checkouts must be at `money_schema_ref` and `match_schema_ref`. Configure re-verifies
everything.

| Configuration | Needs | Builds | CI |
|---|---|---|---|
| `-DWITH_AERON=OFF` (default) | cmake >= 3.16, a C++20 compiler | domain, ports, json adapter, pure client, archive, forwarder and bridge cores, their tests | `ci.yml` job `domain` |
| `-DWITH_CODECS=ON` | cmake >= 3.21, java 21, the pinned jars and schemas | SBE codegen, EngineConfig codec, codec goldens | `ci.yml` job `codecs` |
| `-DWITH_AERON=ON` | cmake >= 3.30 | adds Aeron client and `aeronmd` from the verified tarball, `ct-forwarder`, `switch-behavior`, `poc` | |
| `-DWITH_AERON=ON -DWITH_BRIDGE=ON` | adds the pinned feed harness jar (`fetch-deps.sh --harness`) | adds the Aeron archive client, `ct-bridge`, `ct-journal-tail` | `aeron.yml`, `publish.yml` |

```bash
# pure build: no Aeron, no Java
cmake -S . -B build -DWITH_AERON=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure

# full build (what aeron.yml runs). The harness jar comes from the core releases,
# or from a local build named by CT_FEED_HARNESS_JAR (its sha256 must match the lock).
CT_ARTIFACT_BASE_URL=https://github.com/openexch/core \
    scripts/fetch-deps.sh ~/.cache/ct-deps --harness > deps.args
scripts/fetch-schemas.sh ~/.cache/ct-schemas /path/to/core /path/to/match > schemas.args
cmake -S . -B build-full -DWITH_AERON=ON -DWITH_BRIDGE=ON \
    -DCT_BRIDGE_TEST_BUILDS=ON -DCT_FORWARDER_TEST_BUILDS=ON $(cat deps.args schemas.args)
cmake --build build-full -j
ctest --test-dir build-full --output-on-failure
```

`-DCT_BRIDGE_TEST_BUILDS=ON` and `-DCT_FORWARDER_TEST_BUILDS=ON` add negative-control binaries that some
tests need; they are never published. `scripts/check-configure-gate.sh` checks that configure refuses bad
inputs.

Integration runs are local (they need engine jars built at the pinned commits):

```bash
client/it/run-it.sh <build> <core-cluster.jar> <match-cluster.jar>                    # cluster client
scripts/fetch-deps.sh ~/.cache/ct-deps --it                                            # prints -DCT_IT_AERON_ALL_JAR=...
archive/it/run-it.sh <build> <aeron-all.jar> [scenario...]                             # archive side, real Java archive
app/forwarder/it/run-forwarder-it.sh <build> <core-cluster.jar> <match-cluster.jar>    # 3 core + 3 ME + 2 forwarders
app/bridge/it/run-bridge-it.sh <build> <core-cluster.jar> <harness.jar> <drill|all>    # bridge drills
```

`bridge.parity` (registered when `-DCT_PARITY_CORE_TREE` points at a core checkout) checks that every
Java bridge test has a row in `archive/tests/PARITY.md` or `domain/bridge/tests/PARITY.md`.

## Binaries

Both processes take their whole configuration from the environment. Every key is required and has no
default: a missing or malformed key exits 78 with every problem listed. Both offer GET `/health`, `/ready`
(503 names each failing condition) and `/metrics` (Prometheus); alert rules are in `app/forwarder/alerts.yml`
and `app/bridge/alerts.yml`.

Exit codes, both processes: 0 on SIGTERM/SIGINT; 70 process health (dead or stale media driver, wedged
client conductor, duty watchdog, escaped exception, storage starvation for 30 s); 75 a restartable refusal;
78 configuration or an observed refusal. Run them with `RestartPreventExitStatus=78` and `RestartSec >= 2s`.

Driver modes: `external` uses the host `aeronmd` (production); `embedded` starts a driver in the process
(dev and demo only).

### ct-forwarder

One resumable FORWARDER session on the core and one on the ME. Every pending core `MeDispatch` becomes the
ME's `DurableOrderCommand` 9 or `ReleaseCommandIds` 10 with the values the core minted. A dispatch the ME
would drop unjournaled travels as the INVALID carrier, so only the ME ends an order. It resyncs from the core
(`RequestPendingDispatch`, plus a head probe every 10 s) after anything that may have lost a frame:
registration, a new leader on either cluster, a gap, an admission change, an overflow, an operator `SIGHUP`
or `POST /resync`. Nothing is written to disk; a restart is a cold start.

```bash
env FWD_DRIVER_MODE=external AERON_DIR=/dev/shm/aeron FWD_DRIVER_TIMEOUT_MS=5000 FWD_INSTANCE_ID=1 \
    FWD_CORE_INGRESS=0=10.0.0.1:9002,1=10.0.0.2:9002,2=10.0.0.3:9002 \
    FWD_CORE_RESPONSE_CHANNEL='aeron:udp?endpoint=10.0.0.9:0' CORE_AUTH_CREDENTIAL=6:FORWARDER:<64 hex> \
    FWD_ME_INGRESS=0=10.0.1.1:9002,1=10.0.1.2:9002,2=10.0.1.3:9002 \
    FWD_ME_RESPONSE_CHANNEL='aeron:udp?endpoint=10.0.0.9:0' ME_AUTH_CREDENTIAL=7:FORWARDER:<64 hex> \
    FWD_INGRESS_CHANNEL='aeron:udp?term-length=4m|mtu=8192' FWD_HTTP_BIND=127.0.0.1:9600 \
    ct-forwarder
```

| Variable | Rule |
|---|---|
| `FWD_DRIVER_MODE` | `external` or `embedded` |
| `AERON_DIR` | `external` only: an existing directory, the host driver's |
| `FWD_DRIVER_DIR` | `embedded` only: an absolute, existing, writable directory; the driver runs in `<dir>/ct-forwarder-<FWD_INSTANCE_ID>` under a lock file, so a second instance with the same id exits 78; a live driver in the dir exits 78; a stale dir is reclaimed |
| `FWD_DRIVER_TIMEOUT_MS` | 3000..60000: start-up connect wait, driver liveness check and conductor heartbeat threshold |
| `FWD_INSTANCE_ID` | 1..INT64_MAX: the identity on both cluster sessions; keep it fixed per forwarder |
| `FWD_CORE_INGRESS`, `FWD_ME_INGRESS` | `id=host:port,...` of each cluster's members |
| `FWD_CORE_RESPONSE_CHANNEL`, `FWD_ME_RESPONSE_CHANNEL` | `aeron:udp?endpoint=host:port` (port 0 = driver-assigned), never `0.0.0.0`; two equal fixed ports exit 78 |
| `CORE_AUTH_CREDENTIAL`, `ME_AUTH_CREDENTIAL` | `keyId:FORWARDER:secretHex` |
| `FWD_INGRESS_CHANNEL` | the base ingress channel of both clusters |
| `FWD_HTTP_BIND` | `host:port`; port 0 refused |

`embedded` needs `FWD_DRIVER_DIR` on a tmpfs of at least 64 MB per forwarder (measured: 49.3 MB used at the
peak of a load with both clusters' 4m ingress terms). Below that the forwarder reports `storage_starved`
on `/ready` and exits 70 after 30 s.

Market ticks: per market a top-20 ladder (`LADDER_LEVELS`) is stitched from the ME's `BookSnapshot` 22 and
`BookDelta` 25. A ladder that cannot prove it equals the ME's book is BROKEN and emits nothing until the next
snapshot. Ticks are offered on the core session, stamped with the bound ME epoch. A broken or stale book
clears `forwarder_ticks_ready` and alerts, but it is not a `/ready` condition, so a market-data defect never
stops order dispatch.

### ct-bridge

One resumable BRIDGE session on the core and one ME journal archive attached at a time (the configured
archives in failover order). Every journal record is fed to the core exactly once and in order (money schema
v7 templates 61 to 64, 67 and 68; trades batched up to 64 per 62), or the bridge halts with a FeedHalt. The
core's `lastAppliedJournalSeq` is the only cursor; nothing is written to disk.

```bash
env BRIDGE_DRIVER_MODE=external AERON_DIR=/dev/shm/aeron BRIDGE_DRIVER_TIMEOUT_MS=5000 \
    BRIDGE_ME_JOURNAL_ARCHIVES=10.0.1.1:9010,10.0.1.2:9110,10.0.1.3:9210 BRIDGE_JOURNAL_CONTROL_STREAM=4010 \
    BRIDGE_JOURNAL_STREAM=4001 BRIDGE_JOURNAL_CHANNEL=aeron:ipc BRIDGE_REPLAY_STREAM=9101 BRIDGE_JOURNAL_MTU=1408 \
    BRIDGE_LOCAL_HOST=10.0.0.9 BRIDGE_CORE_CLUSTER_ADDRESSES=10.0.0.1,10.0.0.2,10.0.0.3 BRIDGE_CORE_PORT_BASE=9000 \
    CLUSTER_PORT_STRIDE=100 BRIDGE_CORE_EGRESS_ENDPOINT=10.0.0.9:0 CORE_AUTH_CREDENTIAL=3:BRIDGE:<64 hex> \
    BRIDGE_INSTANCE_ID=1 BRIDGE_HTTP_BIND=10.0.0.9:9600 ct-bridge
```

| Variable | Rule |
|---|---|
| `BRIDGE_DRIVER_MODE` | `external` or `embedded` |
| `AERON_DIR` | `external` only: the host driver's directory |
| `BRIDGE_DRIVER_DIR` | `embedded` only: a writable directory; the driver runs in `<dir>/ct-bridge-<BRIDGE_INSTANCE_ID>` under a lock file |
| `BRIDGE_DRIVER_TIMEOUT_MS` | 3000..60000 |
| `BRIDGE_ME_JOURNAL_ARCHIVES` | `host:port` list in failover order |
| `BRIDGE_JOURNAL_CONTROL_STREAM`, `BRIDGE_JOURNAL_STREAM`, `BRIDGE_JOURNAL_CHANNEL`, `BRIDGE_REPLAY_STREAM` | the ME archive's streams and channel |
| `BRIDGE_JOURNAL_MTU` | multiple of 32, 1144..65504; must match the recording |
| `BRIDGE_LOCAL_HOST` | this host's address for replays (loopback only with loopback archives) |
| `BRIDGE_CORE_CLUSTER_ADDRESSES`, `BRIDGE_CORE_PORT_BASE`, `CLUSTER_PORT_STRIDE` | core member hosts (1..16); ingress port is `base + i * stride + 2` |
| `BRIDGE_CORE_EGRESS_ENDPOINT` | `host:port` (port 0 = driver-assigned), never `0.0.0.0` |
| `CORE_AUTH_CREDENTIAL` | `keyId:BRIDGE:secretHex` |
| `BRIDGE_INSTANCE_ID` | 1..INT64_MAX |
| `BRIDGE_HTTP_BIND` | `ipv4:port` |
| `BRIDGE_HALT_ON_GAP` | refused if present (the core enforces density) |

`embedded` needs `BRIDGE_DRIVER_DIR` on a tmpfs of at least 256 MB (smallest passing size of the
`tmpfs-min` drill with a 64m-term journal). In `external` mode the host driver's directory must hold the
replay image (3 x the ME journal term) plus the 4m ingress and egress terms.

Halts, from `/health` and `/ready`:

1. `GAP`: the bridge re-scans every 60 s and resumes by itself when a source delivers the next record.
2. `SOURCE_SCHEMA`: read `bridge_source_bad_total` and the per-source verdict log, fix the source, restart.
3. `EPOCH_MISMATCH`: the bridge stays parked until the core is re-genesised.
4. `CORE_REFUSED`: check every source at `bridge_core_cursor + 1` with `ct-journal-tail`, then restart.
5. `bridge_source_refused{reason="mtu"} == 1`: fix the recording MTU or `BRIDGE_JOURNAL_MTU`, then restart.

### ct-journal-tail

Reads ME journal records from the configured archives and prints them. It writes nothing and contacts no core.

```bash
ct-journal-tail --aeron-dir /dev/shm/aeron --archive 10.0.0.1:9010 --control-stream 4010 \
    --journal-stream 4001 --channel aeron:ipc --replay-stream 9101 --local-host 10.0.0.9 \
    --journal-mtu 1408 --from-seq 1234 --to-seq 1234 --hex
```

Each admitted frame prints as `FRAME seq=... template=... epoch=...`; a refused frame prints as
`REFUSED <reason> ...` and the tail continues. `--cursor C --epoch E` seeds the resume memo. Exit 0 after
`--to-seq`, 75 on `--timeout-ms`, 78 on a usage error, 70 on a fatal error.

### switch-behavior

```bash
switch-behavior probe --ingress 0=10.0.1.1:9002,1=10.0.1.2:9002,2=10.0.1.3:9002 \
    --response-channel 'aeron:udp?endpoint=10.0.0.9:19999' --frames 5
switch-behavior send-config --ingress ... --response-channel ... --file e2e/demo-config.json
```

`switch-behavior --help` lists every option. `e2e/scripts/run-proof.sh` runs `send-config` end to end
against a dockerized config-mode match cluster.

## Wire schema pins

`dependencies.lock` pins money-schema v7 (core), order-schema v12 and ME journal schema 4 (match), and the
ME journal golden fixtures in `archive/tests/fixtures/mejournal`. To take a newer schema, bump the
`*_ref`/`*_sha256` pins; the codec goldens then fail until `adapters/sbe/tests/golden/regen.sh` is re-run
with engine jars at the new commits.

## Artifacts

`publish.yml` builds on arm64 Linux, runs the tests, and publishes `switch-behavior`, `ct-forwarder`,
`ct-bridge`, `aeronmd`, `libaeron_driver.so`, `libaeron.so` and a `manifest.json` with per-file sha256 under
the commit sha (or the tag). Run them with `LD_LIBRARY_PATH` pointing at the artifact directory.

## License

Apache License 2.0, see [LICENSE](LICENSE).
