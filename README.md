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
ports/                the seams (header-only)
  ClusterSession      connect / offer / poll / close — virtual, cold-path (the header
                      documents why, and why hot-path ports would be templates/concepts)
  Result              tiny expected-style carrier; no exceptions cross a port boundary
adapters/aeron/       AeronClusterSession — the cluster session protocol over the
                      official aeron cpp_wrapper (the only code in the repo that
                      includes an aeron header)
adapters/sbe/         codegen wiring only: cluster-protocol C++ codecs are generated
                      from the schema in the aeron checkout into the build dir —
                      nothing generated is committed
app/switch-behavior/  the operator CLI; subcommand `probe` works today
third_party/doctest/  vendored single-header test framework (MIT)
poc/                  the original flat proof of concept, kept untouched as reference
```

The build matrix:

| configuration | needs | builds | proves |
|---|---|---|---|
| `-DWITH_AERON=OFF` (default, CI) | cmake + a C++17 compiler | domain, ports, their tests | the hexagon: domain logic builds and its tests pass with zero infrastructure |
| `-DWITH_AERON=ON` `-DAERON_SRC=…` `-DAERON_LIB=…` | aeron 1.52.2 checkout, prebuilt `libaeron_static.a` (see `poc/scripts/build-aeron.sh`), java for SBE codegen | everything, incl. the `switch-behavior` binary | the adapter compiles (and links) against the real transport |

```bash
# domain + ports + tests — no aeron, no java, anywhere gcc lives:
cmake -S . -B build -DWITH_AERON=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# full build (poc/ documents the dockerized aeron build that produces AERON_LIB):
cmake -S . -B build -DWITH_AERON=ON \
    -DAERON_SRC=/path/to/aeron -DAERON_LIB=/path/to/libaeron_static.a
cmake --build build
```

## License

Apache-2.0.
