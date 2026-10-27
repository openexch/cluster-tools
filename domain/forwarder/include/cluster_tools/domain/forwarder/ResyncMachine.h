#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_RESYNC_MACHINE_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_RESYNC_MACHINE_H

#include <cstddef>
#include <cstdint>

#include "cluster_tools/domain/forwarder/Dispatch.h"
#include "cluster_tools/domain/forwarder/DispatchRules.h"
#include "cluster_tools/domain/forwarder/SendRing.h"

// ResyncMachine - the dispatch machine D of RC2 S12 contract 4.3 and the resync rules of section 6.
//
// Inputs are what the agent decoded from core egress AFTER the term filter (rule S of 6.2): dispatch
// entries in arrival order, each with its 5.4 classification; PendingDispatchEnd; triggers; time.
// Output: pushes into the SendRing in the 5.5 form, strictly ascending in dispatchSeq between triggers
// (G3), and the RequestPendingDispatch (65) the agent must offer to the core.
//
//   D0 IDLE            not registered: entries dropped; entering D0 clears hold and abandons the request
//   D1 RESYNCING       a probe or page is outstanding or due: entries appended to hold
//   D2 LIVE            seq <= cursor duplicate; seq == cursor + 1 pushed; seq > cursor + 1 a gap (held, T5)
//   D3 OVERFLOW_WAIT   ring or hold overflowed: entries discarded until the ring drains to 32,768,
//                      then RESYNC(cursor)
//
// The reply of a 108 with count c is exactly the last c entries that arrived before it in this core term
// (6.1): the core emits a 65 reply inside one apply into the origin session's FIFO queue. Shape checks
// S1-S3 guard the assumption (a violation is T6, RESYNC(0)).
//
// Cursor invariant: every pending dispatch with seq <= cursor is in the ring, already sent, or known
// resolved. Pure: no I/O, no allocation after construction (hold and ring are fixed buffers).

namespace cluster_tools { namespace domain { namespace forwarder {

enum class Mode : std::uint8_t
{
    IDLE = 0,
    RESYNCING = 1,
    LIVE = 2,
    OVERFLOW_WAIT = 3,
};

enum class Trigger : std::uint8_t
{
    NONE = 0,
    T1 = 1, // entry into C3 (registration of a session, re-registration after a core NewLeaderEvent)
    T2 = 2, // ME NewLeaderEvent in M3, or a new ME session reaching M3
    T3 = 3, // BROADCAST GLOBAL AdmissionState change
    T4 = 4, // operator: SIGHUP or POST /resync
    T5 = 5, // live gap
    T6 = 6, // shape violation, malformed 104/105
    T7 = 7, // head probe answer above the cursor
    T8 = 8, // ring or hold overflow (resume at 32,768)
    T9 = 9, // 108 timeout (re-issue)
};
inline constexpr unsigned TRIGGER_COUNT = 10;

enum class RequestKind : std::uint8_t
{
    NONE,
    PROBE, // maxEntries 0, from = cursor
    PAGE,  // maxEntries 4096, from = pageFrom
};

struct PendingRequest
{
    RequestKind kind = RequestKind::NONE;
    std::int64_t correlationId = 0;
    std::int64_t fromDispatchSeq = 0;
    std::int32_t maxEntries = 0;
};

enum class ResyncVerdict : std::uint8_t
{
    NONE,
    PRE_S8_CORE, // F15: a probe 108 with count != 0 (exit 78)
    INVARIANT,   // F2: the cursor went backwards or a push was not ascending (exit 70)
};

enum class ShapeCheck : std::uint8_t
{
    S1 = 0,
    S2 = 1,
    S3 = 2,
};

struct HeldEntry
{
    Dispatch dispatch;
    Classification classification;
};

struct ResyncCounters
{
    std::uint64_t received = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t droppedIdle = 0;
    std::uint64_t discarded = 0;
    std::uint64_t pushed = 0;
    std::uint64_t unroutableSkipped = 0;
    std::uint64_t resyncs[TRIGGER_COUNT] = {};
    std::uint64_t pages = 0;
    std::uint64_t entries = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t shapeViolations[3] = {};
    std::uint64_t headProbeCovered = 0;
    std::uint64_t headProbeBehind = 0;
    std::uint64_t ringOverflows = 0;
    std::uint64_t holdOverflows = 0;
    std::uint64_t ringClearedEntries = 0;
    std::uint64_t staleEnds = 0;
    std::uint64_t heldResolved = 0;
    std::uint64_t requestsSent = 0;
    std::uint64_t ignoredIdleTriggers = 0;
    std::uint64_t seqBelowOne = 0; // review NOTE-2: refused in every mode
};

class ResyncMachine
{
public:
    static constexpr std::size_t HOLD_CAPACITY = 65'536;
    static constexpr std::size_t RESUME_RING_DEPTH = 32'768;
    static constexpr std::int32_t PAGE_ENTRIES = 4096;
    static constexpr std::int64_t END_TIMEOUT_NS = 5'000'000'000LL;     // T9
    static constexpr std::int64_t HEAD_PROBE_INTERVAL_NS = 10'000'000'000LL;
    static constexpr std::int64_t READY_ANSWER_AGE_NS = 15'000'000'000LL;
    static constexpr std::int64_t CORRELATION_BASE = 0x0065'0000'0000'0000LL;

    explicit ResyncMachine(SendRing &ring) : m_ring(ring) {}

