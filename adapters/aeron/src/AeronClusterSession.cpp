#include "cluster_tools/adapters/aeron/AeronClusterSession.h"

#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#include <Aeron.h>
#include <FragmentAssembler.h>

#include "io_aeron_cluster_codecs/MessageHeader.h"
#include "io_aeron_cluster_codecs/VarAsciiEncoding.h"
#include "io_aeron_cluster_codecs/EventCode.h"
#include "io_aeron_cluster_codecs/SessionConnectRequest.h"
#include "io_aeron_cluster_codecs/SessionCloseRequest.h"
#include "io_aeron_cluster_codecs/SessionEvent.h"
#include "io_aeron_cluster_codecs/SessionMessageHeader.h"
#include "io_aeron_cluster_codecs/SessionKeepAlive.h"
#include "io_aeron_cluster_codecs/NewLeaderEvent.h"

namespace cc = io::aeron::cluster::codecs;

using namespace aeron;
using namespace aeron::concurrent;

namespace cluster_tools { namespace aeron_adapter {

namespace
{

constexpr std::int32_t PROTOCOL_SEMANTIC_VERSION = (0 << 16) | (3 << 8) | 0; // SemanticVersion.compose(0,3,0)

// Cluster egress/ingress template ids (aeron-cluster-codecs.xml, schema 111).
constexpr std::uint16_t TEMPLATE_SESSION_MESSAGE_HEADER = 1;
constexpr std::uint16_t TEMPLATE_SESSION_EVENT = 2;
constexpr std::uint16_t TEMPLATE_NEW_LEADER_EVENT = 6;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Member
{
    int id = -1;
    std::string endpoint;
    std::shared_ptr<Publication> publication;
};

// "id=host:port,..." — mirrors AeronCluster.parseIngressEndpoints. Returns an empty
// vector (with reason) rather than throwing: parse errors are port-boundary failures.
bool parseIngressEndpoints(const std::string &spec, std::vector<Member> &members, std::string &reason)
{
    std::size_t pos = 0;
    while (pos < spec.size())
    {
        std::size_t comma = spec.find(',', pos);
        if (comma == std::string::npos)
        {
            comma = spec.size();
        }
        const std::string token = spec.substr(pos, comma - pos);
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= token.size())
        {
            reason = "bad ingress endpoint token '" + token + "' (want id=host:port)";
            return false;
        }
        Member member;
        try
        {
            member.id = std::stoi(token.substr(0, eq));
        }
        catch (...)
        {
            reason = "bad member id in token '" + token + "'";
            return false;
        }
        member.endpoint = token.substr(eq + 1);
        members.push_back(std::move(member));
        pos = comma + 1;
    }
    if (members.empty())
    {
        reason = "no ingress endpoints in '" + spec + "'";
        return false;
    }
    return true;
}

const char *offerErrorName(std::int64_t result)
{
    if (NOT_CONNECTED == result)
    {
        return "NOT_CONNECTED";
    }
    if (PUBLICATION_CLOSED == result)
    {
        return "PUBLICATION_CLOSED";
    }
    if (MAX_POSITION_EXCEEDED == result)
    {
        return "MAX_POSITION_EXCEEDED";
    }
    return "UNKNOWN";
}

}

struct AeronClusterSession::Impl
{
    explicit Impl(AeronClusterSessionSettings settingsIn) :
        settings(std::move(settingsIn)),
        assembler(
            [this](AtomicBuffer &buffer, util::index_t offset, util::index_t length, logbuffer::Header &header)
            {
                onFragment(buffer, offset, length, header);
            })
    {
        assemblingHandler = assembler.handler();
    }

    ~Impl()
    {
        close();
    }

    AeronClusterSessionSettings settings;

    std::shared_ptr<Aeron> client;
    std::shared_ptr<Subscription> egressSubscription;
    std::vector<Member> members;
    std::shared_ptr<Publication> sessionPublication;

    // Persistent across poll() calls: a fragmented egress frame may span several polls.
    FragmentAssembler assembler;
    fragment_handler_t assemblingHandler;

    ports::SessionInfo info;
    bool connected = false;
    bool closed = false;
    bool sessionClosedByCluster = false;

