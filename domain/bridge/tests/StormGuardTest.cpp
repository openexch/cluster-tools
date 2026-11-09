// StormGuardTest (7 as generalised): the resync storm guard of 9.6.
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

using namespace ct_bridge_test;

namespace {

SimRecording &run(SimArchive &a, std::int64_t from, std::int64_t to, bool stop)
{
    SimRecording &r = a.record();
    appendJournal(a, r, from, to);
    if (stop)
    {
        a.stop(r);
    }
    return r;
}

int offersOf(const StubCore &core, std::int64_t seq)
{
    int n = 0;
    for (std::int64_t s : core.offeredSeqs())
    {
        n += s == seq ? 1 : 0;
    }
    return n;
}

bool halted(Rig &rig)
{
    return rig.domain->view().state == State::HALTED;
}

}

TEST_CASE("StormGuardTest.resyncStormBecomesCoreRefused")
{
    Rig rig(1);
    rig.core.refuseSeqs = { 6 }; // every copy of 6 is refused (latched SOURCE_SCHEMA, no SettleRejected)
    run(rig.archive(0), 1, 12, false);
    rig.up();
    rig.runUntil([&] { return halted(rig); }, 120'000ms);
    const int offersOfSix = offersOf(rig.core, 6);
    CHECK(offersOfSix <= 2); // stormFences x sources
    CHECK(rig.domain->view().halt == BridgeHalt::CORE_REFUSED);
    CHECK(rig.domain->view().coreMirror == HaltReason::SOURCE_SCHEMA);
    CHECK(BridgeMetrics::get(rig.metrics.coreRefused) == 1);
}

TEST_CASE("StormGuardTest.stormRotatesOtherSourcesBeforeCoreRefused")
{
    SUBCASE("SOURCE_SCHEMA: a valid copy on C clears it")
    {
        Rig rig(3);
        for (std::size_t i = 0; i < 2; ++i)
        {
            SimRecording &r = rig.archive(i).record();
            appendJournal(rig.archive(i), r, 1, 5);
            rig.archive(i).append(r, flagged(TEMPLATE_TRADE, 6, EPOCH, FLAG_POISON)); // a corrupt but decodable copy
            appendJournal(rig.archive(i), r, 7, 12);
        }
        run(rig.archive(2), 1, 12, false);
        rig.up();
        rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); }, 300'000ms);
        CHECK(rig.core.cursor == 12);
        CHECK(rig.domain->view().halt == BridgeHalt::NONE);
        CHECK(BridgeMetrics::get(rig.metrics.stormRotations) == 2);
        CHECK(rig.core.latch == 0); // reach-cleared by the valid copy
    }
    SUBCASE("SETTLE_REFUSED")
    {
        Rig rig(2);
        SimRecording &r = rig.archive(0).record();
        appendJournal(rig.archive(0), r, 1, 5);
        rig.archive(0).append(r, flagged(TEMPLATE_TRADE, 6, EPOCH, FLAG_REFUSE));
        appendJournal(rig.archive(0), r, 7, 12);
        run(rig.archive(1), 1, 12, false);
        rig.up();
        rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); }, 300'000ms);
        CHECK(rig.core.cursor == 12);
        CHECK(rig.domain->view().halt == BridgeHalt::NONE);
        CHECK(BridgeMetrics::get(rig.metrics.stormRotations) == 1);
    }
}

TEST_CASE("StormGuardTest.unboundSilentCoreKeepsRetryingAndBindsOnceV3")
{
    // BLOCKER-1: the bridge is up before the genesis script reaches CoreConfig 3: the core answers 0/0/NONE and drops
    // the 68 without a trace; the bridge must not park, and binds once the core switches to 3 (S7 4.1)
    Rig rig(1);
    rig.core.v3 = false;
    run(rig.archive(0), 1, 12, false);
    rig.up();
    rig.runFor(60'000ms);
    const int seqOneOffers = offersOf(rig.core, 1);
    CHECK(seqOneOffers >= 2);
    CHECK(seqOneOffers <= 8); // at most about one per reportInterval (10 s)
    rig.core.v3 = true;
    rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); }, 60'000ms);
    const BridgeHalt halt = rig.domain->view().halt;
    CHECK(halt == BridgeHalt::NONE);
    CHECK(rig.core.cursor == 12);
    CHECK(rig.core.bound == EPOCH);
}

TEST_CASE("StormGuardTest.unboundLatchedStormEndsCoreRefused")
{
    // a v3 core that is not this ME's genesis: the FRESH is refused FEED_UNBOUND and latched SOURCE_UNBOUND
    Rig rig(1);
    rig.core.genesisId = 99;
    run(rig.archive(0), 1, 12, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }, 120'000ms));
    CHECK(rig.domain->view().halt == BridgeHalt::CORE_REFUSED);
    CHECK(rig.domain->view().coreMirror == HaltReason::SOURCE_UNBOUND);
    CHECK(offersOf(rig.core, 1) <= 2);
}

