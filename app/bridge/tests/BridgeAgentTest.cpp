// RC2 S9c 12.2 BridgeAgentTest: the real agent over ClusterClient<FakeTransport> + the StubCore behind it + SimSources,
// a manual clock, egress delivered 1..8 iterations late (checklist C1). The RED column is negative-controls.txt.
#include "doctest/doctest.h"

#include <algorithm>

#include "AppRig.h"

#include "golden/golden_frames.inc"

using namespace ct_bridge_app_test;

namespace {

std::size_t feedFramesBetween(const AppRig &r, long fromStep, long toStep)
{
    std::size_t n = 0;
    for (const auto &f : r.frames)
    {
        n += (f.step > fromStep && f.step < toStep && AppRig::isFeed(f.templateId)) ? 1 : 0;
    }
    return n;
}

std::uint64_t fence(const AppRig &r, FenceTrigger t)
{
    return r.agent->metrics().fenceCount(t);
}

}

TEST_CASE("BridgeAgentTest.registerThenQueryThenStream")
{
    AppRig r(1, {}, 3);
    r.journal(0, 1, 50);
    r.agent->start();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 50; }));
    const auto regs = r.offered(80);
    REQUIRE(!regs.empty());
    CHECK(regs[0]->payload == JAVA_REGISTER_SESSION_BRIDGE); // {BRIDGE, acks|settlements, resumable, 0xB1, sourceEpoch 0}
    const long ack = r.firstDelivered(110);
    REQUIRE(ack > 0);
    const auto queries = r.offered(T_QUERY_FEED_POSITION);
    REQUIRE(!queries.empty());
    const bool queryAfterAck = queries[0]->step >= ack;
    CHECK(queryAfterAck);
    const long report = r.firstDelivered(T_FEED_POSITION_REPORT);
    const long feed = r.firstFeedStepAfter(0);
    REQUIRE(feed > 0);
    const bool feedAfterAck = feed >= ack && feed >= report && report > ack;
    CHECK(feedAfterAck); // REGISTRATION_GATE_OFF RED: "feed offer before the ack"
    CHECK(r.agent->counters().sessionUps == 1);
    CHECK(r.agent->link() == app::CoreLink::C3_REGISTERED);
    CHECK(r.core.gapRejects == 0);
    CHECK(r.core.duplicates == 0);
    CHECK(r.agent->ready());
}

TEST_CASE("BridgeAgentTest.noFeedOfferBeforeNewTermReport")
{
    AppRig r(1, {}, 4);
    SimRecording &rec = r.journal(0, 1, 40);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 40; }));
    r.runFor(200 * MS);
    const long electedAt = r.stepNo;
    r.elect();
    appendJournal(r.archive(0), rec, 41, 80);
    REQUIRE(r.runUntil([&] { return r.core.cursor == 80; }));
    const long ack = r.firstDelivered(110, electedAt);
    const long report = ack > electedAt ? r.firstDelivered(T_FEED_POSITION_REPORT, ack) : -1;
    const long feed = r.firstFeedStepAfter(electedAt);
    const bool waitsForTheNewTermReport = ack > electedAt && report > 0 && feed >= report &&
        0 == feedFramesBetween(r, electedAt, report);
    CHECK(waitsForTheNewTermReport); // FENCE_WAITS_REPORT_OFF RED
    CHECK(fence(r, FenceTrigger::NEW_LEADER) == 1);
    CHECK(r.agent->counters().sessionUps == 2);
    CHECK(r.core.gapRejects == 0);
}

