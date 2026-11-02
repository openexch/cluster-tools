// RC2 S12 contract 11.3: ForwarderAgent<ClusterClient<FakeTransport>, ClusterClient<FakeTransport>> with a
// manual clock. Every frame the agent sees is a real money-schema v7 / order-schema v12 encoding; every frame
// it offers is decoded back.
#include "doctest/doctest.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "AgentRig.h"
#include "MetricsRender.h"

#include "cluster_tools/adapters/sbe/ForwarderCodec.h"
#include "cluster_tools/domain/forwarder/DispatchRules.h"

#include "golden/golden_frames.inc"

using namespace cluster_tools::app::forwarder;
using namespace cluster_tools::app::forwarder::test;
namespace fd = cluster_tools::domain::forwarder;
namespace codec = cluster_tools::sbe_adapter::forwarder_codec;
namespace client = cluster_tools::client;

namespace {

std::int64_t commandIdHighOf(const Bytes &payload)
{
    std::int64_t v;
    std::memcpy(&v, payload.data() + 8, 8);
    return v;
}

struct ReleaseFrame
{
    std::int64_t releaseSeq = 0;
    std::uint16_t count = 0;
    std::vector<fd::ReleaseEntry> entries;
};

ReleaseFrame decodeRelease(const Bytes &payload)
{
    ReleaseFrame f;
    std::memcpy(&f.releaseSeq, payload.data() + 8, 8);
    std::memcpy(&f.count, payload.data() + 18, 2);
    for (std::uint16_t i = 0; i < f.count; i++)
    {
        fd::ReleaseEntry e;
        const std::uint8_t *p = payload.data() + 20 + 17 * i;
        std::memcpy(&e.commandIdHigh, p, 8);
        std::memcpy(&e.idLowFloor, p + 8, 8);
        e.scope = p[16];
        f.entries.push_back(e);
    }
    return f;
}

Bytes encoded(const fd::DurableCommand &c)
{
    Bytes out(codec::DURABLE_COMMAND_FRAME_LENGTH);
    REQUIRE(codec::encodeDurableCommand(out.data(), out.size(), c) == out.size());
    return out;
}

std::vector<Offered> ofTemplate(const std::vector<Offered> &offers, std::uint16_t templateId)
{
    std::vector<Offered> out;
    std::copy_if(offers.begin(), offers.end(), std::back_inserter(out),
        [templateId](const Offered &o) { return o.templateId == templateId; });
    return out;
}

struct FakePublicationRef
{
    client::test::FakePublication *p;
};

FakePublicationRef mePublication(Rig &r, const std::string &endpoint = "10.0.1.1:9102")
{
    const client::PublicationId id = r.meT.publicationTo(endpoint);
    REQUIRE(id != client::NO_PUBLICATION);
    return FakePublicationRef{ &r.meT.publications[static_cast<std::size_t>(id)] };
}
FakePublicationRef corePublication(Rig &r, const std::string &endpoint = "10.0.0.1:9002")
{
    const client::PublicationId id = r.coreT.publicationTo(endpoint);
    REQUIRE(id != client::NO_PUBLICATION);
    return FakePublicationRef{ &r.coreT.publications[static_cast<std::size_t>(id)] };
}
}

TEST_CASE("ForwarderAgentTest.noRegistrationBeforeMeEpoch")
{
    Rig r;
    r.cycles(50);
    REQUIRE(r.core.isConnected());
    REQUIRE(r.me.isConnected());
    for (int i = 0; i < 3; i++)
    {
        r.me_(heartbeat(0)); // the ME is not FRESH yet
        r.runFor(SECOND);
    }
    CHECK(r.coreOffers(80).empty());
    CHECK(r.agent.meLink() == MeLink::UNVERIFIED);
    r.me_(heartbeat(EPOCH));
    r.cycles(5);
    const auto registers = r.coreOffers(80);
    REQUIRE(registers.size() == 1);
    CHECK(decode80(registers[0].payload).sourceEpoch == EPOCH);
    CHECK(registers[0].payload == JAVA_REGISTER_SESSION_EPOCH); // byte-equal to the core's own encoder
    CHECK(r.coreOffers(65).empty());                            // nothing before the AdminAck
    r.core_(adminAck(decode80(registers[0].payload).correlationId, money::AdminResult::ACCEPTED));
    r.core_(admission(0, 0, 0));
    r.cycles(5);
    const auto requests = r.coreOffers(65);
    REQUIRE(requests.size() == 1);
    const Request65 probe = decode65(requests[0].payload);
    CHECK(probe.max == 0); // the head probe first (F15)
    r.core_(pendingEnd(probe.correlationId, 0, 0));
    r.cycles(3);
    const Request65 page = r.last65();
    CHECK(page.from == 0);
    CHECK(page.max == 4096);
    CHECK(page.correlationId != probe.correlationId);
}

TEST_CASE("ForwarderAgentTest.keepaliveBothClustersEverySecond")
{
    Rig r;
    r.bringUp();
    const std::uint64_t core0 = r.core.counters().keepAlivesSent;
    const std::uint64_t me0 = r.me.counters().keepAlivesSent;
    const std::uint64_t opened0 = r.core.counters().sessionsOpened + r.me.counters().sessionsOpened;
    // scripted back-pressure on both ingress publications: retried 10 ms later (S11 N8), none missed
    corePublication(r).p->scriptedResults = { -2, -2, -2 };
    mePublication(r).p->scriptedResults = { -2, -2 };
    r.runFor(30 * SECOND);
    const std::uint64_t coreSent = r.core.counters().keepAlivesSent - core0;
    const std::uint64_t meSent = r.me.counters().keepAlivesSent - me0;
    MESSAGE("keepalives in 30 s: core " << coreSent << ", ME " << meSent);
    CHECK(coreSent >= 29);
    CHECK(coreSent <= 31);
    CHECK(meSent >= 29);
    CHECK(meSent <= 31);
    CHECK(r.core.counters().keepAliveFailures >= 3);
    CHECK(r.core.counters().sessionsOpened + r.me.counters().sessionsOpened == opened0); // no reconnect
}

