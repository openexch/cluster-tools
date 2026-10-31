#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_BRIDGE_DOMAIN_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_BRIDGE_DOMAIN_H

// The bridge domain (RC2 S9b sections 3, 4, 6-10): the composition S9a's follower calls as its FrameSink and
// FollowObserver, and S9c's duty loop calls with the core's egress and the clock. One thread (B) only; every call
// is noexcept and allocates nothing after construction (the memo's own bookkeeping is S9a's).
//
// The rules it obeys (section 0): a record is fed exactly once, in order, or the bridge halts loudly.
//   G1  within one sync starting at the report cursor C, last := C; a record is forwarded only when
//       journalSeq == last + 1; last rises only by a forward; seq <= last is a SKIP; seq > last + 1 is a hole.
//   G2  RAM only; the core's lastAppliedJournalSeq is the only cursor.
//   G3  the domain reads the peek only (template, journalSeq, meEpoch, egressSeq); the core is the one judge.
//   G4  nothing silent: a SKIP, a hole, a refusal and a halt are each counted, and a halt is a FeedHalt.
//   G5  a staged or encoded record leaves only by an OK offer or a fence.
//
// The frozen duty iteration (S9c, section 3):
//   1. egress -> onReport / onSettleRejected / onAdminAck; NewLeader -> onFence(NEW_LEADER); registration accepted
//      this term -> onSessionUp; session lost -> onSessionDown
//   2. client.doWork()
//   3. n = sources.doWork(domain, domain.fragmentLimit(), now)
//   4. domain.onPollDone(n, now)
//   5. cmd = domain.takeSourceCommand(): SWITCH(i) -> sources.switchTo(i, now); REATTACH -> sources.reattach(now)
//   6. domain.tick(now)
//   7. heartbeat; idle; domain.fatal() != NONE -> exit 70

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/archive/Follow.h"
#include "cluster_tools/archive/ResumeMemo.h"
#include "cluster_tools/domain/bridge/BridgeMetrics.h"
#include "cluster_tools/domain/bridge/BridgeSettings.h"
#include "cluster_tools/domain/bridge/HaltMachine.h"
#include "cluster_tools/domain/bridge/InflightRing.h"
#include "cluster_tools/domain/bridge/Ports.h"
#include "cluster_tools/domain/bridge/Stager.h"
#include "cluster_tools/domain/bridge/Sync.h"
#include "cluster_tools/domain/bridge/Types.h"
#include "cluster_tools/domain/bridge/Verdicts.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

class BridgeDomain final : public archive::FrameSink, public archive::FollowObserver
{
public:
    // `archive`: the S9a series the behind dwell reads (8.4, review FIX-6).
    BridgeDomain(const BridgeSettings &settings, archive::ResumeMemo &memo, const archive::ArchiveMetrics &archive,
        FrameEncoder &encoder, CorePort &core, BridgeMetrics &metrics);

    BridgeDomain(const BridgeDomain &) = delete;
    BridgeDomain &operator=(const BridgeDomain &) = delete;

    // core session (S9c, step 1)
    void onSessionUp(TimePoint now) noexcept;
    void onSessionDown(TimePoint now) noexcept;
    void onFence(FenceTrigger trigger, TimePoint now) noexcept;
    void onReport(const FeedPositionReport &report, TimePoint now) noexcept;
    void onSettleRejected(const SettleRejected &reject, TimePoint now) noexcept;
    void onAdminAck(const AdminAck &ack, TimePoint now) noexcept;
    // A 17 / 110 / 114 the codec could not decode (5.2 MALFORMED): a malformed 17 counts bridge_report_invalid_total.
    void onMalformedEgress(std::uint16_t templateId, TimePoint now) noexcept;

    // duty loop (steps 3-6)
    int fragmentLimit() const noexcept;
    void onPollDone(int fragments, TimePoint now) noexcept;
    SourceCommand takeSourceCommand() noexcept;
    void tick(TimePoint now) noexcept;

    // S9a FrameSink / FollowObserver
    archive::SinkAction onFrame(const archive::JournalFrame &frame) noexcept override;
    archive::RefusalAction onRefused(const archive::RefusedFrame &frame) noexcept override;
    void onFault(const archive::SourceFault &fault) noexcept override;
    void onAttached(const archive::AttachInfo &info) noexcept override;
    void onRecordingEnded(std::size_t source, const archive::Recording &recording) noexcept override;
    void onChainEmpty(std::size_t source) noexcept override;
    void onChainExhausted(std::size_t source) noexcept override;
    void onSourceRefused(std::size_t source, const archive::Recording &recording) noexcept override;

    BridgeView view() const noexcept;
    Fatal fatal() const noexcept { return m_fatal; }