TEST_CASE("BridgeAgentTest.noOfferBetweenPublicationSwapAndFence")
{
    // N-8: a report of the new term processed in the SAME poll right after the NewLeaderEvent must find the fence
    // already applied (else it answers the pre-fence query and the domain acts on it before the fence)
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 30);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 30; }));
    // wait for a periodic query and hold its answer
    const std::size_t queriesBefore = r.offered(T_QUERY_FEED_POSITION).size();
    r.holdEgress = true;
    REQUIRE(r.runUntil([&] { return r.offered(T_QUERY_FEED_POSITION).size() > queriesBefore; }, 20 * SECOND));
    r.runFor(50 * MS);
    const std::uint64_t staleBefore = BridgeMetrics::get(r.agent->metrics().reportStale);
    // the election and, in the same poll, the core's (new term) answer to the outstanding query
    r.term++;
    r.leader = 1;
    r.t.pushNewLeader(r.session, r.term, r.leader, CORE_ENDPOINTS, r.image);
    for (auto &q : r.egressQueue)
    {
        q.term = r.term; // the new leader answers the committed query in its term
        q.due = 0;
    }
    r.holdEgress = false;
    r.pumpEgress(); // the held report, stamped with the new term, behind the event: one poll sees both
    const std::size_t framesBefore = r.frames.size();
    r.step();
    const std::uint64_t staleAfter = BridgeMetrics::get(r.agent->metrics().reportStale);
    CHECK(fence(r, FenceTrigger::NEW_LEADER) == 1);
    const bool fenceBeforeSamePollReport = staleAfter == staleBefore + 1;
    CHECK(fenceBeforeSamePollReport); // FENCE_DEFERRED RED: "report before fence"
    appendJournal(r.archive(0), rec, 31, 60);
    REQUIRE(r.runUntil([&] { return r.core.cursor == 60; }));
    // the first app offer of the new term that is not the RegisterSession is the query, never a feed frame
    std::uint16_t firstNonRegister = 0;
    for (std::size_t i = framesBefore; i < r.frames.size(); ++i)
    {
        if (r.frames[i].term == r.term && 80 != r.frames[i].templateId)
        {
            firstNonRegister = r.frames[i].templateId;
            break;
        }
    }
    CHECK(firstNonRegister == T_QUERY_FEED_POSITION);
    CHECK(r.core.gapRejects == 0);
}

TEST_CASE("BridgeAgentTest.twoLeaderChangesAndTwoSessionLossesInOneRun")
{
    AppRig r(1, {}, 3);
    SimRecording &rec = r.journal(0, 1, 20);
    r.bringUp();
    bool upBeforeAck = false;
    std::uint64_t ups = r.agent->counters().sessionUps;
    auto watch = [&] {
        const std::uint64_t now = r.agent->counters().sessionUps;
        if (now != ups)
        {
            ups = now;
            if (0 == r.ackStep.count({ r.session, r.term }))
            {
                upBeforeAck = true; // onSessionUp in a (session, term) whose ack has not been delivered
            }
        }
        return false;
    };
    auto runTo = [&](std::int64_t cursor) {
        REQUIRE(r.runUntil([&] {
            watch();
            return r.core.cursor == cursor;
        }));
    };
    runTo(20);
    r.elect(); // leader change 1
    appendJournal(r.archive(0), rec, 21, 40);
    runTo(40);
    r.closeSession(); // session loss 1
    appendJournal(r.archive(0), rec, 41, 60);
    runTo(60);
    r.loseImage(); // leader change 2, preceded by an egress image loss: AWAIT_NEW_LEADER first
    r.runFor(300 * MS);
    REQUIRE(r.client->state() == cl::ClientState::AWAIT_NEW_LEADER);
    CHECK(r.agent->link() == app::CoreLink::C4_AWAITING);
    r.elect();
    appendJournal(r.archive(0), rec, 61, 80);
    runTo(80);
    r.closeSession(); // session loss 2
    appendJournal(r.archive(0), rec, 81, 100);
    runTo(100);
    CHECK_FALSE(upBeforeAck); // C4_RESTORES_PREVIOUS RED: "onSessionUp before the new term's ack"
    CHECK(r.agent->counters().sessionUps == 5); // the first C3 entry + 2 leader changes + 2 session losses
    CHECK(r.agent->counters().sessionDowns == 2);
    CHECK(fence(r, FenceTrigger::NEW_LEADER) == 2);
    CHECK(r.client->counters().sessionsOpened == 3);
    CHECK(r.core.gapRejects == 0);
}

