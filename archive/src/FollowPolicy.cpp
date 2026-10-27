#include "cluster_tools/archive/FollowPolicy.h"

#include "cluster_tools/archive/TestSeams.h"

#include <algorithm>
#include <limits>

namespace cluster_tools { namespace archive {

using test_seams::Rule;
using test_seams::disabled;

const char *phaseName(Phase phase) noexcept
{
    switch (phase)
    {
        case Phase::IDLE: return "IDLE";
        case Phase::DISCONNECTED: return "DISCONNECTED";
        case Phase::CONNECTING: return "CONNECTING";
        case Phase::LIST_DUE: return "LIST_DUE";
        case Phase::OPEN_DUE: return "OPEN_DUE";
        case Phase::RESOLVING: return "RESOLVING";
        case Phase::START_DUE: return "START_DUE";
        case Phase::FOLLOWING: return "FOLLOWING";
        case Phase::DETACH_DUE: return "DETACH_DUE";
        case Phase::BACKOFF: return "BACKOFF";
        case Phase::REFUSED: return "REFUSED";
        case Phase::FATAL: return "FATAL";
    }
    return "?";
}

const char *faultName(FaultKind kind) noexcept
{
    switch (kind)
    {
        case FaultKind::NONE: return "NONE";
        case FaultKind::FATAL: return "FATAL";
        case FaultKind::CONNECT_FAILED: return "CONNECT_FAILED";
        case FaultKind::LIST_FAILED: return "LIST_FAILED";
        case FaultKind::MALFORMED_EXTENT: return "MALFORMED_EXTENT";
        case FaultKind::REPLAY_STORAGE: return "REPLAY_STORAGE";
        case FaultKind::REPLAY_ENDPOINT_UNRESOLVED: return "REPLAY_ENDPOINT_UNRESOLVED";
        case FaultKind::REPLAY_OPEN: return "REPLAY_OPEN";
        case FaultKind::POSITION_REJECTED: return "POSITION_REJECTED";
        case FaultKind::SCHEMA_REFUSED: return "SCHEMA_REFUSED";
        case FaultKind::SEQ_REGRESSION_IN_RECORDING: return "SEQ_REGRESSION_IN_RECORDING";
        case FaultKind::RESUME_BELT: return "RESUME_BELT";
        case FaultKind::EARLY_EOF: return "EARLY_EOF";
        case FaultKind::IMAGE_LOST: return "IMAGE_LOST";
        case FaultKind::NEVER_CONNECTED: return "NEVER_CONNECTED";
        case FaultKind::ARCHIVE_GONE: return "ARCHIVE_GONE";
        case FaultKind::STALL: return "STALL";
        case FaultKind::STALL_AFTER_STOP: return "STALL_AFTER_STOP";
        case FaultKind::STALL_BOUNDED_STUCK: return "STALL_BOUNDED_STUCK";
        case FaultKind::ARCHIVE_ERROR: return "ARCHIVE_ERROR";
    }
    return "?";
}

const char *archiveErrorName(ArchiveErrorKind kind) noexcept
{
    switch (kind)
    {
        case ArchiveErrorKind::NONE: return "NONE";
        case ArchiveErrorKind::TIMEOUT: return "TIMEOUT";
        case ArchiveErrorKind::NOT_CONNECTED: return "NOT_CONNECTED";
        case ArchiveErrorKind::INVALID_POSITION: return "INVALID_POSITION";
        case ArchiveErrorKind::UNKNOWN_RECORDING: return "UNKNOWN_RECORDING";
        case ArchiveErrorKind::MAX_REPLAYS: return "MAX_REPLAYS";
        case ArchiveErrorKind::STORAGE_SPACE: return "STORAGE_SPACE";
        case ArchiveErrorKind::GENERIC: return "GENERIC";
        case ArchiveErrorKind::FATAL: return "FATAL";
    }
    return "?";
}

bool isControlCall(ControlStep step) noexcept
{
    switch (step)
    {
        case ControlStep::CONNECT:
        case ControlStep::LIST:
        case ControlStep::START_REPLAY:
        case ControlStep::PROBE:
        case ControlStep::STOP_REPLAY:
            return true;
        default:
            return false;
    }
}

namespace {

std::uint8_t of(OpenFailure failure) noexcept
{
    return static_cast<std::uint8_t>(failure);
}

}

FollowPolicy::FollowPolicy(std::size_t sourceIndex, FollowSettings settings, ResumeMemo &memo, ArchiveMetrics *metrics,
    FollowObserver *observer) :
    m_source(sourceIndex), m_settings(settings), m_memo(memo), m_metrics(metrics), m_observer(observer)
{
}

// ---- owner commands

void FollowPolicy::attach(TimePoint now)
{
    if (Phase::IDLE != m_phase)
    {
        return;
    }
    if (m_connected)
    {
        beginAttach(now);
    }
    else
    {
        m_phase = Phase::DISCONNECTED;
        m_dueAt = now;
    }
}

void FollowPolicy::detach(TimePoint now)
{
    switch (m_phase)
    {
        case Phase::FOLLOWING:
        case Phase::START_DUE:
        case Phase::RESOLVING:
            beginDetach(AfterDetach::IDLE, m_replayStarted);
            break;
        case Phase::DETACH_DUE:
            m_afterDetach = AfterDetach::IDLE;
            break;
        case Phase::FATAL:
            break;
        default:
            // A pending async connect (CONNECTING) is cancelled too (review N-8).
            m_disconnectRequested = m_connected || Phase::CONNECTING == m_phase;
            m_phase = Phase::IDLE;
            setAttachedGauge(false);
            break;
    }
    m_continueAfter.reset();
    (void)now;
}

void FollowPolicy::reattach(TimePoint now)
{
    switch (m_phase)
    {
        case Phase::FOLLOWING:
        case Phase::START_DUE:
        case Phase::RESOLVING:
            beginDetach(AfterDetach::REATTACH, m_replayStarted);
            break;
        case Phase::DETACH_DUE:
            m_afterDetach = AfterDetach::REATTACH;
            break;
        case Phase::IDLE:
        case Phase::FATAL:
            break;
        default:
            if (m_connected)
            {
                beginAttach(now);
            }
            else
            {
                m_phase = Phase::DISCONNECTED;
                m_dueAt = now;
            }
            break;
    }
}

#ifdef CT_TEST_SEAMS
void FollowPolicy::forceProbe() noexcept
{
    m_probeNow = true;
}
#endif

void FollowPolicy::onFatal()
{
    if (Phase::FATAL == m_phase)
    {
        return;
    }
    if (disabled(Rule::FOLLOW_ROW1_FATAL))
    {
        return;
    }
    fault(FaultKind::FATAL, TimePoint{}, "the Aeron client reported a fatal error; the conductor is exiting the process");
    m_phase = Phase::FATAL;
    setAttachedGauge(false);
}

void FollowPolicy::setPaused(bool paused) noexcept
{
    if (m_paused && !paused && !disabled(Rule::FOLLOW_PAUSE_RULE))
    {
        // The pause rule: the "previous unpaused probe" baselines restart on resume, so a
        // back-pressured or fenced owner is never mistaken for a stuck replay.
        m_probeBaseline.reset();
        m_boundedBaseline.reset();
    }
    m_paused = paused;
}

void FollowPolicy::onDisconnected() noexcept
{
    m_connected = false;
    m_disconnectRequested = false;
}

// ---- the command the adapter executes this iteration

ControlStep FollowPolicy::nextControl(TimePoint now)
{
    for (int guard = 0; guard < 4; ++guard)
    {
        switch (m_phase)
        {
            case Phase::IDLE:
            case Phase::FATAL:
            case Phase::RESOLVING:
                return ControlStep::NONE;
            case Phase::DISCONNECTED:
                if (now < m_dueAt)
                {
                    return ControlStep::NONE;
                }
                m_phase = Phase::CONNECTING;
                return ControlStep::CONNECT;
            case Phase::CONNECTING:
                return ControlStep::CONNECT;
            case Phase::LIST_DUE:
                return ControlStep::LIST;
            case Phase::OPEN_DUE:
                return ControlStep::OPEN_SUBSCRIPTION;
            case Phase::START_DUE:
                return ControlStep::START_REPLAY;
            case Phase::FOLLOWING:
            {
                const Recording *r = currentRecording();
                if (nullptr != r && r->active() && (m_probeNow || now >= m_nextProbeAt))
                {
                    return ControlStep::PROBE;
                }
                if (nullptr != r && !r->active() && m_probeNow)
                {
                    m_probeNow = false; // a forced probe on a bounded replay has nothing to ask
                }
                return ControlStep::NONE;
            }
            case Phase::DETACH_DUE:
                return (m_stopReplayOnDetach && m_replayStarted && !disabled(Rule::ADAPTER_STOP_REPLAY))
                    ? ControlStep::STOP_REPLAY
                    : ControlStep::CLOSE_SUBSCRIPTION;
            case Phase::BACKOFF:
            case Phase::REFUSED:
                if (now < m_dueAt)
                {
                    return ControlStep::NONE;
                }
                m_phase = m_afterBackoff;
                continue;
        }
    }
    return ControlStep::NONE;
}

// ---- results

void FollowPolicy::onConnect(const ConnectPoll &result, TimePoint now)
{
    if (ArchiveErrorKind::FATAL == result.error.kind)
    {
        onFatal();
        return;
    }
    switch (result.state)
    {
        case ConnectPoll::State::PENDING:
            return;
        case ConnectPoll::State::CONNECTED:
            m_connected = true;
            beginAttach(now);
            return;
        case ConnectPoll::State::FAILED:
            m_connected = false;
            fault(FaultKind::CONNECT_FAILED, now, result.error.message);
            m_disconnectRequested = true; // drop the half-built client before the next attempt
            m_phase = Phase::DISCONNECTED;
            m_dueAt = now + m_settings.backoff;
            return;
    }
}

void FollowPolicy::beginAttach(TimePoint)
{
    m_phase = Phase::LIST_DUE;
    m_relistForInvalidPosition = false;
    m_relistedOnce = false;
    m_continueAfter.reset();
}

void FollowPolicy::onListed(const ListResult &result, TimePoint now)
{
    if (!result.ok)
    {
        if (ArchiveErrorKind::FATAL == result.error.kind)
        {
            onFatal();
            return;
        }
        fault(FaultKind::LIST_FAILED, now, result.error.message);
        if (ArchiveErrorKind::NOT_CONNECTED == result.error.kind || ArchiveErrorKind::TIMEOUT == result.error.kind)
        {
            m_disconnectRequested = true;
            m_phase = Phase::DISCONNECTED;
            m_dueAt = now + m_settings.backoff;
        }
        else
        {
            backoff(now, Phase::LIST_DUE);
        }
        return;
    }
    ChainListing listing = buildChain(result.recordings, m_settings.journalStreamId);
    if (nullptr != m_metrics)
    {
        ArchiveMetrics::add(m_metrics->foreignStreamDescriptorsDropped, listing.droppedForeignStream);
        if (m_source < MAX_SOURCES)
        {
            m_metrics->chainRecordings[m_source].store(listing.recordings.size(), std::memory_order_relaxed);
        }
    }
    const ListingOutcome outcome = m_memo.onListing(m_source, listing.recordings);
    m_chain = std::move(listing.recordings);

    if (!m_refusedChain.empty())
    {
        if (m_chain == m_refusedChain)
        {
            m_phase = Phase::REFUSED; // refused until the chain changes (section 7 row S7)
            m_afterBackoff = Phase::LIST_DUE;
            m_dueAt = now + m_settings.backoff;
            return;
        }
        m_refusedChain.clear();
        if (nullptr != m_metrics && m_source < MAX_SOURCES)
        {
            m_metrics->sourceRefusedMtu[m_source].store(0, std::memory_order_relaxed);
        }
    }

    if (m_relistForInvalidPosition)
    {
        // 4.3 step 6: INVALID_POSITION on a remembered position, re-listed once. A purge that
        // raced the listing moved the start past `from` (5.3 row 5 trimmed the proof): resume at
        // the new resume point. Otherwise the position itself is bad.
        m_relistForInvalidPosition = false;
        const Recording *now_r = nullptr;
        for (const auto &r : m_chain)
        {
            if (r.key == m_invalidKey)
            {
                now_r = &r;
            }
        }
        if (nullptr != now_r && now_r->startPosition > m_invalidFrom)
        {
            resumeAndWalk(now);
            return;
        }
        m_memo.invalidate(m_source, MemoReason::POSITION_REJECTED);
        fault(FaultKind::POSITION_REJECTED, now,
            "archive answered INVALID_POSITION for remembered position " + std::to_string(m_invalidFrom));
        backoff(now, Phase::LIST_DUE);
        return;
    }

    if (m_continueAfter)
    {
        // The read continues after the recording it consumed last (RECORDING_ENDED, or the chain end
        // reached before a back-off): a successor listed late picks up the same read, memo-free flag
        // included (review F-1). A history change, or the recording gone, starts a fresh resume.
        const RecordingKey last = m_continueAfter->key;
        m_continueAfter.reset();
        if (!outcome.historyChanged)
        {
            for (std::size_t j = 0; j < m_chain.size(); ++j)
            {
                if (m_chain[j].key == last)
                {
                    const std::size_t next = j + 1;
                    walk(next, next < m_chain.size() ? m_chain[next].startPosition : 0, false, 0, now);
                    return;
                }
            }
        }
    }
    resumeAndWalk(now);
}

void FollowPolicy::resumeAndWalk(TimePoint now)
{
    const ResumePoint point = m_memo.resume(m_source);
    m_readMemoFree = point.memoFree;
    walk(point.index, point.position, point.fromProof, point.proofSeq, now);
}

void FollowPolicy::walk(std::size_t index, std::int64_t from, bool fromProof, std::int64_t proofSeq, TimePoint now)
{
    for (;;)
    {
        m_index = index;
        if (m_chain.empty())
        {
            // Row 14: nothing recorded yet (or dark): wait 1 s, re-list.
            if (nullptr != m_observer)
            {
                m_observer->onChainEmpty(m_source);
            }
            backoff(now, Phase::LIST_DUE);
            return;
        }
        if (index >= m_chain.size())
        {
            // Row 15: the chain is exhausted with no active recording: back off, re-list. The read is at
            // the end of the last listed recording: keep that as the continuation point (review F-1).
            if (!disabled(Rule::FOLLOW_CONTINUATION))
            {
                m_continueAfter = m_chain.back();
            }
            if (nullptr != m_observer)
            {
                m_observer->onChainExhausted(m_source);
            }
            if (disabled(Rule::FOLLOW_ROW15_BACKOFF))
            {
                m_phase = Phase::LIST_DUE; // RED shape: the hot loop over the chain
            }
            else
            {
                backoff(now, Phase::LIST_DUE);
            }
            return;
        }
        const Recording &r = m_chain[index];
        const RecordingVerdict verdict = classifyRecording(r, from, m_settings.journalMtu);
        switch (verdict)
        {
            case RecordingVerdict::MALFORMED_EXTENT:
                fault(FaultKind::MALFORMED_EXTENT, now, "malformed extent: " + describe(r));
                backoff(now, Phase::LIST_DUE);
                return;
            case RecordingVerdict::EMPTY:
                if (nullptr != m_metrics)
                {
                    ArchiveMetrics::add(m_metrics->emptyRecordingsSkipped);
                }
                [[fallthrough]];
            case RecordingVerdict::DRAINED:
                ++index;
                from = index < m_chain.size() ? m_chain[index].startPosition : 0;
                fromProof = false;
                proofSeq = 0;
                continue;
            case RecordingVerdict::MTU_MISMATCH:
                m_refusedChain = m_chain;
                if (nullptr != m_metrics && m_source < MAX_SOURCES)
                {
                    m_metrics->sourceRefusedMtu[m_source].store(1, std::memory_order_relaxed);
                }
                if (nullptr != m_observer)
                {
                    m_observer->onSourceRefused(m_source, r);
                }
                m_phase = Phase::REFUSED;
                m_afterBackoff = Phase::LIST_DUE;
                m_dueAt = now + m_settings.backoff;
                return;
            case RecordingVerdict::RESUME_OUTSIDE:
                m_memo.invalidate(m_source, MemoReason::POSITION_REJECTED);
                fault(FaultKind::POSITION_REJECTED, now,
                    "resume position " + std::to_string(from) + " outside " + describe(r));
                backoff(now, Phase::LIST_DUE);
                return;
            case RecordingVerdict::FOLLOW_LIVE:
            case RecordingVerdict::REPLAY_BOUNDED:
                if (from % FRAME_ALIGNMENT != 0 && !disabled(Rule::MEMO_UNALIGNED_CHECK))
                {
                    // 4.3 step 3: the archive would answer GENERIC and the memo would retry it forever.
                    m_memo.invalidate(m_source, MemoReason::POSITION_REJECTED);
                    fault(FaultKind::POSITION_REJECTED, now,
                        "remembered position " + std::to_string(from) + " is not frame aligned");
                    backoff(now, Phase::LIST_DUE);
                    return;
                }
                m_from = from;
                m_fromProof = fromProof;
                m_proofSeq = proofSeq;
                m_request = ReplayRequest{};
                m_request.recordingId = r.key.recordingId;
                m_request.from = from;
                m_request.length = r.active() ? std::numeric_limits<std::int64_t>::max() : r.stopPosition - from;
                m_request.termBufferLength = r.key.termBufferLength;
                m_phase = Phase::OPEN_DUE;
                return;
        }
    }
}

void FollowPolicy::onSubscription(const SubscriptionResult &result, TimePoint now)
{
    switch (result.state)
    {
        case SubscriptionResult::State::ADDED:
            m_phase = Phase::RESOLVING;
            m_resolveDeadline = now + m_settings.endpointResolveTimeout;
            return;
        case SubscriptionResult::State::STORAGE_REFUSED:
            if (nullptr != m_metrics)
            {
                m_metrics->openFailure(OpenFailure::STORAGE);
            }
            fault(FaultKind::REPLAY_STORAGE, now,
                "driver directory has " + std::to_string(result.freeBytes) + " B free, the replay image needs " +
                    std::to_string(result.requiredBytes) + " B; no request sent",
                Refusal::NONE, of(OpenFailure::STORAGE));
            backoff(now, Phase::LIST_DUE); // memo kept: the retry resumes at the proof (R-K4)
            return;
        case SubscriptionResult::State::FAILED:
            if (ArchiveErrorKind::FATAL == result.error.kind)
            {
                onFatal();
                return;
            }
            if (nullptr != m_metrics)
            {
                m_metrics->openFailure(OpenFailure::ARCHIVE_ERROR);
            }
            fault(FaultKind::REPLAY_OPEN, now, result.error.message, Refusal::NONE, of(OpenFailure::ARCHIVE_ERROR));
            backoff(now, Phase::LIST_DUE);
            return;
    }
}

void FollowPolicy::onEndpoint(const EndpointPoll &result, TimePoint now)
{
    if (Phase::RESOLVING != m_phase)
    {
        return;
    }
    if (EndpointPoll::State::RESOLVED == result.state)
    {
        m_request.endpoint = result.endpoint;
        m_phase = Phase::START_DUE;
        return;
    }
    if (ArchiveErrorKind::FATAL == result.error.kind)
    {
        onFatal();
        return;
    }
    if (EndpointPoll::State::FAILED == result.state || now >= m_resolveDeadline)
    {
        if (nullptr != m_metrics)
        {
            m_metrics->openFailure(OpenFailure::ENDPOINT);
        }
        fault(FaultKind::REPLAY_ENDPOINT_UNRESOLVED, now,
            EndpointPoll::State::FAILED == result.state ? result.error.message
                                                         : std::string("replay endpoint not resolved within 10 s"),
            Refusal::NONE, of(OpenFailure::ENDPOINT));
        beginDetach(AfterDetach::BACKOFF, false);
    }
}

void FollowPolicy::onReplayStarted(const StartResult &result, TimePoint now)
{
    const Recording *r = currentRecording();
    if (result.ok && nullptr != r)
    {
        m_replayStarted = true;
        if (!disabled(Rule::FOLLOW_RELIST_RESET))
        {
            m_relistedOnce = false; // the 4.3 step 6 re-list is once per open attempt, not per connection (review F-2)
        }
        m_imagePosition = m_from;
        m_replaySessionId = result.replaySessionId;
        m_replayStartedAt = now;
        m_imageSeen = false;
        m_consumed = m_from;
        m_havePrevSeq = false;
        m_prevSeq = 0;
        m_beltPending = m_fromProof;
        m_nextProbeAt = now + m_settings.probeInterval;
        m_probeNow = false;
        m_probeIsImageEnd = false;
        m_probeBaseline.reset();
        m_nextBoundedCheckAt = now + m_settings.probeInterval;
        m_boundedBaseline.reset();
        m_phase = Phase::FOLLOWING;
        setAttachedGauge(true);
        if (nullptr != m_metrics)
        {
            m_metrics->replayConsumedPosition.store(m_consumed, std::memory_order_relaxed);
            m_metrics->replayRecordingPosition.store(r->active() ? m_consumed : r->stopPosition, std::memory_order_relaxed);
        }
        if (nullptr != m_observer)
        {
            m_observer->onAttached(AttachInfo{ m_source, *r, m_index, m_from, m_fromProof, m_readMemoFree });
        }
        return;
    }
    const ArchiveError &error = result.error;
    if (ArchiveErrorKind::FATAL == error.kind)
    {
        onFatal();
        return;
    }
    const std::int64_t start = nullptr != r ? r->startPosition : 0;
    switch (error.kind)
    {
        case ArchiveErrorKind::INVALID_POSITION:
            if (nullptr != m_metrics)
            {
                m_metrics->openFailure(OpenFailure::INVALID_POSITION);
            }
            if (m_from > start && !m_relistedOnce && !disabled(Rule::MEMO_PURGE_RACE_RELIST) && nullptr != r)
            {
                m_relistedOnce = true;
                m_relistForInvalidPosition = true;
                m_invalidFrom = m_from;
                m_invalidKey = r->key;
                beginDetach(AfterDetach::RELIST, false);
                return;
            }
            if (m_from > start)
            {
                m_memo.invalidate(m_source, MemoReason::POSITION_REJECTED);
                fault(FaultKind::POSITION_REJECTED, now, error.message, Refusal::NONE, of(OpenFailure::INVALID_POSITION));
            }
            else
            {
                fault(FaultKind::REPLAY_OPEN, now, error.message, Refusal::NONE, of(OpenFailure::INVALID_POSITION));
            }
            beginDetach(AfterDetach::BACKOFF, false);
            return;
        case ArchiveErrorKind::UNKNOWN_RECORDING:
            // 4.3 step 6: the listing is stale; re-list (no fault unless it repeats).
            if (!m_relistedOnce)
            {
                m_relistedOnce = true;
                beginDetach(AfterDetach::RELIST, false);
                return;
            }
            if (nullptr != m_metrics)
            {
                m_metrics->openFailure(OpenFailure::ARCHIVE_ERROR);
            }
            fault(FaultKind::REPLAY_OPEN, now, error.message, Refusal::NONE, of(OpenFailure::ARCHIVE_ERROR));
            beginDetach(AfterDetach::BACKOFF, false);
            return;
        case ArchiveErrorKind::MAX_REPLAYS:
        case ArchiveErrorKind::STORAGE_SPACE:
        case ArchiveErrorKind::TIMEOUT:
        case ArchiveErrorKind::GENERIC:
        case ArchiveErrorKind::NONE:
        {
            const OpenFailure failure = ArchiveErrorKind::MAX_REPLAYS == error.kind ? OpenFailure::MAX_REPLAYS
                : ArchiveErrorKind::STORAGE_SPACE == error.kind                    ? OpenFailure::STORAGE_SPACE
                : ArchiveErrorKind::TIMEOUT == error.kind                          ? OpenFailure::TIMEOUT
                                                                                   : OpenFailure::ARCHIVE_ERROR;
            if (nullptr != m_metrics)
            {
                m_metrics->openFailure(failure);
            }
            if (disabled(Rule::MEMO_CAPACITY_CLEARS))
            {
                m_memo.invalidate(m_source, MemoReason::POSITION_REJECTED); // RED shape: capacity clears the proofs
            }
            fault(FaultKind::REPLAY_OPEN, now, error.message, Refusal::NONE, of(failure)); // memo kept (cpp: M5)
            beginDetach(AfterDetach::BACKOFF, false);
            return;
        }
        case ArchiveErrorKind::NOT_CONNECTED:
            if (nullptr != m_metrics)
            {
                m_metrics->openFailure(OpenFailure::ARCHIVE_ERROR);
            }
            fault(FaultKind::REPLAY_OPEN, now, error.message, Refusal::NONE, of(OpenFailure::ARCHIVE_ERROR));
            beginDetach(AfterDetach::DISCONNECT, false);
            return;
        case ArchiveErrorKind::FATAL:
            return;
    }
}

void FollowPolicy::onDetached(TimePoint now)
{
    m_replayStarted = false;
    m_probeNow = false;
    m_probeIsImageEnd = false;
    setAttachedGauge(false);
    switch (m_afterDetach)
    {
        case AfterDetach::NEXT_RECORDING:
        {
            const std::size_t next = m_index + 1;
            walk(next, next < m_chain.size() ? m_chain[next].startPosition : 0, false, 0, now);
            return;
        }
        case AfterDetach::SUCCESSOR:
        case AfterDetach::RELIST:
            m_phase = Phase::LIST_DUE;
            return;
        case AfterDetach::REATTACH:
            beginAttach(now);
            return;
        case AfterDetach::BACKOFF:
            backoff(now, Phase::LIST_DUE);
            return;
        case AfterDetach::DISCONNECT:
            m_disconnectRequested = true;
            m_phase = Phase::DISCONNECTED;
            m_dueAt = now + m_settings.backoff;
            return;
        case AfterDetach::IDLE:
            m_disconnectRequested = m_connected;
            m_phase = Phase::IDLE;
            return;
    }
}

// ---- the 4.5 table

FrameCheck FollowPolicy::checkFrame(const Peek &peek, TimePoint now)
{
    // Row 3: seqs ascend densely inside one recording (S1 2 rule 3); a regression is a source fault.
    if (m_havePrevSeq && peek.journalSeq <= m_prevSeq && !disabled(Rule::FOLLOW_ROW3_REGRESSION))
    {
        fault(FaultKind::SEQ_REGRESSION_IN_RECORDING, now,
            "journalSeq " + std::to_string(peek.journalSeq) + " after " + std::to_string(m_prevSeq) + " on the same replay");
        beginDetach(AfterDetach::BACKOFF, true);
        return FrameCheck::FAULTED;
    }
    // Row 4: the first frame after a memo resume at (K, p) must be proof[K].seq + 1.
    if (m_beltPending && peek.journalSeq != m_proofSeq + 1 && !disabled(Rule::FOLLOW_ROW4_BELT))
    {
        fault(FaultKind::RESUME_BELT, now,
            "first frame after a resume at proof seq " + std::to_string(m_proofSeq) + " carries journalSeq " +
                std::to_string(peek.journalSeq));
        beginDetach(AfterDetach::BACKOFF, true);
        return FrameCheck::FAULTED;
    }
    return FrameCheck::OK;
}

void FollowPolicy::onAccepted(const Peek &peek, std::int64_t endPosition)
{
    m_prevSeq = peek.journalSeq;
    m_havePrevSeq = true;
    m_beltPending = false;
    m_consumed = endPosition;
    if (nullptr != m_metrics)
    {
        m_metrics->replayConsumedPosition.store(endPosition, std::memory_order_relaxed);
        if (m_metrics->replayRecordingPosition.load(std::memory_order_relaxed) < endPosition)
        {
            m_metrics->replayRecordingPosition.store(endPosition, std::memory_order_relaxed);
        }
    }
    if (const Recording *r = currentRecording(); nullptr != r)
    {
        m_memo.proveFromSkip(m_source, r->key, peek, endPosition);
    }
    m_memo.onFrameRead(m_source, peek.journalSeq, m_readMemoFree);
}

void FollowPolicy::onRefused(Refusal reason, std::int64_t endPosition, TimePoint now)
{
    fault(FaultKind::SCHEMA_REFUSED, now,
        std::string("frame refused (") + refusalLabel(reason) + ") ending at position " + std::to_string(endPosition), reason);
    beginDetach(AfterDetach::BACKOFF, true);
}

void FollowPolicy::onRefusalSkipped(std::int64_t endPosition)
{
    m_consumed = endPosition;
}

void FollowPolicy::afterPoll(const ImageObservation &image, const AsyncErrorPoll &asyncError, TimePoint now)
{
    if (Phase::FOLLOWING != m_phase)
    {
        return;
    }
    const Recording *r = currentRecording();
    if (nullptr == r)
    {
        return;
    }
    const bool bounded = !r->active();
    if (image.imageCount > 0)
    {
        m_imageSeen = true;
        if (!disabled(Rule::FOLLOW_PADDING_REACHED))
        {
            m_imagePosition = std::max(m_imagePosition, image.position);
        }
    }
    // What the read has reached: consumed, or further when the stretch up to the image position is
    // padding only (an ABORTed frame never advances the image). Proofs stay on frame ends (review N-1).
    const std::int64_t reached = std::max(m_consumed, m_imagePosition);
    const bool gone = m_imageSeen && image.imageCount == 0;
    const bool ended = m_imageSeen && (image.endOfStream || image.imageClosed || gone);
    if (bounded && nullptr != m_metrics)
    {
        m_metrics->sourceBacklogBytes.store(std::max<std::int64_t>(0, r->stopPosition - m_consumed), std::memory_order_relaxed);
    }
    // Row 5: a bounded replay consumed to its stop -> the next recording.
    if (bounded && reached >= r->stopPosition && !disabled(Rule::FOLLOW_ROW5_DRAINED))
    {
        beginDetach(AfterDetach::NEXT_RECORDING, false);
        return;
    }
    // Row 6: a bounded replay that ended before catalog EOF is a failure, never "drained" (cpp: S10).
    if (bounded && ended && reached < r->stopPosition && !disabled(Rule::FOLLOW_ROW6_EARLY_EOF))
    {
        fault(FaultKind::EARLY_EOF, now,
            "stopped replay ended at " + std::to_string(m_consumed) + " before stop " + std::to_string(r->stopPosition));
        beginDetach(AfterDetach::BACKOFF, true);
        return;
    }
    // Row 7a (review B1): the archive ends a live replay when its recording stops and the image goes
    // about 1 s later. An image end on a live follow triggers an immediate recording-state probe.
    if (!bounded && (ended || image.subscriptionClosed) && !disabled(Rule::FOLLOW_ROW7A_IGNORE))
    {
        if (disabled(Rule::FOLLOW_ROW7A_IMAGE_END_PROBE))
        {
            fault(FaultKind::IMAGE_LOST, now, "live replay image ended"); // RED shape: every stop is a fault
            beginDetach(AfterDetach::BACKOFF, true);
            return;
        }
        m_probeNow = true;
        m_probeIsImageEnd = true;
    }
    // Row 7 (bounded): the subscription itself closed.
    if (bounded && image.subscriptionClosed)
    {
        fault(FaultKind::IMAGE_LOST, now, "replay subscription closed");
        beginDetach(AfterDetach::BACKOFF, true);
        return;
    }
    // Row 8: no image 15 s after startReplay (backstop; a reachable archive reports row 13 at ~5 s).
    if (!m_imageSeen && now - m_replayStartedAt >= m_settings.neverConnectedTimeout &&
        !disabled(Rule::FOLLOW_ROW8_NEVER_CONNECTED))
    {
        fault(FaultKind::NEVER_CONNECTED, now, "replay never connected within 15 s");
        beginDetach(AfterDetach::BACKOFF, true);
        return;
    }
    // Row 12b: a bounded replay whose image stays live but delivers nothing (no archive call).
    if (bounded && now >= m_nextBoundedCheckAt)
    {
        m_nextBoundedCheckAt = now + m_settings.probeInterval;
        if (!m_paused || disabled(Rule::FOLLOW_PAUSE_RULE))
        {
            if (m_boundedBaseline && *m_boundedBaseline == m_consumed && m_consumed < r->stopPosition &&
                !disabled(Rule::FOLLOW_ROW12B_BOUNDED_STUCK))
            {
                fault(FaultKind::STALL_BOUNDED_STUCK, now,
                    "bounded replay stuck at " + std::to_string(m_consumed) + " short of stop " + std::to_string(r->stopPosition));
                beginDetach(AfterDetach::BACKOFF, true);
                return;
            }
            m_boundedBaseline = m_consumed;
        }
    }
    // Row 13: an async archive error (e.g. "no connection established for replayChannel").
    if (AsyncErrorPoll::State::ERROR == asyncError.state && !disabled(Rule::FOLLOW_ROW13_ARCHIVE_ERROR))
    {
        fault(FaultKind::ARCHIVE_ERROR, now, asyncError.message);
        beginDetach(AfterDetach::BACKOFF, true);
        return;
    }
    if (AsyncErrorPoll::State::DISCONNECTED == asyncError.state && !disabled(Rule::FOLLOW_ASYNC_DISCONNECT))
    {
        fault(FaultKind::ARCHIVE_GONE, now, asyncError.message.empty() ? std::string("archive control session lost") : asyncError.message);
        beginDetach(AfterDetach::DISCONNECT, false);
        return;
    }
}

void FollowPolicy::onIdleAsyncError(const AsyncErrorPoll &asyncError, TimePoint now)
{
    if (AsyncErrorPoll::State::DISCONNECTED != asyncError.state || disabled(Rule::FOLLOW_ASYNC_DISCONNECT))
    {
        return; // an async error outside a replay belongs to no replay: dropped, never charged to the next one
    }
    switch (m_phase)
    {
        case Phase::LIST_DUE:
        case Phase::OPEN_DUE:
        case Phase::BACKOFF:
        case Phase::REFUSED:
            fault(FaultKind::ARCHIVE_GONE, now, asyncError.message.empty() ? std::string("archive control session lost")
                                                                           : asyncError.message);
            m_disconnectRequested = true;
            m_phase = Phase::DISCONNECTED;
            m_dueAt = now + m_settings.backoff;
            return;
        case Phase::RESOLVING:
        case Phase::START_DUE:
            fault(FaultKind::ARCHIVE_GONE, now, asyncError.message.empty() ? std::string("archive control session lost")
                                                                           : asyncError.message);
            beginDetach(AfterDetach::DISCONNECT, false);
            return;
        default:
            return;
    }
}

void FollowPolicy::onSinkBackPressure() noexcept
{
    if (!disabled(Rule::FOLLOW_ABORT_IS_PAUSE))
    {
        m_paused = true; // rows 11, 12, 12b skip this iteration; the next unpaused one restarts the baselines
    }
}

void FollowPolicy::onProbe(const ProbeResult &result, TimePoint now)
{
    if (Phase::FOLLOWING != m_phase)
    {
        return;
    }
    const bool imageEnd = m_probeIsImageEnd;
    m_probeNow = false;
    m_probeIsImageEnd = false;
    m_nextProbeAt = now + m_settings.probeInterval;
    const Recording *r = currentRecording();
    if (nullptr == r)
    {
        return;
    }
    if (ProbeResult::State::FAILED == result.state)
    {
        if (ArchiveErrorKind::FATAL == result.error.kind)
        {
            onFatal();
            return;
        }
        // Row 9 (and 7a): the probe throws or times out -> the archive is gone.
        if (imageEnd || !disabled(Rule::FOLLOW_ROW9_ARCHIVE_GONE))
        {
            fault(FaultKind::ARCHIVE_GONE, now, result.error.message);
            beginDetach(AfterDetach::DISCONNECT, false);
        }
        return;
    }
    if (ProbeResult::State::STOPPED == result.state)
    {
        const std::int64_t stop = result.position;
        if (nullptr != m_metrics)
        {
            m_metrics->replayRecordingPosition.store(stop, std::memory_order_relaxed);
            m_metrics->sourceBacklogBytes.store(std::max<std::int64_t>(0, stop - m_consumed), std::memory_order_relaxed);
        }
        // Row 10: stopped and fully consumed -> follow the successor; the core session is untouched.
        if (std::max(m_consumed, m_imagePosition) >= stop && !disabled(Rule::FOLLOW_ROW10_RECORDING_ENDED))
        {
            if (nullptr != m_metrics)
            {
                ArchiveMetrics::add(m_metrics->recordingSuccessions);
            }
            Recording ended = *r;
            ended.stopPosition = stop;
            if (nullptr != m_observer)
            {
                m_observer->onRecordingEnded(m_source, ended);
            }
            m_continueAfter = ended;
            beginDetach(AfterDetach::SUCCESSOR, false);
            return;
        }
        if (imageEnd)
        {
            fault(FaultKind::EARLY_EOF, now,
                "live replay ended at " + std::to_string(m_consumed) + " before the recording's stop " + std::to_string(stop));
            beginDetach(AfterDetach::BACKOFF, true);
            return;
        }
        // Row 11.
        if (!m_paused || disabled(Rule::FOLLOW_PAUSE_RULE))
        {
            if (m_probeBaseline && *m_probeBaseline == m_consumed && m_consumed < stop &&
                !disabled(Rule::FOLLOW_ROW11_STALL_AFTER_STOP))
            {
                fault(FaultKind::STALL_AFTER_STOP, now,
                    "recording stopped at " + std::to_string(stop) + " but the replay is stuck at " + std::to_string(m_consumed));
                beginDetach(AfterDetach::BACKOFF, true);
                return;
            }
            m_probeBaseline = m_consumed;
        }
        return;
    }
    // ACTIVE
    const std::int64_t recorded = result.position;
    if (nullptr != m_metrics)
    {
        m_metrics->replayRecordingPosition.store(std::max(recorded, m_consumed), std::memory_order_relaxed);
        m_metrics->sourceBacklogBytes.store(std::max<std::int64_t>(0, recorded - m_consumed), std::memory_order_relaxed);
    }
    if (imageEnd)
    {
        fault(FaultKind::IMAGE_LOST, now, "live replay image ended while the recording is still active");
        beginDetach(AfterDetach::BACKOFF, true);
        return;
    }
    // Row 12 (the 07-10 lesson): recorded grew while the replay stayed put.
    if (!m_paused || disabled(Rule::FOLLOW_PAUSE_RULE))
    {
        if (m_probeBaseline && recorded > m_consumed && *m_probeBaseline == m_consumed &&
            !disabled(Rule::FOLLOW_ROW12_STALL))
        {
            fault(FaultKind::STALL, now,
                "recorded position " + std::to_string(recorded) + " but the replay is stuck at " + std::to_string(m_consumed));
            beginDetach(AfterDetach::BACKOFF, true);
            return;
        }
        m_probeBaseline = m_consumed;
    }
}

// ---- helpers

const Recording *FollowPolicy::currentRecording() const noexcept
{
    return m_index < m_chain.size() ? &m_chain[m_index] : nullptr;
}

void FollowPolicy::fault(FaultKind kind, TimePoint, std::string detail, Refusal refusal, std::uint8_t openFailure)
{
    SourceFault report;
    report.source = m_source;
    report.kind = kind;
    if (const Recording *r = currentRecording(); nullptr != r)
    {
        report.key = r->key;
    }
    report.consumed = m_consumed;
    report.refusal = refusal;
    report.openFailure = openFailure;
    report.detail = std::move(detail);
    if (nullptr != m_metrics)
    {
        switch (kind)
        {
            case FaultKind::NEVER_CONNECTED: m_metrics->stall(StallReason::NEVER_CONNECTED); break;
            case FaultKind::IMAGE_LOST: m_metrics->stall(StallReason::IMAGE_LOST); break;
            case FaultKind::ARCHIVE_GONE: m_metrics->stall(StallReason::ARCHIVE_GONE); break;
            case FaultKind::STALL: m_metrics->stall(StallReason::STALL); break;
            case FaultKind::STALL_AFTER_STOP: m_metrics->stall(StallReason::STALL_AFTER_STOP); break;
            case FaultKind::STALL_BOUNDED_STUCK: m_metrics->stall(StallReason::BOUNDED_STUCK); break;
            case FaultKind::EARLY_EOF: m_metrics->stall(StallReason::EARLY_EOF); break;
            case FaultKind::ARCHIVE_ERROR: m_metrics->stall(StallReason::ARCHIVE_ERROR); break;
            default: break;
        }
    }
    if (FaultKind::SEQ_REGRESSION_IN_RECORDING == kind)
    {
        m_memo.invalidate(m_source, MemoReason::SEQ_REGRESSION);
    }
    else if (FaultKind::RESUME_BELT == kind)
    {
        m_memo.invalidate(m_source, MemoReason::RESUME_BELT);
    }
    m_continueAfter.reset(); // a source fault ends the read: the re-attach resumes from the memo
    ++m_faults;
    m_lastFault = report;
    if (nullptr != m_observer)
    {
        m_observer->onFault(report);
    }
}

void FollowPolicy::beginDetach(AfterDetach next, bool stopReplay)
{
    m_afterDetach = next;
    m_stopReplayOnDetach = stopReplay;
    m_phase = Phase::DETACH_DUE;
}

void FollowPolicy::backoff(TimePoint now, Phase next)
{
    m_phase = Phase::BACKOFF;
    m_afterBackoff = next;
    m_dueAt = now + m_settings.backoff;
}

void FollowPolicy::setAttachedGauge(bool attached) noexcept
{
    if (nullptr != m_metrics && m_source < MAX_SOURCES)
    {
        m_metrics->sourceAttached[m_source].store(attached ? 1 : 0, std::memory_order_relaxed);
    }
}

}}
