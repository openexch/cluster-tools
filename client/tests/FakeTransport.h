#ifndef CLUSTER_TOOLS_CLIENT_TESTS_FAKE_TRANSPORT_H
#define CLUSTER_TOOLS_CLIENT_TESTS_FAKE_TRANSPORT_H

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "cluster_tools/client/Protocol.h"
#include "cluster_tools/client/Transport.h"

#include "io_aeron_cluster_codecs/MessageHeader.h"
#include "io_aeron_cluster_codecs/NewLeaderEvent.h"
#include "io_aeron_cluster_codecs/SessionConnectRequest.h"
#include "io_aeron_cluster_codecs/SessionEvent.h"
#include "io_aeron_cluster_codecs/SessionKeepAlive.h"
#include "io_aeron_cluster_codecs/SessionMessageHeader.h"
#include "io_aeron_cluster_codecs/Challenge.h"

// A scripted in-memory transport: a manual clock, publications whose registration,
// connectivity and next offer results the test sets, an egress queue the test fills with
// encoded cluster frames, and a record of every offered frame. Satisfies ClusterTransport.

namespace cluster_tools { namespace client { namespace test {

namespace cc = io::aeron::cluster::codecs;

struct FakePublication
{
    std::string channel;
    std::int32_t streamId = 0;
    Registration status = Registration::PENDING;
    bool connected = false;
    bool closed = false;
    std::deque<std::int64_t> scriptedResults; // consumed one per offer; empty = success
    std::vector<std::vector<std::uint8_t>> offered;
    std::int64_t position = 0;
};

struct QueuedFragment
{
    std::vector<std::uint8_t> bytes;
    std::int32_t imageSessionId = 0;
};

class FakeTransport
{
public:
    // ---- the ClusterTransport surface
    std::int64_t nanoTime() const { return now; }
    std::int64_t nextCorrelationId() { return ++correlation; }

    bool addEgressSubscription(const std::string &channel, std::int32_t streamId)
    {
        subscriptionAdds++;
        if (refuseSubscription)
        {
            error = "refused";
            return false;
        }
        egressChannel = channel;
        egressStreamId = streamId;
        subscriptionOpen = true;
        return true;
    }
    Registration egressSubscriptionStatus() const { return subscriptionStatus; }
    std::string resolvedEgressChannel() const { return subscriptionOpen ? resolvedChannel : std::string(); }
    void closeEgressSubscription()
    {
        subscriptionOpen = false;
        subscriptionCloses++;
        if (inPoll)
        {
            closedSubscriptionInsidePoll = true;
        }
    }

    template<typename Handler>
    int pollEgress(Handler &&handler, int fragmentLimit)
    {
        pollCalls++;
        int count = 0;
        inPoll = true;
        while (subscriptionOpen && count < fragmentLimit && !egress.empty())
        {
            QueuedFragment fragment = std::move(egress.front());
            egress.pop_front();
            handler(EgressFragment{ fragment.bytes.data(), fragment.bytes.size(), fragment.imageSessionId });
            count++;
        }
        inPoll = false;
        return count;
    }
    void resetEgressAssembly()
    {
        assemblyResets++;
        resetsInsidePoll += inPoll ? 1 : 0;
    }
    bool isEgressImageClosed(std::int32_t imageSessionId) const
    {
        return closedImages.count(imageSessionId) > 0;
    }

    PublicationId addIngressPublication(const std::string &channel, std::int32_t streamId)
    {
        FakePublication publication;
        publication.channel = channel;
        publication.streamId = streamId;
        publication.status = autoReady ? Registration::READY : Registration::PENDING;
        publication.connected = autoConnect && connectedEndpoints(channel);
        publications.push_back(std::move(publication));
        return static_cast<PublicationId>(publications.size() - 1);
    }
    Registration publicationStatus(PublicationId id) const
    {
        const FakePublication &p = publications.at(static_cast<std::size_t>(id));
        return p.closed ? Registration::FAILED : p.status;
    }
    bool isPublicationConnected(PublicationId id) const
    {
        const FakePublication &p = publications.at(static_cast<std::size_t>(id));
        return !p.closed && p.connected;
    }
    std::int64_t offer(PublicationId id, const std::uint8_t *header, std::size_t headerLength,
        const std::uint8_t *body, std::size_t bodyLength)
    {
        offerCalls++;
        FakePublication &p = publications.at(static_cast<std::size_t>(id));
        if (p.closed)
        {
            return OFFER_PUBLICATION_CLOSED;
        }
        if (Registration::READY != p.status || !p.connected)
        {
            return OFFER_NOT_CONNECTED;
        }
        if (!p.scriptedResults.empty())
        {
            const std::int64_t result = p.scriptedResults.front();
            p.scriptedResults.pop_front();
            if (result <= 0)
            {
                return result;
            }
        }
        p.position += static_cast<std::int64_t>(headerLength + bodyLength);
        if (!recordOffers)
        {
            offersNotRecorded++; // an allocation-free fake for allocation counting (RC2 S12 zeroAllocDutyCycle)
            return p.position;
        }
        std::vector<std::uint8_t> frame(header, header + headerLength);
        if (bodyLength > 0)
        {
            frame.insert(frame.end(), body, body + bodyLength);
        }
        p.offered.push_back(std::move(frame));
        return p.position;
    }
    void closePublication(PublicationId id)
    {
        publications.at(static_cast<std::size_t>(id)).closed = true;
    }
    std::string lastError() const { return error; }

