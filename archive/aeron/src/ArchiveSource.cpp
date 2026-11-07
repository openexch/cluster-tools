#include "cluster_tools/archive/aeron/ArchiveSource.h"

#include "cluster_tools/archive/TestSeams.h"

#include "aeron_archive_client_error.h"
#include "aeron_client_error.h"
#include "aeronc.h"

#include <sys/statvfs.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <stdexcept>

namespace cluster_tools { namespace archive { namespace aeron_archive {

using test_seams::Rule;
using test_seams::disabled;
namespace ac = ::aeron::archive::client;

namespace {

struct ReentryError : std::logic_error
{
    ReentryError() : std::logic_error("archive re-entered") {}
};

std::uint64_t elapsedMs(std::chrono::steady_clock::time_point since)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
}

std::optional<std::uint64_t> statvfsFreeBytes(const std::string &directory)
{
    struct statvfs fs{};
    if (0 != ::statvfs(directory.c_str(), &fs))
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(fs.f_bavail) * static_cast<std::uint64_t>(fs.f_frsize);
}

}

std::string udpChannel(const std::string &host, const std::string &port)
{
    const bool v6 = host.find(':') != std::string::npos && host.front() != '[';
    return "aeron:udp?endpoint=" + (v6 ? "[" + host + "]" : host) + ":" + port;
}

ArchiveError classifyArchiveError(const std::exception &error, bool fatal)
{
    ArchiveError result;
    result.message = error.what();
    if (fatal || client::aeron_transport::isFatalAeronError(error))
    {
        result.kind = ArchiveErrorKind::FATAL;
        return result;
    }
    result.kind = ArchiveErrorKind::GENERIC;
    if (const auto *sourced = dynamic_cast<const ::aeron::util::SourcedException *>(&error))
    {
        const int code = sourced->errorCode() < 0 ? -sourced->errorCode() : sourced->errorCode();
        switch (code)
        {
            case AERON_ARCHIVE_ERROR_CODE_INVALID_POSITION: result.kind = ArchiveErrorKind::INVALID_POSITION; return result;
            case AERON_ARCHIVE_ERROR_CODE_UNKNOWN_RECORDING: result.kind = ArchiveErrorKind::UNKNOWN_RECORDING; return result;
            case AERON_ARCHIVE_ERROR_CODE_MAX_REPLAYS: result.kind = ArchiveErrorKind::MAX_REPLAYS; return result;
            case AERON_ARCHIVE_ERROR_CODE_STORAGE_SPACE: result.kind = ArchiveErrorKind::STORAGE_SPACE; return result;
            case ETIMEDOUT: result.kind = ArchiveErrorKind::TIMEOUT; return result;
            default: break;
        }
        if (result.message.find("not connected") != std::string::npos)
        {
            result.kind = ArchiveErrorKind::NOT_CONNECTED;
        }
    }
    return result;
}

ArchiveSource::ArchiveSource(ArchiveSourceConfig config, std::shared_ptr<::aeron::Aeron> aeron,
    std::shared_ptr<client::aeron_transport::AeronFatalMonitor> monitor, Invoker invoker, ArchiveMetrics *metrics,
    ArchiveSourceSeams seams, ExitAction exit) :
    m_config(std::move(config)), m_aeron(std::move(aeron)), m_monitor(std::move(monitor)),
    m_invoker(std::move(invoker)), m_metrics(metrics), m_seams(std::move(seams)), m_exit(std::move(exit))
{
    if (!m_exit)
    {
        m_exit = &client::aeron_transport::AeronFatalMonitor::exitProcess;
    }
}

ArchiveSource::~ArchiveSource()
{
    disconnect();
}

bool ArchiveSource::isFatal() const
{
    return m_monitor && m_monitor->isFatal();
}

template<typename F>
auto ArchiveSource::guarded(F &&body) -> decltype(body())
{
    // Checked BEFORE any C call: the archive client mutex is not recursive.
    if (m_inCall)
    {
        m_exit("archive call made while another archive call is in progress (re-entered from the invoker)");
        throw ReentryError(); // only reached when an injected exit action returns
    }
    m_inCall = true;
    const auto start = std::chrono::steady_clock::now();
    try
    {
        if constexpr (std::is_void_v<decltype(body())>)
        {
            body();
            m_inCall = false;
            if (nullptr != m_metrics)
            {
                ArchiveMetrics::raiseMax(m_metrics->archiveCallMaxMs, elapsedMs(start));
            }
        }
        else
        {
            auto result = body();
            m_inCall = false;
            if (nullptr != m_metrics)
            {
                ArchiveMetrics::raiseMax(m_metrics->archiveCallMaxMs, elapsedMs(start));
            }
            return result;
        }
    }
    catch (...)
    {
        m_inCall = false;
        if (nullptr != m_metrics)
        {
            ArchiveMetrics::raiseMax(m_metrics->archiveCallMaxMs, elapsedMs(start));
        }
        throw;
    }
}

void ArchiveSource::reentryCheck()
{
    if (m_inCall)
    {
        m_exit("archive call made while another archive call is in progress (re-entered from the invoker)");
        throw ReentryError(); // only reached when an injected exit action returns
    }
}

void ArchiveSource::invokeOwner() noexcept
{
    try
    {
        if (m_invoker)
        {
            m_invoker();
        }
    }
    catch (...)
    {
        m_exit("an exception escaped the archive invoker");
    }
}

#ifdef CT_TEST_SEAMS
void ArchiveSource::runAsArchiveCallForTest(const std::function<void()> &body)
{
    guarded([&] { body(); });
}
#endif

ConnectPoll ArchiveSource::connectStep()
{
    reentryCheck();
    ConnectPoll result;
    if (isFatal())
    {
        result.state = ConnectPoll::State::FAILED;
        result.error = { ArchiveErrorKind::FATAL, "fatal monitor fired" };
        return result;
    }
    try
    {
        if (!m_async)
        {
            m_context = std::make_unique<ac::Context>();
            m_context->aeron(m_aeron)
                .controlRequestChannel("aeron:udp?endpoint=" + m_config.identity.endpoint)
                .controlRequestStreamId(m_config.identity.controlStreamId)
                .controlResponseChannel(udpChannel(m_config.localHost, "0"))
                .messageTimeoutNs(m_config.messageTimeoutNs)
                .errorHandler([this](const std::exception &error) {
                    try
                    {
                        m_asyncError = error.what(); // an async archive error (4.5 row 13)
                    }
                    catch (...)
                    {
                        m_exit("an exception escaped the archive error handler");
                    }
                });
            if (!disabled(Rule::ADAPTER_INVOKER))
            {
                m_context->delegatingInvoker([this]() { invokeOwner(); });
            }
            m_async = ac::AeronArchive::asyncConnect(*m_context);
        }
        std::shared_ptr<ac::AeronArchive> archive = guarded([&] { return m_async->poll(); });
        if (!archive)
        {
            return result; // PENDING
        }
        m_archive = std::move(archive);
        m_async.reset();
        result.state = ConnectPoll::State::CONNECTED;
    }
    catch (const ReentryError &)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        m_async.reset();
        m_archive.reset();
        m_context.reset();
        result.state = ConnectPoll::State::FAILED;
        result.error = classifyArchiveError(error, isFatal());
        m_lastError = result.error.message;
    }
    return result;
}

void ArchiveSource::disconnect() noexcept
{
    try
    {
        m_subscription.reset();
        dropOrphans();
        if (!isFatal())
        {
            m_archive.reset(); // aeron_archive_close: a close-session offer, bounded
            m_async.reset();
            m_context.reset();
        }
        else
        {
            // The conductor is exiting the process: do not touch Aeron (row 1); leak on purpose.
            new std::shared_ptr<ac::AeronArchive>(std::move(m_archive));
        }
    }
    catch (...)
    {
    }
    m_asyncError.reset();
}

std::int64_t ArchiveSource::archiveId()
{
    return m_archive ? guarded([&] { return m_archive->archiveId(); }) : -1;
}

ListResult ArchiveSource::list()
{
    reentryCheck();
    ListResult result;
    if (isFatal())
    {
        result.error = { ArchiveErrorKind::FATAL, "fatal monitor fired" }; // 3.3: never touch Aeron again
        return result;
    }
    if (!m_archive)
    {
        result.error = { ArchiveErrorKind::NOT_CONNECTED, "not connected" };
        return result;
    }
    try
    {
        std::vector<Recording> recordings;
        bool consumerFailed = false;
        guarded([&] {
            m_archive->listRecordingsForUri(0, INT32_MAX, m_config.identity.channel, m_config.identity.journalStreamId,
                [&](ac::RecordingDescriptor &d) {
                    try
                    {
                        Recording r;
                        r.key.recordingId = d.m_recordingId;
                        r.key.startTimestamp = d.m_startTimestamp;
                        r.key.initialTermId = d.m_initialTermId;
                        r.key.sessionId = d.m_sessionId;
                        r.key.termBufferLength = d.m_termBufferLength;
                        r.key.mtuLength = d.m_mtuLength;
                        r.key.streamId = d.m_streamId;
                        r.startPosition = d.m_startPosition;
                        r.stopPosition = d.m_stopPosition;
                        recordings.push_back(r);
                    }
                    catch (...)
                    {
                        consumerFailed = true; // never unwind through the C listing loop
                    }
                });
        });
        if (consumerFailed)
        {
            result.error = { ArchiveErrorKind::GENERIC, "out of memory while listing" };
            return result;
        }
        result.ok = true;
        result.recordings = std::move(recordings);
    }
    catch (const ReentryError &)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        result.error = classifyArchiveError(error, isFatal());
        m_lastError = result.error.message;
    }
    return result;
}

