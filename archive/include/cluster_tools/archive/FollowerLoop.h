#ifndef CLUSTER_TOOLS_ARCHIVE_FOLLOWER_LOOP_H
#define CLUSTER_TOOLS_ARCHIVE_FOLLOWER_LOOP_H

// One bounded duty step of one source follower (RC2 S9a contract 3.2 step 3): the driver both the
// Aeron adapter (archive/aeron ChainFollower = FollowerLoop<ArchiveSource>) and the simulated
// archive of the property test use, so the walk under test is the walk that ships.
//
// Port is a duck-typed archive endpoint:
//   bool isFatal() const;                       // the shared fatal monitor fired
//   ConnectPoll connectStep();                  // start or poll the async connect
//   void disconnect() noexcept;
//   ListResult list();
//   SubscriptionResult openSubscription(std::int32_t termBufferLength); // precheck + add port-0 sub
//   EndpointPoll pollEndpoint();
//   StartResult startReplay(const ReplayRequest &request);
//   template<class H> int poll(H &&handler, int fragmentLimit);
//       // handler(std::uint8_t flags, const std::uint8_t *bytes, std::size_t length,
//       //         std::int64_t endPosition) -> SinkAction, mapped onto the controlled poll
//   ImageObservation observeImage();
//   AsyncErrorPoll pollAsyncError();
//   ProbeResult probe(std::int64_t recordingId);
//   void stopReplay(std::int64_t replaySessionId) noexcept;
//   void closeSubscription() noexcept;

#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/archive/FramePeek.h"
#include "cluster_tools/archive/TestSeams.h"

namespace cluster_tools { namespace archive {

template<typename Port>
class FollowerLoop
{
public:
    FollowerLoop(Port &port, FollowPolicy &policy, ArchiveMetrics *metrics) :
        m_port(port), m_policy(policy), m_metrics(metrics)
    {
    }

    // At most `fragmentLimit` replay fragments (0 = paused: none) and at most one archive control
    // call. Returns the number of fragments read.
    int doWork(FrameSink &sink, int fragmentLimit, TimePoint now)
    {
        using test_seams::Rule;
        using test_seams::disabled;

        // Row 1: the conductor is exiting the process; never touch Aeron again.
        if (Phase::FATAL == m_policy.phase())
        {
            return 0;
        }
        if (m_port.isFatal() && !disabled(Rule::FOLLOW_ROW1_FATAL))
        {
            m_policy.onFatal();
            return 0;
        }
        const bool paused = fragmentLimit <= 0;
        m_policy.setPaused(paused);
        int work = 0;
        if (Phase::RESOLVING == m_policy.phase())
        {
            m_policy.onEndpoint(m_port.pollEndpoint(), now);
        }
        if (Phase::FOLLOWING == m_policy.phase())
        {
            if (!paused)
            {
                work += pollFragments(sink, fragmentLimit, now);
            }
            if (Phase::FOLLOWING == m_policy.phase())
            {
                const ImageObservation image = m_port.observeImage();
                const AsyncErrorPoll asyncError = m_port.pollAsyncError();
                m_policy.afterPoll(image, asyncError, now);
            }
        }
        else if (m_policy.connected() && Phase::FATAL != m_policy.phase())
        {
            // 3.2 step 3: the async error poll runs every iteration, not only while a replay is attached.
            m_policy.onIdleAsyncError(m_port.pollAsyncError(), now);
        }
        if (paused && disabled(Rule::FOLLOW_PAUSE_SUPPRESSES_CONTROL))
        {
            return work; // RED shape (review F1): a paused follower that never probes
        }
        for (int guard = 0; guard < 6; ++guard)
        {
            const ControlStep step = m_policy.nextControl(now);
            if (ControlStep::NONE == step)
            {
                break;
            }
            execute(step, now);
            if (m_policy.wantsDisconnect())
            {
                m_port.disconnect();
                m_policy.onDisconnected();
            }
            if (isControlCall(step) || Phase::FATAL == m_policy.phase())
            {
                break;
            }
        }
        if (m_policy.wantsDisconnect())
        {
            m_port.disconnect();
            m_policy.onDisconnected();
        }
        return work;
    }

