#ifndef CLUSTER_TOOLS_ARCHIVE_TESTS_SIM_ARCHIVE_H
#define CLUSTER_TOOLS_ARCHIVE_TESTS_SIM_ARCHIVE_H

// A simulated archive endpoint satisfying the FollowerLoop Port: recordings of real schema 4
// frames at real Aeron positions (32-byte header, 32-byte alignment), a replay that delivers from
// a frame boundary, refuses a position that is not one (INVALID_POSITION) or below the start, an
// image that appears, reaches end of stream and goes away like the C client's, and knobs for every
// failure the 4.5 table names. Deterministic: no clock of its own, no threads.

#include "TestSupport.h"

#include "cluster_tools/archive/Follow.h"
#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/archive/FollowerLoop.h"
#include "cluster_tools/archive/ResumeMemo.h"

#include <chrono>
#include <functional>
#include <memory>

#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ct_archive_test {

using namespace cluster_tools::archive;

struct SimFrame
{
    Bytes bytes;
    std::uint8_t flags = FRAME_FLAGS_UNFRAGMENTED;
    std::int64_t start = 0;
    std::int64_t end = 0;
};

struct SimRecording
{
    Recording rec;
    std::vector<SimFrame> frames;

    std::int64_t recorded() const { return frames.empty() ? rec.startPosition : frames.back().end; }
};

class SimArchive
{
public:
    // ---- building the archive
    std::deque<SimRecording> recordings; // deque: references stay valid as recordings are added
    std::int64_t nextId = 0;
    std::int32_t mtu = 1408;
    std::int32_t streamId = 4001;

    SimRecording &record(std::int64_t startPosition = 0)
    {
        SimRecording r;
        r.rec.key = key(nextId++, mtu, streamId);
        r.rec.startPosition = startPosition;
        r.rec.stopPosition = NULL_POSITION;
        recordings.push_back(r);
        return recordings.back();
    }

    void append(SimRecording &r, Bytes bytes, std::uint8_t flags = FRAME_FLAGS_UNFRAGMENTED)
    {
        SimFrame f;
        f.start = r.recorded();
        f.end = f.start + alignedFrame(bytes.size());
        f.bytes = std::move(bytes);
        f.flags = flags;
        r.frames.push_back(std::move(f));
    }

    void pad(SimRecording &r, std::int64_t bytes)
    {
        // Aeron padding: a frame the poll never delivers, shifting later boundaries.
        SimFrame f;
        f.start = r.recorded();
        f.end = f.start + bytes;
        f.flags = 0; // marker: padding
        r.frames.push_back(std::move(f));
    }

    void appendTrades(SimRecording &r, std::int64_t fromSeq, std::int64_t count, std::int64_t epoch)
    {
        for (std::int64_t s = fromSeq; s < fromSeq + count; ++s)
        {
            append(r, makeFrame(TEMPLATE_TRADE, s, epoch, 64 * s));
        }
    }

    void stop(SimRecording &r) { r.rec.stopPosition = r.recorded(); }

    SimRecording *find(std::int64_t id)
    {
        for (auto &r : recordings)
        {
            if (r.rec.key.recordingId == id)
            {
                return &r;
            }
        }
        return nullptr;
    }

    // ---- failure knobs
    bool fatal = false;
    bool reachable = true;           // connect and every control call fail (timeout) when false
    int connectPendingSteps = 0;     // async connect polls before CONNECTED
    bool storageRefused = false;
    bool endpointNeverResolves = false;
    bool imageNeverConnects = false;
    bool deliverNothing = false;     // the image is there but no fragment ever arrives
    bool dropImageNow = false;       // the image vanishes (source died) on the next observation
    bool probeFails = false;
    std::optional<ArchiveErrorKind> startError;
    int startErrorCount = 0;         // how many startReplay calls fail with startError (0 = all)
    std::optional<std::string> pendingAsyncError;
    bool controlSessionLost = false;
    int maxReplays = 1'000'000;
    std::function<void()> onStartReplay; // e.g. a purge racing the listing
    std::function<void()> onList;

    // ---- counters
    int connectCalls = 0, listCalls = 0, startCalls = 0, probeCalls = 0, stopCalls = 0, closeCalls = 0;
    int disconnects = 0, subscriptionsOpened = 0, controlCalls = 0;
    std::vector<ReplayRequest> requests;