void ArchiveSource::dropOrphans() noexcept
{
    for (auto it = m_orphanRegistrations.begin(); it != m_orphanRegistrations.end();)
    {
        try
        {
            auto sub = m_aeron->findSubscription(*it);
            if (sub)
            {
                it = m_orphanRegistrations.erase(it); // found: dropping the shared_ptr closes it
                continue;
            }
            ++it;
        }
        catch (...)
        {
            it = m_orphanRegistrations.erase(it); // the add failed: nothing to close
        }
    }
}

SubscriptionResult ArchiveSource::openSubscription(std::int32_t termBufferLength)
{
    SubscriptionResult result;
    dropOrphans();
    if (isFatal())
    {
        result.state = SubscriptionResult::State::FAILED;
        result.error = { ArchiveErrorKind::FATAL, "fatal monitor fired" };
        return result;
    }
    // 4.3 step 2: the driver directory of the aeron::Context actually in use, not a setting.
    const std::string driverDir = aeron_context_get_dir(aeron_context(m_aeron->aeron()));
    result.requiredBytes = replayStorageRequirement(termBufferLength);
#ifdef CT_TEST_SEAMS
    const auto free = m_seams.freeBytes ? m_seams.freeBytes(driverDir) : statvfsFreeBytes(driverDir);
#else
    const auto free = statvfsFreeBytes(driverDir);
#endif
    result.freeBytes = free.value_or(0);
    if ((!free || *free < result.requiredBytes) && !disabled(Rule::ADAPTER_STORAGE_PRECHECK))
    {
        result.state = SubscriptionResult::State::STORAGE_REFUSED;
        return result;
    }
    try
    {
        m_subscription.reset();
        m_subscriptionRegistration =
            m_aeron->addSubscription(udpChannel(m_config.localHost, "0"), m_config.replayStreamId);
        result.state = SubscriptionResult::State::ADDED;
    }
    catch (const std::exception &error)
    {
        result.state = SubscriptionResult::State::FAILED;
        result.error = classifyArchiveError(error, isFatal());
        m_lastError = result.error.message;
    }
    return result;
}