TEST_CASE("BridgeAgentTest.stateChangeInsideTheInvokerOnlyRecords")
{
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 10);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 10; }));
    r.loseImage();
    r.step();
    REQUIRE(r.client->state() == cl::ClientState::AWAIT_NEW_LEADER);
    const std::uint64_t downsBefore = r.agent->counters().sessionDowns;
    // a blocked archive call: the invoker runs while the client's NEW_LEADER_TIMEOUT expires
    r.sources.duringDoWork = [&] {
        for (int i = 0; i < 600; ++i)
        {
            r.t.now += 10 * MS;
            r.agent->invoke();
        }
    };
    r.step();
    CHECK(r.client->state() == cl::ClientState::CLOSED);
    const bool noDomainCallInInvoker = 0 == r.agent->domainCallsInsideInvoker();
    CHECK(noDomainCallInInvoker); // LINK_MACHINE_IN_CALLBACK RED: "domain call inside the invoker"
    CHECK(r.agent->counters().sessionDowns == downsBefore);
    r.step();
    CHECK(r.agent->counters().sessionDowns == downsBefore + 1); // delivered in step 2 of the next iteration
    CHECK(r.agent->invokes() == 600);
    appendJournal(r.archive(0), rec, 11, 20);
    REQUIRE(r.runUntil([&] { return r.core.cursor == 20; }));
}

TEST_CASE("BridgeAgentTest.delayedAdminAckStillRegisters")
{
    AppRig r(1);
    r.reg = AppRig::Reg::ANSWER_FIRST_LATE; // the ack of the first send arrives 7 s later, after one re-send
    r.journal(0, 1, 10);
    r.agent->start();
    const bool registeredOnce = r.runUntil([&] { return r.core.cursor == 10; }, 30 * SECOND);
    CHECK(registeredOnce); // RESEND_NEW_CORRELATION RED: the late ack's round was abandoned
    CHECK(r.agent->counters().sessionUps == 1);
    CHECK(r.agent->registrar().counters().resends >= 1);
    CHECK(r.agent->registrar().counters().acksAccepted == 1);
    std::set<std::int64_t> ids(r.registerCorrelations.begin(), r.registerCorrelations.end());
    CHECK(ids.size() == 1); // one round: the re-send keeps the correlation id
}

TEST_CASE("BridgeAgentTest.staleTermReportIsDroppedAndCounted")
{
    AppRig r(1);
    r.journal(0, 1, 10);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 10; }));
    const std::int64_t oldTerm = r.term;
    r.elect();
    REQUIRE(r.runUntil([&] { return app::CoreLink::C3_REGISTERED == r.agent->link(); }));
    r.runFor(500 * MS);
    const std::uint64_t staleBefore = BridgeMetrics::get(r.agent->metrics().reportStale);
    const std::uint64_t invalidBefore = BridgeMetrics::get(r.agent->metrics().reportInvalid);
    const std::uint64_t staleTermBefore = r.agent->counters().coreStaleTerm;
    const std::uint64_t futureTermBefore = r.agent->counters().coreFutureTerm;
    FeedPositionReport old;
    old.correlationId = 1;
    old.meEpoch = EPOCH;
    old.lastAppliedJournalSeq = 3;
    r.inject(reportFrame(old), oldTerm);
    r.inject(reportFrame(old), r.term + 5); // a frame of a later term without its NewLeaderEvent
    r.step();
    CHECK(r.agent->counters().coreStaleTerm == staleTermBefore + 1);
    CHECK(r.agent->counters().coreFutureTerm == futureTermBefore + 1);
    const bool domainNeverSaw = BridgeMetrics::get(r.agent->metrics().reportStale) == staleBefore &&
        BridgeMetrics::get(r.agent->metrics().reportInvalid) == invalidBefore;
    CHECK(domainNeverSaw); // TERM_FILTER_OFF RED
}

