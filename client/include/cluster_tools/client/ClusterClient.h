#ifndef CLUSTER_TOOLS_CLIENT_CLUSTER_CLIENT_H
#define CLUSTER_TOOLS_CLIENT_CLUSTER_CLIENT_H

#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "cluster_tools/client/Backoff.h"
#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/IngressEndpoints.h"
#include "cluster_tools/client/Protocol.h"
#include "cluster_tools/client/Transport.h"

#include "io_aeron_cluster_codecs/Challenge.h"
#include "io_aeron_cluster_codecs/EventCode.h"
#include "io_aeron_cluster_codecs/MessageHeader.h"
#include "io_aeron_cluster_codecs/NewLeaderEvent.h"
#include "io_aeron_cluster_codecs/SessionCloseRequest.h"
#include "io_aeron_cluster_codecs/SessionConnectRequest.h"
#include "io_aeron_cluster_codecs/SessionEvent.h"
#include "io_aeron_cluster_codecs/SessionKeepAlive.h"
#include "io_aeron_cluster_codecs/SessionMessageHeader.h"

// ClusterClient - the production Aeron Cluster session client (RC2 S11, rc2-forwarder.md 2.3).
//
// A non-blocking state machine mirroring io.aeron.cluster.client.AeronCluster 1.53.1
// (AsyncConnect + the CONNECTED / AWAIT_NEW_LEADER_CONNECTION / AWAIT_NEW_LEADER /
// PENDING_CLOSE states), driven entirely by the owner's duty cycle:
//
//   connect()            starts a connect cycle; returns immediately
//   doWork()             advances connect steps, sends keepalives, enforces deadlines
//   poll(handler, n)     drains egress: app messages to the handler, cluster events here
//   tryOffer(bytes, n)   one offer, never retried, never sleeps; the caller owns retry
//
// Nothing sleeps, spins or waits; every call does bounded work and returns. Retries are
// bounded (maxConnectAttempts x messageTimeout with exponential backoff); back-pressure and
// every other offer outcome is returned to the caller as a value.
//
// Session continuity: a NewLeaderEvent re-points ingress at the new leader with the new term
// stamped into every later frame (the consensus module silently drops a frame carrying the old
// term, ConsensusModuleAgent.java:778), and the first positive offer (normally the immediate
// keepalive) links the session to the new leader's ingress image. Offers made in the ended
// term are reported in NewLeaderInfo: they MAY have been dropped, the owner reconciles (the
// forwarder resyncs, the bridge fences). A closed session never comes back: connect() opens
// a NEW one (sessionEpoch increments) and the owner re-registers and resyncs.
//
// Observer (optional, by pointer) receives, on the duty thread:
//   onStateChange(ClientState from, ClientState to)
//   onNewLeader(const NewLeaderInfo &)
//   onSessionEvent(const SessionEventInfo &)   non-OK events for this session
// Each callback is optional (detected with requires-expressions).
//
// Two clusters in one process = two ClusterClient instances over two transports sharing one
// aeron::Aeron (one conductor), each with its own response channel.

namespace cluster_tools { namespace client {

struct EgressMessage
{
    std::int64_t clusterSessionId = -1;
    // The leadership term the service stamped on this frame's SessionMessageHeader (Aeron 1.53.1
    // ClusteredServiceAgent sets it from every NewLeadershipTermEvent and from recovery). RC2 S12 rule S:
    // an owner that frames replies by arrival order drops a frame of another term than leadershipTermId().
    std::int64_t leadershipTermId = -1;
    std::int64_t timestamp = 0;
    const std::uint8_t *payload = nullptr; // engine-schema bytes; valid only during the call
    std::size_t length = 0;
};

struct NewLeaderInfo
{
    std::int64_t clusterSessionId = -1;
    std::int64_t previousLeadershipTermId = -1;
    std::int64_t leadershipTermId = -1;
    std::int32_t leaderMemberId = -1;
    // Positive app offers stamped with the ended term: any of them may have been dropped.
    std::uint64_t offersInEndedTerm = 0;
    // The subset made after the last egress frame of this session arrived (the most suspect).
    std::uint64_t offersAfterLastEgress = 0;
};

struct SessionEventInfo
{
    std::int64_t correlationId = -1;
    std::int64_t clusterSessionId = -1;
    std::int32_t code = -1; // io.aeron.cluster.codecs.EventCode value
    std::string detail;
};

struct ClientCounters
{
    std::uint64_t connectCycles = 0;
    std::uint64_t connectAttempts = 0;
    std::uint64_t connectAttemptsFailed = 0;
    std::uint64_t redirects = 0;
    std::uint64_t sessionsOpened = 0;
    std::uint64_t sessionsClosed = 0;

    std::uint64_t offers = 0;
    std::uint64_t offersBackPressured = 0;
    std::uint64_t offersNotConnected = 0;
    std::uint64_t offersNotReady = 0;
    std::uint64_t offersFailed = 0;

    std::uint64_t keepAlivesSent = 0;
    std::uint64_t keepAliveFailures = 0;

    std::uint64_t newLeaderEvents = 0;
    std::uint64_t egressImageLosses = 0;
    std::uint64_t ingressDisconnects = 0;
    std::uint64_t offersInEndedTerms = 0;
    std::uint64_t possiblyLostOffers = 0;