    // connect-phase state
    int redirectToMemberId = -1;
    int connectedViaMemberIndex = -1;
    bool connectRejected = false;
    std::string connectRejectDetail;
    std::map<std::int64_t, std::size_t> outstandingConnects; // correlationId -> member index

    // poll-phase state
    const ports::EgressHandler *pollHandler = nullptr;
    std::int64_t lastKeepAliveMs = 0;

    // Pre-encoded once at connect; reused for the lifetime of the session.
    std::uint8_t keepAliveFrame[64] = {};
    util::index_t keepAliveLength = 0;

    std::vector<std::uint8_t> offerFrame; // reused offer scratch (cold path)

    void onFragment(AtomicBuffer &buffer, util::index_t offset, util::index_t length, logbuffer::Header &)
    {
        char *buf = reinterpret_cast<char *>(buffer.buffer());
        const std::uint64_t cap = static_cast<std::uint64_t>(offset) + length;
        cc::MessageHeader hdr(buf, offset, cap, 0);
        const std::uint64_t bodyOffset = offset + hdr.encodedLength();

        if (hdr.schemaId() != cc::MessageHeader::sbeSchemaId())
        {
            return; // not the cluster protocol; nothing of ours
        }

        switch (hdr.templateId())
        {
            case TEMPLATE_SESSION_EVENT:
            {
                cc::SessionEvent event(buf, bodyOffset, cap, hdr.blockLength(), hdr.version());
                const std::int64_t correlationId = event.correlationId();

                auto it = outstandingConnects.find(correlationId);
                if (it != outstandingConnects.end())
                {
                    if (cc::EventCode::OK == event.code())
                    {
                        connected = true;
                        info.sessionId = event.clusterSessionId();
                        info.leadershipTermId = event.leadershipTermId();
                        info.leaderMemberId = event.leaderMemberId();
                        connectedViaMemberIndex = static_cast<int>(it->second);
                    }
                    else if (cc::EventCode::REDIRECT == event.code())
                    {
                        redirectToMemberId = event.leaderMemberId();
                    }
                    else if (cc::EventCode::ERROR == event.code() ||
                        cc::EventCode::AUTHENTICATION_REJECTED == event.code())
                    {
                        connectRejected = true;
                        connectRejectDetail = event.getDetailAsString();
                    }
                }
                else if (cc::EventCode::CLOSED == event.code() &&
                    connected && event.clusterSessionId() == info.sessionId)
                {
                    sessionClosedByCluster = true;
                }
                break;
            }

            case TEMPLATE_NEW_LEADER_EVENT:
            {
                // Recorded so sessionInfo() stays truthful. Re-routing ingress to the new
                // leader mid-session is out of scope for this cold-path skeleton: offers
                // keep using the original publication and will surface NOT_CONNECTED if
                // that member is gone. TODO(cluster-client): adopt the new leader's
                // ingress endpoints (NewLeaderEvent carries them) when a tool needs to
                // outlive an election.
                cc::NewLeaderEvent event(buf, bodyOffset, cap, hdr.blockLength(), hdr.version());
                if (connected && event.clusterSessionId() == info.sessionId)
                {
                    info.leadershipTermId = event.leadershipTermId();
                    info.leaderMemberId = event.leaderMemberId();
                }
                break;
            }

            case TEMPLATE_SESSION_MESSAGE_HEADER:
            {
                cc::SessionMessageHeader smh(buf, bodyOffset, cap, hdr.blockLength(), hdr.version());
                const std::uint64_t appOffset = bodyOffset + smh.encodedLength();
                if (appOffset > cap)
                {
                    return;
                }
                // App frames polled while no handler is armed (the tail of a connect()
                // poll batch) are dropped: pre-first-poll egress is noise for a tool.
                if (nullptr != pollHandler && connected)
                {
                    ports::EgressMessage message;
                    message.clusterSessionId = smh.clusterSessionId();
                    message.timestamp = smh.timestamp();
                    message.payload = reinterpret_cast<const std::uint8_t *>(buf) + appOffset;
                    message.length = static_cast<std::size_t>(cap - appOffset);
                    (*pollHandler)(message);
                }
                break;
            }

            default:
                break; // Challenge etc. — not needed for the tools yet
        }
    }

