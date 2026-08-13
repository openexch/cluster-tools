#ifndef CLUSTER_TOOLS_PORTS_CLUSTER_SESSION_H
#define CLUSTER_TOOLS_PORTS_CLUSTER_SESSION_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "cluster_tools/ports/Result.h"

// ClusterSession — the port through which a tool speaks to an Aeron Cluster.
//
// WHY THIS IS A virtual INTERFACE (and when it must not be):
// Operator tools are cold-path — one session, a handful of messages, a latency budget
// measured in network round-trips. At that scale a vtable dispatch is noise, and the
// runtime seam buys real things: domain and app code compile with zero Aeron/SBE
// headers, and tests substitute a fake without linking a transport. On hot paths (the
// future C++ gateway this repo trains for) the same seam must be compile-time instead —
// a template parameter constrained by a concept — so the adapter inlines into the duty
// cycle and the abstraction costs nothing per message. Rule of thumb: if the call sits
// in a poll loop whose throughput anyone measures, it does not go through a vtable.
// (Same goes for the std::function handler below: cold-path convenience; a hot-path
// port would take a template functor.)
//
// Contract:
//   - No exception crosses this boundary. Every fallible method returns a Result;
//     adapters catch everything at the seam and convert.
//   - Implementations are single-threaded: connect/offer/poll/close from one thread.
//   - close() is idempotent and optional — implementations are RAII and release
//     everything from their destructor; close() only exists for early, explicit release.

namespace cluster_tools { namespace ports {

// What the cluster tells us when a session is established (SessionEvent code=OK).
struct SessionInfo
{
    std::int64_t sessionId = -1;
    std::int32_t leaderMemberId = -1;
    std::int64_t leadershipTermId = -1;
};

// One application-level egress message, cluster framing already stripped.
// payload points at engine-schema bytes (an SBE MessageHeader followed by the body)
// and is valid ONLY for the duration of the handler call — copy to keep.
struct EgressMessage
{
    std::int64_t clusterSessionId = -1;
    std::int64_t timestamp = 0;
    const std::uint8_t *payload = nullptr;
    std::size_t length = 0;
};

using EgressHandler = std::function<void(const EgressMessage &message)>;

class ClusterSession
{
public:
    virtual ~ClusterSession() = default;

    // Establish a session. ingressEndpoints is the AeronCluster spec "id=host:port,..."
    // (one entry per member; the adapter handles follower REDIRECTs). Blocks until the
    // session is up or the implementation's connect deadline passes.
    virtual Result<SessionInfo> connect(const std::string &ingressEndpoints) = 0;

    // Send one application message (engine-schema bytes; the adapter adds the cluster
    // session framing). Returns the new stream position on success.
    virtual Result<std::int64_t> offer(const std::uint8_t *buffer, std::size_t length) = 0;

    // Drain up to fragmentLimit egress fragments, invoking handler for each complete
    // application message; cluster-protocol events (new leader, session close) are the
    // adapter's business. Returns the number of fragments processed — 0 is a normal
    // outcome, failure means the session is unusable.
    virtual Result<int> poll(const EgressHandler &handler, int fragmentLimit) = 0;

    // Idempotent early release; the destructor covers the forgotten-close path.
    virtual void close() = 0;
};

}}

#endif