TEST_CASE("StormGuardTest.silentDropStormStops")
{
    // a bound core that drops the frame without a trace (no latch, the cursor stuck)
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 5;
    rig.core.silentDrop = true;
    run(rig.archive(0), 1, 12, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }, 120'000ms));
    CHECK(rig.domain->view().halt == BridgeHalt::CORE_REFUSED);
    CHECK(rig.domain->view().coreMirror == HaltReason::NONE);
    CHECK(offersOf(rig.core, 6) <= 2);
}

TEST_CASE("StormGuardTest.newLeaderFencesDoNotCount")
{
    Direct d;
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.start();
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::CONTINUE);
        d.core.cursor = 10; // the new leader lost it (uncommitted)
        d.domain->onFence(FenceTrigger::NEW_LEADER, d.now);
        d.domain->onSessionUp(d.now);
        d.pump();
        (void)d.domain->takeSourceCommand();
        d.attach(0);
        REQUIRE(d.domain->view().state == State::STREAMING);
    }
    CHECK(d.domain->stormCount() == 0);
    CHECK(d.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("StormGuardTest.coreRefusedSendsNothing")
{
    Rig rig(1);
    rig.core.refuseSeqs = { 6 };
    run(rig.archive(0), 1, 12, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }, 120'000ms));
    REQUIRE(rig.domain->view().halt == BridgeHalt::CORE_REFUSED);
    const int feed = rig.core.feedOffers();
    rig.runFor(60'000ms, 50ms);
    CHECK(rig.core.feedOffers() == feed);
    CHECK(rig.feedHaltOffers() == 0); // the core already latched: no FeedHalt
    CHECK(rig.core.queries > 0);      // the halted bridge still queries
}

TEST_CASE("StormGuardTest.coreOnlyRangeRecordEndsCoreRefused")
{
    // S8 3.9 (a) and (c) are the core's checks, not the bridge's: an outcome with ledgerCapacity 0 or a
    // release entry with commandIdHigh 0 is refused by the core on every copy; the bridge rotates once per source,
    // then parks in CORE_REFUSED without a re-offer loop (13.17 repeats this on the real core)
    Rig rig(2);
    rig.core.refuseSeqs = { 7 };
    for (std::size_t i = 0; i < 2; ++i)
    {
        SimRecording &r = rig.archive(i).record();
        appendJournal(rig.archive(i), r, 1, 6);
        rig.archive(i).append(r, makeFrame(TEMPLATE_OUTCOME, 7, EPOCH, 64 * 7));
        appendJournal(rig.archive(i), r, 8, 12);
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }, 300'000ms));
    CHECK(rig.domain->view().halt == BridgeHalt::CORE_REFUSED);
    CHECK(BridgeMetrics::get(rig.metrics.stormRotations) == 1);
    CHECK(offersOf(rig.core, 7) <= 4);
    CHECK(rig.core.cursor == 6);
}

TEST_CASE("StormGuardTest.stormRetriesAnUntriedSourceOnceItsCopyArrives")
{
    // the 9.6 amendment of 2026-10-31 (the former OQ-3 probe): node 0 carries a corrupt but decodable
    // copy of 11; node 1 is a lagging ME whose 11.. are not journaled yet. The storm switches to node 1, which cannot
    // offer 11: it stays UNTRIED. The bridge parks CORE_REFUSED, and on the haltRescan cadence re-tries node 1 only
    // (nothing tried is re-offered). Once node 1 journals 11..20 the bridge offers its valid copy, which reach-clears
    // the core's latch: cursor 20, no operator action (in particular no 76 skip of a record another node holds).
    Rig rig(2);
    SimRecording &a = rig.archive(0).record();
    appendJournal(rig.archive(0), a, 1, 10);
    rig.archive(0).append(a, flagged(TEMPLATE_TRADE, 11, EPOCH, FLAG_POISON));
    appendJournal(rig.archive(0), a, 12, 20);
    SimRecording &b = rig.archive(1).record();
    appendJournal(rig.archive(1), b, 1, 10);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->view().halt == BridgeHalt::CORE_REFUSED; }, 120'000ms));
    REQUIRE(rig.core.cursor == 10);
    const std::uint32_t tried = rig.domain->stormTried();
    CHECK(tried == 1u); // node 0 offered 11; node 1 was switched to but never offered it
    const int offersOfElevenBefore = offersOf(rig.core, 11);
    rig.runFor(150'000ms); // a few retries while node 1 is still behind: nothing is re-offered
    CHECK(offersOf(rig.core, 11) == offersOfElevenBefore);
    CHECK(rig.core.latch == static_cast<std::uint8_t>(HaltReason::SOURCE_SCHEMA)); // still latched, nothing re-offered
    CHECK(rig.core.cursor == 10);
    appendJournal(rig.archive(1), b, 11, 20); // node 1 catches up: a valid copy of 11
    rig.runUntil([&] { return rig.core.cursor == 20; }, 300'000ms);
    const std::int64_t cursor = rig.core.cursor;
    CHECK(cursor == 20);
    CHECK(rig.core.latch == 0); // reach-cleared by the valid copy
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("StormGuardTest.stormRotationNeverTargetsTheCurrentSource")
{
    // Source 0 offered the refused 11 (tried); the bridge now sits on source 1, which has not offered
    // it. At the second counted sync start the only untried source is the CURRENT one: a SWITCH to it would be a
    // no-op that drops the sync start's REATTACH. The storm rotation never targets it (k < n); the bridge parks
    // CORE_REFUSED and the haltRescan retry reads source 1 (with a rewind).
    Direct d(2);
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.core.refuseSeqs = { 11 };
    d.start();
    REQUIRE(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::CONTINUE); // refused by the core (decode poison, no reject)
    d.tick(10'001ms); // the periodic report: C' 10 < coversSeq 11: REPORT_LOSS, a sync from 10, storm {10, 1}
    REQUIRE(d.domain->stormCount() == 1);
    REQUIRE(d.domain->takeSourceCommand().kind == SourceCommandKind::REATTACH);
    d.attach(0);
    REQUIRE(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::CONTINUE); // offered again from source 0
    d.attach(1);                                                   // the bridge is now on source 1
    d.tick(10'001ms);                                              // the second counted sync start
    const std::uint64_t rotations = BridgeMetrics::get(d.metrics.stormRotations);
    CHECK(rotations == 0);
    CHECK(d.domain->view().halt == BridgeHalt::CORE_REFUSED);
    CHECK(d.domain->stormTried() == 1u);
}

TEST_CASE("StormGuardTest.egressRegressionCopyRotatesToAValidOne")
{
    // row 6 (g < P: an in-epoch ME rewind) latches SOURCE_SCHEMA with skip 0 and no SettleRejected;
    // like any rotatable latch the storm guard tries the other source, whose valid copy reach-clears it
    Rig rig(2);
    SimRecording &a = rig.archive(0).record();
    appendJournal(rig.archive(0), a, 1, 5);
    rig.archive(0).append(a, makeFrame(TEMPLATE_TERMINAL, 6, EPOCH, 64)); // egressSeq below the applied 64 * 5
    appendJournal(rig.archive(0), a, 7, 12);
    run(rig.archive(1), 1, 12, false);
    rig.up();
    rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); }, 300'000ms);
    CHECK(rig.core.cursor == 12);
    CHECK(rig.core.latch == 0);
    CHECK(BridgeMetrics::get(rig.metrics.stormRotations) == 1);
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("StormGuardTest.mixedLatchReportsHigherClass")
{
    // 4R rule 6 (lane R): the core reports one haltReason; with this bridge's GAP record open and a
    // SOURCE_SCHEMA slot latched by another lane it reports the higher exit class. The storm guard reads the mirror
    // (SOURCE_SCHEMA: rotatable), while the bridge's own GAP halt still leaves by exit (a) on the cursor alone
    Rig rig(1);
    SimRecording &r = rig.archive(0).record();
    appendJournal(rig.archive(0), r, 1, 10);
    appendJournal(rig.archive(0), r, 12, 20); // a hole at 11: GAP {11, 12}
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.domain->view().halt == BridgeHalt::GAP);
    rig.runUntil([&] { return rig.domain->view().coreMirror == HaltReason::GAP; }, 30'000ms);
    REQUIRE(rig.domain->view().coreMirror == HaltReason::GAP);
    rig.core.setLatch(3, 11, 11, 0); // another lane's poison: SOURCE_SCHEMA, a higher exit class than GAP
    rig.runFor(6'000ms, 50ms);          // the next halted report (5 s)
    const bool mirrorsHigher = rig.domain->view().coreMirror == HaltReason::SOURCE_SCHEMA;
    CHECK_MESSAGE(mirrorsHigher, "coreMirror GAP with a SOURCE_SCHEMA slot");
    CHECK(rig.domain->view().halt == BridgeHalt::GAP);
    rig.core.cursor = 11; // another lane delivered 11 = seen - 1: exit (a), whatever the reported class
    rig.runUntil([&] { return rig.domain->view().halt == BridgeHalt::NONE; }, 30'000ms);
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    CHECK(rig.metrics.recovered(BridgeHalt::GAP) == 1);
}
