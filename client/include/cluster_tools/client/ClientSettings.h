#ifndef CLUSTER_TOOLS_CLIENT_CLIENT_SETTINGS_H
#define CLUSTER_TOOLS_CLIENT_CLIENT_SETTINGS_H

#include <cstdint>
#include <string>
#include <vector>

// Settings of one cluster session (one ClusterClient). Defaults mirror
// io.aeron.cluster.client.AeronCluster.Context 1.53.1 where Aeron has one, and the RC2 spec
// where it decided (keepalive 1 s, rc2-forwarder.md 2.3 item 5). Endpoints, response channel
// and credentials have NO default: validate() refuses to start without them.

namespace cluster_tools { namespace client {

// Upper bound of every configured timeout (1 h). Deadlines are computed as now + timeout on
// the steady clock; the bound keeps that addition far from int64 overflow, which is UB.
inline constexpr std::int64_t MAX_TIMEOUT_NS = 3'600'000'000'000LL;

struct ClientSettings
{
    // "id=host:port,..." one entry per member (operator config). Required.
    std::string ingressEndpoints;
    // Base ingress channel; the client appends endpoint= per member, e.g.
    // "aeron:udp?term-length=4m|mtu=8192" (bridge A5).
    std::string ingressChannel = "aeron:udp";
    // Egress response channel, e.g. "aeron:udp?endpoint=10.0.0.5:0". The cluster must be able
    // to route to it: never 0.0.0.0 (poc/README.md detail 2). Port 0 is resolved by the driver.
    // Required. Two clients in one process (core + ME) need two distinct channels.
    std::string responseChannel;
    std::int32_t ingressStreamId = 101;
    std::int32_t egressStreamId = 102;

    // Encoded credentials presented on connect (core: CoreCredentials::encode(), 38 bytes).
    std::vector<std::uint8_t> credentials;
    // Empty credentials are refused unless this is set explicitly: a cluster without an
    // authenticator (the ME before RC2 S6) is the only legitimate case.
    bool allowEmptyCredentials = false;

    std::string clientName = "cluster-tools";

    // Per connect attempt deadline and the AWAIT_NEW_LEADER_CONNECTION deadline
    // (AeronCluster messageTimeoutNs, default 5 s).
    std::int64_t messageTimeoutNs = 5'000'000'000LL;
    // Attempts per connect() cycle (bounded), with exponential backoff between them.
    int maxConnectAttempts = 10;
    std::int64_t connectBackoffInitialNs = 100'000'000LL;
    std::int64_t connectBackoffMaxNs = 2'000'000'000LL;

    // Keepalive from doWork(), independent of poll; must be > 0 and <= 1 s (the cluster closes
    // a silent session after 10 s and AeronCluster.pollEgress sends none).
    std::int64_t keepAliveIntervalNs = 1'000'000'000LL;
    // After a failed keepalive offer, retry no sooner than this.
    std::int64_t keepAliveRetryIntervalNs = 10'000'000LL;
    // TEST ONLY: disable keepalives to prove the cluster's 10 s session timeout (control run).
    bool disableKeepAliveForTest = false;

    // AWAIT_NEW_LEADER deadline; 0 = 2 x the leaderHeartbeatTimeout the leader reported on
    // connect (AeronCluster 1.53.1 concludeConnect), i.e. 2 s on both engines.
    std::int64_t newLeaderTimeoutNs = 0;

    // Bytes of the encoded SessionConnectRequest scratch (credentials + channel + client info).
    std::size_t connectRequestCapacity = 1024;
};

// Empty string = valid; otherwise the first problem found.
std::string validate(const ClientSettings &settings);

}}

#endif
