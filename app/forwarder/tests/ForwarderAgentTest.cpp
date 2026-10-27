// RC2 S12 contract 11.3: ForwarderAgent<ClusterClient<FakeTransport>, ClusterClient<FakeTransport>> with a
// manual clock. Every frame the agent sees is a real money-schema v7 / order-schema v12 encoding; every frame
// it offers is decoded back.
#include "doctest/doctest.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "AgentRig.h"

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