    std::uint64_t egressMessages = 0;
    std::uint64_t unknownSchemaFragments = 0;
    std::uint64_t foreignSessionFragments = 0;
    std::uint64_t malformedFragments = 0;
};

struct NullObserver
{
};

template<ClusterTransport Transport, typename Observer = NullObserver>
class ClusterClient
{
public:
    static constexpr std::size_t SESSION_HEADER_LENGTH = 32; // MessageHeader 8 + SessionMessageHeader 24
    static constexpr std::size_t KEEPALIVE_LENGTH = 24;      // MessageHeader 8 + SessionKeepAlive 16

    ClusterClient(Transport &transport, ClientSettings settings, Observer *observer = nullptr) :
        m_transport(transport),
        m_settings(std::move(settings)),
        m_observer(observer),
        m_backoff(m_settings.connectBackoffInitialNs, m_settings.connectBackoffMaxNs)
    {
        static_assert(SESSION_HEADER_LENGTH ==
            io::aeron::cluster::codecs::MessageHeader::encodedLength() +
            io::aeron::cluster::codecs::SessionMessageHeader::sbeBlockLength());
        static_assert(KEEPALIVE_LENGTH ==
            io::aeron::cluster::codecs::MessageHeader::encodedLength() +
            io::aeron::cluster::codecs::SessionKeepAlive::sbeBlockLength());
    }

    ~ClusterClient()
    {
        close();
    }

    ClusterClient(const ClusterClient &) = delete;
    ClusterClient &operator=(const ClusterClient &) = delete;

    // Start a connect cycle. False (with lastDetail) if a session or cycle is already active or
    // the settings are invalid (state FAILED / INVALID_SETTINGS). Never blocks.
    bool connect()
    {
        if (isActive())
        {
            m_lastDetail = "connect: already " + std::string(nameOf(m_state));
            return false;
        }
        m_connectError = ConnectError::NONE;
        m_closeReason = CloseReason::NONE;
        if (std::string error = validate(m_settings); !error.empty())
        {
            m_lastDetail = error;
            m_connectError = ConnectError::INVALID_SETTINGS;
            setState(ClientState::FAILED);
            return false;
        }
        const ParsedEndpoints parsed = parseIngressEndpoints(m_settings.ingressEndpoints);
        m_members.clear();
        for (const IngressMember &member : parsed.members)
        {
            m_members.push_back(Member{ member.id, member.endpoint });
        }
        m_sessionId = -1;
        m_term = -1;
        m_leader = -1;
        m_imageBound = false;
        m_attempts = 0;
        m_backoff.reset();
        m_counters.connectCycles++;
        setState(ClientState::CONNECTING);
        startAttempt(m_transport.nanoTime());
        return true;
    }

    // Advance connect, keepalive and deadlines. Returns the amount of work done (0 = idle).
    int doWork()
    {
        const std::int64_t now = m_transport.nanoTime();
        switch (m_state)
        {
            case ClientState::CONNECTING:
                return stepConnect(now);
            case ClientState::CONNECTED:
            case ClientState::AWAIT_NEW_LEADER_CONNECTION:
            {
                int work = sendKeepAliveIfDue(now);
                work += checkDeadlines(now);
                return work;
            }
            case ClientState::AWAIT_NEW_LEADER:
                return checkDeadlines(now);
            default:
                return 0;
        }
    }

    // Drain up to fragmentLimit egress messages. handler(const EgressMessage &) gets the app
    // messages of THIS session; cluster events are handled here. Poll in every live state,
    // including AWAIT_NEW_LEADER: the NewLeaderEvent arrives on egress.
    template<typename Handler>
    int poll(Handler &&handler, int fragmentLimit)
    {
        if (!isSessionLive())
        {
            return 0;
        }
        m_inPoll = true;
        int work = 0;
        try
        {
            work = m_transport.pollEgress(
                [this, &handler](const EgressFragment &fragment) { onSessionFragment(fragment, handler); },
                fragmentLimit);
        }
        catch (...)
        {
            m_inPoll = false;
            processDeferred();
            throw; // only a throwing handler gets here: the owner's bug, not ours to hide
        }
        m_inPoll = false;
        processDeferred();

        if (m_imageBound &&
            (ClientState::CONNECTED == m_state || ClientState::AWAIT_NEW_LEADER_CONNECTION == m_state) &&
            m_transport.isEgressImageClosed(m_egressImageSessionId))
        {
            m_counters.egressImageLosses++;
            onDisconnected(m_transport.nanoTime());
            work++;
        }
        work += checkDeadlines(m_transport.nanoTime());
        return work;
    }

    // One non-blocking offer of an engine-schema message (the client prepends the session
    // header). Never retried here.
    OfferResult tryOffer(const std::uint8_t *payload, std::size_t length)
    {
        if (ClientState::CONNECTED != m_state && ClientState::AWAIT_NEW_LEADER_CONNECTION != m_state)
        {
            m_counters.offersNotReady++;
            return OfferResult{ OfferStatus::NOT_READY, 0 };
        }
        const std::int64_t result = (NO_PUBLICATION == m_publication) ?
            OFFER_NOT_CONNECTED :
            m_transport.offer(m_publication, m_header, SESSION_HEADER_LENGTH, payload, length);
        const OfferResult mapped = mapOffer(result);
        if (mapped.ok())
        {
            m_counters.offers++;
            m_offersInTerm++;
            m_offersAfterEgress++;
        }
        trackIngressResult(result, m_transport.nanoTime());
        return mapped;
    }

