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
| `ct-genesis` | Operator tool: the genesis of an empty ME and core pair, one verb per phase, plus `status` and `verify`. |
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
runtime/             process machinery of a C++ cluster process (supervisor, HTTP, log ring, idle, embedded driver)
app/common/          what ct-forwarder and ct-bridge share beyond the runtime (the retired-settings table)
app/session-stub/    ct_session_stub, a test-only program on the exported client (never published)
tests/package-consumer/  a separate CMake project that links the installed package
app/forwarder/       ct-forwarder
app/bridge/          ct-bridge
app/journal-tail/    ct-journal-tail
app/genesis/         ct-genesis
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

SBE schema ids are unique across the repos: `sbe-schema-ids.txt` lists every id in use (the same file in core,
match, cluster-tools, gateway and tools). The `sbe_schema_ids` test (every configuration, python3 only) holds the
lock's schema ids, every hand-written `*_SCHEMA_ID` constant and, when configured, the fetched schemas to it; a
new constant has to be mapped to its schema in `CMakeLists.txt`.

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

The Java settlement bridge is gone from core at the pinned `money_schema_ref` (its jar keeps only the `dump` and
`set-money-journal` commands), so the `bridge.parity` check that mapped every Java bridge test to a C++ test, and
its two `PARITY.md` tables, are retired. The tables are in this repository's history.

## Using the client library from another repository