TEST_CASE("ForwarderAgentTest.coreNewLeaderReRegistersAndResyncsFromZero")
{
    Rig r;
    r.bringUp();
    mePublication(r).p->scriptedResults.assign(100'000, -2); // the ME holds everything back
    r.core_(batchFrame({ create(1, 101), create(2, 102), create(3, 103) }));
    r.cycles(5);
    REQUIRE(r.agent.ring().depth() == 3);
    r.coreT.pushNewLeader(r.coreSession, 4, 1, CORE_ENDPOINTS, 901);
    r.coreTerm = 4;
    r.cycles(10);
    CHECK(r.agent.coreLink() == CoreLink::UNREGISTERED);
    const auto registers = r.coreOffers(80);
    REQUIRE(registers.size() == 2);
    CHECK(registers[1].term == 4); // the new term is stamped
    r.core_(adminAck(decode80(registers[1].payload).correlationId, money::AdminResult::DUPLICATE));
    r.core_(admission(0, 0, 0));
    r.cycles(5);
    CHECK(r.agent.coreLink() == CoreLink::REGISTERED);
    CHECK(r.agent.ring().depth() == 0); // RESYNC(0) cleared the ring
    CHECK(r.agent.resync().counters().ringClearedEntries == 3);
    CHECK(r.agent.resync().counters().resyncs[1] == 2); // T1 again
    const Request65 probe = r.last65();
    CHECK(probe.max == 0);
    r.core_(pendingEnd(probe.correlationId, 0, 3));
    r.cycles(3);
    const Request65 page = r.last65();
    CHECK(page.from == 0);
    CHECK(page.max == 4096);
}

TEST_CASE("ForwarderAgentTest.staleTermFrameAfterNewLeaderIsDropped")
{
    Rig r;
    r.bringUp();
    r.coreT.pushNewLeader(r.coreSession, 4, 1, CORE_ENDPOINTS, 901);
    r.coreTerm = 4;
    r.cycles(10);
    const auto registers = r.coreOffers(80);
    REQUIRE(registers.size() == 2);
    r.core_(adminAck(decode80(registers[1].payload).correlationId, money::AdminResult::DUPLICATE));
    r.core_(admission(0, 0, 0));
    r.cycles(5);
    const Request65 probe = r.last65();
    r.core_(pendingEnd(probe.correlationId, 0, 3));
    r.cycles(3);
    const Request65 page = r.last65();
    // the new leader's reply [1, 3] with a deposed leader's (term 3) live entry 2 between its entries (FIX-1)
    r.core_(dispatchFrame(create(1, 101)), 4);
    r.core_(dispatchFrame(create(2, 102)), 3);
    r.core_(dispatchFrame(create(3, 103)), 4);
    r.core_(pendingEnd(page.correlationId, 2, 3), 4);
    r.cycles(5);
    CHECK(r.agent.counters().coreStaleTerm == 1);
    const auto commands = ofTemplate(r.meOffers(), 9);
    std::vector<std::int64_t> ids;
    for (const Offered &o : commands)
    {
        ids.push_back(commandIdHighOf(o.payload));
    }
    CHECK(ids == std::vector<std::int64_t>{ 101, 103 }); // without rule S the block is [2, 3]: 101 never sent
    CHECK(r.agent.resync().mode() == fd::Mode::LIVE);
}

TEST_CASE("ForwarderAgentTest.meNewLeaderResyncsFromZero")
{
    Rig r;
    r.bringUp();
    r.core_(batchFrame({ create(1, 101), create(2, 102) }));
    r.cycles(5);
    REQUIRE(ofTemplate(r.meOffers(), 9).size() == 2);
    CHECK(r.agent.resync().counters().resyncs[2] == 0);
    r.meT.pushNewLeader(r.meSession, 8, 1, ME_ENDPOINTS, 951);
    r.meTerm = 8;
    r.cycles(5);
    CHECK(r.agent.resync().counters().resyncs[2] == 1); // T2
    CHECK(r.agent.resync().mode() == fd::Mode::RESYNCING);
    const Request65 page = r.last65();
    CHECK(page.from == 0);
    // both commands may have been truncated by the ME election: the reply re-sends them, to the new leader
    r.core_(batchFrame({ create(1, 101), create(2, 102) }));
    r.core_(pendingEnd(page.correlationId, 2, 2));
    r.cycles(5);
    const auto onNewLeader = mePublication(r, "10.0.1.2:9102");
    std::vector<std::int64_t> ids;
    for (const Bytes &frame : onNewLeader.p->offered)
    {
        std::uint16_t smh;
        std::memcpy(&smh, frame.data() + 2, 2);
        if (smh == cc::SessionMessageHeader::sbeTemplateId())
        {
            ids.push_back(commandIdHighOf(Bytes(frame.begin() + 32, frame.end())));
        }
    }
    CHECK(ids == std::vector<std::int64_t>{ 101, 102 });
}

TEST_CASE("ForwarderAgentTest.lost108IsReissued")
{
    Rig r;
    r.bringUp();
    r.agent.requestResync(); // T4
    r.cycles(2);
    const Request65 first = r.last65();
    r.runFor(5 * SECOND + 10 * MS); // no core frame at all
    const Request65 again = r.last65();
    CHECK(again.correlationId != first.correlationId);
    CHECK(again.from == first.from);
    CHECK(r.agent.resync().counters().timeouts == 1);
    r.core_(pendingEnd(first.correlationId, 0, 0)); // the late old 108
    r.cycles(3);
    CHECK(r.agent.resync().mode() == fd::Mode::RESYNCING);
    CHECK(r.agent.resync().counters().staleEnds == 1);
    r.core_(pendingEnd(again.correlationId, 0, 0));
    r.cycles(3);
    CHECK(r.agent.resync().mode() == fd::Mode::LIVE);

    // a 108 delayed 8 s while other core frames keep arriving (egress back-pressure) is not re-issued
    r.agent.requestResync();
    r.cycles(2);
    const Request65 slow = r.last65();
    for (int i = 0; i < 8; i++)
    {
        r.core_(admission(5, 1, 1)); // a per-market state: a core frame, not a trigger
        r.runFor(SECOND);
    }
    CHECK(r.last65().correlationId == slow.correlationId);
    CHECK(r.agent.resync().counters().timeouts == 1);
    r.core_(pendingEnd(slow.correlationId, 0, 0));
    r.cycles(3);
    CHECK(r.agent.resync().mode() == fd::Mode::LIVE);
}

TEST_CASE("ForwarderAgentTest.meBackPressureNeverRetriesInPlace")
{
    Rig r;
    r.bringUp();
    auto pub = mePublication(r);
    pub.p->scriptedResults.assign(1000, -2);
    r.core_(batchFrame({ create(1, 101), create(2, 102), create(3, 103) }));
    r.cycles(2);
    for (int i = 0; i < 20; i++)
    {
        const int offers0 = r.meT.offerCalls;
        const int polls0 = r.coreT.pollCalls;
        r.cycle();
        const int attempts = r.meT.offerCalls - offers0;
        CHECK(attempts <= 2); // one drain attempt (+ at most one keepalive retry)
        CHECK(r.coreT.pollCalls - polls0 == 1); // core egress keeps being polled every cycle
    }
    CHECK(r.agent.ring().depth() == 3); // nothing popped
    CHECK(r.agent.counters().meBackPressure >= 20);
    pub.p->scriptedResults.clear();
    r.cycles(3);
    CHECK(r.agent.ring().depth() == 0);
    CHECK(ofTemplate(r.meOffers(), 9).size() == 3);
}

TEST_CASE("ForwarderAgentTest.invalidDispatchReachesTheMe")
{
    Rig r;
    r.bringUp();
    Dispatch valid = create(1, 101);
    Dispatch amend; // class I: B5 (expectedOldRemaining 0 on an AMEND)
    amend.dispatchSeq = 2;
    amend.commandIdHigh = 102;
    amend.commandIdLow = (3 << 2) | 2;
    amend.kind = 2;
    amend.userId = 7;
    amend.omsOrderId = 102;
    amend.oldOrderId = 555;
    amend.marketId = 3;
    amend.price = 10;
    amend.quantity = 2;
    Dispatch carrier; // class C: the GoldenMoney E2E CREATE with userId 0 (R2)
    carrier.dispatchSeq = 3;
    carrier.commandIdHigh = 1001;
    carrier.commandIdLow = (3 << 2) | 0;
    carrier.kind = 0;
    carrier.userId = 0;
    carrier.omsOrderId = 1001;
    carrier.marketId = 7;
    carrier.side = 1;
    carrier.price = 700'000'001;
    carrier.quantity = 800'000'001;
    carrier.budget = 900'000'001;
    carrier.timeInForce = 1;
    Dispatch cancel = create(4, 104);
    cancel.kind = 1;
    cancel.commandIdLow = (1 << 2) | 1;
    cancel.oldOrderId = 9;
    r.core_(batchFrame({ valid, amend, carrier, cancel }));
    r.cycles(5);
    const auto commands = ofTemplate(r.meOffers(), 9);
    REQUIRE(commands.size() == 4); // every entry, in seq order
    CHECK(commandIdHighOf(commands[0].payload) == 101);
    CHECK(commands[1].payload == encoded(fd::verbatimCommand(amend))); // class I verbatim
    CHECK(commands[2].payload == JAVA_DURABLE_ORDER_COMMAND_V12_CARRIER); // class C as the carrier
    CHECK(commandIdHighOf(commands[3].payload) == 104);
    CHECK(r.agent.counters().invalid.byClass(fd::DispatchClass::ME_INVALID) == 1);
    CHECK(r.agent.counters().invalid.byClass(fd::DispatchClass::CARRIER) == 1);
    CHECK(r.agent.counters().invalid.counts[2][static_cast<unsigned>(fd::RuleId::B5)][static_cast<unsigned>(fd::DispatchClass::ME_INVALID)] == 1);
    CHECK(r.agent.counters().invalid.counts[0][static_cast<unsigned>(fd::RuleId::R2)][static_cast<unsigned>(fd::DispatchClass::CARRIER)] == 1);
}

TEST_CASE("ForwarderAgentTest.releaseIdValidationPerKind")
{
    Rig r;
    r.bringUp();
    std::vector<Dispatch> entries;
    entries.push_back(releaseOf(1, 201));
    Dispatch scope2 = releaseOf(2, 202);
    scope2.releaseScope = 2;
    entries.push_back(scope2);
    Dispatch price = releaseOf(3, 203);
    price.price = 5;
    entries.push_back(price);
    Dispatch user = releaseOf(4, 204);
    user.userId = 9;
    entries.push_back(user);
    Dispatch oms = releaseOf(5, 205, 1);
    oms.omsOrderId = 7;
    entries.push_back(oms);
    entries.push_back(releaseOf(6, 206));
    r.core_(batchFrame(entries));
    r.cycles(5);
    const auto frames = ofTemplate(r.meOffers(), 10);
    REQUIRE(frames.size() == 3);
    const ReleaseFrame f1 = decodeRelease(frames[0].payload);
    const ReleaseFrame f2 = decodeRelease(frames[1].payload);
    const ReleaseFrame f3 = decodeRelease(frames[2].payload);
    CHECK(f1.count == 1);
    CHECK(f1.releaseSeq == 1);
    CHECK(f2.count == 1); // RX alone: the ME decode-drops only this frame
    CHECK(f2.releaseSeq == 2);
    CHECK(f2.entries[0].scope == 2);
    CHECK(f3.count == 4); // RA entries travel as normal releases, payload absent from the wire
    CHECK(f3.releaseSeq == 3);
    CHECK(f3.entries[0] == fd::ReleaseEntry{ 203, (2 << 2) | 1, 0 });
    CHECK(f3.entries[2] == fd::ReleaseEntry{ 205, (2 << 2) | 3, 1 });
    CHECK(frames[2].payload.size() == codec::releaseFrameLength(4));
    CHECK(r.agent.counters().invalid.byClass(fd::DispatchClass::RELEASE_REFUSED) == 1);
    CHECK(r.agent.counters().invalid.byClass(fd::DispatchClass::RELEASE_ANOMALY) == 3);
}

TEST_CASE("ForwarderAgentTest.headProbeAtRegistrationAndEveryTenSecondsInLive")
{
    Rig r;
    r.bringUp(5); // goLive checked the C3-entry probe (maxEntries 0)
    const std::size_t before = r.coreOffers(65).size();
    r.runFor(9 * SECOND);
    CHECK(r.coreOffers(65).size() == before);
    r.runFor(SECOND + 10 * MS);
    REQUIRE(r.coreOffers(65).size() == before + 1);
    const Request65 probe = r.last65();
    CHECK(probe.max == 0);
    CHECK(probe.from == 5);
    r.core_(pendingEnd(probe.correlationId, 0, 5));
    r.cycles(3);
    CHECK(r.agent.resync().counters().headProbeCovered == 1);
    CHECK(r.agent.ready() == false); // no ME frame for 10 s: me_egress_stale
    r.me_(heartbeat(EPOCH));
    r.cycles(2);
    CHECK(r.agent.ready());
    r.runFor(10 * SECOND + 10 * MS);
    REQUIRE(r.coreOffers(65).size() == before + 2);
}

TEST_CASE("ForwarderAgentTest.headBehindCursorResyncs")
{
    Rig r;
    r.bringUp(5);
    r.runFor(10 * SECOND + 10 * MS);
    const Request65 probe = r.last65();
    REQUIRE(probe.max == 0);
    r.core_(pendingEnd(probe.correlationId, 0, 7)); // two broadcasts this session never saw
    r.cycles(3);
    CHECK(r.agent.resync().counters().headProbeBehind == 1);
    CHECK(r.agent.resync().counters().resyncs[7] == 1);
    const Request65 page = r.last65();
    CHECK(page.max == 4096);
    CHECK(page.from == 5);
}

TEST_CASE("ForwarderAgentTest.preS8CoreProbeExits78")
{
    Rig r;
    r.connectBoth();
    r.acceptRegistration();
    const Request65 probe = r.last65();
    r.core_(batchFrame({ create(1, 101), create(2, 102), create(3, 103) }));
    r.core_(pendingEnd(probe.correlationId, 3, 3)); // a pre-S8 core answers maxEntries 0 with a page
    r.cycles(3);
    CHECK(r.agent.stopped());
    CHECK(r.agent.stopReason().exitCode == 78);
    CHECK(std::string(r.agent.stopReason().code) == "F15_PRE_S8_CORE");
}

TEST_CASE("ForwarderAgentTest.admissionChangeResyncsAndOriginNeverResyncs")
{
    Rig r;
    r.connectBoth();
    r.acceptRegistration(1, (1u << 0) | (1u << 4)); // ORIGIN: SUSPENDED, ADMIN + ME_CAPACITY
    r.goLive();
    CHECK(r.agent.resync().counters().resyncs[3] == 0); // the ORIGIN only reset the baseline
    r.core_(admission(3, 1, 1u << 4)); // a per-market state: not a trigger
    r.cycles(3);
    CHECK(r.agent.resync().counters().resyncs[3] == 0);
    r.core_(admission(0, 1, 1u << 0)); // BROADCAST: ME_CAPACITY (bit 4) cleared
    r.cycles(3);
    CHECK(r.agent.resync().counters().resyncs[3] == 1);
    CHECK(r.agent.resync().mode() == fd::Mode::RESYNCING);
    CHECK(r.last65().from == 0);
    r.core_(admission(0, 1, 1u << 0)); // equal: nothing
    r.cycles(3);
    CHECK(r.agent.resync().counters().resyncs[3] == 1);
}

TEST_CASE("ForwarderAgentTest.releaseRunGroupedAt48")
{
    Rig r;
    r.bringUp();
    std::vector<Dispatch> entries;
    for (std::int64_t s = 1; s <= 64; s++)
    {
        entries.push_back(releaseOf(s, 300 + s, static_cast<std::uint8_t>(s % 2)));
    }
    r.core_(batchFrame(entries));
    r.cycles(5);
    const auto frames = ofTemplate(r.meOffers(), 10);
    REQUIRE(frames.size() == 2);
    const ReleaseFrame a = decodeRelease(frames[0].payload);
    const ReleaseFrame b = decodeRelease(frames[1].payload);
    CHECK(a.count == 48);
    CHECK(a.releaseSeq == 1);
    CHECK(b.count == 16);
    CHECK(b.releaseSeq == 49);
    CHECK(frames[0].payload.size() == 836);
    CHECK(r.agent.counters().releaseEntriesSent == 64);
    CHECK(r.agent.counters().sentTemplate10 == 2);
}

TEST_CASE("ForwarderAgentTest.sessionLimitErrorRetries")
{
    Rig r;
    r.autoAnswerCore = false;
    r.cycles(30);
    std::int64_t corr = r.coreT.lastConnectCorrelation();
    REQUIRE(corr > 0);
    r.coreT.pushSessionEvent(corr, 0, 0, 0, cc::EventCode::ERROR, "concurrent session limit", CORE_IMAGE);
    r.cycles(5);
    CHECK_FALSE(r.agent.stopped());
    CHECK(r.agent.counters().connectClusterErrors[0] == 1);
    CHECK(r.agent.coreLink() == CoreLink::DOWN);
    r.runFor(900 * MS);
    CHECK(r.coreT.lastConnectCorrelation() == corr); // the backoff (1 s) holds
    r.runFor(200 * MS);
    const std::int64_t retry = r.coreT.lastConnectCorrelation();
    CHECK(retry != corr);
    r.autoAnswerCore = true;
    r.cycles(10);
    CHECK(r.core.isConnected());

    Rig v;
    v.autoAnswerCore = false;
    v.cycles(30);
    corr = v.coreT.lastConnectCorrelation();
    v.coreT.pushSessionEvent(corr, 0, 0, 0, cc::EventCode::ERROR, "invalid client version 0.2.0 local=1.0.0", CORE_IMAGE);
    v.cycles(5);
    CHECK(v.agent.stopped());
    CHECK(v.agent.stopReason().exitCode == 78);
    CHECK(std::string(v.agent.stopReason().code) == "F6_VERSION");
}

TEST_CASE("ForwarderAgentTest.exitSeams")
{
    SUBCASE("AdminAck REJECTED observed: 78")
    {
        Rig r;
        r.connectBoth();
        r.cycles(5);
        const auto registers = r.coreOffers(80);
        REQUIRE(!registers.empty());
        r.core_(adminAck(decode80(registers.back().payload).correlationId, money::AdminResult::REJECTED, 15));
        r.cycles(3);
        CHECK(r.agent.stopReason().exitCode == 78);
        CHECK(std::string(r.agent.stopReason().code) == "F7_REJECTED");
    }
    SUBCASE("REJECTED inferred (3 cluster closes while pending): 75")
    {
        Rig r;
        r.connectBoth();
        for (int i = 0; i < 3 && !r.agent.stopped(); i++)
        {
            r.cycles(5);
            REQUIRE(r.core.isConnected());
            REQUIRE(r.agent.registrar() != nullptr);
            REQUIRE(r.agent.registrar()->state() == client::RegistrationState::PENDING);
            r.coreT.pushSessionEvent(0, r.coreSession, r.coreTerm, 0, cc::EventCode::CLOSED, "closed", CORE_IMAGE);
            r.runFor(9 * SECOND); // backoff 1, 2, 4 s, then the next session
        }
        CHECK(r.agent.stopped());
        CHECK(r.agent.stopReason().exitCode == 75);
        CHECK(std::string(r.agent.stopReason().code) == "F7B_INFERRED");
    }
    SUBCASE("ME epoch change in M3: 78")
    {
        Rig r;
        r.bringUp();
        r.me_(heartbeat(EPOCH + 1));
        r.cycles(3);
        CHECK(r.agent.stopReason().exitCode == 78);
        CHECK(std::string(r.agent.stopReason().code) == "F8_EPOCH");
    }
    SUBCASE("ME epoch 0 in M3: 78")
    {
        Rig r;
        r.bringUp();
        r.me_(heartbeat(0));
        r.cycles(3);
        CHECK(r.agent.stopReason().exitCode == 78);
        CHECK(std::string(r.agent.stopReason().code) == "F8_EPOCH_ZERO");
    }
    SUBCASE("money version 6 frame: 78")
    {
        Rig r;
        r.bringUp();
        r.core_(dispatchFrame(create(1, 101), 6));
        r.cycles(3);
        CHECK(r.agent.stopReason().exitCode == 78);
        CHECK(std::string(r.agent.stopReason().code) == "F9_MONEY_VERSION");
        CHECK(r.meOffers().empty());
    }
    SUBCASE("ME version 11 frame: 78")
    {
        Rig r;
        r.bringUp();
        r.me_(heartbeat(EPOCH, 11));
        r.cycles(3);
        CHECK(r.agent.stopReason().exitCode == 78);
        CHECK(std::string(r.agent.stopReason().code) == "F9_ME_VERSION");
    }
    SUBCASE("OrderReject INVALID for 65: 70")
    {
        Rig r;
        r.bringUp();
        r.core_(orderReject(r.last65().correlationId, 65, money::CoreRejectReason::INVALID));
        r.cycles(3);
        CHECK(r.agent.stopReason().exitCode == 70);
        CHECK(std::string(r.agent.stopReason().code) == "F4_INVALID_65");
    }
    SUBCASE("OrderReject UNREGISTERED_SESSION for 65: re-registration, no stop")
    {
        Rig r;
        r.bringUp();
        r.core_(orderReject(r.last65().correlationId, 65, money::CoreRejectReason::UNREGISTERED_SESSION));
        r.cycles(5);
        CHECK_FALSE(r.agent.stopped());
        CHECK(r.agent.coreLink() == CoreLink::UNREGISTERED);
        CHECK(r.agent.counters().registrationLost == 1);
        const auto registers = r.coreOffers(80);
        REQUIRE(registers.size() == 2);
        CHECK(decode80(registers[1].payload).correlationId != decode80(registers[0].payload).correlationId);
        r.core_(adminAck(decode80(registers[1].payload).correlationId, money::AdminResult::DUPLICATE));
        r.core_(admission(0, 0, 0));
        r.cycles(5);
        CHECK(r.agent.coreLink() == CoreLink::REGISTERED);
        CHECK(r.agent.resync().counters().resyncs[1] == 2);
    }
}

TEST_CASE("ForwarderAgentTest.meEpochWaitReconnects")
{
    Rig r;
    r.cycles(50);
    REQUIRE(r.me.isConnected());
    const std::uint64_t opened = r.me.counters().sessionsOpened;
    r.runFor(5 * SECOND + 10 * MS); // no heartbeat: a busy pre-W9 ME (F-ME-1)
    r.cycles(20);
    CHECK(r.agent.counters().meEpochReconnects == 1);
    CHECK(r.me.counters().sessionsOpened == opened + 1);
    CHECK(r.agent.meLink() == MeLink::UNVERIFIED);
    CHECK(r.coreOffers(80).empty());
}

TEST_CASE("ForwarderAgentTest.sigtermClosesBothSessionsPolitely")
{
    Rig r;
    r.bringUp();
    r.agent.requestStop();
    r.cycle();
    CHECK(r.agent.stopped());
    CHECK(r.agent.stopReason().exitCode == 0);
    CHECK(countFrames(r.coreT, cc::SessionCloseRequest::sbeTemplateId()) == 1);
    CHECK(countFrames(r.meT, cc::SessionCloseRequest::sbeTemplateId()) == 1);
    r.cycles(5); // nothing after the stop
    CHECK(countFrames(r.coreT, cc::SessionCloseRequest::sbeTemplateId()) == 1);
}

TEST_CASE("ForwarderAgentTest.helloInTheSameBatchAsTheOkBinds")
{
    // Review FIX-1 (the reviewer's probe, inverted): the W9 hello is already in the image behind the ME SessionEvent
    // OK, so the poll of the iteration that applies the OK delivers it. The session starts before that poll: it binds,
    // no epoch-wait reconnect, RegisterSession goes out.
    Rig r;
    r.autoAnswerMe = false;
    r.cycles(50);
    REQUIRE(r.core.isConnected());
    const std::int64_t m = r.meT.lastConnectCorrelation();
    REQUIRE(m > 0);
    r.meT.pushSessionEvent(m, ++r.meSession, r.meTerm, 0, cc::EventCode::OK, "", ME_IMAGE);
    r.me_(heartbeat(EPOCH));
    r.cycle();
    CHECK(r.agent.meLink() == MeLink::BOUND);
    CHECK(r.agent.epochBinder().bound());
    r.runFor(6 * SECOND);
    CHECK(r.agent.counters().meEpochReconnects == 0);
    CHECK_FALSE(r.coreOffers(80).empty());
}

TEST_CASE("ForwarderAgentTest.dispatchSeqBelowOneRefusesTheWholeFrame")
{
    // Review NOTE-2: a 105 with one entry of dispatchSeq 0 between valid ones is a malformed frame: nothing of it is
    // sent (no partial send, no carrier), it is counted, and T6 re-asks the core; a 104 with seq -1 likewise.
    Rig r;
    r.bringUp();
    r.core_(batchFrame({ create(1, 101), create(0, 102), create(2, 103) }));
    r.cycles(5);
    CHECK(ofTemplate(r.meOffers(), 9).empty());
    CHECK(r.agent.counters().dispatchSeqBelowOne == 1);
    CHECK(r.agent.resync().counters().resyncs[6] == 1);
    CHECK(r.agent.resync().mode() == fd::Mode::RESYNCING);
    const Request65 page = r.last65();
    CHECK(page.from == 0);
    r.core_(batchFrame({ create(1, 101), create(2, 103) }));
    r.core_(pendingEnd(page.correlationId, 2, 2));
    r.cycles(5);
    CHECK(ofTemplate(r.meOffers(), 9).size() == 2);
    r.core_(dispatchFrame(create(-1, 104)));
    r.cycles(3);
    CHECK(r.agent.counters().dispatchSeqBelowOne == 2);
    CHECK(ofTemplate(r.meOffers(), 9).size() == 2);
}

TEST_CASE("ForwarderAgentTest.readyIsZeroWhileTheRingHeadWaits30s")
{
    // Review NOTE-3: the ME refuses every offer while its egress keeps flowing: ready drops once the head waited 30 s.
    Rig r;
    r.bringUp();
    mePublication(r).p->scriptedResults.assign(1'000'000, -2);
    r.core_(batchFrame({ create(1, 101) }));
    r.cycles(3);
    REQUIRE(r.agent.ring().depth() == 1);
    for (int s = 0; s < 29; s++)
    {
        r.me_(heartbeat(EPOCH));
        r.runFor(SECOND);
    }
    CHECK(0 == (r.agent.notReadyMask() & not_ready::RING_HEAD_STALE));
    for (int s = 0; s < 2; s++)
    {
        r.me_(heartbeat(EPOCH));
        r.runFor(SECOND);
    }
    CHECK(0 != (r.agent.notReadyMask() & not_ready::RING_HEAD_STALE));
    CHECK_FALSE(r.agent.ready());
    mePublication(r).p->scriptedResults.clear();
    r.cycles(3);
    CHECK(r.agent.ring().depth() == 0);
    CHECK(0 == (r.agent.notReadyMask() & not_ready::RING_HEAD_STALE));
}

TEST_CASE("ForwarderAgentTest.readyIsZeroWhileTheCoreClientHasNoLeader")
{
    Rig r;
    r.bringUp();
    r.me_(heartbeat(EPOCH));
    r.cycles(2);
    REQUIRE(r.agent.ready());
    r.coreT.closedImages[CORE_IMAGE] = true; // the core leader's egress image is gone: AWAIT_NEW_LEADER
    r.cycles(2);
    CHECK(r.core.state() == client::ClientState::AWAIT_NEW_LEADER);
    CHECK(r.agent.coreLink() == CoreLink::REGISTERED);
    CHECK(0 != (r.agent.notReadyMask() & not_ready::CORE_NO_LEADER));
    CHECK_FALSE(r.agent.ready());
}

// ======================================================================================================================
// RC2 S13 contract 11.4: the K4 MarketTick producer in the agent (duty steps 4 and 5a, the gate, the ladder resets, E7,
// the 60 reject, known-ignored templates, tick readiness). Timing rule C1: every ME frame is polled at the earliest in
// the iteration after it was produced; every core answer is pushed after the offer it answers was observed.

namespace {

struct Tick
{
    std::int32_t market = 0;
    std::int64_t version = 0;
    std::int64_t bid = 0;
    std::int64_t ask = 0;
    std::int64_t tradePrice = 0;
    std::int64_t tradeId = 0;
    std::int64_t epoch = 0;
    std::int64_t term = 0;
};

Tick decode60(const Offered &o)
{
    Tick t;
    const std::uint8_t *p = o.payload.data();
    std::memcpy(&t.market, p + 8, 4);
    std::memcpy(&t.version, p + 12, 8);
    std::memcpy(&t.bid, p + 20, 8);
    std::memcpy(&t.ask, p + 28, 8);
    std::memcpy(&t.tradePrice, p + 36, 8);
    std::memcpy(&t.tradeId, p + 44, 8);
    std::memcpy(&t.epoch, p + 52, 8);
    t.term = o.term;
    return t;
}

std::vector<Tick> ticksOf(const Rig &r)
{
    std::vector<Tick> out;
    for (const Offered &o : r.coreOffers(60))
    {
        out.push_back(decode60(o));
    }
    return out;
}

ForwarderSnapshot snapOf(const Rig &r)
{
    ForwarderSnapshot s;
    r.agent.fillSnapshot(s);
    return s;
}

std::uint64_t unknownOf(const Rig &r, unsigned cluster)
{
    std::uint64_t n = 0;
    for (std::uint64_t v : r.agent.counters().unknownTemplate[cluster])
    {
        n += v;
    }
    return n;
}

// a one-level book: bid `bid`, ask 2000 (both qty 10)
Bytes oneLevel(std::int32_t market, std::int64_t version, std::int64_t bid)
{
    return bookSnapshot(market, version, { Level{ bid, 10, 1 } }, { Level{ 2000, 10, 1 } });
}

const fd::TickSlotCounters &tickCounters(const Rig &r, int market)
{
    return r.agent.emitter().counters(market);
}

}

TEST_CASE("ForwarderAgentTest.ticksAreOfferedOnTheCoreSessionWithTheBoundEpoch")
{
    // the GoldenMoney JAVA_MARKET_TICK_V7 fields: market 3, the bound epoch 0x2122..28, the book and the trade mark
    const std::int64_t epoch = 0x2122232425262728LL;
    Rig r;
    r.connectBoth(epoch);
    r.acceptRegistration();
    r.goLive();
    r.me_(tradeBatch(3, 0x1112131415161718LL, 1'499'500'003LL));
    r.me_(bookSnapshot(3, 0x0102030405060708LL, { Level{ 1'499'000'001LL, 5, 1 } }, { Level{ 1'500'000'002LL, 5, 1 } }));
    r.cycles(3);
    const auto offers = r.coreOffers(60);
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].payload == JAVA_MARKET_TICK_V7); // byte-equal to the core's own encoder
    CHECK(offers[0].term == r.coreTerm);               // on the core FORWARDER session, this term
    CHECK(decode60(offers[0]).epoch == r.agent.epochBinder().epoch());
    CHECK(r.meOffers().empty());                       // ticks never touch the ME publication
}

TEST_CASE("ForwarderAgentTest.noTickBeforeTheMeEpochIsBound")
{
    Rig r;
    r.cycles(50);
    REQUIRE(r.core.isConnected());
    REQUIRE(r.me.isConnected());
    REQUIRE(r.agent.meLink() == MeLink::UNVERIFIED);
    r.me_(oneLevel(1, 100, 1000)); // the ladder becomes OK, the epoch is not bound
    r.runFor(2 * SECOND);
    const std::size_t tickOffers = r.coreOffers(60).size();
    CHECK( tickOffers == 0 );
    CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::OK);
    CHECK(tickCounters(r, 1).suppressedFor(fd::TickSuppressed::UNBOUND) == 1); // once per episode
    // bound, registered: the held candidate goes when the gate opens, with the bound epoch
    r.me_(heartbeat(EPOCH));
    r.cycles(5);
    r.acceptRegistration();
    const auto ticks = ticksOf(r);
    REQUIRE(ticks.size() == 1);
    CHECK(ticks[0].epoch == EPOCH);
    CHECK(ticks[0].version == 100);
    CHECK(ticks[0].bid == 1000);
}