    FollowPolicy &policy() noexcept { return m_policy; }

private:
    int pollFragments(FrameSink &sink, int fragmentLimit, TimePoint now)
    {
        using test_seams::Rule;
        using test_seams::disabled;
        const Recording *recording = m_policy.currentRecording();
        if (nullptr == recording)
        {
            return 0;
        }
        const RecordingKey key = recording->key;
        const std::size_t source = m_policy.sourceIndex();
        bool sinkAborted = false;
        auto handler = [&](std::uint8_t flags, const std::uint8_t *bytes, std::size_t length,
                           std::int64_t endPosition) -> SinkAction {
            if (!m_policy.readingAllowed())
            {
                return SinkAction::ABORT; // faulted earlier in this poll
            }
            const Admission admission = admitFrame(flags, bytes, length);
            if (!admission.accepted())
            {
                if (nullptr != m_metrics)
                {
                    m_metrics->refused(admission.refusal);
                }
                RefusedFrame refused;
                refused.sourceIndex = source;
                refused.key = key;
                refused.reason = admission.refusal;
                refused.endPosition = endPosition;
                refused.bytes = bytes;
                refused.length = length;
                refused.hasPeek = admission.hasPeek;
                refused.peek = admission.peek;
                if (RefusalAction::SKIP_AND_CONTINUE == sink.onRefused(refused))
                {
                    m_policy.onRefusalSkipped(endPosition);
                    return SinkAction::CONTINUE;
                }
                m_policy.onRefused(admission.refusal, endPosition, now);
                return SinkAction::ABORT;
            }
            if (FrameCheck::FAULTED == m_policy.checkFrame(admission.peek, now))
            {
                return SinkAction::ABORT;
            }
            const bool advanceEarly = disabled(Rule::FOLLOW_ADVANCE_BEFORE_SINK);
            if (advanceEarly)
            {
                m_policy.onAccepted(admission.peek, endPosition); // RED shape (F19)
            }
            JournalFrame frame;
            frame.sourceIndex = source;
            frame.key = key;
            frame.endPosition = endPosition;
            frame.bytes = bytes;
            frame.length = length;
            frame.peek = admission.peek;
            frame.readWasMemoFree = m_policy.readWasMemoFree();
            const SinkAction action = sink.onFrame(frame);
            if (SinkAction::ABORT != action && !advanceEarly)
            {
                m_policy.onAccepted(admission.peek, endPosition);
            }
            if (SinkAction::ABORT == action)
            {
                sinkAborted = true; // owner back-pressure, not a policy fault
            }
            return action;
        };
        const int fragments = m_port.poll(handler, fragmentLimit);
        if (sinkAborted && Phase::FOLLOWING == m_policy.phase())
        {
            m_policy.onSinkBackPressure(); // review F-4: counts as paused for rows 11, 12, 12b
        }
        return fragments;
    }

    void execute(ControlStep step, TimePoint now)
    {
        switch (step)
        {
            case ControlStep::NONE:
                return;
            case ControlStep::CONNECT:
                m_policy.onConnect(m_port.connectStep(), now);
                return;
            case ControlStep::LIST:
                m_policy.onListed(m_port.list(), now);
                return;
            case ControlStep::OPEN_SUBSCRIPTION:
                m_policy.onSubscription(m_port.openSubscription(m_policy.replayRequest().termBufferLength), now);
                return;
            case ControlStep::START_REPLAY:
                m_policy.onReplayStarted(m_port.startReplay(m_policy.replayRequest()), now);
                return;
            case ControlStep::PROBE:
                m_policy.onProbe(m_port.probe(m_policy.replayRequest().recordingId), now);
                return;
            case ControlStep::STOP_REPLAY:
                m_port.stopReplay(m_policy.replaySessionId());
                m_port.closeSubscription();
                m_policy.onDetached(now);
                return;
            case ControlStep::CLOSE_SUBSCRIPTION:
                m_port.closeSubscription();
                m_policy.onDetached(now);
                return;
        }
    }

    Port &m_port;
    FollowPolicy &m_policy;
    ArchiveMetrics *m_metrics;
};

}}

#endif