    // The verdict log (8.1: each verdict logged once): S9c drains it into its log after every duty iteration.
    std::size_t drainVerdictEvents(VerdictEvent *out, std::size_t max) noexcept;

    // Read-only state for the tests and S9c's /health detail.
    const InflightRing &ring() const noexcept { return m_ring; }
    const Stager &stager() const noexcept { return m_stager; }
    const VerdictTable &verdicts() const noexcept { return m_verdicts; }
    const HaltRecord &halt() const noexcept { return m_halt; }
    DecisionRow waiting() const noexcept { return m_waitRow; }
    std::size_t current() const noexcept { return m_current; }
    std::uint64_t syncs() const noexcept { return m_syncs; }
    std::int64_t syncCursor() const noexcept { return m_syncCursor; }
    std::int64_t coversSeq() const noexcept { return m_query.coversSeq; }
    bool registeredThisTerm() const noexcept { return m_registered; }
    std::int64_t stormCursor() const noexcept { return m_storm.active ? m_storm.cursor : -1; }
    int stormCount() const noexcept { return m_storm.active ? m_storm.count : 0; }
    std::uint32_t stormTried() const noexcept { return m_storm.active ? m_storm.tried : 0; }
    bool confirming() const noexcept { return m_confirming; }

private:
    class Output final : public FrameOutput
    {
    public:
        explicit Output(BridgeDomain &d) noexcept : m_d(d) {}
        StepResult emit(const std::uint8_t *frame, std::size_t length, std::uint16_t moneyTemplate,
            const OfferedRecord *records, std::size_t n) noexcept override;

    private:
        BridgeDomain &m_d;
    };

    struct StormRecord
    {
        bool active = false;
        std::int64_t cursor = 0;
        int count = 0;
        std::uint32_t tried = 0;   // sources that OFFERED C' + 1 (bit per source; the 2026-10-31 9.6 amendment)
        std::uint32_t visited = 0; // sources the storm guard switched to for this cursor
    };

    struct StepBack
    {
        static constexpr std::size_t SLOTS = 64;
        std::array<std::int64_t, SLOTS> recordingId{};
        std::array<std::int64_t, SLOTS> firstSeq{};
        std::size_t used = 0;
        std::size_t next = 0;
    };

    // frame path
    archive::SinkAction defer(DeferReason why) noexcept;
    void noteAccepted(const archive::JournalFrame &frame) noexcept;
    void noteForward(const archive::JournalFrame &frame) noexcept;
    StepResult flushStaging() noexcept;
    bool handleStep(StepResult r) noexcept; // true when OK; counts back-pressure, sets CLOSED / fatal

    // offers
    void wantQueryNow() noexcept;
    void tryOfferQuery() noexcept;
    void offerQueryFrame() noexcept;
    void offerFeedHalt() noexcept;
    std::int64_t nextCorrelation() noexcept { return ++m_correlation; }

    // reports and syncs
    void acceptReport(std::int64_t cursor, std::int64_t epoch) noexcept;
    void popAndProve(std::int64_t cursor) noexcept;
    void syncStart(std::int64_t cursor, std::int64_t epoch) noexcept;
    void beginNormalSync(std::int64_t cursor, std::int64_t epoch) noexcept;
    void fence(FenceTrigger trigger, bool external) noexcept;
    void streamingReport(std::int64_t cursor, std::int64_t epoch, std::int64_t coversSeq, std::int64_t previous) noexcept;
    void haltedReport(std::int64_t cursor, std::int64_t epoch, std::int64_t previous) noexcept;
    bool haltExitReached(std::int64_t cursor) const noexcept;
    void leaveHalt(bool recovered) noexcept;

    // failover
    bool feeding() const noexcept;
    std::int64_t e() const noexcept { return m_last + 1; }
    void setVerdict(std::size_t source, Verdict v) noexcept;
    void rotate() noexcept;
    void runDecision() noexcept;
    void requestSwitch(std::size_t index, bool failover) noexcept;
    void requestReattach() noexcept;
    void switchDone(std::size_t source) noexcept;
    void clearWait() noexcept;
    void expireVerdicts() noexcept;
    void behindDwell() noexcept;
    bool lanePastLast() const noexcept { return m_haveReport && m_C > m_last; }

    // halt
    void confirmThenHalt(const Decision &d) noexcept;
    void enterHalting(BridgeHalt halt, std::int64_t seen, std::int64_t foreignEpoch) noexcept;
    void driveHalting() noexcept;
    void enterCoreRefused(std::int64_t refusedSeq) noexcept;
    void keepHalt() noexcept;
    void startRescan() noexcept;
    void startStormRetry() noexcept;
    bool stormRotatable() const noexcept;
    std::size_t untriedStormSource(bool allowCurrent) const noexcept;
    std::size_t ringSourceOf(std::int64_t seq) const noexcept;

