#ifndef CLUSTER_TOOLS_APP_FORWARDER_SNAPSHOT_H
#define CLUSTER_TOOLS_APP_FORWARDER_SNAPSHOT_H

#include <cstdint>
#include <type_traits>

#include "cluster_tools/domain/forwarder/Counters.h"
#include "cluster_tools/domain/forwarder/ResyncMachine.h"

// ForwarderSnapshot - everything /health, /ready and /metrics show (RC2 S12 contract section 10), written by
// the duty thread every 100 ms into the StatusBoard and read by fwd-http and main. Plain data: trivially
// copyable, no pointers, no strings; pure (WITH_AERON=OFF) so MetricsRender tests build everywhere.

namespace cluster_tools { namespace app { namespace forwarder {

// The 4.4 conjuncts of ready, one bit each (a bit SET = that conjunct fails).
namespace not_ready {
inline constexpr std::uint32_t CORE_NOT_REGISTERED = 1u << 0;
inline constexpr std::uint32_t NOT_LIVE = 1u << 1;
inline constexpr std::uint32_t ME_NOT_BOUND = 1u << 2;
inline constexpr std::uint32_t ME_EGRESS_STALE = 1u << 3;
inline constexpr std::uint32_t NO_RECENT_ANSWER = 1u << 4;
inline constexpr std::uint32_t RING_DEEP = 1u << 5;
inline constexpr std::uint32_t STOPPING = 1u << 6;
inline constexpr std::uint32_t RING_HEAD_STALE = 1u << 7; // review NOTE-3: the head waited 30 s for the ME
inline constexpr std::uint32_t CORE_NO_LEADER = 1u << 8;  // review NOTE-3: the core client awaits a new leader
inline constexpr std::uint32_t STORAGE_STARVED = 1u << 9; // S10' Q1: an ingress publication refused for storage space
inline constexpr unsigned COUNT = 10;
}

inline const char *notReadyName(unsigned bit) noexcept
{
    switch (bit)
    {
        case 0: return "core_not_registered";
        case 1: return "not_live";
        case 2: return "me_not_bound";
        case 3: return "me_egress_stale";
        case 4: return "no_probe_or_resync_answer_15s";
        case 5: return "ring_depth_above_32768";
        case 6: return "stopping";
        case 7: return "ring_head_unsent_30s";
        case 8: return "core_no_leader";
        case 9: return "storage_starved";
    }
    return "?";
}

// Core link (4.1) and ME link (4.2) states.
enum class CoreLink : std::uint8_t
{
    DOWN = 0,
    CONNECTING = 1,
    UNREGISTERED = 2,
    REGISTERED = 3,
};

enum class MeLink : std::uint8_t
{
    DOWN = 0,
    CONNECTING = 1,
    UNVERIFIED = 2,
    BOUND = 3,
};

struct ClusterStats
{
    std::uint8_t clientState = 0; // ClientState code
    std::uint64_t newLeaderEvents = 0;
    std::uint64_t sessionsOpened = 0;
    std::uint64_t sessionsClosed = 0;
    std::uint64_t keepAlivesSent = 0;
    std::uint64_t keepAliveFailures = 0;
    std::uint64_t connectClusterErrors = 0;
    std::int64_t clusterSessionId = -1;
    std::int64_t leadershipTermId = -1;
};

struct ForwarderSnapshot
{
    std::uint64_t sequence = 0;       // publish count (0 = never published)
    std::int64_t nowNs = 0;           // duty clock at publish
    std::uint64_t dutyHeartbeat = 0;  // the duty iteration counter at publish
    std::int64_t identity = 0;
    std::int64_t meEpoch = 0;         // bound E, 0 until bound
    char version[48] = {};

    bool ready = false;
    std::uint32_t notReady = 0;       // not_ready bits
    bool stopping = false;
    std::uint8_t coreLink = 0;
    std::uint8_t meLink = 0;
    bool registered = false;
    ClusterStats core;
    ClusterStats me;

    std::uint64_t registrationAccepted = 0;
    std::uint64_t registrationDuplicate = 0;
    std::uint64_t registrationRejected = 0;
    std::uint64_t registrationSends = 0;
    std::uint64_t meEpochReconnects = 0;
    std::int64_t meEgressAgeMs = -1;

    std::uint8_t mode = 0;
    std::int64_t cursor = 0;
    std::int64_t lastSentSeq = 0;
    std::int64_t coreHead = 0;
    std::int64_t lastResyncEndSeq = 0;
    std::int64_t resyncDurationMs = 0;
    std::int64_t headProbeAgeMs = -1;
    std::uint64_t ringDepth = 0;
    std::int64_t ringHeadAgeMs = 0;
    std::uint64_t holdSize = 0;

    domain::forwarder::ResyncCounters resync;
    domain::forwarder::ForwarderCounters counters;

    std::uint64_t aeronErrors = 0;
    std::uint64_t dutyMaxCycleUs = 0;
    std::uint64_t logDropped = 0;

    // RC2 S10' 8.2 rule 6: the media driver in force (DriverMode: 1 embedded, 2 external) and whether the embedded
    // per-instance dir was a stale one reused and cleaned at start
    std::uint8_t driverMode = 0;
    std::uint64_t driverDirReclaimed = 0;
    // S10' Q1: seconds an ingress publication has been refused for storage space (closed episodes + the current one)
    std::uint64_t storageStarvedSeconds = 0;
};

static_assert(std::is_trivially_copyable_v<ForwarderSnapshot>);

}}}

#endif