TEST_CASE("BridgeAgentTest.lostRegistrationReRegistersAndFences")
{
    AppRig r(1, {}, 2);
    SimRecording &rec = r.journal(0, 1, 20);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 20; }));
    const std::size_t regsBefore = r.registerCorrelations.size();
    r.forgetRegistration(); // the core forgets this session's registration (no NewLeaderEvent announces it)
    appendJournal(r.archive(0), rec, 21, 40);
    const long forgotAt = r.stepNo;
    const bool streamsAgain = r.runUntil([&] { return r.core.cursor == 40; }, 60 * SECOND);
    // after the first reject: no feed offer until the new ack and a report after it
    long firstReject = -1;
    for (const auto &d : r.delivered)
    {
        if (d.step > forgotAt && 101 == d.templateId)
        {
            firstReject = d.step;
            break;
        }
    }
    const long newAck = firstReject > 0 ? r.firstDelivered(110, firstReject) : -1;
    const long report = newAck > 0 ? r.firstDelivered(T_FEED_POSITION_REPORT, newAck) : -1;
    std::size_t refusedOffers = 0;
    for (const auto &f : r.frames)
    {
        refusedOffers += (f.step > firstReject && (report < 0 || f.step < report) && AppRig::isFeed(f.templateId)) ? 1 : 0;
    }
    const bool noFeedBeforeReRegistration = firstReject > 0 && refusedOffers <= 2; // at most the frames in flight
    CHECK(noFeedBeforeReRegistration); // at most the frames in flight between the reject and the new term's report
    CHECK(streamsAgain); // UP_FLAG_NEVER_RESET RED: the re-registration in the same term is never re-upped
    CHECK(r.agent->counters().registrationLost >= 1); // UNREGISTERED_IGNORED RED (the domain's offer window bounds the
                                                      // refused offers either way, so the offer count is not its RED)
    CHECK(fence(r, FenceTrigger::SESSION_REESTABLISHED) >= 1);
    REQUIRE(r.registerCorrelations.size() > regsBefore);
    CHECK(r.registerCorrelations.back() != r.registerCorrelations[regsBefore - 1]); // a new round
    CHECK(newAck > 0);
    CHECK(r.core.gapRejects == 0);
}

TEST_CASE("BridgeAgentTest.unauthorizedClassExits78")
{
    AppRig r(1);
    r.journal(0, 1, 5);
    r.bringUp();
    r.inject(orderRejectFrame(61, 20), r.term);
    r.runFor(100 * MS);
    REQUIRE(r.agent->stopped());
    CHECK(r.agent->stopReason().exitCode == 78);
    CHECK(std::string(r.agent->stopReason().code) == "F10_CORE_REFUSES_CLASS");
}

TEST_CASE("BridgeAgentTest.wrongMoneyVersionExits78")
{
    for (const bool registered : { false, true })
    {
        CAPTURE(registered);
        AppRig r(1);
        r.journal(0, 1, 5);
        if (registered)
        {
            r.bringUp();
        }
        else
        {
            r.agent->start();
            r.reg = AppRig::Reg::SILENT;
            REQUIRE(r.runUntil([&] { return r.client->isConnected(); }));
        }
        Bytes v6 = adminAckFrame(1, 66, 0);
        v6[6] = 6;
        r.inject(v6, r.term);
        r.runFor(100 * MS);
        REQUIRE(r.agent->stopped());
        CHECK(r.agent->stopReason().exitCode == 78);
        CHECK(std::string(r.agent->stopReason().code) == "F10_WRONG_CORE_VERSION");
    }
}

TEST_CASE("BridgeAgentTest.invalidQueryRejectExits70")
{
    AppRig r(1);
    r.journal(0, 1, 5);
    r.bringUp();
    r.inject(orderRejectFrame(7, 15), r.term);
    r.runFor(100 * MS);
    REQUIRE(r.agent->stopped());
    CHECK(r.agent->stopReason().exitCode == 70);
    CHECK(std::string(r.agent->stopReason().code) == "F6_ENCODER_BUG");
}

TEST_CASE("BridgeAgentTest.registrarRejectedExits78")
{
    AppRig r(1);
    r.reg = AppRig::Reg::REJECT;
    r.journal(0, 1, 5);
    r.agent->start();
    r.runFor(5 * SECOND);
    REQUIRE(r.agent->stopped());
    const bool observedRefusalIs78 = 78 == r.agent->stopReason().exitCode;
    CHECK(observedRefusalIs78); // EXIT_CODES_ALL_70 RED: the S9a F4 reading
    CHECK(std::string(r.agent->stopReason().code) == "F8_REJECTED");
    CHECK(r.agent->counters().sessionUps == 0);
}

TEST_CASE("BridgeAgentTest.duplicateIdentityRetriesUntilTheOldSessionTimesOut")
{
    // RC2 S10': a restart right after a kill -9 meets the dead session of its identity, still registered at the core
    // until the core session timeout: REJECTED DUPLICATE_IDENTITY (44) is retried, the bridge registers once it is gone
    AppRig r(1);
    r.reg = AppRig::Reg::DUPLICATE_IDENTITY;
    r.duplicateUntilNs = r.t.now + 25 * SECOND; // the dead session times out 25 s later (an election restarted its clock)
    r.journal(0, 1, 5);
    r.agent->start();
    const bool registeredAfterDuplicate = r.runUntil([&] { return r.core.cursor == 5; }, 45 * SECOND);
    CHECK(registeredAfterDuplicate); // DUPLICATE_IDENTITY_EXITS RED: exit 78 at the first REJECTED
    CHECK_FALSE(r.agent->stopped());
    CHECK(r.agent->counters().duplicateIdentityRetries >= 2);
    CHECK(r.agent->registrar().counters().acksRejected >= 2);
    CHECK(r.agent->registrar().counters().acksAccepted == 1);
    CHECK(r.agent->counters().sessionUps == 1);
}

