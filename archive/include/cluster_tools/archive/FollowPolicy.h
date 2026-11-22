#ifndef CLUSTER_TOOLS_ARCHIVE_FOLLOW_POLICY_H
#define CLUSTER_TOOLS_ARCHIVE_FOLLOW_POLICY_H

// The per-source follow state machine (4.3 and the 4.5 decision table) as a
// deterministic function of the adapter's observations and a clock the caller passes in. Pure: it
// issues commands (ControlStep), the adapter (archive/aeron ArchiveSource, or a simulated archive in
// the tests) executes them and reports the result back. FollowerLoop<Port> (FollowerLoop.h) is the
// one driver both use, so the property test exercises the same walk the Aeron build runs.
//
// One duty iteration makes at most ONE archive control call (connect step, list, startReplay,
// probe, stopReplay); fragment polling, image observation and the async-error poll are not control
// calls. A paused follower (fragment limit 0) reads no fragments but still makes its control call,
// so rows 7a, 9 and 13 keep running (the pause rule).

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
    // The journal the frames are admitted as (FramePeek.h). The bridge's default: the ME journal.
    JournalProfile profile = ME_JOURNAL;
    // Re-list while attached to an ACTIVE recording at least this often, so a successor that appears while
    // the old recording still reads ACTIVE (a crash that left no stop position) is found; the old recording is
    // then drained to the position the archive reports, unchanged across two probes, before the successor is
    // followed. 0 = re-list only when the recording stops or the chain is exhausted (the bridge's behaviour).
    std::chrono::milliseconds relistInterval{ 0 };
    // The expected-epoch filter: a recording whose frames carry another epoch is not walked, and counted once
    // (foreignEpochRecordings). Given by the owner, never derived by the library; an epoch below 1 is refused.
    // Empty = no filter (the bridge, whose owner judges the epoch itself).
    std::optional<std::int64_t> expectedEpoch;
    // Recording keys carry the archive id the adapter learned at connect (ArchiveSourceConfig::keyByArchiveId).
    // An archive whose id is not configured takes its Aeron client id, a new value on every archive start: with
    // this on, every restart of such an archive is a history change and the memo of that source is dropped.
    // Off = ARCHIVE_ID_UNKNOWN in every key (the bridge's behaviour).
    bool keyByArchiveId = false;
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

// The stall verdict a source fault counts as (ArchiveMetrics::sourceStalls); empty for a fault that is not a stall
// (a connect, list, open or frame fault, FATAL). FollowPolicy counts its faults with exactly this mapping.
std::optional<StallReason> stallReasonOf(FaultKind kind) noexcept;

// The stall verdicts of one follower, for an owner that judges its source's freshness itself (one that is not the
// bridge; the bridge judges by the fault reports and the counters). A source check is the periodic look at the
// source while FOLLOWING, every probeInterval: the recording-state probe of an active or stopped recording (rows 11
// and 12) or the stuck check of a bounded replay (row 12b, which makes no archive call). A check passes when it
// judged the read and found it moving, or found nothing recorded beyond it. A check that cannot judge does not pass:
// the first one after an attach only sets the stuck rules' baseline, and while the owner pauses the follower the
// stuck rules do not judge (the pause rule), so a paused follower passes only when it is caught up. Every stall
// verdict (stallReasonOf, from any path) lands after the last pass. Times are the owner's clock (the TimePoint it
// passes in).
struct StallVerdicts
{
    std::optional<TimePoint> lastPassAt;   // the last source check that passed; empty before the first
    std::optional<StallReason> sincePass;  // a stall verdict reached after that pass; empty = none
    std::optional<StallReason> last;       // the last stall verdict of this follower, at lastAt
    TimePoint lastAt{};
    std::uint64_t count = 0;               // stall verdicts of this follower
};