    void publish() noexcept;
    void logVerdict(std::size_t source, const Verdict &v) noexcept;
    void dropStaging() noexcept;

    const BridgeSettings m_settings;
    archive::ResumeMemo &m_memo;
    const archive::ArchiveMetrics &m_archive;
    FrameEncoder &m_encoder;
    CorePort &m_core;
    BridgeMetrics &m_metrics;
    Output m_output;
    Stager m_stager;
    InflightRing m_ring;
    KeyTable m_keys;
    VerdictTable m_verdicts;
    std::uint8_t m_frame[OUTPUT_BUFFER]{};

    TimePoint m_now{};
    State m_state = State::DOWN;
    Fatal m_fatal = Fatal::NONE;
    bool m_registered = false;
    bool m_closed = false;
    mutable int m_lastLimit = 0;

    // reports
    QueryTracker m_query;
    bool m_awaitSync = false;   // the answer to the outstanding query starts a sync (or applies the resumption rule)
    bool m_rescanPending = false;
    bool m_haveReport = false;
    std::int64_t m_C = 0;
    std::int64_t m_E = 0;
    bool m_coreHalted = false;
    std::uint8_t m_coreHaltRaw = 0;
    bool m_reportThisTerm = false;
    TimePoint m_nextPeriodicAt{};
    std::int64_t m_correlation = 0;

    // the sync
    std::uint64_t m_syncs = 0;
    std::int64_t m_syncCursor = 0;
    std::int64_t m_last = 0;
    std::int64_t m_syncEpoch = 0;
    std::int64_t m_offeredTail = 0; // the highest seq offered OK in this sync (the ring tail)
    bool m_refusalFenceSinceSync = false;
    bool m_keysFull = false;
    StormRecord m_storm;

    // sources
    std::size_t m_current = 0;
    bool m_attached = false;
    bool m_live = false;
    bool m_exhausted = false;
    // A frame was accepted since the read last started from a resume point (an attach after a SWITCH or REATTACH the
    // domain issued, or after a fault, when the follower resumes from the memo). The follower's successor attaches
    // (the walk entering the next recording) do NOT reset it: the read position is then past the last accepted
    // frame, and a sync start must rewind (7.2).
    bool m_acceptedSinceAttach = false;
    bool m_resumePending = true;
    // review NOTE-8: an own-epoch frame was accepted since the read last started from a resume point (or the read
    // resumed from a proof, which implies one); a foreign frame before it is a stale prefix, not a re-genesis
    bool m_ownEpochSeen = false;
    bool m_readMemoFree = true; // the read started at chain index 0 (the resume point's AttachInfo.memoFree)
    std::array<std::int64_t, archive::MAX_SOURCES> m_refusalVerifiedAt{}; // the e a memo-read refusal was verified at
    bool m_attachMemoFree = true;
    bool m_switchInFlight = false;
    std::size_t m_switchTarget = 0;
    SourceCommand m_command;
    TimePoint m_sourceSince{};
    bool m_dwellRunning = false;
    TimePoint m_dwellStart{};
    std::array<int, archive::MAX_SOURCES> m_strikes{};
    std::array<int, archive::MAX_SOURCES> m_regressions{};
    std::size_t m_lastForwardSource = NO_SOURCE;
    TimePoint m_lastForwardAt{};
    archive::RecordingKey m_readKey{};
    bool m_expectFirstSeq = false;
    std::array<StepBack, archive::MAX_SOURCES> m_stepBack{};

    // the decision wait (8.3 rows 1-3, the confirming report)
    DecisionRow m_waitRow = DecisionRow::UNDECIDED;
    bool m_confirming = false;
    Decision m_confirmDecision;
    std::int64_t m_confirmE = 0; // the e the outstanding confirmation is for (review BLOCKER-1: one query per (row, e))
    // the 9.6 amendment (review FIX-2): a CORE_REFUSED retry restricts the rotation to the untried sources
    bool m_stormProbe = false;
    bool m_stormRetryPending = false;
    TimePoint m_behindRescanAt{};
    std::size_t m_rescanNext = 0; // the source the last all-behind rescan re-derived
    std::uint32_t m_degraded = 0;

    // the halt
    HaltRecord m_halt;
    bool m_rescan = false;   // a GAP re-scan sync is running (9.4 b)
    TimePoint m_nextRescanAt{};
    TimePoint m_nextHaltedQueryAt{};

    // the verdict log
    static constexpr std::size_t VERDICT_LOG = 256;
    std::array<VerdictEvent, VERDICT_LOG> m_log{};
    std::size_t m_logHead = 0;
    std::size_t m_logSize = 0;
};

}}}

#endif