TEST_CASE("BridgeAgentTest.duplicateIdentityPastTheBoundExits78")
{
    // a genuine duplicate BRIDGE_INSTANCE_ID: the identity stays held; after the 60 s bound (2 x the 10 s core session
    // timeout plus a slow election) exit 78
    AppRig r(1);
    r.reg = AppRig::Reg::DUPLICATE_IDENTITY;
    r.duplicateUntilNs = r.t.now + 3600 * SECOND;
    r.journal(0, 1, 5);
    r.agent->start();
    r.runFor(58 * SECOND);
    CHECK_FALSE(r.agent->stopped()); // still inside the bound
    r.runFor(5 * SECOND);
    REQUIRE(r.agent->stopped());
    CHECK(78 == r.agent->stopReason().exitCode);
    CHECK(std::string(r.agent->stopReason().code) == "F8_DUPLICATE_IDENTITY");
    CHECK(r.agent->counters().sessionUps == 0);
}

TEST_CASE("BridgeAgentTest.inferredRejectedExits75")
{
    AppRig r(1);
    r.reg = AppRig::Reg::SILENT; // a refused session's ack is dropped with its egress queue
    r.journal(0, 1, 5);
    r.agent->start();
    for (int i = 0; i < 3 && !r.agent->stopped(); ++i)
    {
        REQUIRE(r.runUntil([&] { return r.client->isConnected() && !r.registerCorrelations.empty(); }, 60 * SECOND));
        r.runFor(200 * MS);
        r.closeSession();
        r.registerCorrelations.clear();
        r.runFor(200 * MS);
    }
    r.runFor(2 * SECOND);
    REQUIRE(r.agent->stopped());
    CHECK(r.agent->stopReason().exitCode == 75);
    CHECK(std::string(r.agent->stopReason().code) == "F8B_INFERRED");
}

TEST_CASE("BridgeAgentTest.transportErrorOfferExits70")
{
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 5);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 5; }));
    auto &pub = r.t.publications.at(static_cast<std::size_t>(r.t.publicationTo("10.0.0.1:19302")));
    for (int i = 0; i < 50; ++i)
    {
        pub.scriptedResults.push_back(-100); // a transport refusal: TRANSPORT_ERROR
    }
    appendJournal(r.archive(0), rec, 6, 10);
    r.runFor(2 * SECOND);
    REQUIRE(r.agent->stopped());
    CHECK(r.agent->stopReason().exitCode == 70);
    CHECK(std::string(r.agent->stopReason().code) == "F5_DOMAIN_FATAL");
    CHECK(r.agent->domain().fatal() == Fatal::OFFER_REFUSED);
}

TEST_CASE("BridgeAgentTest.domainFatalExits70")
{
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 5);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 5; }));
    r.encoder.failNext = true; // the encoder cannot frame an admitted record: ENCODER_OVERFLOW
    appendJournal(r.archive(0), rec, 6, 6, EPOCH, TEMPLATE_TERMINAL);
    r.runFor(2 * SECOND);
    REQUIRE(r.agent->stopped());
    CHECK(r.agent->stopReason().exitCode == 70);
    CHECK(r.agent->domain().fatal() == Fatal::ENCODER_OVERFLOW);
}

TEST_CASE("BridgeAgentTest.malformedEgressIsCountedNotRelabelled")
{
    // C8: an input problem is counted and the run continues; never relabelled 78 (the codec exception is the duty
    // catch-all's, StopPathTest.anEscapedExceptionIs70)
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 5);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 5; }));
    Bytes shortReport = reportFrame(FeedPositionReport{});
    shortReport[0] = 8; // block length 8 (a v7 17 needs 43)
    shortReport.resize(16);
    const std::uint64_t invalidBefore = BridgeMetrics::get(r.agent->metrics().reportInvalid);
    r.inject(shortReport, r.term);
    appendJournal(r.archive(0), rec, 6, 15);
    const bool stillRunning = r.runUntil([&] { return r.core.cursor == 15; }) && !r.agent->stopped();
    CHECK(stillRunning); // MALFORMED_IS_CONFIG_ERROR RED
    CHECK(BridgeMetrics::get(r.agent->metrics().reportInvalid) == invalidBefore + 1);
}

