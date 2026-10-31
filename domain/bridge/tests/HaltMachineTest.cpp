// HaltMachineTest (RC2 S9b 13.7): the halt machine of section 9 (cpp 2.7): the FeedHalt fields, H2, the re-offers,
// the REJECTED fallback, the exits and the resumption rule with a kept halt.
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

using namespace ct_bridge_test;

namespace {

void bindAt(Direct &d, std::int64_t c)
{
    d.core.bound = EPOCH;
    d.core.cursor = c;
    d.start();
    REQUIRE(d.domain->view().state == State::STREAMING);
}

// one source, C = 10, a hole at 11 (the next frame is 12): GAP {11, 12}
void gapHalt(Direct &d)
{
    bindAt(d, 10);
    REQUIRE(d.rec(TEMPLATE_TRADE, 12) == SinkAction::ABORT);
    d.pump(); // the confirming report, then the FeedHalt's AdminAck
    REQUIRE(d.domain->view().state == State::HALTED);
    REQUIRE(d.domain->view().halt == BridgeHalt::GAP);
}

void runFor(Direct &d, std::chrono::milliseconds total, std::chrono::milliseconds step = 100ms)
{
    for (auto t = 0ms; t < total; t += step)
    {
        d.tick(step);
    }
}

SimRecording &run(SimArchive &a, std::int64_t from, std::int64_t to, bool stop, std::int64_t epoch = EPOCH)
{
    SimRecording &r = a.record();
    appendJournal(a, r, from, to, epoch);
    if (stop)
    {
        a.stop(r);
    }
    return r;
}

bool halted(Rig &rig)
{
    return rig.domain->view().state == State::HALTED;
}

}

TEST_CASE("HaltMachineTest.feedHaltFieldsObeyD3a")
{
    // the 9.2 table satisfies the core's field rules for every reason
    const FeedHaltFields gap = feedHaltFor(BridgeHalt::GAP, 11, 12, EPOCH, 0);
    const FeedHaltFields epoch = feedHaltFor(BridgeHalt::EPOCH_MISMATCH, 11, 11, EPOCH, 9);
    const FeedHaltFields schema = feedHaltFor(BridgeHalt::SOURCE_SCHEMA, 1, 0, 0, 0);
    const bool valid = feedHaltFieldsValid(gap) && feedHaltFieldsValid(epoch) && feedHaltFieldsValid(schema);
    CHECK(valid);
    CHECK(gap.reason == HaltReason::GAP);
    CHECK(gap.expected == 11);
    CHECK(gap.seen == 12);
    CHECK(epoch.meEpochSeen == 9);
    CHECK(schema.expected == 1);
    CHECK(schema.seen == 1);
    CHECK(feedHaltFieldsValid(fallbackFeedHalt(1, 0)));
    // and the stub core (D3a) accepts what the domain sends
    Direct d;
    gapHalt(d);
    REQUIRE(d.core.feedHalts.size() == 1);
    CHECK(d.core.latch == static_cast<std::uint8_t>(HaltReason::GAP));
    CHECK(d.domain->halt().acked);
}