    // Polite close (SessionCloseRequest, best effort) and release. Idempotent. Aborts a
    // connect cycle too.
    void close()
    {
        if (ClientState::IDLE == m_state || ClientState::CLOSED == m_state || ClientState::FAILED == m_state)
        {
            return;
        }
        if ((ClientState::CONNECTED == m_state || ClientState::AWAIT_NEW_LEADER_CONNECTION == m_state) &&
            NO_PUBLICATION != m_publication)
        {
            std::uint8_t frame[KEEPALIVE_LENGTH] = {};
            io::aeron::cluster::codecs::SessionCloseRequest request;
            request.wrapAndApplyHeader(reinterpret_cast<char *>(frame), 0, sizeof(frame))
                .leadershipTermId(m_term)
                .clusterSessionId(m_sessionId);
            m_transport.offer(m_publication, frame, sizeof(frame), nullptr, 0);
        }
        closeSession(CloseReason::CLIENT_CLOSE);
    }

    ClientState state() const { return m_state; }
    bool isConnected() const { return ClientState::CONNECTED == m_state; }
    CloseReason closeReason() const { return m_closeReason; }
    ConnectError connectError() const { return m_connectError; }
    const std::string &lastDetail() const { return m_lastDetail; }
    std::int64_t clusterSessionId() const { return m_sessionId; }
    std::int64_t leadershipTermId() const { return m_term; }
    std::int32_t leaderMemberId() const { return m_leader; }
    std::uint64_t sessionEpoch() const { return m_sessionEpoch; }
    std::int64_t leaderHeartbeatTimeoutNs() const { return m_leaderHeartbeatTimeoutNs; }
    std::int64_t newLeaderTimeoutNs() const { return m_newLeaderTimeoutNs; }
    const ClientCounters &counters() const { return m_counters; }
    const ClientSettings &settings() const { return m_settings; }
    // The transport's clock: everything layered on this client (registrar, owners' deadlines)
    // should read time here so the deadlines cannot diverge.
    std::int64_t nanoTime() const { return m_transport.nanoTime(); }

private:
    enum class Step : std::uint8_t
    {
        NONE,
        BACKOFF,
        CREATE_EGRESS,
        AWAIT_EGRESS,
        RESOLVE_EGRESS,
        CREATE_INGRESS,
        AWAIT_PUBLICATION,
        SEND_REQUEST,
        AWAIT_RESPONSE,
    };

    struct Member
    {
        std::int32_t id = -1;
        std::string endpoint;
        PublicationId publication = NO_PUBLICATION;
        bool failed = false;
    };

    // Outcome of the connect response, recorded inside the egress callback and applied after
    // the poll returns (closing the subscription from inside its own poll is not allowed).
    struct ConnectOutcome
    {
        enum class Kind : std::uint8_t { NONE, OK, REDIRECT, FAIL } kind = Kind::NONE;
        ConnectError error = ConnectError::NONE;
        std::int32_t leader = -1;
        std::string detail;
    };

    bool isActive() const
    {
        return ClientState::CONNECTING == m_state || isSessionLive();
    }

    bool isSessionLive() const
    {
        return ClientState::CONNECTED == m_state ||
            ClientState::AWAIT_NEW_LEADER_CONNECTION == m_state ||
            ClientState::AWAIT_NEW_LEADER == m_state;
    }

    void setState(ClientState to)
    {
        const ClientState from = m_state;
        m_state = to;
        if (from != to && nullptr != m_observer)
        {
            if constexpr (requires(Observer &o) { o.onStateChange(from, to); })
            {
                m_observer->onStateChange(from, to);
            }
        }
    }

    // ---------------------------------------------------------------- connect

    void startAttempt(std::int64_t now)
    {
        m_counters.connectAttempts++;
        m_attemptDeadlineNs = now + m_settings.messageTimeoutNs;
        m_chosen = -1;
        m_redirected = false;
        m_outcome = ConnectOutcome{};
        m_step = m_subscriptionActive ? Step::RESOLVE_EGRESS : Step::CREATE_EGRESS;
    }