    // ---- Port
    bool isFatal() const { return fatal; }

    ConnectPoll connectStep()
    {
        ++connectCalls;
        ++controlCalls;
        ConnectPoll p;
        if (!reachable)
        {
            p.state = ConnectPoll::State::FAILED;
            p.error = { ArchiveErrorKind::TIMEOUT, "connect timed out" };
            return p;
        }
        if (m_connectSteps++ < connectPendingSteps)
        {
            return p;
        }
        m_connectSteps = 0;
        m_connected = true;
        p.state = ConnectPoll::State::CONNECTED;
        return p;
    }

    void disconnect() noexcept
    {
        ++disconnects;
        m_connected = false;
        m_replay = false;
    }

    ListResult list()
    {
        ++listCalls;
        ++controlCalls;
        if (onList)
        {
            onList();
        }
        ListResult r;
        if (!reachable || !m_connected)
        {
            r.error = { ArchiveErrorKind::TIMEOUT, "list timed out" };
            return r;
        }
        r.ok = true;
        for (const auto &rec : recordings)
        {
            r.recordings.push_back(rec.rec);
        }
        return r;
    }

    SubscriptionResult openSubscription(std::int32_t termBufferLength)
    {
        SubscriptionResult r;
        if (storageRefused)
        {
            r.state = SubscriptionResult::State::STORAGE_REFUSED;
            r.requiredBytes = replayStorageRequirement(termBufferLength);
            return r;
        }
        ++subscriptionsOpened;
        m_subscription = true;
        return r;
    }

    EndpointPoll pollEndpoint()
    {
        EndpointPoll p;
        if (!endpointNeverResolves && m_subscription)
        {
            p.state = EndpointPoll::State::RESOLVED;
            p.endpoint = "127.0.0.1:40123";
        }
        return p;
    }

    StartResult startReplay(const ReplayRequest &request)
    {
        ++startCalls;
        ++controlCalls;
        requests.push_back(request);
        if (onStartReplay)
        {
            onStartReplay();
        }
        StartResult r;
        if (!reachable || !m_connected)
        {
            r.error = { ArchiveErrorKind::TIMEOUT, "startReplay timed out" };
            return r;
        }
        if (startError && (startErrorCount == 0 || m_startErrorsGiven < startErrorCount))
        {
            ++m_startErrorsGiven;
            r.error = { *startError, "injected" };
            return r;
        }
        if (m_activeReplays >= maxReplays)
        {
            r.error = { ArchiveErrorKind::MAX_REPLAYS, "max replays" };
            return r;
        }
        SimRecording *rec = find(request.recordingId);
        if (nullptr == rec)
        {
            r.error = { ArchiveErrorKind::UNKNOWN_RECORDING, "unknown recording" };
            return r;
        }
        // the archive's isInvalidReplayPosition + the frame check of ReplaySession
        bool boundary = request.from == rec->rec.startPosition && (rec->frames.empty() || rec->frames.front().start == request.from);
        for (const auto &f : rec->frames)
        {
            if (f.start == request.from || f.end == request.from)
            {
                boundary = true;
            }
        }
        if (request.from < rec->rec.startPosition || request.from > rec->recorded() || !boundary || request.from % 32 != 0)
        {
            r.error = { ArchiveErrorKind::INVALID_POSITION, "invalid position " + std::to_string(request.from) };
            return r;
        }
        if (request.length == 0)
        {
            r.error = { ArchiveErrorKind::GENERIC, "zero-length replay" };
            return r;
        }
        m_replay = true;
        ++m_activeReplays;
        m_replayId = request.recordingId;
        m_pos = request.from;
        m_limit = request.length == std::numeric_limits<std::int64_t>::max() ? std::numeric_limits<std::int64_t>::max()
                                                                             : request.from + request.length;
        m_imagePresent = false;
        m_imageEos = false;
        m_imageGone = false;
        r.ok = true;
        r.replaySessionId = ++m_replaySessions;
        return r;
    }

