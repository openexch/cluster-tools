// RC2 S12 contract 11.1 ResyncMachineTest: the dispatch machine (4.3) and the resync rules (section 6).
#include "doctest/doctest.h"

#include <vector>

#include "ResyncHarness.h"

using namespace cluster_tools::domain::forwarder;
using namespace cluster_tools::domain::forwarder::testing;
namespace fx = cluster_tools::domain::forwarder::fixtures;

using Seqs = std::vector<std::int64_t>;

TEST_CASE("ResyncMachineTest.registrationProbesFirstThenPagesFromZero")
{
    Harness h;
    CHECK(h.m.mode() == Mode::IDLE);
    CHECK(nullptr == h.m.requestDue());
    h.m.onRegistered(h.now);
    CHECK(h.m.mode() == Mode::RESYNCING);
    const PendingRequest probe = h.send();
    CHECK(probe.kind == RequestKind::PROBE);
    CHECK(probe.maxEntries == 0);
    CHECK(probe.fromDispatchSeq == 0);
    h.end(probe.correlationId, 0, 7);
    CHECK(h.m.lastHead() == 7);
    const PendingRequest page = h.send();
    CHECK(page.kind == RequestKind::PAGE);
    CHECK(page.fromDispatchSeq == 0);
    CHECK(page.maxEntries == 4096);
    CHECK(page.correlationId != probe.correlationId);
    CHECK(h.m.counters().resyncs[1] == 1); // T1
}

TEST_CASE("ResyncMachineTest.replyBlockIsTheLastCountEntries")
{
    Harness h;
    h.m.onRegistered(h.now);
    const PendingRequest probe = h.send();
    h.entries({ 3, 5 }); // live entries of earlier applies, arriving while the probe is out
    h.end(probe.correlationId, 0, 6);
    const PendingRequest page = h.send();
    h.entries({ 9 });       // a live entry before the query apply
    h.entries({ 2, 4, 6, 9 }); // the reply
    h.end(page.correlationId, 4, 9);
    CHECK(h.ringSeqs() == Seqs{ 2, 4, 6, 9 });
    CHECK(h.m.counters().heldResolved == 3); // 3, 5 and the live 9 were not in the reply
    CHECK(h.m.cursor() == 9);
    CHECK(h.m.mode() == Mode::LIVE);
    CHECK(h.m.holdSize() == 0);
    CHECK(h.m.verdict() == ResyncVerdict::NONE);
    h.entries({ 10, 11 }); // live after the 108
    CHECK(h.ringSeqs() == Seqs{ 2, 4, 6, 9, 10, 11 });
}