TEST_CASE("ForwarderAgentTest.noTickWhileUnregisteredOrWithoutCoreLeader")
{
    Rig r;
    r.connectBoth();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(5);
    CHECK(r.coreOffers(60).empty()); // bound but not registered
    CHECK(tickCounters(r, 1).suppressedFor(fd::TickSuppressed::UNREGISTERED) == 1);
    r.acceptRegistration();
    REQUIRE(r.coreOffers(60).size() == 1);
    r.goLive();
    r.runFor(200 * MS);
    r.coreT.closedImages[CORE_IMAGE] = true; // AWAIT_NEW_LEADER while REGISTERED
    r.cycles(2);
    REQUIRE(r.core.state() == client::ClientState::AWAIT_NEW_LEADER);
    r.me_(moveBestBid(1, 100, 101, 1000, 1001));
    r.runFor(500 * MS);
    CHECK(r.coreOffers(60).size() == 1);
    CHECK(tickCounters(r, 1).suppressedFor(fd::TickSuppressed::UNREGISTERED) == 2); // a second episode
}

TEST_CASE("ForwarderAgentTest.aBurstInOnePollIsDecidedAfterThePoll")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 1);
    r.runFor(200 * MS); // the T_cap window is over: the next change is a leading edge
    // one ME flush burst: 12 deltas, each moving the best bid, all in one poll (F-S13-4)
    for (int i = 0; i < 12; i++)
    {
        r.me_(moveBestBid(1, 100 + i, 101 + i, 1000 + i, 1001 + i));
    }
    r.cycle();
    const auto ticks = ticksOf(r);
    REQUIRE(ticks.size() == 2);
    const Tick firstTick = ticks[1];
    const std::int64_t burstNewestBid = 1012;
    CHECK( firstTick.bid == burstNewestBid );
    CHECK(firstTick.version == 112);
    CHECK(tickCounters(r, 1).offeredOn(fd::TickEdge::LEADING) == 2);
    r.runFor(300 * MS);
    CHECK(r.coreOffers(60).size() == 2); // the newest state already went: no trailing copy
}

