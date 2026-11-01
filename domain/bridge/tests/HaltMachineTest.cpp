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

// ------------------------------------------------------------------ RC2 S10' section 5 (F-1): re-list while halted

namespace {

// a hole at `hole` in an active recording 1..last, halted GAP
SimRecording &holedAndHalted(Rig &rig, std::size_t source, std::int64_t hole, std::int64_t last)
{
    SimRecording &r = rig.archive(source).record();
    appendJournal(rig.archive(source), r, 1, hole - 1);
    appendJournal(rig.archive(source), r, hole + 1, last);
    return r;
}

bool appliedOnceInOrder(const StubCore &core, std::int64_t to)
{
    if (static_cast<std::int64_t>(core.applied.size()) != to)
    {
        return false;
    }
    for (std::int64_t i = 0; i < to; ++i)
    {
        if (core.applied[static_cast<std::size_t>(i)] != i + 1)
        {
            return false;
        }
    }
    return true;
}

}

TEST_CASE("HaltMachineTest.rescanWalksTheSuccessorThatHoldsE")
{
    // R1: the halted re-scan walks past the holed recording into an in-place successor recorded 61 s later (C2)
    Rig rig(1);
    SimRecording &old = holedAndHalted(rig, 0, 11, 20);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.domain->view().halt == BridgeHalt::GAP);
    rig.runFor(61'000ms, 50ms); // the RESTART successor comes seconds after the first re-scan, not at the same instant
    REQUIRE(halted(rig));
    rig.archive(0).stop(old);
    SimRecording &succ = rig.archive(0).record();
    appendJournal(rig.archive(0), succ, 11, 20);
    rig.runUntil([&] { return rig.core.cursor == 20; }, 180'000ms, 50ms);
    const bool resumedToN = rig.core.cursor == 20 && rig.domain->view().halt == BridgeHalt::NONE;
    CHECK_MESSAGE(resumedToN, "still HALTED at N");
    CHECK(rig.metrics.recovered(BridgeHalt::GAP) == 1);
    CHECK(BridgeMetrics::get(rig.metrics.haltSuccessorAttaches) == 1);
    CHECK(rig.metrics.relists(0) >= 1);
    CHECK(appliedOnceInOrder(rig.core, 20));
}

TEST_CASE("HaltMachineTest.fillOnlyESuccessorStepsBack")
{
    // R2 (F-2 trigger): the FILL successor holds only e and later seqs; e + 1 lives in the earlier recording
    Rig rig(1);
    SimRecording &old = holedAndHalted(rig, 0, 11, 20);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    rig.archive(0).stop(old);
    SimRecording &fill = rig.archive(0).record();
    appendJournal(rig.archive(0), fill, 11, 11);
    REQUIRE(rig.runUntil([&] { return rig.core.cursor >= 11; }, 120'000ms, 50ms));
    appendJournal(rig.archive(0), fill, 21, 30); // the story goes on in the active successor
    rig.runUntil([&] { return rig.core.cursor == 30; }, 120'000ms, 50ms);
    bool haltAtEPlus1 = false;
    for (const auto &f : rig.core.feedHalts)
    {
        haltAtEPlus1 = haltAtEPlus1 || 12 == f.expected;
    }
    CHECK_MESSAGE(!haltAtEPlus1, "halted again at e + 1"); // without R2 the next re-scan recovers it 60 s later
    CHECK(rig.core.cursor == 30);
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    CHECK(BridgeMetrics::get(rig.metrics.haltStepbacks) == 1);
    CHECK(rig.domain->steppedBack(0));
    CHECK(appliedOnceInOrder(rig.core, 30)); // offered == [C + 1 .. N], every record once
}

TEST_CASE("HaltMachineTest.schemaHaltRescansAndAttachesValidCopies")
{
    // R0: CORRUPT at e on every source, HALTED(SOURCE_SCHEMA); 61 s later a RESTART successor with valid copies
    Rig rig(2);
    std::vector<SimRecording *> olds;
    for (std::size_t i = 0; i < 2; ++i)
    {
        SimRecording &r = rig.archive(i).record();
        appendJournal(rig.archive(i), r, 1, 5);
        Bytes bad = makeFrame(TEMPLATE_TRADE, 6, EPOCH, 64 * 6);
        putLe16(bad, 4, 3); // schema 3: refused
        rig.archive(i).append(r, bad);
        appendJournal(rig.archive(i), r, 7, 12);
        olds.push_back(&r);
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.domain->view().halt == BridgeHalt::SOURCE_SCHEMA);
    rig.runFor(61'000ms, 50ms);
    REQUIRE(rig.domain->view().halt == BridgeHalt::SOURCE_SCHEMA);
    rig.archive(0).stop(*olds[0]);
    SimRecording &succ = rig.archive(0).record();
    appendJournal(rig.archive(0), succ, 6, 12);
    rig.runUntil([&] { return rig.core.cursor == 12; }, 180'000ms, 50ms);
    const bool attached = rig.core.cursor == 12 && rig.domain->view().halt == BridgeHalt::NONE;
    CHECK_MESSAGE(attached, "halted SOURCE_SCHEMA never attaches the valid copies");
    CHECK(rig.metrics.recovered(BridgeHalt::SOURCE_SCHEMA) == 1);
    CHECK(rig.core.latch == 0); // the stub core reach-cleared the slot (the gate bit is the core's, 4R.4)
    CHECK(appliedOnceInOrder(rig.core, 12));
}

TEST_CASE("HaltMachineTest.successorAboveEKeepsTheHaltWithMinSeen")
{
    // a successor that starts ABOVE e is a hole at e too; seen = the min over the recordings walked (5.2 item 4)
    Rig rig(1);
    SimRecording &old = rig.archive(0).record();
    appendJournal(rig.archive(0), old, 1, 10);
    appendJournal(rig.archive(0), old, 20, 25); // hole at 11, seen 20
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    rig.archive(0).stop(old);
    SimRecording &succ = rig.archive(0).record();
    appendJournal(rig.archive(0), succ, 15, 25); // above e: seen 15
    rig.runUntil([&] { return rig.domain->successorsTried(0); }, 180'000ms, 50ms);
    CHECK(rig.domain->successorsTried(0));
    CHECK(rig.domain->view().halt == BridgeHalt::GAP);
    CHECK(rig.core.cursor == 10);
    const std::int64_t seen = rig.domain->relistSeen(0);
    CHECK_MESSAGE(seen == 15, "seen > truth");
}

TEST_CASE("HaltMachineTest.twoFillsInOneRun")
{
    // C4: two holes, two FILLs 61 s apart: both markers are set and reset twice in one run
    Rig rig(1);
    SimRecording &r0 = rig.archive(0).record();
    appendJournal(rig.archive(0), r0, 1, 10);
    appendJournal(rig.archive(0), r0, 12, 20);
    appendJournal(rig.archive(0), r0, 22, 30); // holes at 11 and 21
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    rig.archive(0).stop(r0);
    SimRecording &f1 = rig.archive(0).record();
    appendJournal(rig.archive(0), f1, 11, 11);
    rig.runUntil([&] { return rig.core.cursor >= 20 && halted(rig); }, 240'000ms, 50ms); // the second halt at 21
    const bool firstMarkers = rig.domain->steppedBack(0) || BridgeMetrics::get(rig.metrics.haltStepbacks) == 1;
    CHECK(firstMarkers);
    rig.runFor(61'000ms, 50ms);
    // the second halt's re-scans started new syncs: the markers of the first fill are gone (reset path, C4)
    CHECK_FALSE(rig.domain->steppedBack(0));
    rig.archive(0).stop(f1);
    SimRecording &f2 = rig.archive(0).record();
    appendJournal(rig.archive(0), f2, 21, 21);
    rig.runUntil([&] { return rig.core.cursor == 30; }, 240'000ms, 50ms);
    const std::uint64_t relists = rig.metrics.relists(0);
    const std::uint64_t stepbacks = BridgeMetrics::get(rig.metrics.haltStepbacks);
    const bool secondWalked = 2 == stepbacks && 2 == rig.metrics.recovered(BridgeHalt::GAP) && relists >= 2;
    CHECK_MESSAGE(secondWalked, "second fill never walked");
    CHECK(rig.domain->steppedBack(0));
    CHECK(appliedOnceInOrder(rig.core, 30));
}

TEST_CASE("HaltMachineTest.epochHaltNeverRelists")
{
    // scope: EPOCH_MISMATCH never re-lists (a new-epoch successor is FOREIGN_EPOCH; its exit is the genesis)
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
    const int lists0 = rig.archive(0).listCalls;
    const std::size_t offered0 = rig.core.offered.size();
    rig.archive(0).stop(r);
    SimRecording &succ = rig.archive(0).record();
    for (std::int64_t s = 4; s <= 12; ++s)
    {
        rig.archive(0).append(succ, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
    }
    rig.runFor(200'000ms, 50ms);
    const std::uint64_t relists = rig.metrics.relists(0);
    const bool noListing = relists == 0 && rig.archive(0).listCalls == lists0;
    CHECK_MESSAGE(noListing, "a re-list was made while EPOCH_MISMATCH");
    std::size_t feedOffers = 0;
    for (std::size_t i = offered0; i < rig.core.offered.size(); ++i)
    {
        feedOffers += T_FEED_HALT != rig.core.offered[i].moneyTemplate && T_QUERY_FEED_POSITION != rig.core.offered[i].moneyTemplate;
    }
    CHECK_MESSAGE(feedOffers == 0, "a foreign record was offered");
    CHECK(rig.domain->view().halt == BridgeHalt::EPOCH_MISMATCH);
}

TEST_CASE("HaltMachineTest.relistCadenceIsBounded")
{
    // 10 min halted, no fill: one listing per re-scan (60 s) plus the verification re-read: bounded, never per tick
    Rig rig(1);
    holedAndHalted(rig, 0, 11, 20);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    const int lists0 = rig.archive(0).listCalls;
    rig.runFor(600'000ms, 50ms);
    const int listings = rig.archive(0).listCalls - lists0;
    MESSAGE("listings in 10 min halted: " << listings);
    // measured 18 (2026-11-01): two listings per 60 s re-scan (the re-scan's memo-free re-list, then the follower's own
    // re-list when the walk reaches the live end); the bound is two per re-scan, never per tick (RED RELIST_EVERY_TICK)
    CHECK_MESSAGE(listings <= 22, "listings > 22");
    CHECK(rig.domain->view().halt == BridgeHalt::GAP);
}

TEST_CASE("HaltMachineTest.genesisExitIsCounted")
{
    // 4.1 rule 8: the dual genesis exit of HALTED(EPOCH_MISMATCH) is a counted recovery
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
    rig.archive(0).recordings.clear(); // the runbook's wipe
    run(rig.archive(0), 1, 10, false, 9);
    rig.core.bound = 0;
    rig.core.cursor = 0;
    rig.core.latch = 0;
    rig.core.latchSkip = 0;
    rig.core.digests.clear();
    rig.core.genesisId = 9;
    rig.runUntil([&] { return rig.core.cursor == 10; }, 120'000ms);
    REQUIRE(rig.domain->view().halt == BridgeHalt::NONE);
    const std::uint64_t recovered = rig.metrics.recovered(BridgeHalt::EPOCH_MISMATCH);
    CHECK_MESSAGE(recovered == 1, "recovered {EPOCH_MISMATCH} 0 after the genesis report");
}

TEST_CASE("HaltMachineTest.rescanTakesExitAAtAQuietHead")
{
    // lane C W2b finding (TwoLaneDomainPropertyTest seeds 709779217, 709779263): a GAP re-scan reading a quiet head
    // (the chain ends below e, no record >= e to forward) must still leave by 9.4 exit (a) when another lane's delivery
    // moves C' >= seen - 1; before the fix it waited for the 20 s behind dwell (or forever with that row paused)
    Rig rig(1);
    SimRecording &old = holedAndHalted(rig, 0, 11, 20);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    // the operator replaced the node's history with a quiet prefix 1..10 (no hole, nothing past it yet)
    rig.archive(0).recordings.clear();
    SimRecording &prefix = rig.archive(0).record();
    appendJournal(rig.archive(0), prefix, 1, 10);
    (void)old;
    REQUIRE(rig.runUntil([&] { return halted(rig) && rig.domain->feedingForTest(); }, 120'000ms, 50ms)); // the re-scan reads
    rig.core.cursor = 11; // another lane delivered e = 11 = seen - 1
    rig.core.latch = 0;
    const TimePoint at = rig.now;
    rig.runUntil([&] { return BridgeHalt::NONE == rig.domain->view().halt; }, 60'000ms, 50ms);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(rig.now - at).count();
    MESSAGE("exit (a) " << took << " ms after C' passed seen - 1");
    const bool exitA = BridgeHalt::NONE == rig.domain->view().halt && took <= 12'000;
    CHECK_MESSAGE(exitA, "stuck GAP halt at a quiet head");
    CHECK(rig.metrics.recovered(BridgeHalt::GAP) == 1);
}
