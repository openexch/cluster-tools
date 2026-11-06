#ifndef CLUSTER_TOOLS_ARCHIVE_ARCHIVE_METRICS_H
#define CLUSTER_TOOLS_ARCHIVE_ARCHIVE_METRICS_H

// The counters S9a owns (RC2 S9a contract 8.1). Single writer (the duty thread B), relaxed atomics,
// labels from a fixed set, so the HTTP thread (S9c) reads without a lock. The three max gauges are
// written by B with a CAS max and read with exchange(0): "max since the previous scrape" needs no
// other write from the reader. Rendering (/metrics) is S9c.

#include "cluster_tools/archive/FramePeek.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace archive {

inline constexpr std::size_t MAX_SOURCES = 16;

// bridge_source_stalls_total{reason}
enum class StallReason : std::uint8_t
{
    NEVER_CONNECTED,
    IMAGE_LOST,
    ARCHIVE_GONE,
    STALL,
    STALL_AFTER_STOP,
    BOUNDED_STUCK,
    EARLY_EOF,
    ARCHIVE_ERROR,
    COUNT
};
const char *stallLabel(StallReason reason) noexcept;

// bridge_replay_open_failures_total{reason}
enum class OpenFailure : std::uint8_t
{
    STORAGE,       // the 4.3 step 2 precheck, no request sent
    ENDPOINT,      // port-0 endpoint not resolved in 10 s
    MAX_REPLAYS,
    STORAGE_SPACE,
    INVALID_POSITION,
    ARCHIVE_ERROR,
    TIMEOUT,
    COUNT
};
const char *openFailureLabel(OpenFailure failure) noexcept;

// bridge_memo_invalidations_total{reason} (5.3 labels)
enum class MemoReason : std::uint8_t
{
    EPOCH_CHANGED,
    CURSOR_REGRESSED,
    HISTORY_CHANGED,
    POSITION_REJECTED,
    SEQ_REGRESSION,
    RESUME_BELT,
    HOLE_VERIFY,
    COUNT
};
const char *memoReasonLabel(MemoReason reason) noexcept;

enum class ResumeKind : std::uint8_t
{
    COLD,
    PROOF,
    COUNT
};

struct ArchiveMetrics
{
    template<std::size_t N>
    using Counters = std::array<std::atomic<std::uint64_t>, N>;

    Counters<MAX_SOURCES> sourceAttached{};          // gauge 0/1 per source
    Counters<MAX_SOURCES> chainRecordings{};         // gauge per source
    Counters<MAX_SOURCES> sourceRefusedMtu{};        // gauge 0/1 per source, reason "mtu"
    Counters<static_cast<std::size_t>(StallReason::COUNT)> sourceStalls{};
    Counters<static_cast<std::size_t>(OpenFailure::COUNT)> replayOpenFailures{};
    Counters<static_cast<std::size_t>(Refusal::COUNT)> framesRefused{};
    Counters<static_cast<std::size_t>(MemoReason::COUNT)> memoInvalidations{};
    Counters<static_cast<std::size_t>(ResumeKind::COUNT)> memoResumes{};
    std::atomic<std::uint64_t> memoTrims{ 0 };
    std::atomic<std::uint64_t> emptyRecordingsSkipped{ 0 };
    std::atomic<std::uint64_t> recordingSuccessions{ 0 };
    std::atomic<std::uint64_t> foreignStreamDescriptorsDropped{ 0 };
    std::atomic<std::int64_t> sourceBacklogBytes{ 0 };
    std::atomic<std::int64_t> replayRecordingPosition{ 0 };
    std::atomic<std::int64_t> replayConsumedPosition{ 0 };
    std::atomic<std::uint64_t> archiveCallMaxMs{ 0 };
    std::atomic<std::uint64_t> invokerMaxGapMs{ 0 };
    std::atomic<std::uint64_t> dutyIterationMaxUs{ 0 };

    static void add(std::atomic<std::uint64_t> &counter, std::uint64_t n = 1) noexcept
    {
        counter.fetch_add(n, std::memory_order_relaxed);
    }

    static std::uint64_t get(const std::atomic<std::uint64_t> &counter) noexcept
    {
        return counter.load(std::memory_order_relaxed);
    }

    // CAS max by the single writer.
    static void raiseMax(std::atomic<std::uint64_t> &gauge, std::uint64_t value) noexcept
    {
        std::uint64_t seen = gauge.load(std::memory_order_relaxed);
        while (value > seen && !gauge.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
    }

    // Reader side: max since the previous scrape.
    static std::uint64_t takeMax(std::atomic<std::uint64_t> &gauge) noexcept
    {
        return gauge.exchange(0, std::memory_order_relaxed);
    }

    void stall(StallReason reason) noexcept { add(sourceStalls[static_cast<std::size_t>(reason)]); }
    void openFailure(OpenFailure failure) noexcept { add(replayOpenFailures[static_cast<std::size_t>(failure)]); }
    void refused(Refusal refusal) noexcept { add(framesRefused[static_cast<std::size_t>(refusal)]); }
    void memoInvalidated(MemoReason reason) noexcept { add(memoInvalidations[static_cast<std::size_t>(reason)]); }
    void memoResumed(ResumeKind kind) noexcept { add(memoResumes[static_cast<std::size_t>(kind)]); }

    std::uint64_t stalls(StallReason reason) const noexcept { return get(sourceStalls[static_cast<std::size_t>(reason)]); }
    std::uint64_t openFailures(OpenFailure failure) const noexcept
    {
        return get(replayOpenFailures[static_cast<std::size_t>(failure)]);
    }
    std::uint64_t refusals(Refusal refusal) const noexcept { return get(framesRefused[static_cast<std::size_t>(refusal)]); }
    std::uint64_t invalidations(MemoReason reason) const noexcept
    {
        return get(memoInvalidations[static_cast<std::size_t>(reason)]);
    }
    std::uint64_t resumes(ResumeKind kind) const noexcept { return get(memoResumes[static_cast<std::size_t>(kind)]); }
    std::uint64_t totalStalls() const noexcept
    {
        std::uint64_t total = 0;
        for (const auto &c : sourceStalls)
        {
            total += get(c);
        }
        return total;
    }
};

}}

#endif