`cmake --install <build> --prefix <dir>` installs the cluster client and the process machinery as a CMake package.
Another C++20 repository pins this repository by commit sha and source tarball sha256, builds it with the same
verified inputs (and its own money schema, which configure checks against the lock's floor and templates), installs
it, and links it. The package records the source sha from git at configure time; a build from the source tarball
has no `.git`, so it must pass `-DCT_BUILD_SOURCE_SHA=<the pinned 40-hex sha>` (configure warns when the sha is
`unknown`, and a consumer's pin check refuses such a package).

```cmake
find_package(cluster_tools CONFIG REQUIRED COMPONENTS client_core runtime client client_aeron runtime_aeron
    runtime_embedded_driver)
target_link_libraries(my-process PRIVATE cluster_tools::runtime_embedded_driver   # first: see runtime/CMakeLists.txt
    cluster_tools::runtime_aeron cluster_tools::client_aeron cluster_tools::client)
```

| Component | Configuration | Content |
|---|---|---|
| `client_core` | every | endpoints, credentials, backoff, client settings, protocol vocabulary |
| `runtime` | every | Supervisor, HttpServer, LogRing, StatusBoardT, IdleStrategy, DriverTimeout, DirProbe, StorageStarvation |
| `client` | `WITH_AERON` | the `ClusterClient` state machine and `CoreSessionRegistrar` (header-only) |
| `client_aeron` | `WITH_AERON` | the Aeron transport and the fatal-error policy |
| `runtime_aeron` | `WITH_AERON` | ConductorWatch |
| `runtime_embedded_driver` | `WITH_AERON`, `CT_BUILD_MEDIA_DRIVER` | the embedded media driver; link it only from a final executable, first in the list, never from a library (it compiles into whatever links it, so a static library would carry the driver object and lose the link order). The installed driver links `uuid`, so the consumer host needs libuuid |
| `sbe_codecs` | `WITH_AERON` | the generated cluster-protocol and money-schema codecs and `cluster_tools/build_info.h` |
| `archive_core` | every | the journal-neutral archive follow library: frame admission by journal profile, recording chain, resume memo, follow policy |
| `archive_aeron`, `aeron_archive_client` | `WITH_BRIDGE` | the Aeron archive source set over the C archive client |

The package config sets `cluster_tools_SOURCE_SHA`, `cluster_tools_MONEY_SCHEMA_VERSION` and
`cluster_tools_MONEY_SCHEMA_SHA256`; a consumer compares them with its own lock. A component the build did not make
fails `find_package` by name. Binaries, test builds and the session stub are never installed. `package.consumer`
(`tests/package-consumer/check-package.sh`) proves all of this in every configuration.

### Archive follow library

`archive/` is journal-neutral. `FollowSettings::profile` picks the journal: `ME_JOURNAL` (schema 4, the bridge's default) or `CORE_ORDER_JOURNAL` (schema 5, stream 4102 by setting, one 216-byte record per order event, head `journalSeq`, `epoch` and core log position).

- Settings take a key prefix: `parseArchiveSettings(lookup, scope, ArchiveSettingsKeys::withPrefix("<PREFIX>_", CORE_ORDER_JOURNAL))` reads `<PREFIX>_JOURNAL_ARCHIVES`, `_JOURNAL_CONTROL_STREAM`, `_JOURNAL_STREAM`, `_JOURNAL_CHANNEL`, `_REPLAY_STREAM`, `_LOCAL_HOST`, `_JOURNAL_MTU` and `_DRIVER_DIR`. The bridge keeps its `BRIDGE_` names.
- An owner that stores its memo rows may hand them back with `ResumeMemo::seed()` after `onReport()`. A seed is checked against the next listing (key, extent, epoch, cursor) and is used only through the resume belt.
- `FollowSettings::expectedEpoch` (at least 1, given by the owner, or `SourceSet::expectEpoch`) skips recordings of another epoch and counts them in `foreignEpochRecordings`.
- `relistInterval` (0 by default, so the bridge is unchanged) finds a successor while the old recording still reads ACTIVE.
- `SourceSet::readerCheckpoint(logicalSeq)` reports the attached source's own recording and position.
- `keyByArchiveId` puts the archive id in recording keys. Enable it only when the archives have a configured, stable `aeron.archive.id`; Aeron's default id changes on every archive start.

## Session stub (test-only)

`-DCT_SESSION_STUB=ON` (needs `WITH_AERON`) builds `ct_session_stub`: one GATEWAY core session on the exported client,
registered with the channel mask given on the command line, driven by a script on stdin, with a poll throttle that
leaves `doWork()` and the keepalives running, so a slow consumer can be produced on purpose. It is never installed
and never published. Options and commands are documented in `app/session-stub/StubOptions.h` and `StubCommands.h`.

```bash
printf 'await registered 10000\nthrottle 500 1\nsleep 60000\nstatus\nquit\n' | \
  CORE_AUTH_CREDENTIAL=<keyId>:gateway:<secretHex> ct_session_stub --driver-mode external --aeron-dir /dev/shm/aeron \
  --instance-id 7 --ingress-endpoints 0=10.0.0.1:9302,1=10.0.0.2:9302,2=10.0.0.3:9302 \
  --ingress-channel 'aeron:udp?term-length=4m|mtu=8192' --response-channel 'aeron:udp?endpoint=10.0.0.9:0|term-length=4m|mtu=8192' \
  --channels acks,orders,balances,control
```

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

Idle modes (`FWD_IDLE_MODE`, `BRIDGE_IDLE_MODE`; the same words as the engines' `TRANSPORT_IDLE_MODE`):
`busy_spin` never yields or sleeps; `backoff` (the default) is each process's own policy; `sleep` parks 1 ms after
an iteration without work. In `embedded` mode the driver follows the same mode on every agent: `busy_spin` is
Aeron's `spin`, `sleep` is `sleep-ns` with `1ms`, `backoff` leaves Aeron's default `backoff`. An external driver
takes it from `driver-idle-<mode>.properties` (below).

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
| `FWD_IDLE_MODE` | optional: `busy_spin`, `backoff` (unset: spin 100, yield 100, park 100 us) or `sleep` (park 1 ms); the duty thread and the embedded driver follow it; any other value exits 78; `forwarder_idle_mode` shows it |
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
v10 templates 61 to 64, 67 and 68; trades batched up to 64 per 62), or the bridge halts with a FeedHalt. The
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
| `BRIDGE_IDLE_MODE` | optional: `busy_spin`, `backoff` (unset: yield 10, park 1 ms) or `sleep` (park 1 ms, no yield); the duty thread and the embedded driver follow it; any other value exits 78; `bridge_idle_mode` shows it |
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

`--profile me|core-order` picks the journal (default `me`); a core order journal frame prints `log=` for the core log position. Each admitted frame prints as `FRAME seq=... template=... epoch=...`; a refused frame prints as
`REFUSED <reason> ...` and the tail continues. `--cursor C --epoch E` seeds the resume memo. Exit 0 after
`--to-seq`, 75 on `--timeout-ms`, 78 on a usage error, 70 on a fatal error.

### ct-genesis

The genesis of an empty ME and core pair from one input file (`app/genesis/genesis.example.json`), one phase
per invocation, so the operator (or the console) starts processes between the phases:

```bash
ct-genesis <verb> genesis.json
```

| Verb | Sends | Done when the clusters show |
|---|---|---|
| `me_genesis` | ME `EngineConfig` FRESH (ADMIN) | the ME epoch equals the input's `genesisId` |
| `arm_money_journal` | `SetMoneyJournal` on (ADMIN) | the feed report says the journal is armed |
| `core_genesis` | `CoreConfig` 1, 2, 3, then the ADMIN OPEN of the global gate, retried until a bridge's bind of the ME epoch is in the log | config 3 with the input's `genesisId`, gate OPEN |
| `configure_risk` | a `RiskConfigSet` per input row, then `CoreLimitsSet` (request cap, default 2048) | every risk row and the limits record equal to the input |
| `fund` | the TOOL deposits | the balances equal the funding table, no hold |
| `verify_funded` | nothing (a check) | the funded balances, no hold, no order, the journal armed, the gate OPEN |
| `activate` | `CoreConfig` 4 (behaviour 4), then the activation record file | behaviour 4 and a record naming this `genesisId` |
| `status` | nothing | prints every phase, the last complete one and the next |
| `verify` | nothing | exit 0 only for a complete genesis: every phase, behaviour 4, the request cap, the gate OPEN, the record |

The order between the phases: `me_genesis`, `arm_money_journal`, start the money projector and the bridges,
`core_genesis`, `configure_risk`, `fund`, `verify_funded`, start the forwarders, `activate`, then the
gateway set. `InitTradeHighWater` is never part of a genesis; an input that names it is refused.

Every verb first reads the replicated state (never a log position): the ME epoch, the core's feed report,
config report and balance, hold and order snapshots. `me_genesis` runs only on an empty pair, else
`cluster is not empty: genesis refused`. Every other verb runs only when the state is the exact prefix of
the phases before it, else `genesis state is not a prefix of the plan: <detail>`. A genesis that stopped in
the middle of a phase is not a prefix: wipe both clusters and start again (before the gate OPEN nothing
outside the clusters has seen it). A stop between two phases is a prefix: run the next verb.

Settings (environment): `GENESIS_DRIVER_MODE` (`embedded` or `external`, required), `AERON_DIR` (external)
or `GENESIS_DRIVER_DIR` (embedded, with `GENESIS_INSTANCE_ID`, default 1), `GENESIS_CORE_INGRESS`,
`GENESIS_ME_INGRESS`, `GENESIS_RESPONSE_CHANNEL`, `GENESIS_CORE_ADMIN_CREDENTIAL`,
`GENESIS_CORE_TOOL_CREDENTIAL` (fund), `GENESIS_ME_ADMIN_CREDENTIAL`, `GENESIS_ACTIVATION_RECORD` (all
required); waits with defaults: `GENESIS_CONNECT_TIMEOUT_MS` 60000, `GENESIS_REPLY_TIMEOUT_MS` 30000,
`GENESIS_ME_EPOCH_TIMEOUT_MS` 30000, `GENESIS_JOURNAL_ARM_TIMEOUT_MS` 30000, `GENESIS_GATE_OPEN_RETRIES` 600,
`GENESIS_GATE_OPEN_RETRY_INTERVAL_MS` 100, `GENESIS_DRIVER_TIMEOUT_MS` 10000, `GENESIS_INGRESS_CHANNEL`
`aeron:udp`. Exit 0 done, 1 the phase did not complete (or `verify`: not done), 3 refused before anything
was sent, 64 usage, 78 configuration or input, 70 a fatal Aeron error.

### switch-behavior

```bash
switch-behavior probe --ingress 0=10.0.1.1:9002,1=10.0.1.2:9002,2=10.0.1.3:9002 \
    --response-channel 'aeron:udp?endpoint=10.0.0.9:19999' --frames 5
switch-behavior send-config --ingress ... --response-channel ... --file e2e/demo-config.json
```

`switch-behavior --help` lists every option. `e2e/scripts/run-proof.sh` runs `send-config` end to end
against a dockerized config-mode match cluster.

## Aeron patches

The Aeron C tree is built from the pinned tarball plus the patches `dependencies.lock` lists under `aeron_patches`, each pinned by sha256. Configure applies them in order and refuses a patch that is missing, edited without a lock change, or no longer applies exactly. The applied list is printed (`Aeron patches:`) and compiled into `build_info::AERON_PATCHES`.

`0001-elicit-setup-keeps-pending-setup-on-send-failure.patch` fixes the 1.53.1 C driver. When the status message that asks a source for a setup frame failed to send (for example EPERM from a local firewall rule, or an unreachable route while inbound traffic had already recovered), the driver ignored that session until a restart. With the patch it logs the error and asks again after the pending-setup timeout, as the Java driver does. `aeron_driver.elicit_setup_retry` proves it, and `aeron_driver.elicit_setup_retry_unpatched_control` proves the test fails without the patch. `aeronmd` and `libaeron_driver.so` ship together, since the fix is in the library.

## Wire schema pins

`dependencies.lock` pins money-schema v10 (core), order-schema v12 and ME journal schema 4 (match), and the
ME journal golden fixtures in `archive/tests/fixtures/mejournal`. To take a newer schema, bump the
`*_ref`/`*_sha256` pins; the codec goldens then fail until `adapters/sbe/tests/golden/regen.sh` is re-run
with engine jars built at the new commits (the script checks each jar's `Build-Sha`). A money-schema bump
also moves the feed harness pin (`feed_harness_core_ref`, `feed_harness_url`, `feed_harness_sha256`,
`feed_harness_main_diff_empty_vs`), and its regen run rewrites `domain/bridge/tests/fixtures/e1/`, whose
`SHA256SUMS` hash is `me_journal_e1_fixtures_sha256sums_sha256`. Every pinned core commit carries a
`money-schema-<sha>` tag in openexch/core so CI can always fetch it.

## Artifacts

`publish.yml` builds on arm64 Linux, runs the tests, and publishes `switch-behavior`, `ct-forwarder`,
`ct-bridge`, `ct-genesis`, `aeronmd`, `libaeron_driver.so`, `libaeron.so`, the external media driver's `driver.properties`,
`driver-dev.properties`, `driver-prod.properties`, the idle overlays `driver-idle-busy_spin.properties`,
`driver-idle-backoff.properties` and `driver-idle-sleep.properties`, and a `manifest.json` with per-file sha256 under
the commit sha (or the tag). Run them with `LD_LIBRARY_PATH` pointing at the artifact directory; run a driver as
`aeronmd driver.properties driver-<dev|prod>.properties [driver-idle-<mode>.properties]` (a later file wins: the
profile overlay sets the threading, the idle overlay the idle strategies; `deploy/bin/aeronmd-launch` adds it from
`AERON_DRIVER_IDLE_MODE`).

`driver.properties` is match's `deploy/media-driver/driver.properties` at the commit `dependencies.lock`
pins (`driver_properties_ref`), verified against `driver_properties_sha256` by
`scripts/stage-driver-properties.sh`; it is never copied into this repo. The two overlays live in
`deploy/media-driver/` and may set only the threading mode and idle strategies (dev: SHARED + backoff,
prod: DEDICATED + noop). The three idle overlays set only the idle strategy (and init args) of the conductor,
sender, receiver, shared and sharednetwork agents: `spin`, `backoff`, or `sleep-ns` with `1ms`. Take a newer
`driver.properties` by bumping the ref and sha256 in the lock.
`scripts/check-stage-driver-properties.sh` (ctest `driver_properties.stage_gate`) proves every refusal.

## License

Apache License 2.0, see [LICENSE](LICENSE).