TEST_CASE("BridgeAgentTest.sessionLossReconnectsWithBackoff")
{
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 10);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 10; }));
    r.answerConnects = false; // connects time out: every cycle exhausts its attempts
    r.closeSession();
    std::vector<std::int64_t> cycleStarts;
    std::uint64_t cycles = r.agent->counters().connectCycles;
    r.runUntil([&] {
        if (r.agent->counters().connectCycles != cycles)
        {
            cycles = r.agent->counters().connectCycles;
            cycleStarts.push_back(r.t.now);
        }
        return cycleStarts.size() >= 4;
    }, 300 * SECOND, 50 * MS);
    REQUIRE(cycleStarts.size() >= 4);
    const bool sessionDownOnce = 1 == r.agent->counters().sessionDowns;
    CHECK(sessionDownOnce); // RECONNECT_WITHOUT_SESSION_DOWN RED: the domain keeps its query
    // the waits between cycles grow (1, 2, 4 s backoff after each exhausted cycle)
    std::vector<std::int64_t> gaps;
    for (std::size_t i = 1; i < cycleStarts.size(); ++i)
    {
        gaps.push_back(cycleStarts[i] - cycleStarts[i - 1]);
    }
    CHECK(gaps[1] - gaps[0] >= 900 * MS);
    CHECK(gaps[2] - gaps[1] >= 1900 * MS);
    r.answerConnects = true;
    appendJournal(r.archive(0), rec, 11, 20);
    REQUIRE(r.runUntil([&] { return r.core.cursor == 20; }, 120 * SECOND));
    CHECK(r.agent->counters().sessionUps == 2);
    CHECK(r.agent->counters().sessionLosses[2] == 1);
}

TEST_CASE("BridgeAgentTest.keptHaltReadsHaltedAcrossTheSessionLoss")
{
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 10);
    appendJournal(r.archive(0), rec, 12, 20); // seq 11 is missing on the only source: GAP
    r.bringUp();
    REQUIRE(r.runUntil([&] { return BridgeHalt::GAP == r.agent->domain().view().halt; }, 120 * SECOND));
    r.closeSession();
    bool alwaysHalted = true;
    r.runUntil([&] {
        app::BridgeSnapshot s;
        r.agent->fillSnapshot(s);
        alwaysHalted = alwaysHalted && app::healthHalted(s);
        return app::CoreLink::C3_REGISTERED == r.agent->link() && r.agent->counters().sessionUps == 2;
    }, 60 * SECOND);
    CHECK(alwaysHalted);
    CHECK(r.agent->counters().sessionDowns == 1);
    CHECK(BridgeHalt::GAP == r.agent->domain().view().halt);
}

TEST_CASE("BridgeAgentTest.offerStatusMapping")
{
    using cl::OfferStatus;
    CHECK(app::mapOfferStatus(OfferStatus::OK) == OfferOutcome::OK);
    CHECK(app::mapOfferStatus(OfferStatus::BACK_PRESSURED) == OfferOutcome::RETRY);
    CHECK(app::mapOfferStatus(OfferStatus::NOT_CONNECTED) == OfferOutcome::RETRY);
    CHECK(app::mapOfferStatus(OfferStatus::NOT_READY) == OfferOutcome::RETRY);
    CHECK(app::mapOfferStatus(OfferStatus::CLOSED) == OfferOutcome::CLOSED);
    const bool maxPositionCloses = app::mapOfferStatus(OfferStatus::MAX_POSITION_EXCEEDED) == OfferOutcome::CLOSED;
    CHECK(maxPositionCloses); // MAX_POSITION_IS_RETRY RED
    CHECK(app::mapOfferStatus(OfferStatus::TRANSPORT_ERROR) == OfferOutcome::REFUSED);
    // CLOSED pauses reads until the session comes back
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 5);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 5; }));
    auto &pub = r.t.publications.at(static_cast<std::size_t>(r.t.publicationTo("10.0.0.1:19302")));
    pub.scriptedResults.push_back(cl::OFFER_PUBLICATION_CLOSED);
    appendJournal(r.archive(0), rec, 6, 10);
    REQUIRE(r.runUntil([&] { return 0 == r.agent->domain().fragmentLimit(); }, 5 * SECOND));
    CHECK(r.agent->counters().offerRetry[1] == 0);
}

