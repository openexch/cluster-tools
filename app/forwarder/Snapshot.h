#ifndef CLUSTER_TOOLS_APP_FORWARDER_SNAPSHOT_H
#define CLUSTER_TOOLS_APP_FORWARDER_SNAPSHOT_H

#include <cstdint>
#include <type_traits>

#include "cluster_tools/domain/forwarder/BookLadder.h"
#include "cluster_tools/domain/forwarder/Counters.h"
#include "cluster_tools/domain/forwarder/ResyncMachine.h"
#include "cluster_tools/domain/forwarder/TickEmitter.h"

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

// RC2 S13 10.2: BOOK_STALE is a TICK-readiness condition (PO-1): it drives forwarder_ticks_ready, /health.ticksReady
// and ForwarderTicksNotReady, never a bit of the dispatch /ready mask above (COUNT stays 10).
namespace ticks_not_ready {
inline constexpr std::uint8_t NONE = 0;
inline constexpr std::uint8_t BROKEN_30S = 1;        // a ladder BROKEN for >= 30 s
inline constexpr std::uint8_t PENDING_5S = 2;        // E11 pending age >= 5 s (a stuck emitter)
inline constexpr std::uint8_t SNAPSHOT_AGE_30S = 3;  // an OK ladder without a snapshot for >= 30 s while the ME is BOUND
inline constexpr std::int64_t BROKEN_NS = 30'000'000'000LL;
inline constexpr std::int64_t PENDING_NS = 5'000'000'000LL;
inline constexpr std::int64_t SNAPSHOT_AGE_NS = 30'000'000'000LL;
}

inline const char *ticksNotReadyName(std::uint8_t condition) noexcept
{
    switch (condition)
    {
        case ticks_not_ready::NONE: return "none";
        case ticks_not_ready::BROKEN_30S: return "book_stale_broken_30s";
        case ticks_not_ready::PENDING_5S: return "book_stale_tick_pending_5s";
        case ticks_not_ready::SNAPSHOT_AGE_30S: return "book_stale_snapshot_age_30s";
    }
    return "?";
}

// One market's ladder, trade mark and emitter slot as the render shows them (10.1, 10.2).
struct BookView
{
    std::uint8_t state = 0;              // LadderState: 0 EMPTY, 1 OK, 2 BROKEN
    std::int64_t version = 0;            // 0 when EMPTY
    std::int64_t brokenAgeMs = 0;        // 0 unless BROKEN
    std::int64_t snapshotAgeMs = -1;     // -1 never
    std::int64_t brokenNsTotal = 0;      // closed episodes plus the open one
    std::int64_t lastOfferedVersion = 0;
    std::int64_t pendingAgeMs = 0;       // E11
    std::int64_t lastTradeId = 0;
    std::uint64_t tradeMalformed = 0;
    domain::forwarder::LadderCounters ladder;
    domain::forwarder::TickSlotCounters tick;
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

    // RC2 S13 (K4): the five ladders / emitter slots, the stitcher's frame-level counters and the tick readiness
    BookView books[domain::forwarder::TICK_MARKETS];
    std::uint64_t bookForeignMarket = 0;
    std::uint64_t bookMalformedNoMarket = 0;
    bool ticksReady = true;
    std::int32_t ticksNotReadyMarket = 0;   // the first market that cleared it (0 none)
    std::uint8_t ticksNotReadyCondition = 0; // ticks_not_ready::*
};

static_assert(std::is_trivially_copyable_v<ForwarderSnapshot>);

}}}

#endif