TEST_CASE("ForwarderAgentTest.tickOfferBackPressureRetriesTheNewestState")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 1);
    r.runFor(200 * MS);
    r.me_(moveBestBid(1, 100, 101, 1000, 1001));
    corePublication(r).p->scriptedResults = { -2 }; // the tick offer is back-pressured
    r.cycle();
    CHECK(r.coreOffers(60).size() == 1);
    CHECK(tickCounters(r, 1).offerFailures == 1);
    CHECK(r.agent.emitter().lastOffered(1).version == 100); // C3: nothing moved on a failed offer
    r.me_(moveBestBid(1, 101, 102, 1001, 1002));
    r.cycle(); // E6: retried at the next iteration (not after T_cap) with the newest state
    const auto ticks = ticksOf(r);
    REQUIRE(ticks.size() == 2);
    CHECK(ticks[1].bid == 1002);
    CHECK(ticks[1].version == 102);
    CHECK(tickCounters(r, 1).offeredOn(fd::TickEdge::RETRY) == 1);
}

TEST_CASE("ForwarderAgentTest.coreReRegistrationReOffersEveryOkMarket")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.me_(oneLevel(2, 200, 1500));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 2);
    for (std::int64_t term = 4; term <= 5; term++) // two core leader changes in one run (C4)
    {
        r.coreT.pushNewLeader(r.coreSession, term, 1, CORE_ENDPOINTS, 901);
        r.coreTerm = term;
        r.cycles(10);
        CHECK(r.agent.coreLink() == CoreLink::UNREGISTERED);
        const auto registers = r.coreOffers(80);
        r.core_(adminAck(decode80(registers.back().payload).correlationId, money::AdminResult::DUPLICATE));
        r.core_(admission(0, 0, 0));
        r.cycles(5);
        REQUIRE(r.agent.coreLink() == CoreLink::REGISTERED);
        const auto ticks = ticksOf(r);
        REQUIRE(ticks.size() == static_cast<std::size_t>(2 + 2 * (term - 3)));
        // E7: the same state again, on the new term, equal and at the same version
        CHECK(ticks[ticks.size() - 2].market == 1);
        CHECK(ticks[ticks.size() - 2].version == 100);
        CHECK(ticks[ticks.size() - 2].term == term);
        CHECK(ticks[ticks.size() - 1].market == 2);
        CHECK(ticks[ticks.size() - 1].version == 200);
    }
    CHECK(tickCounters(r, 1).offeredOn(fd::TickEdge::REOFFER) == 2);
    CHECK(tickCounters(r, 2).offeredOn(fd::TickEdge::REOFFER) == 2);
}