    int stepConnect(std::int64_t now)
    {
        if (Step::BACKOFF == m_step)
        {
            if (now < m_nextAttemptNs)
            {
                return 0;
            }
            startAttempt(now);
            return 1;
        }
        if (now - m_attemptDeadlineNs >= 0)
        {
            failAttempt(now, "connect attempt timed out in step " + std::string(stepName(m_step)) +
                (m_transport.lastError().empty() ? "" : " (" + std::string(m_transport.lastError()) + ")"));
            return 1;
        }

        switch (m_step)
        {
            case Step::CREATE_EGRESS:
                if (!m_transport.addEgressSubscription(m_settings.responseChannel, m_settings.egressStreamId))
                {
                    failAttempt(now, "egress subscription refused: " + std::string(m_transport.lastError()));
                    return 1;
                }
                m_subscriptionActive = true;
                m_step = Step::AWAIT_EGRESS;
                return 1;

            case Step::AWAIT_EGRESS:
                switch (m_transport.egressSubscriptionStatus())
                {
                    case Registration::READY:
                        m_step = Step::RESOLVE_EGRESS;
                        return 1;
                    case Registration::FAILED:
                        m_transport.closeEgressSubscription();
                        m_subscriptionActive = false;
                        failAttempt(now, "egress subscription failed: " + std::string(m_transport.lastError()));
                        return 1;
                    default:
                        return 0;
                }

            case Step::RESOLVE_EGRESS:
            {
                std::string resolved = m_transport.resolvedEgressChannel();
                if (resolved.empty())
                {
                    return 0;
                }
                m_resolvedResponseChannel = std::move(resolved);
                m_step = Step::CREATE_INGRESS;
                return 1;
            }

            case Step::CREATE_INGRESS:
                for (Member &member : m_members)
                {
                    member.failed = false;
                    if (NO_PUBLICATION == member.publication)
                    {
                        member.publication = m_transport.addIngressPublication(
                            ingressChannelFor(m_settings.ingressChannel, member.endpoint), m_settings.ingressStreamId);
                        if (NO_PUBLICATION == member.publication)
                        {
                            member.failed = true;
                        }
                    }
                }
                m_step = Step::AWAIT_PUBLICATION;
                return 1;

            case Step::AWAIT_PUBLICATION:
                return awaitPublication(now);

            case Step::SEND_REQUEST:
            {
                const PublicationId publication = m_members[static_cast<std::size_t>(m_chosen)].publication;
                const std::int64_t result = m_transport.offer(publication, m_request.data(), m_requestLength, nullptr, 0);
                if (result > 0)
                {
                    m_step = Step::AWAIT_RESPONSE;
                    return 1;
                }
                if (OFFER_NOT_CONNECTED == result || OFFER_PUBLICATION_CLOSED == result ||
                    OFFER_MAX_POSITION_EXCEEDED == result || OFFER_TRANSPORT_ERROR == result)
                {
                    failAttempt(now, "connect request offer failed (" + std::to_string(result) + ")");
                    return 1;
                }
                return 0; // back-pressured: retry on the next doWork
            }

            case Step::AWAIT_RESPONSE:
                return awaitResponse(now);

            default:
                return 0;
        }
    }

    int awaitPublication(std::int64_t now)
    {
        bool anyAlive = false;
        for (std::size_t i = 0; i < m_members.size(); i++)
        {
            Member &member = m_members[i];
            if (m_redirected && static_cast<int>(i) != m_chosen)
            {
                continue;
            }
            if (member.failed || NO_PUBLICATION == member.publication)
            {
                continue;
            }
            const Registration status = m_transport.publicationStatus(member.publication);
            if (Registration::FAILED == status)
            {
                member.failed = true;
                continue;
            }
            anyAlive = true;
            if (Registration::READY == status && m_transport.isPublicationConnected(member.publication))
            {
                m_chosen = static_cast<int>(i);
                if (!encodeConnectRequest())
                {
                    return 1;
                }
                m_step = Step::SEND_REQUEST;
                return 1;
            }
        }
        if (!anyAlive)
        {
            failAttempt(now, "no ingress publication could be added: " + std::string(m_transport.lastError()));
            return 1;
        }
        return 0;
    }

    bool encodeConnectRequest()
    {
        namespace cc = io::aeron::cluster::codecs;
        m_correlationId = m_transport.nextCorrelationId();
        m_request.assign(m_settings.connectRequestCapacity, 0);
        try
        {
            cc::SessionConnectRequest request;
            request.wrapAndApplyHeader(reinterpret_cast<char *>(m_request.data()), 0, m_request.size())
                .correlationId(m_correlationId)
                .responseStreamId(m_settings.egressStreamId)
                .version(PROTOCOL_SEMANTIC_VERSION);
            request.putResponseChannel(m_resolvedResponseChannel);
            request.putEncodedCredentials(
                reinterpret_cast<const char *>(m_settings.credentials.data()),
                static_cast<std::uint32_t>(m_settings.credentials.size()));
            request.putClientInfo("name=" + m_settings.clientName + " cluster-tools-client");
            m_requestLength = cc::MessageHeader::encodedLength() + request.encodedLength();
            return true;
        }
        catch (const std::exception &e)
        {
            finishFailed(ConnectError::INVALID_SETTINGS,
                std::string("connect request does not fit connectRequestCapacity: ") + e.what());
            return false;
        }
    }

    int awaitResponse(std::int64_t now)
    {
        // One fragment per poll: the OK must be applied before any later fragment of the same
        // batch is looked at (AeronCluster's EgressPoller breaks on the match for the same reason).
        int work = 0;
        for (int i = 0; i < 16 && Step::AWAIT_RESPONSE == m_step && ConnectOutcome::Kind::NONE == m_outcome.kind; i++)
        {
            m_inPoll = true;
            const int fragments = m_transport.pollEgress(
                [this](const EgressFragment &fragment) { onConnectFragment(fragment); }, 1);
            m_inPoll = false;
            if (0 == fragments)
            {
                break;
            }
            work += fragments;
        }
        processDeferred();
        applyConnectOutcome(now);
        return work;
    }