    // ---- test controls
    bool connectedEndpoints(const std::string &channel) const
    {
        for (const std::string &dead : deadEndpoints)
        {
            if (channel.find(dead) != std::string::npos)
            {
                return false;
            }
        }
        return true;
    }

    PublicationId publicationTo(const std::string &endpoint, bool includeClosed = false) const
    {
        for (std::size_t i = publications.size(); i-- > 0;)
        {
            if (publications[i].channel.find("endpoint=" + endpoint) != std::string::npos &&
                (includeClosed || !publications[i].closed))
            {
                return static_cast<PublicationId>(i);
            }
        }
        return NO_PUBLICATION;
    }

    // The correlation id of the last SessionConnectRequest offered anywhere.
    std::int64_t lastConnectCorrelation(PublicationId *on = nullptr) const
    {
        std::int64_t correlationId = -1;
        for (std::size_t i = 0; i < publications.size(); i++)
        {
            for (const auto &frame : publications[i].offered)
            {
                cc::MessageHeader header(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 0, frame.size(), 0);
                if (header.templateId() == cc::SessionConnectRequest::sbeTemplateId())
                {
                    cc::SessionConnectRequest request(
                        const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 8, frame.size(),
                        header.blockLength(), header.version());
                    correlationId = request.correlationId();
                    if (nullptr != on)
                    {
                        *on = static_cast<PublicationId>(i);
                    }
                }
            }
        }
        return correlationId;
    }

    void pushSessionEvent(std::int64_t correlationId, std::int64_t sessionId, std::int64_t term,
        std::int32_t leader, cc::EventCode::Value code, const std::string &detail, std::int32_t image,
        std::int64_t heartbeatNs = 1'000'000'000LL)
    {
        std::vector<std::uint8_t> bytes(512);
        cc::SessionEvent event;
        event.wrapAndApplyHeader(reinterpret_cast<char *>(bytes.data()), 0, bytes.size())
            .clusterSessionId(sessionId)
            .correlationId(correlationId)
            .leadershipTermId(term)
            .leaderMemberId(leader)
            .code(code)
            .version(0x300)
            .leaderHeartbeatTimeoutNs(heartbeatNs);
        event.putDetail(detail);
        bytes.resize(cc::MessageHeader::encodedLength() + event.encodedLength());
        egress.push_back(QueuedFragment{ std::move(bytes), image });
    }

    void pushChallenge(std::int64_t correlationId, std::int64_t sessionId, std::int32_t image)
    {
        std::vector<std::uint8_t> bytes(128);
        cc::Challenge challenge;
        challenge.wrapAndApplyHeader(reinterpret_cast<char *>(bytes.data()), 0, bytes.size())
            .correlationId(correlationId)
            .clusterSessionId(sessionId);
        challenge.putEncodedChallenge("x", 1);
        bytes.resize(cc::MessageHeader::encodedLength() + challenge.encodedLength());
        egress.push_back(QueuedFragment{ std::move(bytes), image });
    }

    void pushNewLeader(std::int64_t sessionId, std::int64_t term, std::int32_t leader,
        const std::string &endpoints, std::int32_t image)
    {
        std::vector<std::uint8_t> bytes(512);
        cc::NewLeaderEvent event;
        event.wrapAndApplyHeader(reinterpret_cast<char *>(bytes.data()), 0, bytes.size())
            .leadershipTermId(term)
            .clusterSessionId(sessionId)
            .leaderMemberId(leader);
        event.putIngressEndpoints(endpoints);
        bytes.resize(cc::MessageHeader::encodedLength() + event.encodedLength());
        egress.push_back(QueuedFragment{ std::move(bytes), image });
    }

    void pushAppMessage(std::int64_t sessionId, const std::vector<std::uint8_t> &payload, std::int32_t image,
        std::int64_t leadershipTermId = 0)
    {
        std::vector<std::uint8_t> bytes(32 + payload.size());
        cc::SessionMessageHeader header;
        header.wrapAndApplyHeader(reinterpret_cast<char *>(bytes.data()), 0, bytes.size())
            .leadershipTermId(leadershipTermId)
            .clusterSessionId(sessionId)
            .timestamp(42);
        std::memcpy(bytes.data() + 32, payload.data(), payload.size());
        egress.push_back(QueuedFragment{ std::move(bytes), image });
    }

    std::int64_t now = 1'000'000'000LL;
    std::int64_t correlation = 100;
    std::string error;

    bool refuseSubscription = false;
    bool subscriptionOpen = false;
    Registration subscriptionStatus = Registration::READY;
    std::string resolvedChannel = "aeron:udp?endpoint=127.0.0.1:40123";
    std::string egressChannel;
    std::int32_t egressStreamId = 0;
    int subscriptionAdds = 0;
    int subscriptionCloses = 0;
    bool inPoll = false;
    bool closedSubscriptionInsidePoll = false;

    bool autoReady = true;
    bool autoConnect = true;
    std::vector<std::string> deadEndpoints;
    std::vector<FakePublication> publications;

    std::deque<QueuedFragment> egress;
    std::map<std::int32_t, bool> closedImages;
    int assemblyResets = 0;
    int resetsInsidePoll = 0;
    int pollCalls = 0;
    int offerCalls = 0;
    bool recordOffers = true;
    std::uint64_t offersNotRecorded = 0;
};

static_assert(ClusterTransport<FakeTransport>);

}}}

#endif