    bool awaitRegistration(std::int64_t deadlineMs, std::int64_t subscriptionRegistrationId,
        const std::vector<std::int64_t> &publicationRegistrationIds, std::string &reason)
    {
        while (!(egressSubscription = client->findSubscription(subscriptionRegistrationId)))
        {
            if (nowMs() > deadlineMs)
            {
                reason = "egress subscription not registered before the connect deadline";
                return false;
            }
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < members.size(); i++)
        {
            while (!(members[i].publication = client->findPublication(publicationRegistrationIds[i])))
            {
                if (nowMs() > deadlineMs)
                {
                    reason = "ingress publication to member " + std::to_string(members[i].id) +
                        " not registered before the connect deadline";
                    return false;
                }
                std::this_thread::yield();
            }
        }
        return true;
    }

    void encodeKeepAlive()
    {
        cc::SessionKeepAlive keepAlive;
        keepAlive.wrapAndApplyHeader(
                reinterpret_cast<char *>(keepAliveFrame), 0, sizeof(keepAliveFrame))
            .leadershipTermId(info.leadershipTermId)
            .clusterSessionId(info.sessionId);
        keepAliveLength = static_cast<util::index_t>(
            cc::MessageHeader::encodedLength() + keepAlive.encodedLength());
    }

    void offerKeepAliveIfDue()
    {
        const std::int64_t now = nowMs();
        if (now - lastKeepAliveMs >= settings.keepAliveIntervalMs)
        {
            AtomicBuffer frame(keepAliveFrame, static_cast<std::size_t>(keepAliveLength));
            sessionPublication->offer(frame, 0, keepAliveLength); // best effort; next poll retries
            lastKeepAliveMs = now;
        }
    }

    // Full teardown; also the failure path of a half-built connect so connect() can be
    // retried on a clean object. Idempotent. Releasing the shared_ptrs closes the
    // underlying aeron resources (the cpp_wrapper is RAII throughout) — there is no
    // manual close sequence to forget.
    void teardown()
    {
        sessionPublication.reset();
        for (Member &member : members)
        {
            member.publication.reset();
        }
        members.clear();
        egressSubscription.reset();
        client.reset();
        outstandingConnects.clear();
        redirectToMemberId = -1;
        connectedViaMemberIndex = -1;
        connectRejected = false;
        connected = false;
    }