    void onConnectFragment(const EgressFragment &fragment)
    {
        namespace cc = io::aeron::cluster::codecs;
        if (ConnectOutcome::Kind::NONE != m_outcome.kind)
        {
            return;
        }
        try
        {
            char *buffer = reinterpret_cast<char *>(const_cast<std::uint8_t *>(fragment.data));
            const std::uint64_t capacity = fragment.length;
            cc::MessageHeader header(buffer, 0, capacity, 0);
            if (header.schemaId() != cc::MessageHeader::sbeSchemaId())
            {
                return;
            }
            const std::uint64_t body = cc::MessageHeader::encodedLength();
            if (header.templateId() == cc::SessionEvent::sbeTemplateId())
            {
                cc::SessionEvent event(buffer, body, capacity, header.blockLength(), header.version());
                if (event.correlationId() != m_correlationId)
                {
                    return;
                }
                switch (event.code())
                {
                    case cc::EventCode::OK:
                    {
                        m_outcome.kind = ConnectOutcome::Kind::OK;
                        m_sessionId = event.clusterSessionId();
                        m_term = event.leadershipTermId();
                        m_leader = event.leaderMemberId();
                        const std::int64_t heartbeat = event.leaderHeartbeatTimeoutNs();
                        m_leaderHeartbeatTimeoutNs =
                            // A wire value is not trusted for deadline arithmetic: absent,
                            // non-positive or beyond MAX_TIMEOUT_NS / 2 (it is doubled) = default.
                            (heartbeat == cc::SessionEvent::leaderHeartbeatTimeoutNsNullValue() || heartbeat <= 0 ||
                                heartbeat > MAX_TIMEOUT_NS / 2) ?
                            LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS : heartbeat;
                        m_egressImageSessionId = fragment.imageSessionId;
                        m_imageBound = true;
                        break;
                    }
                    case cc::EventCode::REDIRECT:
                        m_outcome.kind = ConnectOutcome::Kind::REDIRECT;
                        m_outcome.leader = event.leaderMemberId();
                        m_outcome.detail = event.getDetailAsString();
                        break;
                    case cc::EventCode::AUTHENTICATION_REJECTED:
                        m_outcome.kind = ConnectOutcome::Kind::FAIL;
                        m_outcome.error = ConnectError::AUTHENTICATION_REJECTED;
                        m_outcome.detail = event.getDetailAsString();
                        break;
                    case cc::EventCode::ERROR:
                        m_outcome.kind = ConnectOutcome::Kind::FAIL;
                        m_outcome.error = ConnectError::CLUSTER_ERROR;
                        m_outcome.detail = event.getDetailAsString();
                        break;
                    default:
                        break; // CLOSED / NULL_VAL for a connect: ignored, as AeronCluster does
                }
            }
            else if (header.templateId() == cc::Challenge::sbeTemplateId())
            {
                cc::Challenge challenge(buffer, body, capacity, header.blockLength(), header.version());
                if (challenge.correlationId() == m_correlationId)
                {
                    m_outcome.kind = ConnectOutcome::Kind::FAIL;
                    m_outcome.error = ConnectError::CHALLENGE_UNSUPPORTED;
                    m_outcome.detail = "the cluster challenged; v1 credentials do not answer challenges";
                }
            }
        }
        catch (const std::exception &)
        {
            m_counters.malformedFragments++;
        }
    }

    void applyConnectOutcome(std::int64_t now)
    {
        ConnectOutcome outcome = std::move(m_outcome);
        m_outcome = ConnectOutcome{};
        switch (outcome.kind)
        {
            case ConnectOutcome::Kind::OK:
                concludeConnect(now);
                break;
            case ConnectOutcome::Kind::REDIRECT:
                onRedirect(now, outcome.leader, outcome.detail);
                break;
            case ConnectOutcome::Kind::FAIL:
                finishFailed(outcome.error, outcome.detail);
                break;
            default:
                break;
        }
    }

    void onRedirect(std::int64_t now, std::int32_t leader, const std::string &detail)
    {
        m_counters.redirects++;
        const ParsedEndpoints parsed = parseIngressEndpoints(detail);
        const IngressMember *target = parsed.ok() ? parsed.find(leader) : nullptr;
        if (nullptr == target)
        {
            failAttempt(now, "REDIRECT to member " + std::to_string(leader) + " with unusable endpoints '" + detail + "'");
            return;
        }
        // Keep the old publication to the leader if it points at the same endpoint (AeronCluster
        // updateMembers), close everything else, then talk only to the leader.
        PublicationId reuse = NO_PUBLICATION;
        for (Member &member : m_members)
        {
            if (member.id == leader && member.endpoint == target->endpoint && !member.failed)
            {
                reuse = member.publication;
                member.publication = NO_PUBLICATION;
            }
        }
        closeMemberPublications();
        m_members.clear();
        for (const IngressMember &member : parsed.members)
        {
            m_members.push_back(Member{ member.id, member.endpoint });
        }
        for (std::size_t i = 0; i < m_members.size(); i++)
        {
            if (m_members[i].id == leader)
            {
                m_chosen = static_cast<int>(i);
                m_members[i].publication = (NO_PUBLICATION != reuse) ? reuse :
                    m_transport.addIngressPublication(
                        ingressChannelFor(m_settings.ingressChannel, m_members[i].endpoint), m_settings.ingressStreamId);
                m_members[i].failed = NO_PUBLICATION == m_members[i].publication;
            }
        }
        m_redirected = true;
        m_step = Step::AWAIT_PUBLICATION;
    }

