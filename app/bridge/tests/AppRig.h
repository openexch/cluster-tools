#ifndef CLUSTER_TOOLS_APP_BRIDGE_TESTS_APP_RIG_H
#define CLUSTER_TOOLS_APP_BRIDGE_TESTS_APP_RIG_H

// The ct_bridge_app_tests rig: the real BridgeAgent over a real ClusterClient on the scripted
// FakeTransport, with the StubCore (the core model: admit, D3a, latch precedence) BEHIND the transport and the
// SimSources (SourceSet semantics over SimArchive) as the sources.
//   - every app frame the agent offers is read off the transport after the iteration (FIFO, per publication, with
//     its session-header term: a frame of an ended term is dropped as the consensus module drops it);
//   - RegisterSession 80 is answered by a registration model (ACCEPTED first, DUPLICATE after, or scripted);
//   - the domain's frames (FakeEncoder bytes) go to the StubCore, whose egress (17 / 114 / 110 ref 66) is SBE-encoded
//     with the generated money codecs and delivered on egress with a lag of 1..8 iterations (never before
//     the next poll), stamped with the cluster's current term;
//   - elections (NewLeaderEvent, the uncommitted tail lost), session losses (SessionEvent CLOSED), image losses and a
//     core that forgets the registration are scripted.

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "BridgeTestSupport.h"
#include "FakeTransport.h"

#include "BridgeAgent.h"
#include "BridgeMetricsRender.h"

#include "com_openexchange_core_infrastructure_generated/AdminAck.h"
#include "com_openexchange_core_infrastructure_generated/FeedPositionReport.h"
#include "com_openexchange_core_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_core_infrastructure_generated/OrderReject.h"
#include "com_openexchange_core_infrastructure_generated/RegisterSession.h"
#include "com_openexchange_core_infrastructure_generated/SettleRejected.h"
#include "com_openexchange_core_infrastructure_generated/SettlementApplied.h"
#include "io_aeron_cluster_codecs/SessionMessageHeader.h"

