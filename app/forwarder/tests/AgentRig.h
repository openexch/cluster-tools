#ifndef CLUSTER_TOOLS_APP_FORWARDER_TESTS_AGENT_RIG_H
#define CLUSTER_TOOLS_APP_FORWARDER_TESTS_AGENT_RIG_H

// The ForwarderAgent over two scripted FakeTransports (core and ME) with one manual clock, plus encoders for
// every core and ME frame the tests inject and decoders for what the agent offered.

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "FakeTransport.h"
#include "ForwarderAgent.h"

#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/AdminAck.h"
#include "com_openexchange_assets_infrastructure_generated/AdmissionState.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/OrderReject.h"
#include "com_openexchange_assets_infrastructure_generated/PendingDispatchEnd.h"
#include "com_openexchange_assets_infrastructure_generated/RegisterSession.h"
#include "com_openexchange_assets_infrastructure_generated/RequestPendingDispatch.h"

namespace cluster_tools { namespace app { namespace forwarder { namespace test {

namespace cc = io::aeron::cluster::codecs;
namespace money = com::openexchange::assets::infrastructure::generated;
namespace me = com::match::infrastructure::generated;
using client::test::FakeTransport;
using Bytes = std::vector<std::uint8_t>;
using Dispatch = domain::forwarder::Dispatch;

inline constexpr std::int64_t MS = 1'000'000LL;
inline constexpr std::int64_t SECOND = 1'000'000'000LL;
inline constexpr std::int64_t EPOCH = 0x0A1B2C3D4E5F6071LL;
inline constexpr std::int64_t IDENTITY = 0x1234567890ABCDEFLL;
inline const std::string CORE_ENDPOINTS = "0=10.0.0.1:9002,1=10.0.0.2:9002,2=10.0.0.3:9002";
inline const std::string ME_ENDPOINTS = "0=10.0.1.1:9102,1=10.0.1.2:9102,2=10.0.1.3:9102";
inline constexpr std::int32_t CORE_IMAGE = 900;
inline constexpr std::int32_t ME_IMAGE = 950;

using Client = client::ClusterClient<FakeTransport>;
using Agent = ForwarderAgent<Client, Client>;

inline client::ClientSettings clientSettings(const std::string &endpoints, const std::string &response)
{
    client::ClientSettings s;
    s.ingressEndpoints = endpoints;
    s.responseChannel = response;
    s.credentials = Bytes(38, 0x66);
    s.maxConnectAttempts = 3;
    return s;
}

inline AgentSettings agentSettings()
{
    AgentSettings s;
    s.identity = IDENTITY;
    s.version = "test";
    return s;
}

// ---- core frames (money-schema v7)
inline Bytes rawDispatch(const Dispatch &d)
{
    Bytes b(89);
    auto put = [&](std::size_t at, const void *v, std::size_t n) { std::memcpy(b.data() + at, v, n); };
    put(0, &d.dispatchSeq, 8);
    put(8, &d.commandIdHigh, 8);
    put(16, &d.commandIdLow, 8);
    put(24, &d.kind, 1);
    put(25, &d.userId, 8);
    put(33, &d.omsOrderId, 8);
    put(41, &d.oldOrderId, 8);
    put(49, &d.marketId, 4);
    put(53, &d.orderType, 1);
    put(54, &d.side, 1);
    put(55, &d.price, 8);
    put(63, &d.quantity, 8);
    put(71, &d.budget, 8);
    put(79, &d.timeInForce, 1);
    put(80, &d.releaseScope, 1);
    put(81, &d.expectedOldRemaining, 8);
    return b;
}

inline Bytes header(std::uint16_t block, std::uint16_t templateId, std::uint16_t schemaId, std::uint16_t version)
{
    Bytes b(8);
    std::memcpy(b.data(), &block, 2);
    std::memcpy(b.data() + 2, &templateId, 2);
    std::memcpy(b.data() + 4, &schemaId, 2);
    std::memcpy(b.data() + 6, &version, 2);
    return b;
}

inline Bytes dispatchFrame(const Dispatch &d, std::uint16_t version = 7)
{
    Bytes b = header(89, 104, 2, version);
    const Bytes body = rawDispatch(d);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

inline Bytes batchFrame(const std::vector<Dispatch> &entries)
{
    Bytes b = header(0, 105, 2, 7);
    const std::uint16_t block = 89;
    const std::uint16_t n = static_cast<std::uint16_t>(entries.size());
    b.resize(12);
    std::memcpy(b.data() + 8, &block, 2);
    std::memcpy(b.data() + 10, &n, 2);
    for (const Dispatch &d : entries)
    {
        const Bytes body = rawDispatch(d);
        b.insert(b.end(), body.begin(), body.end());
    }
    return b;
}

inline Bytes adminAck(std::int64_t correlationId, money::AdminResult::Value result, std::uint8_t reason = 0)
{
    Bytes b(64);
    money::AdminAck e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(correlationId).refTemplateId(80).result(result).reason(reason).configVersion(0);
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes admission(std::int32_t marketId, std::uint8_t state, std::uint32_t reasons)
{
    Bytes b(64);
    money::AdmissionState e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .marketId(marketId)
        .gateState(0 == state ? money::GateState::OPEN : money::GateState::SUSPENDED)
        .gateReason(money::GateReason::ADMIN)
        .circuitBreaker(money::BoolFlag::FALSE)
        .atMs(1).logPosition(2);
    std::memcpy(b.data() + 8 + 23, &reasons, 4);
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes pendingEnd(std::int64_t correlationId, std::int32_t count, std::int64_t head)
{
    Bytes b(64);
    money::PendingDispatchEnd e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(correlationId).count(count).headDispatchSeq(head);
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes orderReject(std::int64_t correlationId, std::uint16_t refTemplateId, money::CoreRejectReason::Value reason)
{
    Bytes b(128);
    money::OrderReject e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(correlationId).requestIdHigh(0).requestIdLow(0).userId(0).orderId(0).marketId(0)
        .refTemplateId(refTemplateId).reason(reason).meReason(255).detail(0).rejectedAtMs(0);
    b.resize(8 + e.encodedLength());
    return b;
}

// ---- ME frames (order-schema v12)
inline Bytes heartbeat(std::int64_t meEpoch, std::uint16_t version = 12)
{
    Bytes b(64);
    me::ClusterHeartbeat e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size()).nodeId(0).timestamp(1).meEpoch(meEpoch);
    b.resize(8 + e.encodedLength());
    std::memcpy(b.data() + 6, &version, 2);
    return b;
}

inline Bytes meOther(std::uint16_t templateId)
{
    Bytes b = header(16, templateId, 1, 12);
    b.resize(24, 0);
    return b;
}

// What the agent offered on one transport: app payloads (after the 32 B session header), in order.
struct Offered
{
    std::uint16_t templateId = 0;
    std::int64_t term = 0;
    Bytes payload;
};

inline std::vector<Offered> appOffers(const FakeTransport &t)
{
    std::vector<Offered> out;
    for (const auto &p : t.publications)
    {
        for (const Bytes &frame : p.offered)
        {
            if (frame.size() < 40)
            {
                continue;
            }
            std::uint16_t smhTemplate;
            std::memcpy(&smhTemplate, frame.data() + 2, 2);
            if (smhTemplate != cc::SessionMessageHeader::sbeTemplateId())
            {
                continue;
            }
            Offered o;
            std::memcpy(&o.term, frame.data() + 8, 8);
            o.payload.assign(frame.begin() + 32, frame.end());
            std::memcpy(&o.templateId, o.payload.data() + 2, 2);
            out.push_back(std::move(o));
        }
    }
    return out;
}

inline std::size_t countFrames(const FakeTransport &t, std::uint16_t clusterTemplateId)
{
    std::size_t n = 0;
    for (const auto &p : t.publications)
    {
        for (const Bytes &frame : p.offered)
        {
            std::uint16_t id;
            std::memcpy(&id, frame.data() + 2, 2);
            n += id == clusterTemplateId ? 1 : 0;
        }
    }
    return n;
}

struct Request65
{
    std::int64_t correlationId = 0;
    std::int64_t from = 0;
    std::int32_t max = 0;
};

inline Request65 decode65(const Bytes &payload)
{
    Request65 r;
    std::memcpy(&r.correlationId, payload.data() + 8, 8);
    std::memcpy(&r.from, payload.data() + 16, 8);
    std::memcpy(&r.max, payload.data() + 24, 4);
    return r;
}

struct Register80
{
    std::int64_t correlationId = 0;
    std::int64_t identity = 0;
    std::int64_t sourceEpoch = 0;
};

inline Register80 decode80(const Bytes &payload)
{
    money::RegisterSession d(const_cast<char *>(reinterpret_cast<const char *>(payload.data())), 8, payload.size(),
        money::RegisterSession::sbeBlockLength(), 7);
    return Register80{ d.correlationId(), d.identity(), d.sourceEpoch() };
}

struct Rig
{
    FakeTransport coreT;
    FakeTransport meT;
    Client core{ coreT, clientSettings(CORE_ENDPOINTS, "aeron:udp?endpoint=127.0.0.1:0") };
    Client me{ meT, clientSettings(ME_ENDPOINTS, "aeron:udp?endpoint=127.0.0.1:0") };
    LogRing log;
    Agent agent{ core, me, agentSettings(), &log };
    std::int64_t coreSession = 501;
    std::int64_t meSession = 601;
    std::int64_t coreTerm = 3;
    std::int64_t meTerm = 7;
    std::set<std::int64_t> answered;
    bool autoAnswerCore = true;
    bool autoAnswerMe = true;
    std::size_t corePolls = 0;

    Rig()
    {
        meT.correlation = 50'000; // distinct correlation spaces per transport
    }

    void advance(std::int64_t ns)
    {
        coreT.now += ns;
        meT.now += ns;
    }

    void answerConnects()
    {
        const std::int64_t c = coreT.lastConnectCorrelation();
        if (autoAnswerCore && c > 0 && answered.insert(c).second)
        {
            coreT.pushSessionEvent(c, ++coreSession, coreTerm, 0, cc::EventCode::OK, "", CORE_IMAGE);
        }
        const std::int64_t m = meT.lastConnectCorrelation();
        if (autoAnswerMe && m > 0 && answered.insert(m).second)
        {
            meT.pushSessionEvent(m, ++meSession, meTerm, 0, cc::EventCode::OK, "", ME_IMAGE);
        }
    }

    int cycle(std::int64_t stepNs = MS)
    {
        advance(stepNs);
        answerConnects();
        return agent.doWork();
    }

    void cycles(int n, std::int64_t stepNs = MS)
    {
        for (int i = 0; i < n && !agent.stopped(); i++)
        {
            cycle(stepNs);
        }
    }

    void runFor(std::int64_t ns, std::int64_t stepNs = MS)
    {
        const std::int64_t end = coreT.now + ns;
        while (coreT.now < end && !agent.stopped())
        {
            cycle(stepNs);
        }
    }

    void core_(const Bytes &payload, std::int64_t term)
    {
        coreT.pushAppMessage(coreSession, payload, CORE_IMAGE, term);
    }
    void core_(const Bytes &payload) { core_(payload, coreTerm); }
    void me_(const Bytes &payload) { meT.pushAppMessage(meSession, payload, ME_IMAGE, meTerm); }

    std::vector<Offered> coreOffers(std::uint16_t templateId) const
    {
        std::vector<Offered> out;
        for (Offered &o : appOffers(coreT))
        {
            if (o.templateId == templateId)
            {
                out.push_back(std::move(o));
            }
        }
        return out;
    }
    std::vector<Offered> meOffers() const { return appOffers(meT); }

    // Both sessions open, the ME epoch bound (M3).
    void connectBoth(std::int64_t meEpoch = EPOCH)
    {
        cycles(50);
        REQUIRE(core.isConnected());
        REQUIRE(me.isConnected());
        me_(heartbeat(meEpoch));
        cycles(5);
        REQUIRE(agent.meLink() == MeLink::BOUND);
    }

    // The registration round answered ACCEPTED + ORIGIN AdmissionState.
    std::int64_t acceptRegistration(std::uint8_t gateState = 0, std::uint32_t reasons = 0)
    {
        cycles(5);
        const auto registers = coreOffers(80);
        REQUIRE(!registers.empty());
        const std::int64_t corr = decode80(registers.back().payload).correlationId;
        core_(adminAck(corr, money::AdminResult::ACCEPTED));
        core_(admission(0, gateState, reasons));
        cycles(5);
        REQUIRE(agent.coreLink() == CoreLink::REGISTERED);
        return corr;
    }

    // Answers the last offered 65 (must be a request we did not answer yet).
    Request65 last65() const
    {
        const auto requests = coreOffers(65);
        REQUIRE(!requests.empty());
        return decode65(requests.back().payload);
    }

    // C3 entry probe {0, head}, then the RESYNC(0) page with the given reply entries: LIVE.
    void goLive(std::int64_t head = 0, const std::vector<Dispatch> &reply = {})
    {
        const Request65 probe = last65();
        REQUIRE(probe.max == 0);
        core_(pendingEnd(probe.correlationId, 0, head));
        cycles(3);
        const Request65 page = last65();
        REQUIRE(page.max == 4096);
        REQUIRE(page.from == 0);
        if (!reply.empty())
        {
            core_(batchFrame(reply));
        }
        core_(pendingEnd(page.correlationId, static_cast<std::int32_t>(reply.size()), head));
        cycles(3);
        REQUIRE(agent.resync().mode() == domain::forwarder::Mode::LIVE);
    }

    void bringUp(std::int64_t head = 0)
    {
        connectBoth();
        acceptRegistration();
        goLive(head);
    }
};

inline Dispatch create(std::int64_t seq, std::int64_t orderId)
{
    Dispatch d;
    d.dispatchSeq = seq;
    d.commandIdHigh = orderId;
    d.commandIdLow = (1 << 2) | 0;
    d.kind = 0;
    d.userId = 7;
    d.omsOrderId = orderId;
    d.marketId = 3;
    d.price = 100;
    d.quantity = 5;
    return d;
}

inline Dispatch releaseOf(std::int64_t seq, std::int64_t orderId, std::uint8_t scope = 0)
{
    Dispatch d;
    d.dispatchSeq = seq;
    d.commandIdHigh = orderId;
    d.commandIdLow = (2 << 2) | (0 == scope ? 1 : 3);
    d.kind = 3;
    d.releaseScope = scope;
    return d;
}

}}}}

#endif