    void concludeConnect(std::int64_t now)
    {
        m_publication = m_members[static_cast<std::size_t>(m_chosen)].publication;
        m_members[static_cast<std::size_t>(m_chosen)].publication = NO_PUBLICATION;
        closeMemberPublications();
        m_newLeaderTimeoutNs = m_settings.newLeaderTimeoutNs > 0 ?
            m_settings.newLeaderTimeoutNs : 2 * m_leaderHeartbeatTimeoutNs;
        encodeSessionFrames();
        m_offersInTerm = 0;
        m_offersAfterEgress = 0;
        m_nextKeepAliveNs = now + m_settings.keepAliveIntervalNs;
        m_step = Step::NONE;
        m_sessionEpoch++;
        m_counters.sessionsOpened++;
        m_lastDetail.clear();
        setState(ClientState::CONNECTED);
    }

    void failAttempt(std::int64_t now, std::string reason)
    {
        m_counters.connectAttemptsFailed++;
        m_attempts++;
        m_lastDetail = std::move(reason);
        closeMemberPublications();
        if (m_attempts >= m_settings.maxConnectAttempts)
        {
            finishFailed(ConnectError::ATTEMPTS_EXHAUSTED,
                m_lastDetail + " [attempt " + std::to_string(m_attempts) + "/" +
                std::to_string(m_settings.maxConnectAttempts) + "]");
            return;
        }
        m_step = Step::BACKOFF;
        m_nextAttemptNs = now + m_backoff.next();
    }

    void finishFailed(ConnectError error, std::string detail)
    {
        m_connectError = error;
        m_lastDetail = std::move(detail);
        m_step = Step::NONE;
        releaseAll();
        setState(ClientState::FAILED);
    }

    static const char *stepName(Step step)
    {
        switch (step)
        {
            case Step::NONE: return "NONE";
            case Step::BACKOFF: return "BACKOFF";
            case Step::CREATE_EGRESS: return "CREATE_EGRESS";
            case Step::AWAIT_EGRESS: return "AWAIT_EGRESS";
            case Step::RESOLVE_EGRESS: return "RESOLVE_EGRESS";
            case Step::CREATE_INGRESS: return "CREATE_INGRESS";
            case Step::AWAIT_PUBLICATION: return "AWAIT_PUBLICATION";
            case Step::SEND_REQUEST: return "SEND_REQUEST";
            case Step::AWAIT_RESPONSE: return "AWAIT_RESPONSE";
        }
        return "?";
    }

    // ---------------------------------------------------------------- session

    void encodeSessionFrames()
    {
        namespace cc = io::aeron::cluster::codecs;
        cc::SessionMessageHeader header;
        header.wrapAndApplyHeader(reinterpret_cast<char *>(m_header), 0, sizeof(m_header))
            .leadershipTermId(m_term)
            .clusterSessionId(m_sessionId)
            .timestamp(0);
        cc::SessionKeepAlive keepAlive;
        keepAlive.wrapAndApplyHeader(reinterpret_cast<char *>(m_keepAlive), 0, sizeof(m_keepAlive))
            .leadershipTermId(m_term)
            .clusterSessionId(m_sessionId);
    }

    template<typename Handler>
    void onSessionFragment(const EgressFragment &fragment, Handler &handler)
    {
        namespace cc = io::aeron::cluster::codecs;
        if (!isSessionLive())
        {
            return; // closed earlier in this batch
        }
        try
        {
            char *buffer = reinterpret_cast<char *>(const_cast<std::uint8_t *>(fragment.data));
            const std::uint64_t capacity = fragment.length;
            cc::MessageHeader header(buffer, 0, capacity, 0);
            if (header.schemaId() != cc::MessageHeader::sbeSchemaId())
            {
                m_counters.unknownSchemaFragments++;
                return;
            }
            const std::uint64_t body = cc::MessageHeader::encodedLength();
            const std::uint16_t templateId = header.templateId();
            if (templateId == cc::SessionMessageHeader::sbeTemplateId())
            {
                cc::SessionMessageHeader smh(buffer, body, capacity, header.blockLength(), header.version());
                if (smh.clusterSessionId() != m_sessionId)
                {
                    m_counters.foreignSessionFragments++;
                    return;
                }
                const std::uint64_t payloadOffset = body + header.blockLength();
                if (payloadOffset > capacity)
                {
                    m_counters.malformedFragments++;
                    return;
                }
                m_offersAfterEgress = 0;
                m_counters.egressMessages++;
                EgressMessage message;
                message.clusterSessionId = m_sessionId;
                message.leadershipTermId = smh.leadershipTermId();
                message.timestamp = smh.timestamp();
                message.payload = fragment.data + payloadOffset;
                message.length = static_cast<std::size_t>(capacity - payloadOffset);
                handler(message);
            }
            else if (templateId == cc::SessionEvent::sbeTemplateId())
            {
                cc::SessionEvent event(buffer, body, capacity, header.blockLength(), header.version());
                if (event.clusterSessionId() != m_sessionId)
                {
                    return;
                }
                m_offersAfterEgress = 0;
                SessionEventInfo info;
                info.correlationId = event.correlationId();
                info.clusterSessionId = event.clusterSessionId();
                info.code = static_cast<std::int32_t>(event.codeRaw());
                info.detail = event.getDetailAsString();
                if (cc::EventCode::CLOSED == event.code())
                {
                    m_deferredClose = CloseReason::SESSION_CLOSED_BY_CLUSTER;
                    m_lastDetail = "session closed by cluster: " + info.detail;
                }
                if (nullptr != m_observer)
                {
                    if constexpr (requires(Observer &o, const SessionEventInfo &i) { o.onSessionEvent(i); })
                    {
                        m_observer->onSessionEvent(info);
                    }
                }
            }
            else if (templateId == cc::NewLeaderEvent::sbeTemplateId())
            {
                cc::NewLeaderEvent event(buffer, body, capacity, header.blockLength(), header.version());
                if (event.clusterSessionId() != m_sessionId)
                {
                    return;
                }
                const std::int64_t term = event.leadershipTermId();
                const std::int32_t leader = event.leaderMemberId();
                m_egressImageSessionId = fragment.imageSessionId;
                m_imageBound = true;
                onNewLeader(term, leader, event.getIngressEndpointsAsString());
            }
        }
        catch (const std::exception &)
        {
            m_counters.malformedFragments++;
        }
    }