TEST_CASE("BridgeAgentTest.egressDispatchOrder")
{
    AppRig r(1);
    r.journal(0, 1, 5);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 5; }));
    const std::uint64_t ignoredAcks = r.agent->registrar().counters().acksIgnored;
    r.inject(adminAckFrame(424242, 80, 0), r.term);      // a RegisterSession ack of another round: the registrar's
    r.inject(adminAckFrame(1, 66, 1), r.term);           // a FeedHalt ack: the domain's (no FeedHalt outstanding)
    r.inject(settlementAppliedFrame(99), r.term);        // 13: the ack map (unknown trade)
    r.inject(orderRejectFrame(1, 9), r.term);            // an OrderReject naming a money command: ignored
    r.inject(orderRejectFrame(61, 18), r.term);          // naming a feed template, another reason: counted
    Bytes other = adminAckFrame(1, 66, 0);
    other[2] = 102; // OrderEvent 102 (any template the bridge does not consume)
    r.inject(other, r.term);
    r.step();
    CHECK(r.agent->registrar().counters().acksIgnored == ignoredAcks + 1);
    CHECK(r.agent->ackMap().unknown() == 1);
    CHECK(r.agent->counters().egressIgnored[101] == 1);
    CHECK(r.agent->counters().orderReject[18] == 1);
    CHECK(r.agent->counters().egressIgnored[102] == 1);
    CHECK_FALSE(r.agent->stopped());
}