TEST_CASE("ForwarderAgentTest.meSessionEndResetsLaddersAndTheOpenSnapshotRepairs")
{
    Rig r;
    r.bringUp();
    r.me_(tradeBatch(1, 77, 1000));
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 1);
    REQUIRE(r.agent.stitcher().mark(1).tradeId() == 77);
    r.meT.pushSessionEvent(0, r.meSession, r.meTerm, 0, cc::EventCode::CLOSED, "closed", ME_IMAGE);
    r.cycles(3);
    CHECK(r.agent.meLink() == MeLink::DOWN);
    CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::EMPTY); // 4.1 rule 6
    CHECK(r.agent.stitcher().mark(1).tradeId() == 0);
    CHECK(snapOf(r).books[0].version == 0);
    // the reconnect (1 s backoff), the new session's epoch, the open snapshot
    r.runFor(1500 * MS);
    REQUIRE(r.me.isConnected());
    r.me_(heartbeat(EPOCH));
    r.cycles(5);
    REQUIRE(r.agent.meLink() == MeLink::BOUND);
    r.me_(oneLevel(1, 100, 1000)); // the same state again: not a change (the slot survives the session end, 6.0)
    r.cycles(2);
    CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::OK);
    CHECK(r.coreOffers(60).size() == 1);
    r.runFor(200 * MS);
    r.me_(oneLevel(1, 130, 1003)); // above the offered version: offered
    r.cycles(2);
    const auto ticks = ticksOf(r);
    REQUIRE(ticks.size() == 2);
    CHECK(ticks[1].version == 130);
    CHECK(ticks[1].tradeId == 0); // the mark was reset with the session
}

