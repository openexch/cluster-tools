#ifndef CLUSTER_TOOLS_ARCHIVE_AERON_ARCHIVE_SOURCE_H
#define CLUSTER_TOOLS_ARCHIVE_AERON_ARCHIVE_SOURCE_H

// One archive endpoint over the Aeron C archive client's cpp_wrapper (RC2 S9a contract 3.3, 4.3):
// the FollowerLoop Port of archive/FollowerLoop.h. Async connect polled one step per iteration
// (the archive Context kept alive with the client), listRecordingsForUri, the 4.3 replay open
// (storage precheck on the driver dir of the aeron::Context in use, the port-0 subscription, its
// resolved endpoint, then startReplay), the recording probe, stopReplay, the async error poll.
//
// Every archive control call is bounded by the message timeout (5 s) and goes through one guard:
//   - a re-entrancy flag checked BEFORE calling into C (the C client mutex is not recursive): an
//     archive call made while one is in progress (from the owner's invoker) is a programming error
//     and ends the process with exit 70;
//   - the fatal monitor: once it fired, no Aeron object is touched again (FATAL);
//   - the call duration feeds bridge_archive_call_max_ms.
// Inside the archive wait loop the wrapper yields, then calls the owner's invoker (the only park on
// that path); the invoker is wrapped noexcept so an exception never unwinds through the C frames of
// aeron_archive_context.c, and the frame handler of the controlled poll likewise.

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/archive/Chain.h"
#include "cluster_tools/archive/Follow.h"
#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/archive/TestSeams.h"
#include "cluster_tools/client/aeron/FatalErrors.h"

#include <Aeron.h>
#include <client/archive/AeronArchive.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cluster_tools { namespace archive { namespace aeron_archive {

using Invoker = std::function<void()>;
using ExitAction = std::function<void(const std::string &message)>;
using FreeBytesProbe = std::function<std::optional<std::uint64_t>(const std::string &directory)>;

inline constexpr std::uint64_t ARCHIVE_MESSAGE_TIMEOUT_NS = 5'000'000'000ULL; // the projector's value

struct ArchiveSourceConfig
{
    SourceIdentity identity;
    std::string localHost;              // BRIDGE_LOCAL_HOST: control responses and the replay come here
    std::int32_t replayStreamId = 0;    // BRIDGE_REPLAY_STREAM
    std::uint64_t messageTimeoutNs = ARCHIVE_MESSAGE_TIMEOUT_NS;
};

// Test seams of the 9.3 integration run. In a production build (no CT_TEST_SEAMS) the struct is
// empty, so nothing can reroute the replay or replace statvfs (review N-7).
struct ArchiveSourceSeams
{
#ifdef CT_TEST_SEAMS
    FreeBytesProbe freeBytes;           // storagePrecheckRefusesWithoutRescan: a tmpfs below 3 terms
    std::string advertisedHostOverride; // neverConnected15s: the replay goes to TEST-NET-1
#endif
};

std::string udpChannel(const std::string &host, const std::string &port);
ArchiveError classifyArchiveError(const std::exception &error, bool fatal);

class ArchiveSource
{
public:
    ArchiveSource(ArchiveSourceConfig config, std::shared_ptr<::aeron::Aeron> aeron,
        std::shared_ptr<client::aeron_transport::AeronFatalMonitor> monitor, Invoker invoker, ArchiveMetrics *metrics,
        ArchiveSourceSeams seams = {}, ExitAction exit = &client::aeron_transport::AeronFatalMonitor::exitProcess);
    ~ArchiveSource();

    ArchiveSource(const ArchiveSource &) = delete;
    ArchiveSource &operator=(const ArchiveSource &) = delete;

    const ArchiveSourceConfig &config() const noexcept { return m_config; }

    // ---- the FollowerLoop Port
    bool isFatal() const;
    ConnectPoll connectStep();
    void disconnect() noexcept;
    ListResult list();
    SubscriptionResult openSubscription(std::int32_t termBufferLength);
    EndpointPoll pollEndpoint();
    StartResult startReplay(const ReplayRequest &request);
    ImageObservation observeImage();
    AsyncErrorPoll pollAsyncError();
    ProbeResult probe(std::int64_t recordingId);
    void stopReplay(std::int64_t replaySessionId) noexcept;
    void closeSubscription() noexcept;

    template<typename H>
    int poll(H &&handler, int fragmentLimit)
    {
        if (!m_subscription || isFatal())
        {
            return 0;
        }
        auto fragment = [&](::aeron::concurrent::AtomicBuffer &buffer, ::aeron::util::index_t offset,
                            ::aeron::util::index_t length, ::aeron::concurrent::logbuffer::Header &header) noexcept
            -> ::aeron::ControlledPollAction {
            try
            {
                const SinkAction action = handler(header.flags(), buffer.buffer() + offset,
                    static_cast<std::size_t>(length < 0 ? 0 : length), header.position());
                switch (action)
                {
                    case SinkAction::CONTINUE: return ::aeron::ControlledPollAction::CONTINUE;
                    case SinkAction::BREAK: return ::aeron::ControlledPollAction::BREAK;
                    case SinkAction::ABORT: return ::aeron::ControlledPollAction::ABORT;
                }
            }
            catch (...)
            {
                m_exit("an exception escaped the replay frame handler");
            }
            return ::aeron::ControlledPollAction::ABORT;
        };
        try
        {
            return m_subscription->controlledPoll(fragment, fragmentLimit);
        }
        catch (const std::exception &error)
        {
            m_lastError = error.what();
            return 0;
        }
    }

    // ---- state for tests and the owner
    bool connected() const noexcept { return static_cast<bool>(m_archive); }
    const std::string &lastReplayChannel() const noexcept { return m_lastReplayChannel; }
    const std::string &lastError() const noexcept { return m_lastError; }
    std::int64_t archiveId();

#ifdef CT_TEST_SEAMS
    // Test seam (W3 ArchiveSourceTest.reentryFromInvokerExits70): run `body` as if inside an archive
    // call, so an archive call made from it hits the re-entrancy guard.
    void runAsArchiveCallForTest(const std::function<void()> &body);
    // Test seam: the invoker exactly as the archive wait loop calls it.
    void invokeOwnerForTest() noexcept { invokeOwner(); }
#endif

private:
    template<typename F>
    auto guarded(F &&body) -> decltype(body());
    void reentryCheck();
    void invokeOwner() noexcept;
    void dropOrphans() noexcept;
    std::string replayHost(const std::string &resolved) const;

    ArchiveSourceConfig m_config;
    std::shared_ptr<::aeron::Aeron> m_aeron;
    std::shared_ptr<client::aeron_transport::AeronFatalMonitor> m_monitor;
    Invoker m_invoker;
    ArchiveMetrics *m_metrics;
    ArchiveSourceSeams m_seams;
    ExitAction m_exit;

    std::unique_ptr<::aeron::archive::client::Context> m_context; // alive as long as the client (3.3)
    std::shared_ptr<::aeron::archive::client::AeronArchive::AsyncConnect> m_async;
    std::shared_ptr<::aeron::archive::client::AeronArchive> m_archive;
    bool m_inCall = false;

    std::int64_t m_subscriptionRegistration = -1;
    std::shared_ptr<::aeron::Subscription> m_subscription;
    std::vector<std::int64_t> m_orphanRegistrations;

    std::optional<std::string> m_asyncError;
    std::string m_lastReplayChannel;
    std::string m_lastError;
};

}}}

#endif