    void close()
    {
        if (closed)
        {
            return;
        }
        closed = true;
        try
        {
            if (connected && !sessionClosedByCluster && sessionPublication)
            {
                // Best-effort polite close; the cluster reaps silent sessions by
                // keepalive timeout anyway, so a failed offer is not an error.
                std::uint8_t frame[64] = {};
                cc::SessionCloseRequest request;
                request.wrapAndApplyHeader(reinterpret_cast<char *>(frame), 0, sizeof(frame))
                    .leadershipTermId(info.leadershipTermId)
                    .clusterSessionId(info.sessionId);
                const util::index_t frameLength = static_cast<util::index_t>(
                    cc::MessageHeader::encodedLength() + request.encodedLength());
                AtomicBuffer atomicFrame(frame, static_cast<std::size_t>(frameLength));
                sessionPublication->offer(atomicFrame, 0, frameLength);
            }
        }
        catch (...)
        {
            // close() must not throw — teardown below still runs.
        }
        teardown();
    }
};

AeronClusterSession::AeronClusterSession(AeronClusterSessionSettings settings) :
    m_impl(std::make_unique<Impl>(std::move(settings)))
{
}

AeronClusterSession::~AeronClusterSession() = default; // Impl's destructor closes

ports::Result<ports::SessionInfo> AeronClusterSession::connect(const std::string &ingressEndpoints)
{
    using ResultType = ports::Result<ports::SessionInfo>;
    Impl &impl = *m_impl;

    if (impl.closed)
    {
        return ResultType::failure("session is closed");
    }
    if (impl.connected)
    {
        return ResultType::failure("already connected");
    }
    if (impl.settings.responseChannel.empty())
    {
        return ResultType::failure(
            "responseChannel is required — an egress channel the cluster can route back to "
            "(a real interface address, never 0.0.0.0 or localhost)");
    }

    try
    {
        std::string reason;
        if (!parseIngressEndpoints(ingressEndpoints, impl.members, reason))
        {
            impl.teardown();
            return ResultType::failure("connect: " + reason);
        }

        Context context;
        if (!impl.settings.aeronDir.empty())
        {
            context.aeronDir(impl.settings.aeronDir);
        }
        impl.client = Aeron::connect(context);

        const std::int64_t deadlineMs = nowMs() + impl.settings.connectTimeoutMs;

        const std::int64_t subscriptionRegistrationId = impl.client->addSubscription(
            impl.settings.responseChannel, impl.settings.egressStreamId);
        std::vector<std::int64_t> publicationRegistrationIds;
        publicationRegistrationIds.reserve(impl.members.size());
        for (const Member &member : impl.members)
        {
            publicationRegistrationIds.push_back(impl.client->addPublication(
                "aeron:udp?endpoint=" + member.endpoint, impl.settings.ingressStreamId));
        }
        if (!impl.awaitRegistration(deadlineMs, subscriptionRegistrationId, publicationRegistrationIds, reason))
        {
            impl.teardown();
            return ResultType::failure("connect: " + reason);
        }

        // Retry SessionConnectRequest until the leader's OK arrives. Followers answer
        // REDIRECT(leaderMemberId), narrowing the next round to that member.
        char requestBuffer[512];
        std::int64_t lastSendMs = 0;
        std::int64_t correlationSequence = nowMs() * 1000;

        while (!impl.connected)
        {
            if (impl.connectRejected)
            {
                const std::string detail = impl.connectRejectDetail;
                impl.teardown();
                return ResultType::failure(
                    "connect: refused by cluster (ERROR/AUTHENTICATION_REJECTED): " + detail);
            }
            if (nowMs() > deadlineMs)
            {
                impl.teardown();
                return ResultType::failure(
                    "connect: no SessionEvent OK within " +
                    std::to_string(impl.settings.connectTimeoutMs) + "ms");
            }

            if (nowMs() - lastSendMs > impl.settings.connectRetryIntervalMs)
            {
                for (std::size_t i = 0; i < impl.members.size(); i++)
                {
                    if (impl.redirectToMemberId >= 0 && impl.members[i].id != impl.redirectToMemberId)
                    {
                        continue;
                    }
                    if (!impl.members[i].publication->isConnected())
                    {
                        continue;
                    }
                    const std::int64_t correlationId = ++correlationSequence;
                    cc::SessionConnectRequest request;
                    request.wrapAndApplyHeader(requestBuffer, 0, sizeof(requestBuffer))
                        .correlationId(correlationId)
                        .responseStreamId(impl.settings.egressStreamId)
                        .version(PROTOCOL_SEMANTIC_VERSION);
                    request.putResponseChannel(impl.settings.responseChannel);
                    request.putEncodedCredentials("", 0);
                    request.putClientInfo(impl.settings.clientInfo);
                    const util::index_t requestLength = static_cast<util::index_t>(
                        cc::MessageHeader::encodedLength() + request.encodedLength());

                    AtomicBuffer frame(
                        reinterpret_cast<std::uint8_t *>(requestBuffer),
                        static_cast<std::size_t>(requestLength));
                    if (impl.members[i].publication->offer(frame, 0, requestLength) > 0)
                    {
                        impl.outstandingConnects[correlationId] = i;
                    }
                }
                lastSendMs = nowMs();
            }

            impl.egressSubscription->poll(impl.assemblingHandler, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // The session must ride the publication that carried the winning connect.
        impl.sessionPublication = impl.members[impl.connectedViaMemberIndex].publication;
        for (std::size_t i = 0; i < impl.members.size(); i++)
        {
            if (static_cast<int>(i) != impl.connectedViaMemberIndex)
            {
                impl.members[i].publication.reset(); // release the losers' publications
            }
        }
        impl.outstandingConnects.clear();
        impl.encodeKeepAlive();
        impl.lastKeepAliveMs = nowMs();

        return ResultType::success(impl.info);
    }
    catch (const std::exception &e)
    {
        impl.teardown();
        return ResultType::failure(std::string("connect: ") + e.what());
    }
    catch (...)
    {
        impl.teardown();
        return ResultType::failure("connect: unknown exception");
    }
}

ports::Result<std::int64_t> AeronClusterSession::offer(const std::uint8_t *buffer, std::size_t length)
{
    using ResultType = ports::Result<std::int64_t>;
    Impl &impl = *m_impl;

    if (!impl.connected || impl.closed)
    {
        return ResultType::failure("offer: not connected");
    }
    if (impl.sessionClosedByCluster)
    {
        return ResultType::failure("offer: session closed by cluster");
    }
    if (nullptr == buffer || 0 == length)
    {
        return ResultType::failure("offer: empty payload");
    }

    try
    {
        // MessageHeader(cluster schema) + SessionMessageHeader{term, session, timestamp=0}
        // + app payload — timestamp stays 0 exactly like the Java client's headerBuffer.
        const std::size_t sessionFrameLength =
            cc::MessageHeader::encodedLength() + cc::SessionMessageHeader::sbeBlockLength();
        impl.offerFrame.resize(sessionFrameLength + length);

        cc::SessionMessageHeader smh;
        smh.wrapAndApplyHeader(
                reinterpret_cast<char *>(impl.offerFrame.data()), 0, impl.offerFrame.size())
            .leadershipTermId(impl.info.leadershipTermId)
            .clusterSessionId(impl.info.sessionId)
            .timestamp(0);
        std::memcpy(impl.offerFrame.data() + sessionFrameLength, buffer, length);

        AtomicBuffer frame(impl.offerFrame.data(), impl.offerFrame.size());
        const util::index_t frameLength = static_cast<util::index_t>(impl.offerFrame.size());

        const std::int64_t deadlineMs = nowMs() + impl.settings.offerTimeoutMs;
        while (true)
        {
            const std::int64_t position = impl.sessionPublication->offer(frame, 0, frameLength);
            if (position > 0)
            {
                return ResultType::success(position);
            }
            if (BACK_PRESSURED == position || ADMIN_ACTION == position)
            {
                if (nowMs() > deadlineMs)
                {
                    return ResultType::failure(
                        "offer: still back-pressured after " +
                        std::to_string(impl.settings.offerTimeoutMs) + "ms");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return ResultType::failure(
                std::string("offer: ") + offerErrorName(position) +
                " (" + std::to_string(position) + ")");
        }
    }
    catch (const std::exception &e)
    {
        return ResultType::failure(std::string("offer: ") + e.what());
    }
    catch (...)
    {
        return ResultType::failure("offer: unknown exception");
    }
}

ports::Result<int> AeronClusterSession::poll(const ports::EgressHandler &handler, int fragmentLimit)
{
    using ResultType = ports::Result<int>;
    Impl &impl = *m_impl;

    if (!impl.connected || impl.closed)
    {
        return ResultType::failure("poll: not connected");
    }
    if (impl.sessionClosedByCluster)
    {
        return ResultType::failure("poll: session closed by cluster");
    }

    // Exception-safe disarm: the handler pointer never outlives this call.
    struct HandlerGuard
    {
        Impl &impl;
        ~HandlerGuard() { impl.pollHandler = nullptr; }
    } guard{ impl };
    impl.pollHandler = &handler;

    try
    {
        const int fragments = impl.egressSubscription->poll(impl.assemblingHandler, fragmentLimit);
        impl.offerKeepAliveIfDue();
        if (impl.sessionClosedByCluster)
        {
            return ResultType::failure("poll: session closed by cluster");
        }
        return ResultType::success(fragments);
    }
    catch (const std::exception &e)
    {
        return ResultType::failure(std::string("poll: ") + e.what());
    }
    catch (...)
    {
        return ResultType::failure("poll: unknown exception");
    }
}

void AeronClusterSession::close()
{
    m_impl->close();
}

const ports::SessionInfo &AeronClusterSession::sessionInfo() const
{
    return m_impl->info;
}

}}