TEST_CASE("HaltMachineTest.preHaltStagedIsFlushedFirst")
{
    // H2: a source defect with trades staged: the trades go before the FeedHalt
    Direct d;
    bindAt(d, 10);
    for (std::int64_t s = 11; s <= 13; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    SourceFault f;
    f.source = 0;
    f.kind = FaultKind::MALFORMED_EXTENT;
    d.domain->onFault(f);
    d.pump();
    REQUIRE(d.domain->view().state == State::HALTED);
    int batchAt = -1, haltAt = -1;
    for (std::size_t i = 0; i < d.core.offered.size(); ++i)
    {
        if (T_ME_JOURNAL_TRADE_BATCH == d.core.offered[i].moneyTemplate)
        {
            batchAt = static_cast<int>(i);
        }
        if (T_FEED_HALT == d.core.offered[i].moneyTemplate && haltAt < 0)
        {
            haltAt = static_cast<int>(i);
        }
    }
    const bool flushedFirst = batchAt >= 0 && haltAt > batchAt;
    CHECK(flushedFirst);
    CHECK(d.core.cursor == 13);
    REQUIRE(!d.core.feedHalts.empty());
    CHECK(d.core.feedHalts.front().reason == HaltReason::SOURCE_SCHEMA);
    CHECK(d.core.feedHalts.front().expected == 14);
}

TEST_CASE("HaltMachineTest.feedHaltResentAfterReconnectAndNewLeader")
{
    // R-B4-5 (domain half of S9c's feedHaltResentAfterLeaderChange)
    Direct d;
    gapHalt(d);
    CHECK(d.feedHaltOffers() == 1);
    d.domain->onSessionDown(d.now);
    d.domain->onSessionUp(d.now);
    d.pump();
    CHECK(d.domain->view().state == State::HALTED);
    d.domain->onFence(FenceTrigger::NEW_LEADER, d.now);
    d.domain->onSessionUp(d.now);
    d.pump();
    const int halts = d.feedHaltOffers();
    CHECK(halts == 3);
    CHECK(d.domain->view().halt == BridgeHalt::GAP);
}

TEST_CASE("HaltMachineTest.feedHaltResentUntilAcked")
{
    Direct d;
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.start();
    REQUIRE(d.rec(TEMPLATE_TRADE, 12) == SinkAction::ABORT);
    while (d.pumpOne())
    {
        // drop every AdminAck: none comes back
        while (!d.core.egress.empty() && d.core.egress.front().kind == CoreEgress::ACK)
        {
            d.core.egress.pop_front();
        }
    }
    REQUIRE(d.domain->view().state == State::HALTED);
    for (int i = 0; i < 200; ++i) // 20 s, acks lost
    {
        d.now += 100ms;
        d.domain->tick(d.now);
        while (!d.core.egress.empty())
        {
            if (d.core.egress.front().kind == CoreEgress::ACK)
            {
                d.core.egress.pop_front();
            }
            else
            {
                d.pumpOne();
            }
        }
    }
    const int unacked = d.feedHaltOffers();
    CHECK(unacked >= 4);
    CHECK(unacked <= 6);
    runFor(d, 20'000ms); // acks delivered now: the next re-offer is acked, then no more
    const int afterAck = d.feedHaltOffers();
    runFor(d, 20'000ms);
    CHECK(d.feedHaltOffers() == afterAck);
}

TEST_CASE("HaltMachineTest.rejectedFeedHaltFallsBackToSchemaThenExits")
{
    // FIX-10: a REJECTED means the core did not latch: offer the always-valid SOURCE_SCHEMA {e, e}, then exit 70
    Direct d;
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.core.rejectFeedHalts = 1;
    d.start();
    REQUIRE(d.rec(TEMPLATE_TRADE, 12) == SinkAction::ABORT);
    REQUIRE(d.pumpOne()); // the confirming report: HALTING, FeedHalt GAP offered (REJECTED by the core)
    REQUIRE(d.domain->view().state == State::HALTED);
    REQUIRE(d.pumpOne()); // the REJECTED ack: the fallback is offered
    const Fatal fatalBeforeFallbackAck = d.domain->fatal();
    CHECK(fatalBeforeFallbackAck == Fatal::NONE);
    REQUIRE(d.core.feedHalts.size() == 2);
    CHECK(d.core.feedHalts[1].reason == HaltReason::SOURCE_SCHEMA);
    CHECK(d.core.feedHalts[1].expected == 11);
    CHECK(d.core.feedHalts[1].seen == 11);
    CHECK(d.core.latch == static_cast<std::uint8_t>(HaltReason::SOURCE_SCHEMA)); // the gate is closed
    REQUIRE(d.pumpOne()); // its ACCEPTED ack
    CHECK(d.domain->fatal() == Fatal::FEEDHALT_REJECTED);
    CHECK(BridgeMetrics::get(d.metrics.feedHaltRejected) == 1);
}

TEST_CASE("HaltMachineTest.gapHaltRecoversWhenTheCursorPassesSeen")
{
    Direct d;
    gapHalt(d);
    d.core.cursor = 11; // another lane (or an operator-filled archive) delivered 11: C' >= seen - 1
    runFor(d, 6'000ms);
    const State state = d.domain->view().state;
    CHECK(state == State::STREAMING);
    CHECK(d.domain->view().last == 11);
    CHECK(d.metrics.recovered(BridgeHalt::GAP) == 1);
    CHECK(d.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("HaltMachineTest.gapHaltRescanFindsTheFilledSeq")
{
    Rig rig(2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        run(rig.archive(i), 1, 10, true);
        run(rig.archive(i), 13, 20, false);
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.domain->view().halt == BridgeHalt::GAP);
    // an operator refills source 1's archive with the missing records (a new recording from 11)
    rig.archive(1).recordings.pop_back();
    run(rig.archive(1), 11, 20, false);
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 20; }, 300'000ms));
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    CHECK(rig.domain->view().state == State::STREAMING);
    CHECK(rig.metrics.recovered(BridgeHalt::GAP) == 1);
    CHECK(rig.feedHaltOffers() >= 1);
}

TEST_CASE("HaltMachineTest.schemaAndCoreRefusedResumeWhenTheCorePassesE")
{
    SUBCASE("SOURCE_SCHEMA")
    {
        Rig rig(1);
        SimRecording &r = rig.archive(0).record();
        appendJournal(rig.archive(0), r, 1, 5);
        Bytes bad = makeFrame(TEMPLATE_TRADE, 6, EPOCH, 64 * 6);
        putLe16(bad, 4, 3);
        rig.archive(0).append(r, bad);
        appendJournal(rig.archive(0), r, 7, 12);
        rig.up();
        REQUIRE(rig.runUntil([&] { return halted(rig); }));
        REQUIRE(rig.domain->view().halt == BridgeHalt::SOURCE_SCHEMA);
        rig.core.cursor = 6; // the core decided: an operator 76 skip of 6
        rig.core.latch = 0;
        rig.core.latchSkip = 0;
        rig.runUntil([&] { return rig.domain->view().state == State::STREAMING; }, 30'000ms);
        const bool resumed = rig.domain->view().state == State::STREAMING;
        CHECK(resumed);
        CHECK(rig.metrics.recovered(BridgeHalt::SOURCE_SCHEMA) == 1);
    }
    SUBCASE("CORE_REFUSED")
    {
        Rig rig(1);
        rig.core.refuseSeqs = { 6 }; // a core-only range rule refuses seq 6 (decode poison, no SettleRejected)
        run(rig.archive(0), 1, 12, false);
        rig.up();
        REQUIRE(rig.runUntil([&] { return halted(rig); }, 120'000ms));
        REQUIRE(rig.domain->view().halt == BridgeHalt::CORE_REFUSED);
        rig.core.refuseSeqs.clear();
        rig.core.cursor = 6; // the operator skipped 6
        rig.core.latch = 0;
        rig.core.latchSkip = 0;
        rig.runUntil([&] { return rig.core.cursor == 12; }, 60'000ms);
        const bool resumed = rig.core.cursor == 12;
        CHECK(resumed);
        CHECK(rig.metrics.recovered(BridgeHalt::CORE_REFUSED) == 1);
    }
}

TEST_CASE("HaltMachineTest.epochHaltStaysParked")
{
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 3;
    SimRecording &r = rig.archive(0).record();
    appendJournal(rig.archive(0), r, 1, 3, EPOCH);
    for (std::int64_t s = 4; s <= 10; ++s)
    {
        rig.archive(0).append(r, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.domain->view().halt == BridgeHalt::EPOCH_MISMATCH);
    rig.runFor(120'000ms, 50ms);
    CHECK(rig.domain->view().halt == BridgeHalt::EPOCH_MISMATCH);
    CHECK(rig.domain->view().state == State::HALTED);
    CHECK(rig.core.cursor == 3);
}

TEST_CASE("HaltMachineTest.epochHaltLeavesOnCoreRegenesis")
{
    // FIX-3: the operator runs the dual genesis the halt waits for; the bridge leaves the halt without a restart
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 3;
    SimRecording &r = rig.archive(0).record();
    appendJournal(rig.archive(0), r, 1, 3, EPOCH);
    for (std::int64_t s = 4; s <= 10; ++s)
    {
        rig.archive(0).append(r, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.domain->view().halt == BridgeHalt::EPOCH_MISMATCH);
    // the genesis runbook: the ME archives are wiped, the ME journals epoch 9 from seq 1, the core is re-genesised
    rig.archive(0).recordings.clear();
    run(rig.archive(0), 1, 10, false, 9);
    rig.core.bound = 0;
    rig.core.cursor = 0;
    rig.core.latch = 0;
        rig.core.latchSkip = 0;
    rig.core.digests.clear();
    rig.core.genesisId = 9;
    rig.runUntil([&] { return rig.core.cursor == 10; }, 120'000ms);
    const bool left = rig.domain->view().halt == BridgeHalt::NONE;
    CHECK(left);
    CHECK(rig.core.cursor == 10);
    CHECK(rig.core.bound == 9);
}

TEST_CASE("HaltMachineTest.sessionLossWhileHaltingRefeedsThePrefix")
{
    // FIX-2: HALTING with the pre-halt flush back-pressured, the session closes: staging dropped; the next report
    // shows C' < e - 1, so the halt is left and the prefix below e is re-forwarded before the FeedHalt
    Direct d;
    bindAt(d, 10);
    for (std::int64_t s = 11; s <= 13; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    d.core.outcomeFor = [](const FakeFrame &f) {
        return T_ME_JOURNAL_TRADE_BATCH == f.moneyTemplate ? OfferOutcome::RETRY : OfferOutcome::OK;
    };
    SourceFault f;
    f.source = 0;
    f.kind = FaultKind::MALFORMED_EXTENT;
    d.domain->onFault(f);
    d.pump();
    REQUIRE(d.domain->view().state == State::HALTING);
    d.domain->onSessionDown(d.now);
    CHECK(BridgeMetrics::get(d.metrics.stagedDropped) == 3);
    d.core.outcomeFor = nullptr;
    d.domain->onSessionUp(d.now);
    d.pump();
    CHECK(d.domain->view().state == State::STREAMING);
    CHECK(d.domain->view().halt == BridgeHalt::NONE);
    CHECK(d.domain->view().last == 10);
    CHECK(d.feedHaltOffers() == 0);
    CHECK(d.domain->takeSourceCommand().kind == SourceCommandKind::REATTACH);
    d.attach(0); // the rewound source re-reads from 11
    for (std::int64_t s = 11; s <= 13; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    d.domain->onFault(f);
    d.pump();
    CHECK(d.domain->view().state == State::HALTED);
    CHECK(d.core.cursor == 13);
    CHECK(d.feedHaltOffers() == 1);
}

TEST_CASE("HaltMachineTest.sessionLossWhileHaltedKeepsTheHalt")
{
    Direct d;
    gapHalt(d);
    const int before = d.feedHaltOffers();
    d.domain->onSessionDown(d.now);
    CHECK(d.domain->view().state == State::DOWN);
    CHECK(d.domain->view().halt == BridgeHalt::GAP); // kept
    d.domain->onSessionUp(d.now);
    d.pump();
    const State state = d.domain->view().state;
    CHECK(state == State::HALTED);
    CHECK(d.feedHaltOffers() == before + 1);
    CHECK(d.domain->fragmentLimit() == 0);
}

TEST_CASE("HaltMachineTest.haltedQueriesEveryFiveSeconds")
{
    Direct d;
    gapHalt(d);
    const int before = d.core.queries;
    runFor(d, 30'000ms);
    const int queries = d.core.queries - before;
    CHECK(queries >= 5);
    CHECK(queries <= 7);
}

TEST_CASE("HaltMachineTest.haltedOffersNoFeedFrame")
{
    Direct d;
    gapHalt(d);
    const int feedBefore = d.core.feedOffers();
    CHECK(d.domain->fragmentLimit() == 0);
    CHECK(d.rec(TEMPLATE_TRADE, 11) == SinkAction::ABORT); // even the exactly-next record
    CHECK(d.rec(TEMPLATE_TERMINAL, 13) == SinkAction::ABORT);
    runFor(d, 10'000ms);
    CHECK(d.core.feedOffers() == feedBefore);
}

TEST_CASE("HaltMachineTest.feedHaltToAV2CoreIsReofferedBounded")
{
    // N-6: a FeedHalt to a behaviour 1/2 core is unhandled: no AdminAck, re-offered every 5 s, bounded and visible
    Rig rig(1);
    rig.core.v3 = false;
    SimRecording &r = rig.archive(0).record();
    for (std::int64_t s = 1; s <= 5; ++s)
    {
        rig.archive(0).append(r, makeFrame(TEMPLATE_TRADE, s, EPOCH, 64 * s)); // a 61 at seq 1: the head rule
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    const int first = rig.feedHaltOffers();
    rig.runFor(60'000ms, 50ms);
    const int offers = rig.feedHaltOffers() - first;
    CHECK(offers >= 11);
    CHECK(offers <= 13);
    CHECK(rig.metrics.haltsSent(HaltReason::SOURCE_SCHEMA) == static_cast<std::uint64_t>(rig.feedHaltOffers()));
    CHECK(rig.core.feedOffers() == 0);
}
