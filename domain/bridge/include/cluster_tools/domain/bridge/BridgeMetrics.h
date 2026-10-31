#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_BRIDGE_METRICS_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_BRIDGE_METRICS_H

// The counters and gauges the bridge domain owns (RC2 S9b section 12): relaxed atomics, single writer (the duty
// thread B), fixed label sets, so the HTTP thread (S9c) reads them without a lock. Rendering, the latency
// histograms, /ready and /health are S9c's (from BridgeView and these series).

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/domain/bridge/Types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

// bridge_forwarded_total{kind} / bridge_forwarded_records_total{kind}: the frame kinds 61..68 by index.
enum class FrameKind : std::uint8_t
{
    K61,
    K62,
    K63,
    K64,
    K67,
    K68,
    COUNT
};
const char *frameKindLabel(FrameKind kind) noexcept;
FrameKind frameKindOf(std::uint16_t moneyTemplate) noexcept;

// bridge_frames_deferred_total{why}
enum class DeferReason : std::uint8_t
{
    STATE,      // not STREAMING (or halted), or fatal
    COMMAND,    // a source command is pending or a switch is in flight
    SOURCE,     // the frame is not from the attached source
    RING_FULL,
    KEYTABLE_FULL,
    CLOSED,     // the core port reported CLOSED
    COUNT
};
const char *deferReasonLabel(DeferReason reason) noexcept;

// bridge_settle_rejected_total{reason}: the codes 0..8, then other.
inline constexpr std::size_t SETTLE_REASON_LABELS = 10;
inline constexpr std::size_t HALT_REASON_LABELS = 6;
inline constexpr std::size_t SKIP_KIND_LABELS = 5;   // record templates 1..5
inline constexpr std::size_t BRIDGE_HALT_LABELS = 5; // BridgeHalt NONE..CORE_REFUSED

struct BridgeMetrics
{
    template<std::size_t N>
    using Counters = std::array<std::atomic<std::uint64_t>, N>;

    // gauges
    std::atomic<std::int64_t> halted{ 0 };            // bridge_halted
    std::atomic<std::int64_t> haltReason{ 0 };        // bridge_halt_reason (BridgeHalt code)
    std::atomic<std::int64_t> coreHaltReason{ 0 };    // bridge_core_halt_reason (the report's)
    std::atomic<std::int64_t> coreCursor{ 0 };        // bridge_core_cursor
    std::atomic<std::int64_t> lastOfferedSeq{ 0 };    // bridge_last_offered_seq
    std::atomic<std::int64_t> meEpoch{ 0 };           // bridge_me_epoch (syncEpoch)
    std::atomic<std::int64_t> inflightRingDepth{ 0 }; // bridge_inflight_ring_depth
    std::atomic<std::int64_t> stagedTrades{ 0 };      // bridge_staged_trades
    std::atomic<std::int64_t> degraded{ 0 };          // bridge_degraded (bitmask)
    std::array<std::atomic<std::int64_t>, archive::MAX_SOURCES> sourceVerdicts{}; // bridge_source_verdicts{source}

    // counters with fixed label sets
    Counters<static_cast<std::size_t>(FrameKind::COUNT)> forwarded{};        // bridge_forwarded_total{kind}
    Counters<static_cast<std::size_t>(FrameKind::COUNT)> forwardedRecords{}; // bridge_forwarded_records_total{kind}
    Counters<SKIP_KIND_LABELS> skipped{};                                     // bridge_skipped_total{kind=1..5}
    Counters<HALT_REASON_LABELS> feedHaltsSent{};                             // bridge_feed_halts_sent_total{reason}
    Counters<static_cast<std::size_t>(FenceTrigger::COUNT)> fences{};       // bridge_fences_total{trigger}
    Counters<static_cast<std::size_t>(BadReason::COUNT)> sourceBad{};       // bridge_source_bad_total{reason}
    Counters<SETTLE_REASON_LABELS> settleRejected{};                          // bridge_settle_rejected_total{reason}
    Counters<static_cast<std::size_t>(DeferReason::COUNT)> framesDeferred{}; // bridge_frames_deferred_total{why}
    Counters<BRIDGE_HALT_LABELS> haltRecovered{};                             // bridge_halt_recovered_total{reason}

    // plain counters
    std::atomic<std::uint64_t> sourceFailovers{ 0 };
    std::atomic<std::uint64_t> stagedDropped{ 0 };
    std::atomic<std::uint64_t> holeVerifications{ 0 };
    std::atomic<std::uint64_t> refusalVerifications{ 0 }; // a refusal on a memo read re-read memo-free (2026-10-31)
    std::atomic<std::uint64_t> stormRotations{ 0 };
    std::atomic<std::uint64_t> coreRefused{ 0 };
    std::atomic<std::uint64_t> reportQueries{ 0 };
    std::atomic<std::uint64_t> reportInvalid{ 0 };
    std::atomic<std::uint64_t> reportTimeouts{ 0 };
    std::atomic<std::uint64_t> reportStale{ 0 };
    std::atomic<std::uint64_t> backpressureAborts{ 0 };
    std::atomic<std::uint64_t> ringFullPauses{ 0 };
    std::atomic<std::uint64_t> recordingStartStepBack{ 0 };
    std::atomic<std::uint64_t> epochs{ 0 };              // bridge_epochs_total (Java name, "sync" meaning)
    std::atomic<std::uint64_t> feedHaltRejected{ 0 };    // bridge_feed_halt_rejected_total (alert)
    std::atomic<std::uint64_t> recordingsEnded{ 0 };
    std::atomic<std::uint64_t> sourceParks{ 0 };         // switches by 8.3 rows 1-3 (a park, not a failover)
    std::atomic<std::uint64_t> verdictLogDropped{ 0 };   // verdict events S9c did not drain in time

    static void add(std::atomic<std::uint64_t> &c, std::uint64_t n = 1) noexcept
    {
        c.fetch_add(n, std::memory_order_relaxed);
    }
    static std::uint64_t get(const std::atomic<std::uint64_t> &c) noexcept
    {
        return c.load(std::memory_order_relaxed);
    }
    static void set(std::atomic<std::int64_t> &g, std::int64_t v) noexcept
    {
        g.store(v, std::memory_order_relaxed);
    }
    static std::int64_t value(const std::atomic<std::int64_t> &g) noexcept
    {
        return g.load(std::memory_order_relaxed);
    }

    std::uint64_t fenceCount(FenceTrigger t) const noexcept { return get(fences[static_cast<std::size_t>(t)]); }
    std::uint64_t totalFences() const noexcept
    {
        std::uint64_t n = 0;
        for (const auto &c : fences)
        {
            n += get(c);
        }
        return n;
    }
    std::uint64_t forwardedFrames(FrameKind k) const noexcept { return get(forwarded[static_cast<std::size_t>(k)]); }
    std::uint64_t badCount(BadReason r) const noexcept { return get(sourceBad[static_cast<std::size_t>(r)]); }
    std::uint64_t haltsSent(HaltReason r) const noexcept { return get(feedHaltsSent[static_cast<std::size_t>(r)]); }
    std::uint64_t deferred(DeferReason r) const noexcept { return get(framesDeferred[static_cast<std::size_t>(r)]); }
    std::uint64_t recovered(BridgeHalt h) const noexcept { return get(haltRecovered[static_cast<std::size_t>(h)]); }
};

}}}

#endif