TEST_CASE("ResyncMachineTest.pageContinuationFromTheLastReplySeq")
{
    Harness h;
    h.m.onRegistered(h.now);
    h.end(h.send().correlationId, 0, 100'000);
    const PendingRequest first = h.send();
    for (std::int64_t s = 1; s <= 4096; s++)
    {
        h.entry(2 * s);
    }
    h.end(first.correlationId, 4096, 100'000);
    CHECK(h.m.mode() == Mode::RESYNCING);
    CHECK(h.m.cursor() == 8192);
    const PendingRequest second = h.send();
    CHECK(second.kind == RequestKind::PAGE);
    CHECK(second.fromDispatchSeq == 8192);
    h.entries({ 9000, 9001 });
    h.end(second.correlationId, 2, 100'000);
    CHECK(h.m.mode() == Mode::LIVE);
    CHECK(h.m.cursor() == 100'000); // max(cursor, head): everything else in (cursor, head] is resolved
    CHECK(h.ring.depth() == 4098);
    CHECK(h.m.counters().pages == 2);
    CHECK(h.m.counters().entries == 4098);
}

TEST_CASE("ResyncMachineTest.shapeS1CountBounds")
{
    SUBCASE("count above hold size")
    {
        Harness h;
        h.registerLive(0);
        h.m.resyncFromZero(Trigger::T4, h.now);
        const PendingRequest page = h.send();
        h.entries({ 1, 2 });
        h.end(page.correlationId, 3, 2);
        CHECK(h.m.counters().shapeViolations[0] == 1);
        CHECK(h.m.counters().resyncs[6] == 1); // T6
        CHECK(h.ring.empty());
        CHECK(h.m.mode() == Mode::RESYNCING);
        REQUIRE(nullptr != h.m.requestDue());
        CHECK(h.m.requestDue()->fromDispatchSeq == 0);
    }
    SUBCASE("count above the page")
    {
        Harness h;
        h.registerLive(0);
        h.m.resyncFromZero(Trigger::T4, h.now);
        const PendingRequest page = h.send();
        for (std::int64_t s = 1; s <= 4097; s++)
        {
            h.entry(s);
        }
        h.end(page.correlationId, 4097, 4097);
        CHECK(h.m.counters().shapeViolations[0] == 1);
        CHECK(h.ring.empty());
    }
    SUBCASE("negative count")
    {
        Harness h;
        h.registerLive(0);
        h.m.resyncFromZero(Trigger::T4, h.now);
        h.end(h.send().correlationId, -1, 0);
        CHECK(h.m.counters().shapeViolations[0] == 1);
    }
    SUBCASE("probe with count != 0 is a pre-S8 core (F15)")
    {
        Harness h;
        h.m.onRegistered(h.now);
        const PendingRequest probe = h.send();
        h.entries({ 1, 2, 3 });
        h.end(probe.correlationId, 3, 3);
        CHECK(h.m.verdict() == ResyncVerdict::PRE_S8_CORE);
        CHECK(h.m.verdictDetail() == 3);
    }
}

TEST_CASE("ResyncMachineTest.shapeS2BlockOrderAndBounds")
{
    SUBCASE("non-ascending")
    {
        Harness h;
        h.registerLive(0);
        h.m.resyncFromZero(Trigger::T4, h.now);
        const PendingRequest page = h.send();
        h.entries({ 3, 2 });
        h.end(page.correlationId, 2, 5);
        CHECK(h.m.counters().shapeViolations[1] == 1);
        CHECK(h.ring.empty());
    }
    SUBCASE("seq at or below pageFrom")
    {
        Harness h;
        h.registerLive(10);
        h.entry(12); // gap: RESYNC(10)
        REQUIRE(h.m.mode() == Mode::RESYNCING);
        const PendingRequest page = h.send();
        CHECK(page.fromDispatchSeq == 10);
        h.entries({ 10, 12 });
        h.end(page.correlationId, 2, 12);
        CHECK(h.m.counters().shapeViolations[1] == 1);
    }
    SUBCASE("seq above head")
    {
        Harness h;
        h.registerLive(0);
        h.m.resyncFromZero(Trigger::T4, h.now);
        const PendingRequest page = h.send();
        h.entries({ 1, 9 });
        h.end(page.correlationId, 2, 8);
        CHECK(h.m.counters().shapeViolations[1] == 1);
    }
}

TEST_CASE("ResyncMachineTest.shapeS3HeldEntryAboveHeadAtTheFinalPage")
{
    Harness h;
    h.registerLive(0);
    h.m.resyncFromZero(Trigger::T4, h.now);
    const PendingRequest page = h.send();
    h.entries({ 20 }); // arrived before the 108 yet above head: FIFO broken
    h.entries({ 3 });
    h.end(page.correlationId, 1, 5);
    CHECK(h.m.counters().shapeViolations[2] == 1);
    CHECK(h.ring.empty()); // RESYNC(0) cleared the block it had pushed
}

TEST_CASE("ResyncMachineTest.staleCorrelationEndIgnored")
{
    Harness h;
    h.registerLive(0);
    h.m.resyncFromZero(Trigger::T4, h.now);
    const PendingRequest page = h.send();
    h.entries({ 1 });
    h.end(page.correlationId + 77, 1, 1);
    CHECK(h.m.counters().staleEnds == 1);
    CHECK(h.m.mode() == Mode::RESYNCING);
    CHECK(h.m.holdSize() == 1);
    h.end(page.correlationId, 1, 1);
    CHECK(h.m.mode() == Mode::LIVE);
    CHECK(h.ringSeqs() == Seqs{ 1 });
}

TEST_CASE("ResyncMachineTest.t9ReissuesWithANewIdAndItsTimerRestartsOnEveryCoreFrame")
{
    Harness h;
    h.registerLive(0);
    h.m.resyncFromZero(Trigger::T4, h.now);
    const PendingRequest page = h.send();
    // 8 s with a core frame every second: the reply is delayed by back-pressure, not lost.
    for (int i = 0; i < 8; i++)
    {
        h.advance(SECOND);
        h.m.onCoreFrame(h.now);
        h.m.tick(h.now);
    }
    CHECK(nullptr == h.m.requestDue());
    CHECK(h.m.counters().timeouts == 0);
    h.advance(5 * SECOND - 1);
    CHECK(nullptr == h.m.requestDue());
    h.advance(1);
    REQUIRE(nullptr != h.m.requestDue());
    const PendingRequest again = h.send();
    CHECK(again.correlationId != page.correlationId);
    CHECK(again.fromDispatchSeq == page.fromDispatchSeq);
    CHECK(again.kind == RequestKind::PAGE);
    CHECK(h.m.counters().timeouts == 1);
    CHECK(h.m.counters().resyncs[9] == 1);
    // both replies arrive: the late first one is stale, its entries stay held and are dropped at the end
    h.entries({ 1, 2 });
    h.end(page.correlationId, 2, 2);
    CHECK(h.m.counters().staleEnds == 1);
    h.entries({ 1, 2 });
    h.end(again.correlationId, 2, 2);
    CHECK(h.ringSeqs() == Seqs{ 1, 2 });
    CHECK(h.m.mode() == Mode::LIVE);
}

TEST_CASE("ResyncMachineTest.resyncZeroInD1RestartsAndClearsRingAndHold")
{
    Harness h;
    h.registerLive(0);
    h.entries({ 1, 2, 3 });
    REQUIRE(h.ring.depth() == 3);
    h.m.resyncFromZero(Trigger::T4, h.now);
    const PendingRequest first = h.send();
    h.entries({ 4, 5 });
    CHECK(h.m.holdSize() == 2);
    h.m.resyncFromZero(Trigger::T2, h.now);
    CHECK(h.ring.empty());
    CHECK(h.m.holdSize() == 0);
    CHECK(h.m.cursor() == 0);
    CHECK(h.m.counters().ringClearedEntries == 3);
    const PendingRequest second = h.send();
    CHECK(second.correlationId != first.correlationId);
    CHECK(second.fromDispatchSeq == 0);
    h.end(first.correlationId, 0, 5); // the abandoned request's end
    CHECK(h.m.counters().staleEnds == 1);
}

TEST_CASE("ResyncMachineTest.ringFullMidPageEntersD3WithTheCursorAtTheLastPushThenResyncCursorAt32768")
{
    Harness h;
    h.registerLive(0);
    for (std::int64_t s = 1; s <= 65'535; s++)
    {
        h.entry(s);
    }
    REQUIRE(h.ring.depth() == 65'535);
    REQUIRE(h.m.cursor() == 65'535);
    h.advance(10 * SECOND); // head probe due
    const PendingRequest probe = h.send();
    REQUIRE(probe.kind == RequestKind::PROBE);
    h.end(probe.correlationId, 0, 65'538); // three broadcasts missed: T7
    CHECK(h.m.counters().headProbeBehind == 1);
    const PendingRequest page = h.send();
    CHECK(page.fromDispatchSeq == 65'535);
    h.entries({ 65'536, 65'537, 65'538 });
    h.end(page.correlationId, 3, 65'538);
    CHECK(h.m.mode() == Mode::OVERFLOW_WAIT);
    CHECK(h.m.cursor() == 65'536);
    CHECK(h.m.counters().ringOverflows == 1);
    CHECK(h.m.counters().discarded == 2);
    h.entries({ 65'539 });
    CHECK(h.m.counters().discarded == 3);
    h.ring.pop(65'536 - 32'769);
    h.m.tick(h.now);
    CHECK(h.m.mode() == Mode::OVERFLOW_WAIT);
    h.ring.pop(1);
    h.m.tick(h.now);
    CHECK(h.m.mode() == Mode::RESYNCING);
    CHECK(h.m.counters().resyncs[8] == 1);
    const PendingRequest again = h.send();
    CHECK(again.fromDispatchSeq == 65'536); // the discarded part of the page is re-requested
    CHECK(h.ring.depth() == 32'768);        // RESYNC(cursor) never clears the ring
}

TEST_CASE("ResyncMachineTest.holdFullEntersD3ThenResyncCursor")
{
    Harness h;
    h.registerLive(0);
    h.m.resyncFromZero(Trigger::T4, h.now);
    h.send();
    for (std::int64_t s = 1; s <= 65'536; s++)
    {
        h.entry(s);
    }
    REQUIRE(h.m.holdSize() == 65'536);
    h.entry(65'537);
    CHECK(h.m.mode() == Mode::OVERFLOW_WAIT);
    CHECK(h.m.counters().holdOverflows == 1);
    CHECK(h.m.holdSize() == 0);
    h.m.tick(h.now);
    CHECK(h.m.mode() == Mode::RESYNCING); // the ring is empty: resume at once
    CHECK(h.send().fromDispatchSeq == 0);
}

TEST_CASE("ResyncMachineTest.resyncZeroInD3ClearsAndStartsAtOnce")
{
    Harness h;
    h.registerLive(0);
    for (std::int64_t s = 1; s <= 65'536; s++)
    {
        h.entry(s);
    }
    h.entry(65'537); // ring full in LIVE
    REQUIRE(h.m.mode() == Mode::OVERFLOW_WAIT);
    h.m.resyncFromZero(Trigger::T2, h.now);
    CHECK(h.m.mode() == Mode::RESYNCING);
    CHECK(h.ring.empty());
    CHECK(h.m.cursor() == 0);
    CHECK(h.send().fromDispatchSeq == 0);
}

TEST_CASE("ResyncMachineTest.liveGapIsHeldAndResyncCursorRuns")
{
    Harness h;
    h.registerLive(5);
    h.entry(7);
    CHECK(h.m.mode() == Mode::RESYNCING);
    CHECK(h.m.holdSize() == 1);
    CHECK(h.m.counters().resyncs[5] == 1); // T5
    const PendingRequest page = h.send();
    CHECK(page.fromDispatchSeq == 5);
    h.entries({ 6, 7 });
    h.end(page.correlationId, 2, 7);
    CHECK(h.ringSeqs() == Seqs{ 6, 7 });
    CHECK(h.m.cursor() == 7);
    CHECK(h.m.mode() == Mode::LIVE);
}

TEST_CASE("ResyncMachineTest.duplicatesDroppedInLive")
{
    Harness h;
    h.registerLive(5);
    h.entries({ 5, 3, 6, 6 });
    CHECK(h.ringSeqs() == Seqs{ 6 });
    CHECK(h.m.counters().duplicates == 3);
    CHECK(h.m.verdict() == ResyncVerdict::NONE);
}

TEST_CASE("ResyncMachineTest.enteringD0ClearsHoldAndAbandonsTheRequest")
{
    Harness h;
    h.registerLive(0);
    h.m.resyncFromZero(Trigger::T4, h.now);
    const PendingRequest page = h.send();
    h.entries({ 1, 2 });
    h.m.onUnregistered();
    CHECK(h.m.mode() == Mode::IDLE);
    CHECK(h.m.holdSize() == 0);
    CHECK(nullptr == h.m.requestDue());
    h.entries({ 3 });
    CHECK(h.m.counters().droppedIdle == 1);
    h.end(page.correlationId, 2, 2); // the old term's end: ignored
    CHECK(h.m.counters().staleEnds == 1);
    CHECK(h.ring.empty());
    h.m.resyncFromZero(Trigger::T3, h.now); // D0 waits for C3
    CHECK(h.m.mode() == Mode::IDLE);
    CHECK(h.m.counters().ignoredIdleTriggers == 1);
}

TEST_CASE("ResyncMachineTest.headProbeEveryTenSecondsInLiveOnlyWithMaxEntriesZero")
{
    Harness h;
    h.registerLive(4);
    h.advance(10 * SECOND - 1);
    CHECK(nullptr == h.m.requestDue());
    h.advance(1);
    REQUIRE(nullptr != h.m.requestDue());
    const PendingRequest probe = h.send();
    CHECK(probe.kind == RequestKind::PROBE);
    CHECK(probe.maxEntries == 0);
    CHECK(probe.fromDispatchSeq == 4);
    h.end(probe.correlationId, 0, 4);
    CHECK(h.m.counters().headProbeCovered == 1);
    CHECK(h.m.mode() == Mode::LIVE);
    h.advance(10 * SECOND);
    CHECK(h.send().kind == RequestKind::PROBE);
    // no periodic probe outside LIVE
    Harness r;
    r.registerLive(0);
    r.m.resyncFromZero(Trigger::T4, r.now);
    r.send();
    r.m.onCoreFrame(r.now);
    for (int i = 0; i < 30; i++)
    {
        r.advance(SECOND);
        r.m.onCoreFrame(r.now);
    }
    CHECK(nullptr == r.m.requestDue());
    CHECK(r.m.counters().headProbeCovered == 0);
}

TEST_CASE("ResyncMachineTest.headAboveCursorIsT7")
{
    Harness h;
    h.registerLive(4);
    h.advance(10 * SECOND);
    const PendingRequest probe = h.send();
    h.end(probe.correlationId, 0, 6);
    CHECK(h.m.counters().headProbeBehind == 1);
    CHECK(h.m.counters().resyncs[7] == 1);
    CHECK(h.m.mode() == Mode::RESYNCING);
    const PendingRequest page = h.send();
    CHECK(page.kind == RequestKind::PAGE);
    CHECK(page.fromDispatchSeq == 4);
}

TEST_CASE("ResyncMachineTest.triggersFromEachMode")
{
    for (Trigger t : { Trigger::T2, Trigger::T3, Trigger::T4, Trigger::T6 })
    {
        CAPTURE(static_cast<int>(t));
        // LIVE
        Harness live;
        live.registerLive(3);
        live.entries({ 4 });
        live.m.resyncFromZero(t, live.now);
        CHECK(live.m.mode() == Mode::RESYNCING);
        CHECK(live.ring.empty());
        CHECK(live.m.cursor() == 0);
        CHECK(live.m.counters().resyncs[static_cast<unsigned>(t)] == 1);
        // RESYNCING
        Harness d1;
        d1.registerLive(0);
        d1.m.resyncFromZero(Trigger::T4, d1.now);
        d1.send();
        d1.entries({ 1 });
        d1.m.resyncFromZero(t, d1.now);
        CHECK(d1.m.holdSize() == 0);
        REQUIRE(nullptr != d1.m.requestDue());
        CHECK(d1.m.requestDue()->fromDispatchSeq == 0);
        // IDLE
        Harness idle;
        idle.m.resyncFromZero(t, idle.now);
        CHECK(idle.m.mode() == Mode::IDLE);
        CHECK(nullptr == idle.m.requestDue());
    }
    // T9 on a probe in LIVE: re-issued as a probe
    Harness h;
    h.registerLive(2);
    h.advance(10 * SECOND);
    const PendingRequest probe = h.send();
    h.advance(5 * SECOND);
    REQUIRE(nullptr != h.m.requestDue());
    CHECK(h.m.requestDue()->kind == RequestKind::PROBE);
    CHECK(h.m.requestDue()->correlationId != probe.correlationId);
    // T1 again (re-registration after a core NewLeaderEvent): probe first, ring cleared
    h.entries({ 3 });
    h.m.onUnregistered();
    h.m.onRegistered(h.now);
    CHECK(h.ring.empty());
    CHECK(h.send().kind == RequestKind::PROBE);
    CHECK(h.m.counters().resyncs[1] == 2);
}

TEST_CASE("ResyncMachineTest.seqBelowOneIsRefusedInEveryMode")
{
    // Review NOTE-2: never held, never pushed (no carrier), the cursor stays, counted; in LIVE, RESYNCING and D3 alike.
    Harness h;
    h.registerLive(5);
    h.dispatch(fx::create(0, 777));
    h.dispatch(fx::release(-3, 778));
    CHECK(h.ring.empty());
    CHECK(h.m.cursor() == 5);
    CHECK(h.m.counters().seqBelowOne == 2);
    h.entries({ 6 });
    CHECK(h.ringSeqs() == Seqs{ 6 });
    h.m.resyncFromZero(Trigger::T4, h.now);
    const PendingRequest page = h.send();
    h.dispatch(fx::create(0, 779));
    CHECK(h.m.holdSize() == 0); // not held, so it can never be framed into a reply block (the old S2 livelock)
    h.entries({ 6 });
    h.end(page.correlationId, 1, 6);
    CHECK(h.m.mode() == Mode::LIVE);
    CHECK(h.m.counters().seqBelowOne == 3);
    CHECK(h.m.counters().shapeViolations[1] == 0);
    CHECK(h.m.verdict() == ResyncVerdict::NONE);
}

TEST_CASE("ResyncMachineTest.unroutableAdvancesTheCursorWithoutAPush")
{
    Harness h;
    h.registerLive(5);
    Dispatch d = fx::create(6, 1);
    d.commandIdHigh = 0;
    d.commandIdLow = 0;
    h.dispatch(d);
    CHECK(h.ring.empty());
    CHECK(h.m.cursor() == 6);
    CHECK(h.m.counters().unroutableSkipped == 1);
    h.entries({ 7 });
    CHECK(h.ringSeqs() == Seqs{ 7 });
}

TEST_CASE("ResyncMachineTest.readinessNeedsAnAnswerWithinFifteenSeconds")
{
    Harness h;
    CHECK_FALSE(h.m.answeredWithin(h.now));
    h.registerLive(0);
    CHECK(h.m.answeredWithin(h.now));
    h.now += 15 * SECOND;
    CHECK_FALSE(h.m.answeredWithin(h.now));
}