std::string ArchiveSource::replayHost(const std::string &resolved) const
{
#ifdef CT_TEST_SEAMS
    if (!m_seams.advertisedHostOverride.empty())
    {
        const auto colon = resolved.rfind(':');
        return m_seams.advertisedHostOverride + (colon == std::string::npos ? std::string() : resolved.substr(colon));
    }
#endif
    return resolved;
}

EndpointPoll ArchiveSource::pollEndpoint()
{
    EndpointPoll result;
    try
    {
        if (!m_subscription)
        {
            m_subscription = m_aeron->findSubscription(m_subscriptionRegistration);
            if (!m_subscription)
            {
                return result; // PENDING
            }
        }
        if (disabled(Rule::ADAPTER_RESOLVE_ENDPOINT))
        {
            // RED shape: the port-0 endpoint goes to the archive unresolved
            result.state = EndpointPoll::State::RESOLVED;
            result.endpoint = (m_config.localHost.find(':') != std::string::npos ? "[" + m_config.localHost + "]"
                                                                                   : m_config.localHost) + ":0";
            return result;
        }
        const std::string resolved = m_subscription->resolvedEndpoint();
        if (resolved.empty())
        {
            return result;
        }
        result.state = EndpointPoll::State::RESOLVED;
        result.endpoint = replayHost(resolved);
    }
    catch (const std::exception &error)
    {
        result.state = EndpointPoll::State::FAILED;
        result.error = classifyArchiveError(error, isFatal());
        m_lastError = result.error.message;
    }
    return result;
}