TEST_CASE("ForwarderAgentTest.meNewLeaderKeepsLaddersAndTheNextSnapshotRepairs")
{
    for (int lateBy = 1; lateBy <= 8; lateBy += 7)
    {
        CAPTURE(lateBy);
        Rig r;
        r.bringUp();
        r.me_(oneLevel(1, 100, 1000));
        r.cycles(2);
        REQUIRE(r.coreOffers(60).size() == 1);
        r.runFor(200 * MS);
        r.meT.pushNewLeader(r.meSession, 8, 1, ME_ENDPOINTS, 951);
        r.meTerm = 8;
        r.cycle();
        CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::OK); // D-S13-3: kept across a term change
        CHECK(r.agent.stitcher().ladder(1).version() == 100);
        // the promoted follower's own chain (F-S13-2), one iteration later
        r.meT.pushAppMessage(r.meSession, moveBestBid(1, 90, 130, 1000, 1004), 951, 8);
        r.cycle();
        CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::BROKEN);
        CHECK(r.agent.stitcher().ladder(1).breakCause() == fd::BreakCause::FROM_MISMATCH);
        r.cycles(lateBy - 1);
        const std::size_t before = r.coreOffers(60).size();
        r.meT.pushAppMessage(r.meSession, oneLevel(1, 140, 1005), 951, 8); // W-ME1's snapshot
        r.cycle();
        CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::OK);
        const auto ticks = ticksOf(r);
        REQUIRE(ticks.size() == before + 1); // a tick in the same iteration as the snapshot
        CHECK(ticks.back().version == 140);
        CHECK(ticks.back().bid == 1005);
        CHECK(ticks.back().term == r.coreTerm);
    }
}