    // ---- link
    // Entry into C3 (T1): head probe first (F15 check), then RESYNC(0).
    void onRegistered(std::int64_t nowNs) noexcept;
    // Every exit from C3: D0. hold cleared, the outstanding request abandoned. The ring is kept (its
    // entries are valid commands; the RESYNC(0) at the next C3 clears it).
    void onUnregistered() noexcept;

    // ---- triggers
    // T2, T3, T4, T6 (and T1 internally): clears ring and hold, cursor 0, starts at once (D0: waits for C3).
    void resyncFromZero(Trigger trigger, std::int64_t nowNs) noexcept;

    // ---- core input (after rule S)
    // Every core app frame of this term restarts the T9 timer.
    void onCoreFrame(std::int64_t nowNs) noexcept { m_lastCoreFrameNs = nowNs; m_haveCoreFrame = true; }
    void onDispatch(const Dispatch &d, const Classification &c, std::int64_t nowNs) noexcept;
    // A malformed 104/105 (T6): it may have carried a dispatch.
    void onMalformedDispatch(std::int64_t nowNs) noexcept { resyncFromZero(Trigger::T6, nowNs); }
    void onPendingEnd(std::int64_t correlationId, std::int32_t count, std::int64_t headDispatchSeq,
        std::int64_t nowNs) noexcept;

    // ---- time and the request the agent must offer
    void tick(std::int64_t nowNs) noexcept;
    // The request due now (nullptr if none). The agent offers it and calls onRequestSent() on a positive
    // offer; on back-pressure it simply asks again next cycle (same correlation id).
    const PendingRequest *requestDue() const noexcept { return m_requestDue ? &m_request : nullptr; }
    void onRequestSent(std::int64_t nowNs) noexcept;

    // ---- state
    Mode mode() const noexcept { return m_mode; }
    std::int64_t cursor() const noexcept { return m_cursor; }
    std::int64_t lastHead() const noexcept { return m_lastHead; }
    std::int64_t lastResyncEndSeq() const noexcept { return m_lastResyncEndSeq; }
    std::int64_t lastResyncDurationNs() const noexcept { return m_lastResyncDurationNs; }
    bool requestInFlight() const noexcept { return m_requestInFlight; }
    const PendingRequest &request() const noexcept { return m_request; }
    std::size_t holdSize() const noexcept { return m_hold.size(); }
    const HeldEntry &held(std::size_t i) const noexcept { return m_hold.at(i); }
    // A head probe or a resync end answered within READY_ANSWER_AGE_NS (readiness 4.4).
    bool answeredWithin(std::int64_t nowNs) const noexcept
    {
        return m_haveAnswer && nowNs - m_lastAnswerNs < READY_ANSWER_AGE_NS;
    }
    std::int64_t lastAnswerAgeNs(std::int64_t nowNs) const noexcept { return m_haveAnswer ? nowNs - m_lastAnswerNs : -1; }
    ResyncVerdict verdict() const noexcept { return m_verdict; }
    std::int64_t verdictDetail() const noexcept { return m_verdictDetail; }
    const ResyncCounters &counters() const noexcept { return m_counters; }
    Trigger lastTrigger() const noexcept { return m_lastTrigger; }

private:
    void startResync(std::int64_t from, Trigger trigger, std::int64_t nowNs) noexcept;
    void requestPage(std::int64_t nowNs) noexcept;
    void requestProbe(std::int64_t nowNs) noexcept;
    void abandonRequest() noexcept;
    // Push one entry in its 5.5 form; false = ring full (the caller enters D3).
    bool push(const HeldEntry &entry) noexcept;
    void enterOverflow(bool ringOverflow) noexcept;
    void shapeViolation(ShapeCheck check, std::int64_t nowNs) noexcept;
    void fail(ResyncVerdict verdict, std::int64_t detail) noexcept;
    void finishPage(std::int32_t count, std::int64_t head, std::int64_t nowNs) noexcept;
    void onProbeAnswer(std::int32_t count, std::int64_t head, std::int64_t nowNs) noexcept;

    SendRing &m_ring;
    FixedRing<HeldEntry, HOLD_CAPACITY> m_hold;

    Mode m_mode = Mode::IDLE;
    std::int64_t m_cursor = 0;
    std::int64_t m_pageFrom = 0;
    std::int64_t m_resyncFrom = 0;
    std::int64_t m_lastHead = 0;
    std::int64_t m_lastPushedSeq = 0; // ascending check within one resync / LIVE period
    std::int64_t m_lastResyncEndSeq = 0;
    std::int64_t m_resyncStartNs = 0;
    std::int64_t m_lastResyncDurationNs = 0;

    PendingRequest m_request;
    bool m_requestDue = false;
    bool m_requestInFlight = false;
    std::int64_t m_requestSentNs = 0;
    std::int64_t m_nextCorrelation = CORRELATION_BASE;
    bool m_probeVerified = false; // the C3-entry probe of this registration was answered
    std::int64_t m_nextProbeNs = 0;
    bool m_probeScheduled = false;

    std::int64_t m_lastCoreFrameNs = 0;
    bool m_haveCoreFrame = false;
    std::int64_t m_lastAnswerNs = 0;
    bool m_haveAnswer = false;

    ResyncVerdict m_verdict = ResyncVerdict::NONE;
    std::int64_t m_verdictDetail = 0;
    Trigger m_lastTrigger = Trigger::NONE;
    ResyncCounters m_counters;
};

const char *nameOf(Mode mode) noexcept;
const char *nameOf(Trigger trigger) noexcept;
const char *nameOf(ShapeCheck check) noexcept;

}}}

#endif
