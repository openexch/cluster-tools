# cluster-tools

C++ tools that speak the Aeron Cluster protocol used by the openexchange engines.

Aeron ships no cluster *client* outside Java — the official C/C++ APIs cover transport
and archive only. The cluster session protocol itself, however, is SBE-defined and
language-neutral, so a client can be written against the C++ wrapper directly. This
repo is where those tools live.

## Contents

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
| `-DWITH_AERON=OFF` (default, CI) | cmake + a C++17 compiler | domain, ports, the json adapter, their tests | the hexagon: domain logic and input parsing build and their tests pass with zero infrastructure |
| `-DWITH_AERON=ON` `-DAERON_SRC=…` `-DAERON_LIB=…` `-DMATCH_SCHEMA=…` | aeron 1.52.2 checkout, prebuilt `libaeron_static.a` (see `poc/scripts/build-aeron.sh`), the match `order-schema.xml` extracted from match main, java for SBE codegen | everything, incl. the `switch-behavior` binary | the adapters compile (and link) against the real transport and the real wire schema |

```bash
# domain + ports + json adapter + tests — no aeron, no java, anywhere gcc lives:
cmake -S . -B build -DWITH_AERON=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# full build (poc/ documents the dockerized aeron build that produces AERON_LIB):
git -C /path/to/match show origin/main:match-common/src/main/resources/sbe/order-schema.xml > order-schema.xml
cmake -S . -B build -DWITH_AERON=ON \
    -DAERON_SRC=/path/to/aeron -DAERON_LIB=/path/to/libaeron_static.a \
    -DMATCH_SCHEMA=$PWD/order-schema.xml
cmake --build build
```

## License

Apache-2.0.

## Artifacts

Every push to main publishes arm64 Linux binaries to the org's artifact pipeline
(`cluster-tools/{sha}/` — see `.github/workflows/publish.yml`): `switch-behavior`,
`aeronmd`, `libaeron_driver.so`, `libaeron.so`, plus a `manifest.json` with per-file
sha256. Run them with `LD_LIBRARY_PATH` pointing at the artifact directory; nothing
is ever compiled on a deployment box.
