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

## License

Apache-2.0.