    void onNewLeader(std::int64_t term, std::int32_t leader, const std::string &endpoints)
    {
        const std::int64_t now = m_transport.nanoTime();
        NewLeaderInfo info;
        info.clusterSessionId = m_sessionId;
        info.previousLeadershipTermId = m_term;
        info.leadershipTermId = term;
        info.leaderMemberId = leader;
        info.offersInEndedTerm = m_offersInTerm;
        info.offersAfterLastEgress = m_offersAfterEgress;
        m_counters.newLeaderEvents++;
        m_counters.offersInEndedTerms += m_offersInTerm;
        m_counters.possiblyLostOffers += m_offersAfterEgress;

        setState(ClientState::AWAIT_NEW_LEADER_CONNECTION);
        m_stateDeadlineNs = now + m_settings.messageTimeoutNs;
        m_term = term;
        m_leader = leader;
        encodeSessionFrames();

        if (NO_PUBLICATION != m_publication)
        {
            m_transport.closePublication(m_publication);
            m_publication = NO_PUBLICATION;
        }
        const ParsedEndpoints parsed = parseIngressEndpoints(endpoints);
        const IngressMember *target = parsed.ok() ? parsed.find(leader) : nullptr;
        if (nullptr == target)
        {
            m_lastDetail = "NewLeaderEvent for member " + std::to_string(leader) +
                " with unusable endpoints '" + endpoints + "'";
            m_deferredClose = CloseReason::TRANSPORT_ERROR;
        }
        else
        {
            m_publication = m_transport.addIngressPublication(
                ingressChannelFor(m_settings.ingressChannel, target->endpoint), m_settings.ingressStreamId);
            m_members.clear();
            for (const IngressMember &member : parsed.members)
            {
                m_members.push_back(Member{ member.id, member.endpoint });
            }
        }
        // Drop partial reassemblies NOW, at the event, as AeronCluster.onNewLeader does: fragments
        // that follow the event in this same poll (the new leader's next message) must assemble
        // normally. A reset after the poll would discard a message whose BEGIN is already in.
        m_transport.resetEgressAssembly();
        m_offersInTerm = 0;
        m_offersAfterEgress = 0;
        m_nextKeepAliveNs = now; // the first positive offer links the session to the new leader

        if (nullptr != m_observer)
        {
            if constexpr (requires(Observer &o, const NewLeaderInfo &i) { o.onNewLeader(i); })
            {
                m_observer->onNewLeader(info);
            }
        }
    }

    void onDisconnected(std::int64_t now)
    {
        if (NO_PUBLICATION != m_publication)
        {
            m_transport.closePublication(m_publication);
            m_publication = NO_PUBLICATION;
        }
        setState(ClientState::AWAIT_NEW_LEADER);
        m_stateDeadlineNs = now + m_newLeaderTimeoutNs;
    }

    void trackIngressResult(std::int64_t result, std::int64_t now)
    {
        if (ClientState::CONNECTED == m_state)
        {
            if (OFFER_NOT_CONNECTED == result || OFFER_PUBLICATION_CLOSED == result)
            {
                m_counters.ingressDisconnects++;
                onDisconnected(now);
            }
            else if (OFFER_MAX_POSITION_EXCEEDED == result)
            {
                m_lastDetail = "ingress publication reached its max position";
                closeSession(CloseReason::MAX_POSITION_EXCEEDED);
            }
        }
        else if (ClientState::AWAIT_NEW_LEADER_CONNECTION == m_state)
        {
            if (result > 0)
            {
                setState(ClientState::CONNECTED);
            }
        }
    }

    OfferResult mapOffer(std::int64_t result)
    {
        if (result > 0)
        {
            return OfferResult{ OfferStatus::OK, result };
        }
        if (OFFER_BACK_PRESSURED == result || OFFER_ADMIN_ACTION == result)
        {
            m_counters.offersBackPressured++;
            return OfferResult{ OfferStatus::BACK_PRESSURED, 0 };
        }
        if (OFFER_NOT_CONNECTED == result)
        {
            m_counters.offersNotConnected++;
            return OfferResult{ OfferStatus::NOT_CONNECTED, 0 };
        }
        m_counters.offersFailed++;
        if (OFFER_PUBLICATION_CLOSED == result)
        {
            return OfferResult{ OfferStatus::CLOSED, 0 };
        }
        if (OFFER_MAX_POSITION_EXCEEDED == result)
        {
            return OfferResult{ OfferStatus::MAX_POSITION_EXCEEDED, 0 };
        }
        return OfferResult{ OfferStatus::TRANSPORT_ERROR, 0 };
    }