    template<typename H>
    int poll(H &&handler, int limit)
    {
        materialiseImage();
        if (!m_replay || !m_imagePresent || m_imageGone || deliverNothing)
        {
            return 0;
        }
        SimRecording *rec = find(m_replayId);
        if (nullptr == rec)
        {
            return 0;
        }
        int delivered = 0;
        for (const auto &f : rec->frames)
        {
            if (delivered >= limit)
            {
                break;
            }
            if (f.start < m_pos || f.start >= m_limit)
            {
                continue;
            }
            if (0 == f.flags && f.bytes.empty())
            {
                m_pos = f.end; // padding is skipped by the poll
                continue;
            }
            const SinkAction action = handler(f.flags, f.bytes.data(), f.bytes.size(), f.end);
            if (SinkAction::ABORT == action)
            {
                break;
            }
            m_pos = f.end;
            ++delivered;
            if (SinkAction::BREAK == action)
            {
                break;
            }
        }
        updateEos();
        return delivered;
    }

    ImageObservation observeImage()
    {
        materialiseImage();
        ImageObservation o;
        if (dropImageNow && m_imagePresent)
        {
            m_imageGone = true;
            dropImageNow = false;
        }
        updateEos();
        if (m_imageEos && m_eosSeen)
        {
            m_imageGone = true; // removed after it was drained
        }
        m_eosSeen = m_imageEos;
        o.imageCount = (m_imagePresent && !m_imageGone) ? 1 : 0;
        o.endOfStream = m_imageEos && !m_imageGone;
        o.position = m_pos;
        return o;
    }

    AsyncErrorPoll pollAsyncError()
    {
        AsyncErrorPoll p;
        if (controlSessionLost)
        {
            p.state = AsyncErrorPoll::State::DISCONNECTED;
            return p;
        }
        if (pendingAsyncError)
        {
            p.state = AsyncErrorPoll::State::ERROR;
            p.message = *pendingAsyncError;
            pendingAsyncError.reset();
        }
        return p;
    }

    ProbeResult probe(std::int64_t recordingId)
    {
        ++probeCalls;
        ++controlCalls;
        ProbeResult p;
        if (probeFails || !reachable || !m_connected)
        {
            p.state = ProbeResult::State::FAILED;
            p.error = { ArchiveErrorKind::TIMEOUT, "probe timed out" };
            return p;
        }
        SimRecording *rec = find(recordingId);
        if (nullptr == rec)
        {
            p.state = ProbeResult::State::FAILED;
            p.error = { ArchiveErrorKind::UNKNOWN_RECORDING, "unknown recording" };
            return p;
        }
        if (rec->rec.active())
        {
            p.state = ProbeResult::State::ACTIVE;
            p.position = rec->recorded();
        }
        else
        {
            p.state = ProbeResult::State::STOPPED;
            p.position = rec->rec.stopPosition;
        }
        return p;
    }

    void stopReplay(std::int64_t) noexcept
    {
        ++stopCalls;
        ++controlCalls;
        if (m_replay)
        {
            m_replay = false;
            --m_activeReplays;
        }
    }

    void closeSubscription() noexcept
    {
        ++closeCalls;
        m_subscription = false;
        if (m_replay)
        {
            m_replay = false; // the archive's replay publication loses its subscriber
            if (!leakReplayOnClose)
            {
                --m_activeReplays;
            }
        }
    }

    bool leakReplayOnClose = false; // models a replay the archive keeps until stopReplay
    bool replayRunning() const { return m_replay; }
    std::int64_t replayRecordingId() const { return m_replayId; }
    int activeReplays() const { return m_activeReplays; }
    std::int64_t replayPosition() const { return m_pos; }

private:
    void materialiseImage()
    {
        if (m_replay && !m_imagePresent && !imageNeverConnects)
        {
            m_imagePresent = true;
        }
    }

    void updateEos()
    {
        if (!m_replay || !m_imagePresent)
        {
            return;
        }
        SimRecording *rec = find(m_replayId);
        if (nullptr == rec)
        {
            return;
        }
        const bool stopped = !rec->rec.active();
        const std::int64_t end = std::min(m_limit, stopped ? rec->rec.stopPosition : std::numeric_limits<std::int64_t>::max());
        if (m_pos >= end || (stopped && m_pos >= rec->rec.stopPosition))
        {
            m_imageEos = true;
        }
    }