namespace ct_bridge_app_test {

using namespace ct_bridge_test;
namespace app = cluster_tools::app::bridge;
namespace common = cluster_tools::app::common;
namespace cl = cluster_tools::client;
namespace cc = io::aeron::cluster::codecs;
namespace money = com::openexchange::core::infrastructure::generated;
using cl::test::FakeTransport;

inline constexpr std::int64_t MS = 1'000'000LL;
inline constexpr std::int64_t SECOND = 1'000'000'000LL;
inline const std::string CORE_ENDPOINTS = "0=10.0.0.1:19302,1=10.0.0.2:19402,2=10.0.0.3:19502";

// ------------------------------------------------------------------ SBE money egress frames (the core's side)

inline Bytes moneyFrame(std::size_t capacity)
{
    return Bytes(capacity, 0);
}

inline Bytes reportFrame(const FeedPositionReport &r)
{
    Bytes b = moneyFrame(128);
    money::FeedPositionReport e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(r.correlationId)
        .consumePosition(r.consumePosition)
        .lastAppliedTradeId(r.lastAppliedTradeId)
        .journalEnabled(r.journalEnabled ? money::BoolFlag::TRUE : money::BoolFlag::FALSE)
        .meEpoch(r.meEpoch)
        .lastAppliedJournalSeq(r.lastAppliedJournalSeq)
        .feedHalted(static_cast<money::BoolFlag::Value>(r.feedHaltedRaw))
        .haltReason(static_cast<money::FeedHaltReason::Value>(r.haltReasonRaw));
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes settleRejectedFrame(const SettleRejected &r)
{
    Bytes b = moneyFrame(128);
    money::SettleRejected e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .tradeId(r.tradeId)
        .marketId(r.marketId)
        .journalPosition(r.journalPosition)
        .reason(static_cast<money::SettleRejectReason::Value>(r.reasonRaw))
        .detail(r.detail);
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes adminAckFrame(std::int64_t correlationId, std::uint16_t refTemplateId, std::uint8_t resultRaw,
    std::uint8_t reason = 0)
{
    Bytes b = moneyFrame(64);
    money::AdminAck e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(correlationId)
        .refTemplateId(refTemplateId)
        .result(static_cast<money::AdminResult::Value>(resultRaw))
        .reason(reason)
        .configVersion(0);
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes orderRejectFrame(std::uint16_t refTemplateId, std::uint8_t reasonRaw, std::int64_t correlationId = 0)
{
    Bytes b = moneyFrame(128);
    money::OrderReject e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(correlationId).requestIdHigh(0).requestIdLow(0).userId(0).orderId(0).marketId(0)
        .refTemplateId(refTemplateId)
        .reason(static_cast<money::CoreRejectReason::Value>(reasonRaw))
        .meReason(255).detail(0).rejectedAtMs(0);
    b.resize(8 + e.encodedLength());
    return b;
}

inline Bytes settlementAppliedFrame(std::int64_t tradeId)
{
    Bytes b = moneyFrame(64);
    money::SettlementApplied e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size()).tradeId(tradeId).buyerUserId(1).sellerUserId(2);
    b.resize(8 + e.encodedLength());
    return b;
}

inline std::uint16_t rd16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline bool isRegisterSession(const std::uint8_t *p, std::size_t n)
{
    return n >= 8 && 80 == rd16(p + 2) && 2 == rd16(p + 4) && 7 == rd16(p + 6);
}

// ------------------------------------------------------------------ an allocation-free transport (zeroAllocDutyIteration)

// FakeTransport whose offers are copied into a preallocated ring instead of a vector per frame.
struct RingTransport : FakeTransport
{
    static constexpr std::size_t SLOTS = 1024;
    static constexpr std::size_t SLOT = 8256;
    struct Slot
    {
        std::size_t length = 0;
        std::uint8_t bytes[SLOT];
    };
    std::unique_ptr<Slot[]> ring{ new Slot[SLOTS] };
    std::uint64_t head = 0;
    std::uint64_t tail = 0;

    std::int64_t offer(cl::PublicationId id, const std::uint8_t *header, std::size_t headerLength, const std::uint8_t *body,
        std::size_t bodyLength)
    {
        if (headerLength >= 4 && cc::SessionConnectRequest::sbeTemplateId() == rd16(header + 2))
        {
            return FakeTransport::offer(id, header, headerLength, body, bodyLength); // recorded: the rig answers it
        }
        offerCalls++;
        cl::test::FakePublication &p = publications.at(static_cast<std::size_t>(id));
        if (p.closed)
        {
            return cl::OFFER_PUBLICATION_CLOSED;
        }
        if (cl::Registration::READY != p.status || !p.connected)
        {
            return cl::OFFER_NOT_CONNECTED;
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
        if (tail - head >= SLOTS || headerLength + bodyLength > SLOT)
        {
            return cl::OFFER_BACK_PRESSURED;
        }
        Slot &s = ring[tail % SLOTS];
        std::memcpy(s.bytes, header, headerLength);
        if (bodyLength > 0)
        {
            std::memcpy(s.bytes + headerLength, body, bodyLength);
        }
        s.length = headerLength + bodyLength;
        tail++;
        p.position += static_cast<std::int64_t>(headerLength + bodyLength);
        return p.position;
    }
};
static_assert(cl::ClusterTransport<RingTransport>);

// ------------------------------------------------------------------ sources: SimSources plus a blocked-call hook

struct RigSources
{
    SimSources sim;
    std::function<void()> duringDoWork; // e.g. a blocked archive call that runs the agent's invoker
    int stops = 0;

    void start(TimePoint now, std::size_t first = 0) { sim.start(now, first); }
    int doWork(FrameSink &sink, int limit, TimePoint now)
    {
        if (duringDoWork)
        {
            auto hook = std::move(duringDoWork);
            duringDoWork = nullptr;
            hook();
        }
        return sim.doWork(sink, limit, now);
    }
    void switchTo(std::size_t index, TimePoint now) { sim.switchTo(index, now); }
    void reattach(TimePoint now) { sim.reattach(now); }
    std::function<void()> duringStop; // e.g. a stopReplay that blocks on a slow archive
    void stop(TimePoint now)
    {
        stops++;
        if (duringStop)
        {
            duringStop();
        }
        sim.pending.reset();
        sim.policies[sim.current]->detach(now);
    }
    std::size_t current() const { return sim.current; }
    bool currentIdle() const { return sim.policies[sim.current]->idle(); }
};

// ------------------------------------------------------------------ the rig

struct AppFrame
{
    long step = 0;
    std::size_t publication = 0;
    std::int64_t term = 0;
    std::int64_t session = 0;
    std::uint16_t templateId = 0; // money template (RegisterSession 80) or the fake frame's money template
    bool registerSession = false;
    Bytes payload;
};

template<typename Transport = FakeTransport>
struct AppRigT
{
    using Client = cl::ClusterClient<Transport, app::LinkObserver>;
    using Agent = app::BridgeAgent<Client, RigSources>;

    Transport t;
    app::LinkObserver observer;
    std::unique_ptr<Client> client;
    ArchiveMetrics archiveMetrics;
    std::unique_ptr<ResumeMemo> memo;
    app::ObserverTap tap;
    RigSources sources;
    FakeEncoder encoder;
    StubCore core;
    common::LogRing log;
    common::DutyHeartbeat heartbeat;
    std::unique_ptr<Agent> agent;
    std::size_t sourceCount = 1;

    // the cluster model
    std::int64_t session = 700;
    std::int64_t term = 3;
    std::int32_t leader = 0;
    std::int32_t image = 900;
    bool answerConnects = true;
    std::set<std::int64_t> answered;
    std::uint16_t moneyVersion = 7;

    // the registration model
    enum class Reg
    {
        ACCEPT,
        REJECT,
        SILENT,
        ANSWER_FIRST_LATE, // only the first RegisterSession of a session is answered, `lateNs` later
        DUPLICATE_IDENTITY // REJECTED reason 44 until `duplicateUntilNs`, then ACCEPT (the old session timed out)
    } reg = Reg::ACCEPT;
    std::int64_t duplicateUntilNs = 0;
    std::int64_t lateNs = 7 * SECOND;
    std::set<std::int64_t> registeredSessions;
    std::set<std::int64_t> forgotten;            // sessions the core forgot the registration of (6.2)
    std::map<std::pair<std::int64_t, std::int64_t>, long> ackStep; // (session, term) -> step its ack was delivered
    std::set<std::int64_t> answeredFirst;
    std::vector<std::int64_t> registerCorrelations;

    // egress lag (C1)
    int lagMin = 1;
    int lagMax = 1;
    std::mt19937_64 rng{ 1 };
    long stepNo = 0;
    struct Pending
    {
        long due = 0;
        std::int64_t atNs = -1; // or a time
        std::int64_t term = 0;
        std::int64_t session = 0;
        Bytes payload;
    };
    std::deque<Pending> egressQueue;
    bool holdEgress = false;
    struct Delivered
    {
        long step = 0;
        std::uint16_t templateId = 0;
    };
    std::vector<Delivered> delivered; // what the model put on egress, by step (before the agent's poll of that step)
    long firstDelivered(std::uint16_t templateId, long after = -1) const
    {
        for (const auto &d : delivered)
        {
            if (d.templateId == templateId && d.step > after)
            {
                return d.step;
            }
        }
        return -1;
    }

    // what the agent offered
    std::vector<AppFrame> frames;
    std::map<std::size_t, std::size_t> scanned; // publication -> frames read
    std::uint64_t droppedOldTerm = 0;
    std::int64_t epochBaseMs = 1'700'000'000'000LL;

    explicit AppRigT(std::size_t n = 1, std::function<void(app::AgentSettings &)> tweak = {}, int lagMax_ = 1) :
        sourceCount(n), lagMax(lagMax_)
    {
        client = std::make_unique<Client>(t, clientSettings(), &observer);
        memo = std::make_unique<ResumeMemo>(n, &archiveMetrics);
        FollowSettings fs;
        fs.backoff = 100ms;
        sources.sim.build(n, *memo, archiveMetrics, &tap, fs);
        app::AgentSettings s;
        s.identity = 0xB1;
        s.sourceCount = n;
        s.probeInterval = fs.probeInterval;
        s.version = "s9c-test";
        s.epochMs = [this] { return epochBaseMs + t.now / MS; };
        s.tweakDomain = [n](BridgeSettings &d) {
            const BridgeSettings base = testSettings(n);
            d.correlationBase = base.correlationBase;
            d.ringCapacity = base.ringCapacity;
            d.keyTableCapacity = base.keyTableCapacity;
        };
        if (tweak)
        {
            tweak(s);
        }
        agent = std::make_unique<Agent>(*client, sources, tap, *memo, archiveMetrics, encoder, s, &log, &heartbeat);
        observer.target = agent.get();
    }

    ~AppRigT()
    {
        observer.target = nullptr; // the client's close() in its destructor must not reach the destroyed agent
        tap.bind(nullptr);
    }

    static cl::ClientSettings clientSettings()
    {
        cl::ClientSettings s;
        s.ingressEndpoints = CORE_ENDPOINTS;
        s.responseChannel = "aeron:udp?endpoint=127.0.0.1:0|term-length=4m";
        s.ingressChannel = "aeron:udp?term-length=4m|mtu=8192";
        s.credentials = Bytes(38, 0x33);
        s.maxConnectAttempts = 3;
        return s;
    }

    SimArchive &archive(std::size_t i) { return *sources.sim.archives[i]; }
    TimePoint now() const { return TimePoint(std::chrono::nanoseconds(t.now)); }

    int lag()
    {
        if (lagMax <= lagMin)
        {
            return lagMin;
        }
        return lagMin + static_cast<int>(rng() % static_cast<std::uint64_t>(lagMax - lagMin + 1));
    }

    void emit(Bytes payload, int extraLag = 0)
    {
        egressQueue.push_back(Pending{ stepNo + lag() + extraLag, -1, term, session, std::move(payload) });
    }
    void emitAt(std::int64_t atNs, Bytes payload)
    {
        egressQueue.push_back(Pending{ 0, atNs, term, session, std::move(payload) });
    }
    // An egress frame now, outside the model (old-term reports, unknown templates, a wrong version).
    void inject(const Bytes &payload, std::int64_t frameTerm)
    {
        t.pushAppMessage(session, payload, image, frameTerm);
    }

    void pumpEgress()
    {
        for (auto it = egressQueue.begin(); it != egressQueue.end();)
        {
            const bool due = it->atNs >= 0 ? t.now >= it->atNs : stepNo >= it->due;
            if (!due || holdEgress)
            {
                ++it;
                continue;
            }
            if (it->session == session) // a closed session's egress queue is dropped with it
            {
                t.pushAppMessage(it->session, it->payload, image, it->term);
                delivered.push_back({ stepNo, rd16(it->payload.data() + 2) });
                if (80 == ackOf(it->payload))
                {
                    ackStep[{ it->session, it->term }] = stepNo;
                }
            }
            it = egressQueue.erase(it);
        }
    }

    static int ackOf(const Bytes &p)
    {
        if (p.size() < 18 || 110 != rd16(p.data() + 2))
        {
            return -1;
        }
        return rd16(p.data() + 8 + 8);
    }

    void doAnswerConnects()
    {
        const std::int64_t c = t.lastConnectCorrelation();
        if (answerConnects && c > 0 && answered.insert(c).second)
        {
            ++session;
            t.pushSessionEvent(c, session, term, leader, cc::EventCode::OK, "", image);
        }
    }

    template<typename F>
    void forEachNewOffer(F &&f)
    {
        if constexpr (std::is_same_v<Transport, RingTransport>)
        {
            while (t.head < t.tail)
            {
                const auto &s = t.ring[t.head % RingTransport::SLOTS];
                f(0, s.bytes, s.length);
                t.head++;
            }
        }
        else
        {
            for (std::size_t i = 0; i < t.publications.size(); ++i)
            {
                auto &offered = t.publications[i].offered;
                std::size_t &seen = scanned[i];
                for (; seen < offered.size(); ++seen)
                {
                    f(i, offered[seen].data(), offered[seen].size());
                }
            }
        }
    }

    void collectOffers()
    {
        forEachNewOffer([&](std::size_t pub, const std::uint8_t *frame, std::size_t length) {
            if (length < 32 || cc::SessionMessageHeader::sbeTemplateId() != rd16(frame + 2))
            {
                return; // keepalive, connect, close
            }
            std::int64_t frameTerm = 0, frameSession = 0;
            std::memcpy(&frameTerm, frame + 8, 8);
            std::memcpy(&frameSession, frame + 16, 8);
            AppFrame a;
            a.step = stepNo;
            a.publication = pub;
            a.term = frameTerm;
            a.session = frameSession;
            a.payload.assign(frame + 32, frame + length);
            a.registerSession = isRegisterSession(a.payload.data(), a.payload.size());
            a.templateId = a.registerSession ? 80 : rd16(a.payload.data());
            if (frameTerm != term || frameSession != session)
            {
                droppedOldTerm++; // the consensus module drops a frame stamped with another term
                a.templateId = 0xFFFF;
                frames.push_back(std::move(a));
                return;
            }
            onAppFrame(a);
            frames.push_back(std::move(a));
        });
        while (!core.egress.empty())
        {
            const CoreEgress e = core.egress.front();
            core.egress.pop_front();
            switch (e.kind)
            {
                case CoreEgress::REPORT: emit(reportFrame(e.report)); break;
                case CoreEgress::REJECT: emit(settleRejectedFrame(e.reject)); break;
                case CoreEgress::ACK: emit(adminAckFrame(e.ack.correlationId, e.ack.refTemplateId, e.ack.resultRaw, e.ack.reason)); break;
            }
        }
    }

    void onAppFrame(const AppFrame &a)
    {
        if (a.registerSession)
        {
            money::RegisterSession d(const_cast<char *>(reinterpret_cast<const char *>(a.payload.data())), 8,
                a.payload.size(), money::RegisterSession::sbeBlockLength(), 7);
            const std::int64_t corr = d.correlationId();
            registerCorrelations.push_back(corr);
            switch (reg)
            {
                case Reg::ACCEPT:
                {
                    const bool forgot = forgotten.erase(session) > 0;
                    const bool first = registeredSessions.insert(session).second || forgot;
                    emit(adminAckFrame(corr, 80, first ? ADMIN_ACCEPTED : ADMIN_DUPLICATE));
                    return;
                }
                case Reg::REJECT: emit(adminAckFrame(corr, 80, ADMIN_REJECTED, 1)); return;
                case Reg::SILENT: return;
                case Reg::DUPLICATE_IDENTITY:
                    if (t.now < duplicateUntilNs)
                    {
                        emit(adminAckFrame(corr, 80, ADMIN_REJECTED, 44));
                        return;
                    }
                    registeredSessions.insert(session);
                    emit(adminAckFrame(corr, 80, ADMIN_ACCEPTED));
                    return;
                case Reg::ANSWER_FIRST_LATE:
                    if (answeredFirst.insert(session).second)
                    {
                        registeredSessions.insert(session);
                        emitAt(t.now + lateNs, adminAckFrame(corr, 80, ADMIN_ACCEPTED));
                    }
                    return;
            }
            return;
        }
        const FakeFrame f = decodeFake(a.payload.data(), a.payload.size());
        if (forgotten.count(session) > 0 && T_QUERY_FEED_POSITION != f.moneyTemplate)
        {
            emit(orderRejectFrame(f.moneyTemplate, 35)); // UNREGISTERED_SESSION: the core forgot this session
            return;
        }
        core.tryOffer(a.payload.data(), a.payload.size());
    }

    int step(std::int64_t advanceNs = 10 * MS)
    {
        ++stepNo;
        t.now += advanceNs;
        doAnswerConnects();
        pumpEgress();
        const int work = agent->doWork();
        collectOffers();
        return work;
    }

    template<typename Pred>
    bool runUntil(Pred pred, std::int64_t limitNs = 120 * SECOND, std::int64_t advanceNs = 10 * MS)
    {
        const std::int64_t end = t.now + limitNs;
        while (t.now < end && !agent->stopped())
        {
            if (pred())
            {
                return true;
            }
            step(advanceNs);
        }
        return pred();
    }

    void runFor(std::int64_t ns, std::int64_t advanceNs = 10 * MS)
    {
        const std::int64_t end = t.now + ns;
        while (t.now < end && !agent->stopped())
        {
            step(advanceNs);
        }
    }

    // ---- scripted cluster events
    void elect(bool loseUncommitted = true)
    {
        if (loseUncommitted)
        {
            core.elect();
        }
        term++;
        leader = (leader + 1) % 3;
        t.pushNewLeader(session, term, leader, CORE_ENDPOINTS, image);
    }
    void loseImage()
    {
        t.closedImages[image] = true;
        image++;
    }
    void closeSession()
    {
        t.pushSessionEvent(0, session, term, leader, cc::EventCode::CLOSED, "closed by the cluster (test)", image);
    }
    void forgetRegistration() { forgotten.insert(session); registeredSessions.erase(session); }

    // ---- journals
    SimRecording &journal(std::size_t source, std::int64_t from, std::int64_t to)
    {
        SimRecording &r = archive(source).record();
        appendJournal(archive(source), r, from, to);
        return r;
    }

    void bringUp()
    {
        agent->start();
        REQUIRE(runUntil([&] { return app::CoreLink::C3_REGISTERED == agent->link(); }, 30 * SECOND));
    }

    // ---- views of what was offered
    std::vector<const AppFrame *> offered(std::uint16_t templateId) const
    {
        std::vector<const AppFrame *> out;
        for (const auto &f : frames)
        {
            if (f.templateId == templateId)
            {
                out.push_back(&f);
            }
        }
        return out;
    }
    static bool isFeed(std::uint16_t t)
    {
        return T_ME_JOURNAL_TRADE == t || T_ME_JOURNAL_TRADE_BATCH == t || T_ME_JOURNAL_TERMINAL == t ||
            T_ME_COMMAND_OUTCOME == t || T_ME_COMMAND_RELEASE == t || T_ME_ENGINE_CONFIG == t;
    }
    long firstFeedStepAfter(long step) const
    {
        for (const auto &f : frames)
        {
            if (f.step > step && isFeed(f.templateId))
            {
                return f.step;
            }
        }
        return -1;
    }
};

using AppRig = AppRigT<FakeTransport>;

}

#endif