// True when the last passed check is at most maxAge old at `now` and no stall verdict came after it. A follower that
// is not FOLLOWING makes no check, so its verdicts age out: false.
bool checkedClean(const StallVerdicts &verdicts, TimePoint now, std::chrono::milliseconds maxAge) noexcept;

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
    // The expected epoch of the filter (FollowSettings::expectedEpoch), learned by the owner at run time.
    // Throws std::invalid_argument below 1. A change forgets the recordings counted foreign and re-attaches.
    void expectEpoch(std::int64_t epoch, TimePoint now);
    const std::optional<std::int64_t> &expectedEpoch() const noexcept { return m_expectedEpoch; }
    const JournalProfile &profile() const noexcept { return m_settings.profile; }
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
    // 3.2 step 3: the async error poll runs every iteration; outside FOLLOWING an error
    // belongs to no replay and is dropped, a lost control session ends the connection.
    void onIdleAsyncError(const AsyncErrorPoll &asyncError, TimePoint now);
    // The poll ended on an owner sink ABORT (back-pressure): this iteration counts as paused
    // for rows 11, 12 and 12b.
    void onSinkBackPressure() noexcept;

    // Frame path (4.4 / 4.5 rows 2-4). checkFrame runs BEFORE the sink; onAccepted only after the
    // sink answered CONTINUE or BREAK, so an aborted and redelivered frame is never a regression.
    FrameCheck checkFrame(const Peek &peek, TimePoint now);
    // The epoch filter: true when the frame's epoch is not the expected one. onForeignEpoch marks the
    // recording foreign (counted once), finishes the replay and walks on to the next recording.
    bool foreignEpoch(const Peek &peek) const noexcept;
    void onForeignEpoch(const Peek &peek, TimePoint now);
    bool isForeign(const RecordingKey &key) const noexcept;
    void onAccepted(const Peek &peek, std::int64_t endPosition);
    // R1: SinkAction::CONSUME: the read position moves, no proof is recorded (and no hole marker read)
    void onConsumed(const Peek &peek, std::int64_t endPosition);
    void onRefused(Refusal reason, std::int64_t endPosition, TimePoint now);
    void onRefusalSkipped(std::int64_t endPosition);

    // State the adapter and the owner read.
    const Recording *currentRecording() const noexcept;
    std::size_t chainIndex() const noexcept { return m_index; }
    // R1: the listing walked holds a recording after the current one (recordingId order)
    bool hasLaterRecording() const noexcept { return m_index + 1 < m_chain.size(); }
    std::int64_t consumed() const noexcept { return m_consumed; }
    bool readWasMemoFree() const noexcept { return m_readMemoFree; }
    bool hasContinuation() const noexcept { return m_continueAfter.has_value(); }
    const std::optional<SourceFault> &lastFault() const noexcept { return m_lastFault; }
    void clearLastFault() noexcept { m_lastFault.reset(); }
    std::uint64_t faultCount() const noexcept { return m_faults; }
    const StallVerdicts &stallVerdicts() const noexcept { return m_verdicts; }
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
    void onListedWhileFollowing(const ListResult &result, TimePoint now);
    bool relistDue(TimePoint now) const noexcept;
    void resumeAndWalk(TimePoint now);
    void walk(std::size_t index, std::int64_t from, bool fromProof, std::int64_t proofSeq, TimePoint now);
    void fault(FaultKind kind, TimePoint now, std::string detail, Refusal refusal = Refusal::NONE,
        std::uint8_t openFailure = 0);
    void beginDetach(AfterDetach next, bool stopReplay);
    void backoff(TimePoint now, Phase next);
    void setAttachedGauge(bool attached) noexcept;
    void sourcePassed(TimePoint now) noexcept;

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
    bool m_fromSeed = false;      // the proof this read starts at was seeded by the owner
    bool m_resumeSeeded = false;
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
    // The continuation point of the current read: the last recording the read has fully
    // consumed (RECORDING_ENDED, or the chain end reached by DRAINED/EMPTY/a proof at a stop). Kept across
    // the chain-exhausted back-off, so a successor listed seconds later continues the same read; cleared
    // on attach, detach, reattach and on any source fault.
    std::optional<Recording> m_continueAfter;
    std::int64_t m_imagePosition = 0; // the image position seen last (>= consumed; the gap is padding only)
    std::vector<Recording> m_refusedChain;
    bool m_chainEmptyReported = false;

    std::optional<SourceFault> m_lastFault;
    std::uint64_t m_faults = 0;
    StallVerdicts m_verdicts;

    std::optional<std::int64_t> m_expectedEpoch;
    std::vector<RecordingKey> m_foreign;               // recordings of another epoch on this source
    TimePoint m_nextRelistAt{};
    bool m_successorSeen = false;                      // a timed re-list showed a recording after the ACTIVE one
    std::optional<std::int64_t> m_successorRecorded;   // the recorded position at the previous probe since then
};

}}

#endif