TEST_CASE("ForwarderAgentTest.snapshotNineSecondsLateRepairs")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 1);
    r.me_(moveBestBid(1, 90, 120, 1000, 1001)); // a lossy drop at the ME: from_mismatch
    r.cycles(2);
    REQUIRE(r.agent.stitcher().ladder(1).state() == fd::LadderState::BROKEN);
    int notReady = 0;
    r.runLive(9 * SECOND, [&] { notReady += snapOf(r).ticksReady ? 0 : 1; });
    CHECK(notReady == 0); // 9 s BROKEN is below the 30 s BOOK_STALE threshold
    CHECK(r.agent.ready());
    CHECK(snapOf(r).books[0].brokenAgeMs >= 9000);
    const std::size_t before = r.coreOffers(60).size();
    r.me_(oneLevel(1, 900, 1009)); // the periodic resnapshot (C2)
    r.cycle();
    CHECK(r.agent.stitcher().ladder(1).state() == fd::LadderState::OK);
    CHECK(r.agent.stitcher().ladder(1).counters().recoveries == 1);
    REQUIRE(r.coreOffers(60).size() == before + 1);
    CHECK(ticksOf(r).back().bid == 1009);
}

TEST_CASE("ForwarderAgentTest.knownMeTemplatesAreNotUnknown")
{
    Rig r;
    r.bringUp();
    r.me_(meOther(23));
    r.me_(meOther(24));
    r.me_(meOther(27));
    r.me_(meOther(99));
    r.me_(oneLevel(1, 100, 1000));
    r.me_(tradeBatch(1, 5, 1000));
    r.cycles(3);
    const std::uint64_t unknownMe = unknownOf(r, fd::CLUSTER_ME);
    CHECK( unknownMe == 1 );
    CHECK(r.agent.counters().unknownTemplate[fd::CLUSTER_ME][99] == 1);
    CHECK(r.agent.counters().meKnownIgnored[0] == 1);
    CHECK(r.agent.counters().meKnownIgnored[1] == 1);
    CHECK(r.agent.counters().meKnownIgnored[2] == 1);
    const std::string metrics = renderPrometheus(snapOf(r));
    CHECK(metrics.find("forwarder_me_frames_total{template=\"24\"} 1\n") != std::string::npos);
}

TEST_CASE("ForwarderAgentTest.bookStaleClearsTicksReadyNotDispatchReady")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 1);
    REQUIRE(r.agent.ready());
    REQUIRE(snapOf(r).ticksReady);
    r.runFor(200 * MS);

    // condition 2: a change the emitter may send and cannot (the core publication refuses it) for >= 5 s
    corePublication(r).p->scriptedResults.assign(1'000'000, -2);
    r.me_(moveBestBid(1, 100, 101, 1000, 1001));
    r.cycle();
    REQUIRE(r.coreOffers(60).size() == 1);
    r.runLive(4500 * MS);
    CHECK(snapOf(r).ticksReady);
    r.runLive(1000 * MS);
    {
        const ForwarderSnapshot snap = snapOf(r);
        CHECK( snap.ticksReady == 0 );
        CHECK(snap.ticksNotReadyMarket == 1);
        CHECK(snap.ticksNotReadyCondition == ticks_not_ready::PENDING_5S);
        CHECK(snap.notReady == 0); // the dispatch /ready mask is untouched (PO-1)
        CHECK(snap.ready);
    }
    corePublication(r).p->scriptedResults.clear();
    r.runLive(100 * MS);
    CHECK(snapOf(r).ticksReady);

    // condition 3: an OK ladder with no snapshot for >= 30 s while the ME is BOUND (the rhythm is dead)
    r.me_(oneLevel(1, 300, 1003));
    r.cycle();
    r.runLive(29 * SECOND);
    CHECK(snapOf(r).ticksReady);
    r.runLive(1500 * MS);
    {
        const ForwarderSnapshot snap = snapOf(r);
        CHECK( snap.ticksReady == 0 );
        CHECK(snap.ticksNotReadyCondition == ticks_not_ready::SNAPSHOT_AGE_30S);
        CHECK(snap.notReady == 0);
        CHECK(r.agent.ready());
    }
    r.me_(oneLevel(1, 300, 1003));
    r.cycle();
    CHECK(snapOf(r).ticksReady);

    // condition 1, twice: BROKEN for >= 30 s; the negative case (BROKEN 9 s) stays ticks-ready
    for (int round = 0; round < 2; round++)
    {
        CAPTURE(round);
        const std::int64_t v = 1000 + 1000 * round;
        r.me_(moveBestBid(1, v - 50, v, 1003, 1004)); // foreign from: BROKEN
        r.cycle();
        REQUIRE(r.agent.stitcher().ladder(1).state() == fd::LadderState::BROKEN);
        r.runLive(9 * SECOND);
        CHECK(snapOf(r).ticksReady);
        r.runLive(21500 * MS);
        {
            const ForwarderSnapshot snap = snapOf(r);
            CHECK( snap.ticksReady == 0 );
            CHECK(snap.ticksNotReadyCondition == ticks_not_ready::BROKEN_30S);
            CHECK(snap.notReady == 0);
            const std::string health = renderHealthJson(snap, false);
            CHECK(health.find("\"ticksReady\":{\"ready\":false,\"market\":1,\"condition\":\"book_stale_broken_30s\"}") !=
                std::string::npos);
            CHECK(renderReady(snap) == "ready\n");
        }
        r.me_(oneLevel(1, v + 100, 1003));
        r.cycle();
        CHECK(snapOf(r).ticksReady);
    }
}

TEST_CASE("ForwarderAgentTest.aQuietMarketStaysReady")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    int seconds = 0;
    int notReady = 0;
    r.runLive(60 * SECOND, [&] {
        if (0 == (++seconds % 10))
        {
            r.me_(oneLevel(1, 100, 1000)); // the periodic resnapshot of a quiet book
        }
        notReady += snapOf(r).ticksReady ? 0 : 1;
    });
    CHECK(notReady == 0);
    CHECK(r.coreOffers(60).size() == 1); // a quiet market offers nothing more
    CHECK(snapOf(r).books[0].pendingAgeMs == 0);
}