StartResult ArchiveSource::startReplay(const ReplayRequest &request)
{
    reentryCheck();
    StartResult result;
    if (isFatal())
    {
        result.error = { ArchiveErrorKind::FATAL, "fatal monitor fired" };
        return result;
    }
    if (!m_archive)
    {
        result.error = { ArchiveErrorKind::NOT_CONNECTED, "not connected" };
        return result;
    }
    try
    {
        // The C aeron_archive_replay helper is not used: it sends the channel verbatim and a port-0
        // endpoint reaches the archive unresolved (contract 1.4, cHelperSendsPortZeroUnresolved).
        m_lastReplayChannel = "aeron:udp?endpoint=" + request.endpoint;
        ac::ReplayParams params;
        params.position(request.from).length(request.length);
        result.replaySessionId = guarded([&] {
            return m_archive->startReplay(request.recordingId, m_lastReplayChannel, m_config.replayStreamId, params);
        });
        result.ok = true;
    }
    catch (const ReentryError &)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        result.error = classifyArchiveError(error, isFatal());
        m_lastError = result.error.message;
    }
    return result;
}

ImageObservation ArchiveSource::observeImage()
{
    ImageObservation o;
    if (!m_subscription)
    {
        o.subscriptionClosed = true;
        return o;
    }
    try
    {
        o.subscriptionClosed = m_subscription->isClosed();
        o.imageCount = m_subscription->imageCount();
        if (o.imageCount > 0)
        {
            auto image = m_subscription->imageByIndex(0);
            o.endOfStream = image->isEndOfStream();
            o.imageClosed = image->isClosed();
            o.position = image->position();
        }
    }
    catch (const std::exception &error)
    {
        m_lastError = error.what();
        o.imageCount = 0;
    }
    return o;
}

AsyncErrorPoll ArchiveSource::pollAsyncError()
{
    reentryCheck();
    AsyncErrorPoll result;
    if (isFatal())
    {
        return result; // the loop sees the monitor on its next check; Aeron is not touched
    }
    if (!m_archive)
    {
        result.state = AsyncErrorPoll::State::DISCONNECTED;
        return result;
    }
    if (m_asyncError)
    {
        result.state = AsyncErrorPoll::State::ERROR;
        result.message = *m_asyncError;
        m_asyncError.reset();
        return result;
    }
    try
    {
        const std::string error = guarded([&] { return m_archive->pollForErrorResponse(); });
        if (!error.empty())
        {
            result.state = AsyncErrorPoll::State::ERROR;
            result.message = error;
        }
    }
    catch (const ReentryError &)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        const ArchiveError e = classifyArchiveError(error, isFatal());
        result.state = ArchiveErrorKind::NOT_CONNECTED == e.kind ? AsyncErrorPoll::State::DISCONNECTED
                                                                 : AsyncErrorPoll::State::ERROR;
        result.message = e.message;
    }
    return result;
}

ProbeResult ArchiveSource::probe(std::int64_t recordingId)
{
    reentryCheck();
    ProbeResult result;
    if (isFatal())
    {
        result.error = { ArchiveErrorKind::FATAL, "fatal monitor fired" };
        return result;
    }
    if (!m_archive)
    {
        result.error = { ArchiveErrorKind::NOT_CONNECTED, "not connected" };
        return result;
    }
    try
    {
        // One probe step: the recorded position, and the stop position when the recording stopped.
        const std::int64_t position = guarded([&] { return m_archive->getRecordingPosition(recordingId); });
        if (ac::NULL_POSITION != position)
        {
            result.state = ProbeResult::State::ACTIVE;
            result.position = position;
            return result;
        }
        result.position = guarded([&] { return m_archive->getStopPosition(recordingId); });
        result.state = ProbeResult::State::STOPPED;
    }
    catch (const ReentryError &)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        result.state = ProbeResult::State::FAILED;
        result.error = classifyArchiveError(error, isFatal());
        m_lastError = result.error.message;
    }
    return result;
}

void ArchiveSource::stopReplay(std::int64_t replaySessionId) noexcept
{
    if (m_inCall)
    {
        m_exit("archive call made while another archive call is in progress (re-entered from the invoker)");
        return;
    }
    if (!m_archive || isFatal())
    {
        return;
    }
    try
    {
        guarded([&] { m_archive->stopReplay(replaySessionId); });
    }
    catch (const ReentryError &)
    {
        m_exit("archive re-entered from the invoker");
    }
    catch (const std::exception &error)
    {
        m_lastError = error.what(); // a replay the archive already ended: UNKNOWN_REPLAY, harmless
    }
}

void ArchiveSource::closeSubscription() noexcept
{
    try
    {
        if (!m_subscription && m_subscriptionRegistration >= 0)
        {
            m_orphanRegistrations.push_back(m_subscriptionRegistration); // the add is still in flight
        }
        m_subscription.reset();
        m_subscriptionRegistration = -1;
        dropOrphans();
    }
    catch (...)
    {
    }
}

}}}
