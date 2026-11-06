#ifndef CLUSTER_TOOLS_ADAPTERS_AERON_AERON_CLUSTER_SESSION_H
#define CLUSTER_TOOLS_ADAPTERS_AERON_AERON_CLUSTER_SESSION_H

#include <cstdint>
#include <memory>
#include <string>

#include "cluster_tools/ports/ClusterSession.h"

// AeronClusterSession — the cold-path aeron adapter behind the virtual ClusterSession port
// (switch-behavior), extracted from poc/poc.cpp (which stays untouched as reference).
// Built against Aeron 1.53.1 since RC2 S11. Runtime components (forwarder, bridge) use the
// non-blocking client/ library instead; this adapter blocks, sends empty credentials and does
// not follow a leader change, which is fine for a one-shot operator command and nothing else.
// It speaks the cluster session protocol the way io.aeron.cluster.client.AeronCluster does:
//
//   - connect: SessionConnectRequest on every member's ingress publication; followers
//     answer SessionEvent code=REDIRECT (leaderMemberId), the leader answers code=OK.
//     The session then lives on the SAME publication that carried the winning connect —
//     the leader links the connect's ingress image, so a correct header on a fresh
//     publication is not your session (poc/README.md, detail 1).
//   - offer: every app message rides MessageHeader(cluster schema) +
//     SessionMessageHeader{leadershipTermId, clusterSessionId, timestamp=0}.
//   - poll: egress via a persistent FragmentAssembler (large frames fragment, and a
//     fragmented frame may span poll calls); cluster-protocol events are handled here,
//     app payloads go to the caller's handler; a SessionKeepAlive is offered on a
//     timer from inside poll.
//
// RAII: the destructor closes everything (best-effort SessionCloseRequest, then the
// aeron resources); close() exists only for early explicit release and is idempotent.
// No exception escapes a port method — the aeron wrapper throws, this adapter converts
// at the boundary (see ports/Result.h).
//
// This header pulls in no aeron or generated-codec headers (pimpl): exactly one
// translation unit in the repo speaks aeron.
//
// Namespace note: cluster_tools::aeron_adapter, not ...::adapters::aeron — a namespace
// component named `aeron` would shadow the ::aeron library namespace in every
// qualified name here.

namespace cluster_tools { namespace aeron_adapter {

// Defaults mirror the PoC and io.aeron.cluster.client.AeronCluster.Configuration.
struct AeronClusterSessionSettings
{
    // Media driver directory; empty means the aeron default for this host.
    std::string aeronDir;

    // Egress response channel, e.g. "aeron:udp?endpoint=172.28.0.20:19999". Must be an
    // address the cluster can route to — never 0.0.0.0 or localhost (poc/README.md,
    // detail 2). Required.
    std::string responseChannel;

    std::string clientInfo = "name=cluster-tools version=0.1";

    std::int32_t ingressStreamId = 101;         // AeronCluster INGRESS_STREAM_ID_DEFAULT
    std::int32_t egressStreamId = 102;          // AeronCluster EGRESS_STREAM_ID_DEFAULT

    std::int64_t connectTimeoutMs = 60 * 1000;  // long enough to sit out an election
    std::int64_t connectRetryIntervalMs = 600;
    std::int64_t offerTimeoutMs = 5 * 1000;     // bounded retry on backpressure
    std::int64_t keepAliveIntervalMs = 500;
};

class AeronClusterSession final : public ports::ClusterSession
{
public:
    explicit AeronClusterSession(AeronClusterSessionSettings settings);
    ~AeronClusterSession() override;

    AeronClusterSession(const AeronClusterSession &) = delete;
    AeronClusterSession &operator=(const AeronClusterSession &) = delete;

    ports::Result<ports::SessionInfo> connect(const std::string &ingressEndpoints) override;
    ports::Result<std::int64_t> offer(const std::uint8_t *buffer, std::size_t length) override;
    ports::Result<int> poll(const ports::EgressHandler &handler, int fragmentLimit) override;
    void close() override;

    // Last known session facts; leaderMemberId/leadershipTermId are updated when the
    // cluster announces a NewLeaderEvent mid-session.
    const ports::SessionInfo &sessionInfo() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}}

#endif
