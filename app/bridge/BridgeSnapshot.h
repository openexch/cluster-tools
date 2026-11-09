#ifndef CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_SNAPSHOT_H
#define CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_SNAPSHOT_H

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

#include "cluster_tools/archive/Follow.h"
#include "cluster_tools/domain/bridge/Types.h"

#include "AppSeams.h"

// BridgeSnapshot - everything /health, /ready and /metrics show that is not already a relaxed atomic of the domain
// (BridgeMetrics) or of the archive (ArchiveMetrics). Written by the duty thread every
// 100 ms through the StatusBoard (plain data, trivially copyable: a publish allocates nothing), read by bridge-http.
// Also here: the O3 / O4 latency histograms and the bounded ack map the agent owns (single writer B).

namespace cluster_tools { namespace app { namespace bridge {

// ------------------------------------------------------------------ 4.1 the core link and 4.3 readiness

enum class CoreLink : std::uint8_t
{
    C0_DOWN,
    C1_CONNECTING,
    C2_CONNECTED,   // connected, not registered in this (sessionEpoch, term)
    C3_REGISTERED,
    C4_AWAITING     // the client is AWAIT_NEW_LEADER or AWAIT_NEW_LEADER_CONNECTION
};
const char *coreLinkName(CoreLink link) noexcept;

namespace not_ready {
inline constexpr std::uint32_t CORE_NOT_CONNECTED = 1u << 0;
inline constexpr std::uint32_t CORE_NO_LEADER = 1u << 1;
inline constexpr std::uint32_t CORE_NOT_REGISTERED = 1u << 2;
inline constexpr std::uint32_t NO_REPORT_THIS_TERM = 1u << 3;
inline constexpr std::uint32_t REPORT_STALE = 1u << 4;
inline constexpr std::uint32_t NO_SOURCE_ATTACHED = 1u << 5;
inline constexpr std::uint32_t HALTED = 1u << 6;
inline constexpr std::uint32_t FATAL = 1u << 7;
inline constexpr std::uint32_t BACKLOG_WITHOUT_PROGRESS = 1u << 8;
inline constexpr std::uint32_t NO_REACHABLE_SOURCE = 1u << 9;
inline constexpr std::uint32_t REPORT_DEGRADED = 1u << 10;
inline constexpr std::uint32_t NO_SOURCE_BEYOND_E = 1u << 11;
inline constexpr std::uint32_t HOLE_GRACE = 1u << 12;
inline constexpr std::uint32_t STOPPING = 1u << 13;
inline constexpr std::uint32_t STALLED = 1u << 14; // main's view, added by the HTTP route
inline constexpr std::uint32_t STORAGE_STARVED = 1u << 15; // Our publication / replay image refused for storage
inline constexpr int BITS = 16;
}
const char *notReadyBitName(int bit) noexcept;

// Readiness inputs the duty thread has (4.3 bits 0-13). Pure, so the rule is unit tested apart from the agent.
struct ReadyInputs
{
    CoreLink link = CoreLink::C0_DOWN;
    domain::bridge::BridgeView view;
    archive::TimePoint now{};
    archive::TimePoint progressAt{};   // the last time view.consumedPosition or view.lastForwardAt advanced
    archive::TimePoint forwardAt{};    // view.lastForwardAt (READY_COUNTS_FORWARDS_ONLY reads only this)
    std::int64_t backlogBytes = 0;     // bridge_source_backlog_bytes
    bool anySourceAttached = false;
    bool stopping = false;
    bool storageStarved = false;       // The core ingress or every replay image refused for storage space
    std::chrono::milliseconds reportInterval{ 10'000 };
    std::chrono::milliseconds reportIntervalHalted{ 5'000 };
};
inline constexpr std::chrono::milliseconds BACKLOG_PROGRESS_BOUND{ 30'000 };
std::uint32_t notReadyBits(const ReadyInputs &in) noexcept;

// ------------------------------------------------------------------ 5.4 O3 / O4

// Fixed buckets 1 ms .. 60 s (+Inf). Single writer (B); copied into the snapshot at publish.
struct LatencyHistogram
{
    static constexpr std::size_t BOUNDS = 15;
    static constexpr std::array<std::int64_t, BOUNDS> UPPER_MS{ 1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000,
        10000, 30000, 60000 };
    std::array<std::uint64_t, BOUNDS + 1> buckets{}; // per bucket (not cumulative); the last is +Inf
    std::uint64_t count = 0;
    std::int64_t sumMs = 0;

    void record(std::int64_t ms) noexcept
    {
        std::size_t i = 0;
        while (i < BOUNDS && ms > UPPER_MS[i])
        {
            i++;
        }
        buckets[i]++;
        count++;
        sumMs += ms;
    }
};

// O3: a cross-host sample (the ME leader's epoch-ms trade timestamp vs this host's clock at offer return). Negative
// or more than 1 h is garbage: counted as a clock anomaly and never recorded (cpp O3; the Java rule).
inline constexpr std::int64_t MAX_FORWARD_LATENCY_MS = 3'600'000;
inline bool recordForwardLatency(LatencyHistogram &h, std::uint64_t &anomalies, std::int64_t offerAtEpochMs,
    std::int64_t tradeTimestampMs) noexcept
{
    const std::int64_t latency = offerAtEpochMs - tradeTimestampMs;
    if ((latency < 0 || latency > MAX_FORWARD_LATENCY_MS || tradeTimestampMs <= 0) &&
        !app_seams::disabled(app_seams::Rule::LATENCY_RECORDS_GARBAGE))
    {
        anomalies++;
        return false;
    }
    h.record(latency);
    return true;
}

// O4: tradeId -> the monotonic time its offer returned OK. Bounded at 2^20 entries, direct-mapped by tradeId (dense
// ids: a live window of up to 1M trades never collides); a slot still holding another in-flight trade is overwritten
// and counted (bridge_ack_map_skips_total). Cleared at every onSessionDown (the Java P4 rule). Observability only.
class AckMap
{
public:
    static constexpr std::size_t CAPACITY = std::size_t(1) << 20;

    AckMap() : m_slots(new Slot[CAPACITY]) { clear(); }

    void onOffered(std::int64_t tradeId, std::int64_t atNs) noexcept
    {
        Slot &s = m_slots[static_cast<std::size_t>(tradeId) & (CAPACITY - 1)];
        if (s.used && s.tradeId != tradeId)
        {
            if (app_seams::disabled(app_seams::Rule::ACKMAP_UNBOUNDED))
            {
                m_unboundedExtra++; // RED shape: the map keeps growing instead
                return;
            }
            m_skips++;
        }
        else if (!s.used)
        {
            m_size++;
        }
        s.tradeId = tradeId;
        s.atNs = atNs;
        s.used = true;
    }

    // SettlementApplied 13: true (with the latency) when the trade was in flight here.
    bool onApplied(std::int64_t tradeId, std::int64_t nowNs, std::int64_t &latencyMs) noexcept
    {
        Slot &s = m_slots[static_cast<std::size_t>(tradeId) & (CAPACITY - 1)];
        if (!s.used || s.tradeId != tradeId)
        {
            m_unknown++;
            return false;
        }
        latencyMs = (nowNs - s.atNs) / 1'000'000;
        s.used = false;
        m_size--;
        return true;
    }

    void clear() noexcept
    {
        std::memset(static_cast<void *>(m_slots.get()), 0, sizeof(Slot) * CAPACITY);
        m_size = 0;
        m_unboundedExtra = 0;
    }

    std::size_t size() const noexcept { return m_size + m_unboundedExtra; }
    std::uint64_t skips() const noexcept { return m_skips; }
    std::uint64_t unknown() const noexcept { return m_unknown; }

private:
    struct Slot
    {
        std::int64_t tradeId;
        std::int64_t atNs;
        bool used;
    };
    std::unique_ptr<Slot[]> m_slots;
    std::size_t m_size = 0;
    std::size_t m_unboundedExtra = 0;
    std::uint64_t m_skips = 0;
    std::uint64_t m_unknown = 0;
};

// ------------------------------------------------------------------ the app's own counters (single writer B)

inline constexpr std::size_t CLOSE_REASONS = 7;            // client::CloseReason NONE..TRANSPORT_ERROR
inline constexpr std::size_t OFFER_STATUSES = 7;           // client::OfferStatus OK..TRANSPORT_ERROR
inline constexpr std::size_t FAULT_KINDS = 20;             // archive::FaultKind NONE..ARCHIVE_ERROR
inline constexpr std::size_t EGRESS_TEMPLATE_LABELS = 129; // money templates 0..127, then "other"

struct AppCounters
{
    std::array<std::uint64_t, CLOSE_REASONS> sessionLosses{}; // bridge_session_losses_total{reason}
    std::uint64_t connectClusterErrors = 0;                   // bridge_connect_cluster_error_total
    std::uint64_t connectCycles = 0;                          // bridge_connect_cycles_total (cycles started)
    std::uint64_t connectCyclesFailed = 0;                    // cycles that ended FAILED (retried)
    std::uint64_t storageStarvedSeconds = 0;                  // bridge_storage_starved_seconds_total
    std::uint64_t coreStaleTerm = 0;                          // bridge_core_stale_term_total
    std::uint64_t coreFutureTerm = 0;                         // bridge_core_future_term_total
    std::uint64_t coreMalformed = 0;                          // a core frame shorter than a money header
    std::array<std::uint64_t, EGRESS_TEMPLATE_LABELS> egressIgnored{}; // bridge_egress_ignored_total{template}
    std::array<std::uint64_t, 256> orderReject{};             // bridge_order_reject_total{reason} (counted ones)
    std::array<std::uint64_t, OFFER_STATUSES> offerRetry{};   // bridge_offer_retry_total{status}
    std::array<std::uint64_t, FAULT_KINDS> sourceFaults{};    // bridge_source_faults_total{kind} (ObserverTap)
    std::uint64_t clockAnomalies = 0;                         // bridge_clock_anomalies_total
    std::uint64_t ackMapSkips = 0;                            // bridge_ack_map_skips_total
    std::uint64_t ackUnknown = 0;                             // an ack for a trade not in flight here
    std::uint64_t registrationLost = 0;                       // 6.2 OrderReject UNREGISTERED_SESSION handled
    std::uint64_t duplicateIdentityRetries = 0;               // RegisterSession re-sent after REJECTED DUPLICATE_IDENTITY
    std::uint64_t sessionUps = 0;                             // onSessionUp calls (= C3 entries)
    std::uint64_t sessionDowns = 0;                           // onSessionDown calls
    std::uint64_t verdictEvents = 0;                          // verdict events drained into the log
};

// The client and registrar counters the endpoints show (copied by the agent from ClientCounters / RegistrarCounters;
// plain here so the pure renderer builds without the generated cluster codecs).
struct ClientStats
{
    std::uint64_t sessionsOpened = 0;
    std::uint64_t newLeaderEvents = 0;
    std::uint64_t keepAlivesSent = 0;
    std::uint64_t keepAliveFailures = 0;
    std::uint64_t offersInEndedTerms = 0;
    std::uint64_t egressImageLosses = 0;
};

struct RegistrationStats
{
    std::uint64_t acksAccepted = 0;
    std::uint64_t acksDuplicate = 0;
    std::uint64_t acksRejected = 0;
    std::uint64_t closesWhilePending = 0;
    std::uint64_t resends = 0;
    std::uint64_t sends = 0;
};

struct BridgeSnapshot
{
    std::uint64_t sequence = 0; // publish count (0 = never published)
    std::int64_t nowNs = 0;
    std::int64_t identity = 0;
    char version[32] = {};
    char build[48] = {}; // The binary's source_sha (compile-time), /health.build
    std::uint8_t driverMode = 0; // DriverMode
    std::uint8_t coreLink = 0;   // CoreLink
    std::uint8_t clientState = 0;
    std::uint8_t closeReason = 0;
    bool aeConnected = false;    // the client is CONNECTED (the Java meaning)
    bool registered = false;     // C3
    bool stopping = false;
    std::uint32_t notReady = 0;  // bits 0..13 (bit 14 is main's, added by the route)
    domain::bridge::BridgeView view;
    std::uint64_t laneAheadFences = 0; // 13: /health detail (bridge_fences_total{trigger="lane_ahead"})
    std::int64_t reportAgeMs = -1;          // from view.lastReportAt (-1: none this session)
    std::int64_t consumedPositionAgeMs = -1; // since view.consumedPosition last changed
    std::int64_t lastForwardAgeMs = -1;
    int stormCount = 0;
    bool confirming = false;
    std::uint64_t syncs = 0;
    ClientStats client;
    RegistrationStats registrar;
    AppCounters app;
    LatencyHistogram forwardLatency; // bridge_forward_latency_ms
    LatencyHistogram ackLatency;     // bridge_ack_latency_ms
    std::uint64_t dutyHeartbeat = 0;
    std::uint64_t dutyMaxCycleUs = 0;
    std::uint64_t logDropped = 0;
    std::uint64_t aeronErrors = 0;
};

static_assert(std::is_trivially_copyable_v<BridgeSnapshot>);

}}}

#endif