TEST_CASE("ForwarderAgentTest.unversionedMarketsStayEmptyAndTicksReady")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    for (int m = 2; m <= 3; m++)
    {
        r.me_(bookSnapshot(m, 0, {}, {})); // rule U: no versioned state at the ME (markets 4, 5 get no frame at all)
    }
    r.cycles(2);
    int seconds = 0;
    r.runLive(60 * SECOND, [&] {
        if (0 == (++seconds % 10))
        {
            r.me_(oneLevel(1, 100, 1000));
            for (int m = 2; m <= 3; m++)
            {
                r.me_(bookSnapshot(m, 0, {}, {}));
            }
        }
    });
    r.cycles(2); // the second-60 frames
    for (int m = 2; m <= 5; m++)
    {
        CAPTURE(m);
        CHECK(r.agent.stitcher().ladder(m).state() == fd::LadderState::EMPTY);
    }
    CHECK(r.agent.stitcher().ladder(2).counters().unversioned == 7);
    const ForwarderSnapshot snap = snapOf(r);
    CHECK( snap.ticksReady == 1 );
    CHECK(r.coreOffers(60).size() == 1); // only market 1 ever ticked
}

TEST_CASE("ForwarderAgentTest.oldTermBookFrameAfterMeNewLeaderIsDropped")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 1000, 1000));
    r.cycles(2);
    r.meT.pushNewLeader(r.meSession, 8, 1, ME_ENDPOINTS, 951);
    r.meTerm = 8;
    r.cycles(2);
    const std::int64_t newTermVersion = 2000;
    r.meT.pushAppMessage(r.meSession, oneLevel(1, newTermVersion, 1001), 951, 8);
    r.cycle();
    REQUIRE(r.agent.stitcher().ladder(1).version() == newTermVersion);
    // the SIGSTOPped old leader's late egress: a term-7 snapshot at v - 10 in the next poll (RC-F1)
    r.meT.pushAppMessage(r.meSession, oneLevel(1, newTermVersion - 10, 999), ME_IMAGE, 7);
    r.cycle();
    const auto ladderVersion = [&](int market) { return r.agent.stitcher().ladder(market).version(); };
    CHECK( ladderVersion(1) == newTermVersion );
    CHECK(r.agent.counters().meStaleTerm == 1);
    CHECK(r.agent.stitcher().ladder(1).counters().versionRegressions == 0);
}

TEST_CASE("ForwarderAgentTest.coreRejectOfATickReRegisters")
{
    Rig r;
    r.bringUp();
    r.me_(oneLevel(1, 100, 1000));
    r.cycles(2);
    REQUIRE(r.coreOffers(60).size() == 1);
    const std::uint64_t unknownCore = unknownOf(r, fd::CLUSTER_CORE);
    r.core_(orderReject(0, 60, money::CoreRejectReason::UNREGISTERED_SESSION));
    r.cycles(5);
    const std::size_t registerOffers = r.coreOffers(80).size();
    CHECK( registerOffers == 2 );
    CHECK(r.agent.coreLink() == CoreLink::UNREGISTERED); // the gate is closed
    CHECK(unknownOf(r, fd::CLUSTER_CORE) == unknownCore);
    CHECK(r.agent.counters().tickRejected[0] == 1);
    r.core_(adminAck(decode80(r.coreOffers(80).back().payload).correlationId, money::AdminResult::ACCEPTED));
    r.core_(admission(0, 0, 0));
    r.cycles(5);
    REQUIRE(r.agent.coreLink() == CoreLink::REGISTERED);
    CHECK(tickCounters(r, 1).offeredOn(fd::TickEdge::REOFFER) == 1); // E7 after the re-registration
    // UNAUTHORIZED_CLASS re-registers too; any other reason is a counted defect, never an unknown template (C8)
    r.core_(orderReject(0, 60, money::CoreRejectReason::INVALID));
    r.cycles(3);
    CHECK(r.agent.counters().tickRejected[2] == 1);
    CHECK(r.agent.coreLink() == CoreLink::REGISTERED);
    CHECK(unknownOf(r, fd::CLUSTER_CORE) == unknownCore);
}

TEST_CASE("ForwarderAgentTest.resyncRequestPrecedesTicksOnTheCorePublication")
{
    // CL-F-3: the core publication accepts exactly one offer per iteration; at the registration the T1 head probe (65)
    // and five held ticks are due in the same iteration: the 65 goes first, the ticks one per iteration after it.
    Rig r;
    r.connectBoth();
    for (int m = 1; m <= 5; m++)
    {
        r.me_(oneLevel(m, 100 * m, 1000 + m));
    }
    r.cycles(3);
    REQUIRE(r.coreOffers(60).empty());
    const std::int64_t corr = decode80(r.coreOffers(80).back().payload).correlationId;
    r.core_(adminAck(corr, money::AdminResult::ACCEPTED));
    r.core_(admission(0, 0, 0));
    bool checked = false;
    for (int i = 0; i < 20; i++)
    {
        corePublication(r).p->scriptedResults.assign(64, -2);
        corePublication(r).p->scriptedResults.front() = 1; // one accepted offer per iteration
        r.cycle();
        if (!checked && r.agent.coreLink() == CoreLink::REGISTERED)
        {
            checked = true;
            CHECK( r.coreOffers(65).size() == 1 );
            CHECK(r.coreOffers(60).empty());
        }
    }
    corePublication(r).p->scriptedResults.clear();
    REQUIRE(checked);
    const auto ticks = ticksOf(r);
    REQUIRE(ticks.size() == 5);
    for (int m = 1; m <= 5; m++)
    {
        CHECK(ticks[static_cast<std::size_t>(m - 1)].market == m); // market order, one per iteration (E6 retries)
    }
}

TEST_CASE("ForwarderAgentTest.tickStepWorkIsBounded")
{
    Rig r;
    r.bringUp();
    std::int64_t version[6] = {};
    std::int64_t bid[6] = {};
    for (int m = 1; m <= 5; m++)
    {
        version[m] = 100;
        bid[m] = 1000;
        r.me_(oneLevel(m, version[m], bid[m]));
    }
    r.cycles(2);
    auto applied = [&] {
        std::uint64_t n = 0;
        for (int m = 1; m <= 5; m++)
        {
            n += r.agent.stitcher().ladder(m).counters().snapshots + r.agent.stitcher().ladder(m).counters().deltas;
        }
        return n;
    };
    for (int iteration = 0; iteration < 50; iteration++)
    {
        const std::uint64_t appliedBefore = applied();
        const std::size_t ticksBefore = r.coreOffers(60).size();
        int frames = 0;
        for (int k = 0; k < 4; k++)
        {
            for (int m = 1; m <= 5; m++)
            {
                r.me_(moveBestBid(m, version[m], version[m] + 1, bid[m], bid[m] + 1));
                version[m]++;
                bid[m]++;
                frames++;
            }
        }
        r.cycle(40 * MS); // each iteration starts a new T_cap window for some markets
        CHECK(r.coreOffers(60).size() - ticksBefore <= 5);
        CHECK(applied() - appliedBefore == static_cast<std::uint64_t>(frames));
    }
    for (int m = 1; m <= 5; m++)
    {
        CHECK(r.agent.stitcher().ladder(m).state() == fd::LadderState::OK);
    }
}