    bool m_connected = false;
    int m_connectSteps = 0;
    bool m_subscription = false;
    bool m_replay = false;
    int m_activeReplays = 0;
    std::int64_t m_replaySessions = 0;
    std::int64_t m_replayId = -1;
    std::int64_t m_pos = 0;
    std::int64_t m_limit = 0;
    bool m_imagePresent = false;
    bool m_imageEos = false;
    bool m_eosSeen = false;
    bool m_imageGone = false;
    int m_startErrorsGiven = 0;
};

// A sink that records every frame, answers CONTINUE (or a scripted action) and refuses-close.
struct RecordingSink : FrameSink
{
    std::vector<std::int64_t> seqs;
    std::vector<std::int64_t> ends;
    std::vector<Refusal> refused;
    RefusalAction onRefusal = RefusalAction::CLOSE_REPLAY;
    int abortNext = 0;  // ABORT this many frames first
    int abortEvery = 0; // ABORT every n-th delivery attempt
    int attempts = 0;

    SinkAction onFrame(const JournalFrame &frame) noexcept override
    {
        ++attempts;
        if (abortNext > 0)
        {
            --abortNext;
            return SinkAction::ABORT;
        }
        if (abortEvery > 0 && attempts % abortEvery == 0)
        {
            return SinkAction::ABORT;
        }
        seqs.push_back(frame.peek.journalSeq);
        ends.push_back(frame.endPosition);
        return SinkAction::CONTINUE;
    }

    RefusalAction onRefused(const RefusedFrame &frame) noexcept override
    {
        refused.push_back(frame.reason);
        return onRefusal;
    }
};

struct RecordingObserver : FollowObserver
{
    std::vector<SourceFault> faults;
    std::vector<AttachInfo> attaches;
    int ended = 0, empty = 0, exhausted = 0, refusedSources = 0;

    void onFault(const SourceFault &f) noexcept override { faults.push_back(f); }
    void onAttached(const AttachInfo &a) noexcept override { attaches.push_back(a); }
    void onRecordingEnded(std::size_t, const Recording &) noexcept override { ++ended; }
    void onChainEmpty(std::size_t) noexcept override { ++empty; }
    void onChainExhausted(std::size_t) noexcept override { ++exhausted; }
    void onSourceRefused(std::size_t, const Recording &) noexcept override { ++refusedSources; }

    bool sawFault(FaultKind kind) const
    {
        for (const auto &f : faults)
        {
            if (f.kind == kind)
            {
                return true;
            }
        }
        return false;
    }
};

// One source: a simulated archive, the memo, the policy and the loop, on a manual clock.
struct Harness
{
    SimArchive archive;
    ArchiveMetrics metrics;
    std::unique_ptr<ResumeMemo> ownMemo;
    ResumeMemo *memo;
    RecordingObserver observer;
    FollowSettings settings;
    std::unique_ptr<FollowPolicy> policy;
    std::unique_ptr<FollowerLoop<SimArchive>> loop;
    RecordingSink sink;
    TimePoint now{};
    std::size_t source;

    explicit Harness(ResumeMemo *shared = nullptr, std::size_t sourceIndex = 0, FollowSettings s = FollowSettings{}) :
        ownMemo(shared ? nullptr : std::make_unique<ResumeMemo>(1, &metrics)), memo(shared ? shared : ownMemo.get()),
        settings(s), source(sourceIndex)
    {
        policy = std::make_unique<FollowPolicy>(source, settings, *memo, &metrics, &observer);
        loop = std::make_unique<FollowerLoop<SimArchive>>(archive, *policy, &metrics);
    }

    void attach() { policy->attach(now); }

    int step(int limit = 32, std::chrono::milliseconds advance = std::chrono::milliseconds(1))
    {
        now += advance;
        return loop->doWork(sink, limit, now);
    }

    template<typename Pred>
    bool runUntil(Pred pred, int maxIterations = 10'000, std::chrono::milliseconds advance = std::chrono::milliseconds(1),
        int limit = 32)
    {
        for (int i = 0; i < maxIterations; ++i)
        {
            if (pred())
            {
                return true;
            }
            step(limit, advance);
        }
        return pred();
    }

    void run(int iterations, std::chrono::milliseconds advance = std::chrono::milliseconds(1), int limit = 32)
    {
        for (int i = 0; i < iterations; ++i)
        {
            step(limit, advance);
        }
    }
};

}

#endif