TEST_CASE("BridgeAgentTest.readyBitsEachFail")
{
    using namespace app::not_ready;
    AppRig r(1);
    SimRecording &rec = r.journal(0, 1, 10);
    CHECK(0 != (r.agent->notReadyMask() & CORE_NOT_CONNECTED));
    r.agent->start();
    r.reg = AppRig::Reg::SILENT;
    REQUIRE(r.runUntil([&] { return app::CoreLink::C2_CONNECTED == r.agent->link(); }));
    CHECK(0 != (r.agent->notReadyMask() & CORE_NOT_REGISTERED));
    CHECK(0 != (r.agent->notReadyMask() & NO_REPORT_THIS_TERM));
    r.reg = AppRig::Reg::ACCEPT;
    REQUIRE(r.runUntil([&] { return r.core.cursor == 10 && r.agent->ready(); }, 30 * SECOND));
    // report_stale: the periodic answers stop for > 30 s
    r.holdEgress = true;
    r.runFor(15 * SECOND, 100 * MS); // the last accepted report is at most 10 s + 15 s old
    CHECK(0 == (r.agent->notReadyMask() & REPORT_STALE));
    r.runFor(20 * SECOND, 100 * MS); // at least 35 s
    CHECK(r.agent->snapshotReportAgeMs() > 30'000);
    const bool reportAgeBit = 0 != (r.agent->notReadyMask() & REPORT_STALE);
    CHECK(reportAgeBit); // READY_IGNORES_REPORT_AGE RED
    r.holdEgress = false;
    REQUIRE(r.runUntil([&] { return 0 == (r.agent->notReadyMask() & REPORT_STALE); }, 30 * SECOND));
    // core_no_leader
    r.loseImage();
    r.step();
    CHECK(0 != (r.agent->notReadyMask() & CORE_NO_LEADER));
    r.elect();
    REQUIRE(r.runUntil([&] { return r.agent->ready(); }, 30 * SECOND));
    // stopping
    r.agent->requestStop();
    CHECK(0 != (r.agent->notReadyMask() & STOPPING));
    (void)rec;
}

TEST_CASE("BridgeAgentTest.readySkipWalkIsProgress")
{
    // FIX-10: a cold restart behind applied records is a skip walk at full speed and stays READY; backlog without the
    // consumed position moving is not
    using namespace app::not_ready;
    AppRig r(1);
    r.core.bound = EPOCH;
    r.core.cursor = 1500;
    r.journal(0, 1, 2000);
    r.bringUp();
    r.archiveMetrics.sourceBacklogBytes.store(1'000'000);
    bool readyDuringWalk = true;
    for (int i = 0; i < 40; ++i) // 40 s at 1 s per iteration: 32 skips per iteration, no forward before seq 1501
    {
        r.step(SECOND);
        r.archiveMetrics.sourceBacklogBytes.store(1'000'000);
        if (i > 31 && r.core.cursor == 1500)
        {
            readyDuringWalk = readyDuringWalk && 0 == (r.agent->notReadyMask() & BACKLOG_WITHOUT_PROGRESS);
        }
    }
    CHECK(readyDuringWalk); // READY_COUNTS_FORWARDS_ONLY RED: the skip walk reads not ready
    REQUIRE(r.runUntil([&] { return r.core.cursor == 2000; }, 60 * SECOND));
    r.runFor(31 * SECOND, 500 * MS); // nothing moves for 31 s
    r.step();
    r.archiveMetrics.sourceBacklogBytes.store(1'000'000); // while a backlog is claimed
    CHECK(0 != (r.agent->notReadyMask() & BACKLOG_WITHOUT_PROGRESS));
}

TEST_CASE("BridgeAgentTest.everyFaultKindIsCounted")
{
    struct Spy final : FollowObserver
    {
        std::vector<FaultKind> faults;
        int attaches = 0;
        void onFault(const SourceFault &f) noexcept override { faults.push_back(f.kind); }
        void onAttached(const AttachInfo &) noexcept override { attaches++; }
    } spy;
    app::ObserverTap tap;
    tap.bind(&spy);
    std::vector<FaultKind> sent;
    for (std::size_t k = 1; k < app::FAULT_KINDS; ++k)
    {
        SourceFault f;
        f.source = k % 2;
        f.kind = static_cast<FaultKind>(k);
        tap.onFault(f);
        sent.push_back(f.kind);
    }
    for (std::size_t k = 1; k < app::FAULT_KINDS; ++k)
    {
        CAPTURE(k);
        CHECK(tap.faults()[k] == 1);
    }
    const bool forwardedInOrder = spy.faults == sent;
    CHECK(forwardedInOrder); // TAP_SWALLOWS RED
    CHECK(tap.lastFault(1) == FaultKind::ARCHIVE_ERROR);
    SourceFault gone;
    gone.source = 0;
    gone.kind = FaultKind::ARCHIVE_GONE;
    tap.onFault(gone);
    CHECK(tap.sourceIsDead(0));
    AttachInfo info;
    info.source = 0;
    tap.onAttached(info);
    CHECK_FALSE(tap.sourceIsDead(0));
    CHECK(spy.attaches == 1);
}

TEST_CASE("BridgeAgentTest.mtuRefusedEverywhereExits78")
{
    AppRig r(2);
    r.journal(0, 1, 5);
    r.journal(1, 1, 5);
    r.bringUp();
    r.archiveMetrics.sourceRefusedMtu[0].store(1);
    r.step();
    CHECK_FALSE(r.agent->stopped()); // one refused source: alerted, not fatal
    r.archiveMetrics.sourceRefusedMtu[1].store(1);
    r.step();
    r.step();
    REQUIRE(r.agent->stopped());
    CHECK(r.agent->stopReason().exitCode == 78);
    CHECK(std::string(r.agent->stopReason().code) == "C2_MTU");
}

TEST_CASE("BridgeAgentTest.negctlDropsOutcomesAndTerminals")
{
    // ct_bridge_negctl (the A02-T2 control, used by S10' / S15): records 2 and 3 never reach the domain
    // (smoke-tested here with the seam forced on: the run below is only meaningful under CT_TEST_DISABLE_NEGCTL_...)
    AppRig r(1);
    SimRecording &rec = r.archive(0).record();
    appendJournal(r.archive(0), rec, 1, 5);
    appendJournal(r.archive(0), rec, 6, 6, EPOCH, TEMPLATE_TERMINAL);
    appendJournal(r.archive(0), rec, 7, 9);
    r.bringUp();
    r.runFor(3 * SECOND);
    if (app::app_seams::disabled(app::app_seams::Rule::NEGCTL_DROP_RECORDS_2_3))
    {
        CHECK(r.core.cursor == 5); // seq 6 (a terminal) was dropped: the domain sees a hole at 6
    }
    else
    {
        CHECK(r.core.cursor == 9);
    }
}
