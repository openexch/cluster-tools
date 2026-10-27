#ifndef CLUSTER_TOOLS_ARCHIVE_FOLLOW_POLICY_H
#define CLUSTER_TOOLS_ARCHIVE_FOLLOW_POLICY_H

// The per-source follow state machine (RC2 S9a contract 4.2, 4.3 and the 4.5 decision table) as a
// deterministic function of the adapter's observations and a clock the caller passes in. Pure: it
// issues commands (ControlStep), the adapter (archive/aeron ArchiveSource, or a simulated archive in
// the tests) executes them and reports the result back. FollowerLoop<Port> (FollowerLoop.h) is the
// one driver both use, so the property test exercises the same walk the Aeron build runs.
//
// One duty iteration makes at most ONE archive control call (connect step, list, startReplay,
// probe, stopReplay); fragment polling, image observation and the async-error poll are not control
// calls. A paused follower (fragment limit 0) reads no fragments but still makes its control call,
// so rows 7a, 9 and 13 keep running (the pause rule; review F1).

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/archive/Chain.h"
#include "cluster_tools/archive/Follow.h"
#include "cluster_tools/archive/ResumeMemo.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cluster_tools { namespace archive {

struct FollowSettings
{
    std::int32_t journalMtu = 1408;     // BRIDGE_JOURNAL_MTU (4.2 row 4)
    std::int32_t journalStreamId = 4001; // BRIDGE_JOURNAL_STREAM (the chain's stream filter)
    std::chrono::milliseconds neverConnectedTimeout{ 15'000 };    // row 8 (BridgeAgent.java:47)
    std::chrono::milliseconds probeInterval{ 10'000 };            // rows 9-12, 12b (BridgeAgent.java:49)
    std::chrono::milliseconds endpointResolveTimeout{ 10'000 };   // 4.3 step 5
    std::chrono::milliseconds backoff{ 1'000 };                   // rows 14, 15, faults (BridgeAgent.java:44)
};

enum class Phase : std::uint8_t
{
    IDLE,         // not attached (another source is, or the owner detached it)
    DISCONNECTED, // connect due at m_dueAt
    CONNECTING,   // async connect being polled
    LIST_DUE,
    OPEN_DUE,     // storage precheck + add the port-0 replay subscription
    RESOLVING,    // waiting for the resolved endpoint (10 s)
    START_DUE,    // startReplay to the resolved endpoint
    FOLLOWING,
    DETACH_DUE,   // stopReplay (unless connect level) + close the subscription
    BACKOFF,      // re-list at m_dueAt
    REFUSED,      // MTU_MISMATCH: re-list every backoff until the chain changes
    FATAL
};
const char *phaseName(Phase phase) noexcept;

enum class ControlStep : std::uint8_t
{
    NONE,
    CONNECT,           // control call (one async-connect poll step)
    LIST,              // control call
    OPEN_SUBSCRIPTION, // driver call: storage precheck, add the replay subscription
    START_REPLAY,      // control call
    PROBE,             // control call (getRecordingPosition, then getStopPosition when stopped)
    STOP_REPLAY,       // control call, then the subscription is closed
    CLOSE_SUBSCRIPTION // no control call
};

bool isControlCall(ControlStep step) noexcept;

struct ReplayRequest
{
    std::int64_t recordingId = 0;
    std::int64_t from = 0;
    std::int64_t length = 0; // INT64_MAX for the active recording, stop - from for a stopped one
    std::int32_t termBufferLength = 0;
    std::string endpoint;    // the resolved replay endpoint (START_REPLAY)
};

enum class FrameCheck : std::uint8_t
{
    OK,
    FAULTED // rows 3/4: the follower faulted; the frame is not delivered
};

class FollowPolicy
{
public:
    FollowPolicy(std::size_t sourceIndex, FollowSettings settings, ResumeMemo &memo, ArchiveMetrics *metrics,
        FollowObserver *observer);

    std::size_t sourceIndex() const noexcept { return m_source; }
    Phase phase() const noexcept { return m_phase; }
    bool attached() const noexcept { return Phase::FOLLOWING == m_phase; }
    bool idle() const noexcept { return Phase::IDLE == m_phase; }
    bool connected() const noexcept { return m_connected; }

    // Owner commands.
    void attach(TimePoint now);   // IDLE -> connect (or list when still connected)
    void detach(TimePoint now);   // finish the replay (stopReplay) and go IDLE, disconnected
    void reattach(TimePoint now); // rewind: finish the replay, re-list, resume from the memo (5.2 rule 5, fences)
#ifdef CT_TEST_SEAMS
    void forceProbe() noexcept;   // test seam (9.3 invokerRunsWhileArchiveCallBlocks): probe on the next step
#endif

    // Duty iteration inputs.
    void onFatal();
    void setPaused(bool paused) noexcept;
    ControlStep nextControl(TimePoint now);
    const ReplayRequest &replayRequest() const noexcept { return m_request; }
    std::int64_t replaySessionId() const noexcept { return m_replaySessionId; }
    bool readingAllowed() const noexcept { return Phase::FOLLOWING == m_phase && !m_paused; }

    void onConnect(const ConnectPoll &result, TimePoint now);
    void onListed(const ListResult &result, TimePoint now);
    void onSubscription(const SubscriptionResult &result, TimePoint now);
    void onEndpoint(const EndpointPoll &result, TimePoint now);
    void onReplayStarted(const StartResult &result, TimePoint now);
    void onDetached(TimePoint now);
    void onProbe(const ProbeResult &result, TimePoint now);
    void afterPoll(const ImageObservation &image, const AsyncErrorPoll &asyncError, TimePoint now);
    // 3.2 step 3 (review N-4): the async error poll runs every iteration; outside FOLLOWING an error
    // belongs to no replay and is dropped, a lost control session ends the connection.
    void onIdleAsyncError(const AsyncErrorPoll &asyncError, TimePoint now);
    // Review F-4: the poll ended on an owner sink ABORT (back-pressure): this iteration counts as paused
    // for rows 11, 12 and 12b.
    void onSinkBackPressure() noexcept;

    // Frame path (4.4 / 4.5 rows 2-4). checkFrame runs BEFORE the sink; onAccepted only after the
    // sink answered CONTINUE or BREAK, so an aborted and redelivered frame is never a regression.
    FrameCheck checkFrame(const Peek &peek, TimePoint now);
    void onAccepted(const Peek &peek, std::int64_t endPosition);
    void onRefused(Refusal reason, std::int64_t endPosition, TimePoint now);
    void onRefusalSkipped(std::int64_t endPosition);

    // State the adapter and the owner read.
    const Recording *currentRecording() const noexcept;
    std::size_t chainIndex() const noexcept { return m_index; }
    std::int64_t consumed() const noexcept { return m_consumed; }
    bool readWasMemoFree() const noexcept { return m_readMemoFree; }
    bool hasContinuation() const noexcept { return m_continueAfter.has_value(); }
    const std::optional<SourceFault> &lastFault() const noexcept { return m_lastFault; }
    void clearLastFault() noexcept { m_lastFault.reset(); }
    std::uint64_t faultCount() const noexcept { return m_faults; }
    bool wantsDisconnect() const noexcept { return m_disconnectRequested; }
    void onDisconnected() noexcept;

private:
    enum class AfterDetach : std::uint8_t
    {
        NEXT_RECORDING, // DRAINED: next recording of this listing
        SUCCESSOR,      // RECORDING_ENDED: re-list, follow the successor
        RELIST,         // stale listing or the 4.3 step 6 re-list
        REATTACH,       // owner rewind: re-list and resume from the memo
        BACKOFF,        // a source fault: back off 1 s, re-attach the same source (re-list)
        DISCONNECT,     // connect level fault: back off 1 s, reconnect
        IDLE            // owner detach
    };

    void beginAttach(TimePoint now);
    void resumeAndWalk(TimePoint now);
    void walk(std::size_t index, std::int64_t from, bool fromProof, std::int64_t proofSeq, TimePoint now);
    void fault(FaultKind kind, TimePoint now, std::string detail, Refusal refusal = Refusal::NONE,
        std::uint8_t openFailure = 0);
    void beginDetach(AfterDetach next, bool stopReplay);
    void backoff(TimePoint now, Phase next);
    void setAttachedGauge(bool attached) noexcept;

    std::size_t m_source;
    FollowSettings m_settings;
    ResumeMemo &m_memo;
    ArchiveMetrics *m_metrics;
    FollowObserver *m_observer;

    Phase m_phase = Phase::IDLE;
    Phase m_afterBackoff = Phase::LIST_DUE;
    TimePoint m_dueAt{};
    bool m_connected = false;
    bool m_disconnectRequested = false;
    bool m_paused = false;

    std::vector<Recording> m_chain;
    std::size_t m_index = 0;
    std::int64_t m_from = 0;
    bool m_fromProof = false;
    std::int64_t m_proofSeq = 0;
    bool m_readMemoFree = true;

    ReplayRequest m_request;
    TimePoint m_resolveDeadline{};
    bool m_replayStarted = false;
    std::int64_t m_replaySessionId = 0;
    TimePoint m_replayStartedAt{};
    bool m_imageSeen = false;
    std::int64_t m_consumed = 0;
    bool m_havePrevSeq = false;
    std::int64_t m_prevSeq = 0;
    bool m_beltPending = false;
    TimePoint m_nextProbeAt{};
    bool m_probeNow = false;      // row 7a (image end on a live follow) or the test seam
    bool m_probeIsImageEnd = false;
    std::optional<std::int64_t> m_probeBaseline;   // rows 11, 12: consumed at the previous unpaused probe
    TimePoint m_nextBoundedCheckAt{};
    std::optional<std::int64_t> m_boundedBaseline; // row 12b

    AfterDetach m_afterDetach = AfterDetach::BACKOFF;
    bool m_stopReplayOnDetach = false;
    bool m_relistForInvalidPosition = false;
    std::int64_t m_invalidFrom = 0;
    RecordingKey m_invalidKey;
    bool m_relistedOnce = false;
    // The continuation point of the current read (review F-1): the last recording the read has fully
    // consumed (RECORDING_ENDED, or the chain end reached by DRAINED/EMPTY/a proof at a stop). Kept across
    // the chain-exhausted back-off, so a successor listed seconds later continues the same read; cleared
    // on attach, detach, reattach and on any source fault.
    std::optional<Recording> m_continueAfter;
    std::int64_t m_imagePosition = 0; // the image position seen last (>= consumed; the gap is padding only)
    std::vector<Recording> m_refusedChain;
    bool m_chainEmptyReported = false;

    std::optional<SourceFault> m_lastFault;
    std::uint64_t m_faults = 0;
};

}}

#endif