    int sendKeepAliveIfDue(std::int64_t now)
    {
        if (m_settings.disableKeepAliveForTest || now < m_nextKeepAliveNs)
        {
            return 0;
        }
        const std::int64_t result = (NO_PUBLICATION == m_publication) ?
            OFFER_NOT_CONNECTED : m_transport.offer(m_publication, m_keepAlive, KEEPALIVE_LENGTH, nullptr, 0);
        if (result > 0)
        {
            m_counters.keepAlivesSent++;
            m_nextKeepAliveNs = now + m_settings.keepAliveIntervalNs; // advances only on success
        }
        else
        {
            m_counters.keepAliveFailures++;
            m_nextKeepAliveNs = now + m_settings.keepAliveRetryIntervalNs;
        }
        trackIngressResult(result, now);
        return 1;
    }

    int checkDeadlines(std::int64_t now)
    {
        if (ClientState::AWAIT_NEW_LEADER_CONNECTION == m_state && NO_PUBLICATION != m_publication &&
            Registration::FAILED == m_transport.publicationStatus(m_publication))
        {
            m_lastDetail = "new leader ingress publication failed: " + std::string(m_transport.lastError());
            closeSession(CloseReason::TRANSPORT_ERROR);
            return 1;
        }
        if ((ClientState::AWAIT_NEW_LEADER == m_state || ClientState::AWAIT_NEW_LEADER_CONNECTION == m_state) &&
            now - m_stateDeadlineNs >= 0)
        {
            const bool waitingForLeader = ClientState::AWAIT_NEW_LEADER == m_state;
            m_lastDetail = waitingForLeader ?
                "no NewLeaderEvent within " + std::to_string(m_newLeaderTimeoutNs / 1'000'000) + " ms" :
                "no positive offer to the new leader within " + std::to_string(m_settings.messageTimeoutNs / 1'000'000) + " ms";
            closeSession(waitingForLeader ? CloseReason::NEW_LEADER_TIMEOUT : CloseReason::NEW_LEADER_CONNECTION_TIMEOUT);
            return 1;
        }
        return 0;
    }

    void closeSession(CloseReason reason)
    {
        if (m_inPoll)
        {
            if (CloseReason::NONE == m_deferredClose)
            {
                m_deferredClose = reason;
            }
            return;
        }
        if (!isActive())
        {
            return; // already closed or failed
        }
        m_closeReason = reason;
        m_step = Step::NONE;
        releaseAll();
        m_counters.sessionsClosed++;
        setState(ClientState::CLOSED);
    }

    void processDeferred()
    {
        if (CloseReason::NONE != m_deferredClose)
        {
            const CloseReason reason = m_deferredClose;
            m_deferredClose = CloseReason::NONE;
            closeSession(reason);
        }
        if (m_deferredRelease)
        {
            m_deferredRelease = false;
            releaseAll();
        }
    }

    void closeMemberPublications()
    {
        for (Member &member : m_members)
        {
            if (NO_PUBLICATION != member.publication)
            {
                m_transport.closePublication(member.publication);
                member.publication = NO_PUBLICATION;
            }
        }
    }

    void releaseAll()
    {
        if (m_inPoll)
        {
            m_deferredRelease = true;
            return;
        }
        closeMemberPublications();
        if (NO_PUBLICATION != m_publication)
        {
            m_transport.closePublication(m_publication);
            m_publication = NO_PUBLICATION;
        }
        if (m_subscriptionActive)
        {
            m_transport.closeEgressSubscription();
            m_subscriptionActive = false;
        }
        m_imageBound = false;
    }

    Transport &m_transport;
    ClientSettings m_settings;
    Observer *m_observer;

    ClientState m_state = ClientState::IDLE;
    CloseReason m_closeReason = CloseReason::NONE;
    ConnectError m_connectError = ConnectError::NONE;
    std::string m_lastDetail;
    ClientCounters m_counters;

    // connect
    Step m_step = Step::NONE;
    std::vector<Member> m_members;
    int m_chosen = -1;
    bool m_redirected = false;
    bool m_subscriptionActive = false;
    std::string m_resolvedResponseChannel;
    std::vector<std::uint8_t> m_request;
    std::size_t m_requestLength = 0;
    std::int64_t m_correlationId = -1;
    std::int64_t m_attemptDeadlineNs = 0;
    std::int64_t m_nextAttemptNs = 0;
    int m_attempts = 0;
    ExponentialBackoff m_backoff;
    ConnectOutcome m_outcome;

    // session
    std::int64_t m_sessionId = -1;
    std::int64_t m_term = -1;
    std::int32_t m_leader = -1;
    std::uint64_t m_sessionEpoch = 0;
    PublicationId m_publication = NO_PUBLICATION;
    std::int32_t m_egressImageSessionId = 0;
    bool m_imageBound = false;
    std::int64_t m_leaderHeartbeatTimeoutNs = LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS;
    std::int64_t m_newLeaderTimeoutNs = 2 * LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS;
    std::int64_t m_stateDeadlineNs = 0;
    std::int64_t m_nextKeepAliveNs = 0;
    std::uint64_t m_offersInTerm = 0;
    std::uint64_t m_offersAfterEgress = 0;
    std::uint8_t m_header[SESSION_HEADER_LENGTH] = {};
    std::uint8_t m_keepAlive[KEEPALIVE_LENGTH] = {};

    // deferred out of the egress callback
    bool m_inPoll = false;
    bool m_deferredRelease = false;
    CloseReason m_deferredClose = CloseReason::NONE;
};

}}

#endif
