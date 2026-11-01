// RotationTest (RC2 S9b 13.5): per-source verdicts, the rotation of 8.2 and the decision table of 8.3, driven
// through S9a's real follower (FollowPolicy + FollowerLoop + ResumeMemo over SimArchive sources).
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

using namespace ct_bridge_test;

namespace {

// [from, to] of seqs on one recording of `a` (seq 1 is a 68 FRESH); stopped unless it is the last one
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

TEST_CASE("RotationTest.holeOnOneSourceFailsOver")
{
    Rig rig(2);
    run(rig.archive(0), 1, 10, true);
    run(rig.archive(0), 13, 20, false);
    run(rig.archive(1), 1, 20, false);
    rig.up();
    rig.runUntil([&] { return rig.core.cursor == 20 || halted(rig); });
    const bool noHalt = rig.core.feedHalts.empty();
    CHECK(noHalt);
    CHECK(rig.core.cursor == 20);
    CHECK(rig.switchesTo(1) >= 1);
}

TEST_CASE("RotationTest.holeOnEverySourceHaltsGap")
{
    Rig rig(2);
    run(rig.archive(0), 1, 10, true);
    run(rig.archive(0), 13, 20, false);
    run(rig.archive(1), 1, 10, true);
    run(rig.archive(1), 12, 20, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(rig.core.feedHalts.size() >= 1);
    const FakeFrame fh = rig.core.feedHalts.front();
    CHECK(fh.reason == HaltReason::GAP);
    CHECK(fh.expected == 11);
    CHECK(fh.seen == 12); // the minimum seen: the earliest reach point at the core
    CHECK(fh.meEpochSeen == EPOCH);
    CHECK(fh.identity == 0xB1);
    CHECK(rig.core.latch == static_cast<std::uint8_t>(HaltReason::GAP));
    CHECK(rig.core.cursor == 10);
}

TEST_CASE("RotationTest.holeFromAMemoReadIsVerifiedFirst")
{
    // the K8 shape: a long run, a short re-journal that crashed, a restart above it. A memo read resumed after the
    // short run's proof sees a hole at 11 that a memo-free read does not have (R0 holds 11 and 12).
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 10;
    run(rig.archive(0), 1, 12, true);
    run(rig.archive(0), 8, 10, true);
    run(rig.archive(0), 13, 20, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->view().state == State::STREAMING; }));
    rig.core.commitImmediately = false; // from here nothing commits, no report comes back
    REQUIRE(rig.runUntil([&] { return rig.domain->view().last == 20; }, 5'000ms));
    rig.core.elect(); // a new leader: 11..20 were never committed
    rig.core.commitImmediately = true;
    rig.domain->onFence(FenceTrigger::NEW_LEADER, rig.now);
    rig.domain->onSessionUp(rig.now);
    rig.core.commit();
    rig.runUntil([&] { return rig.core.cursor == 20 || halted(rig); });
    const bool noFalseGap = rig.core.feedHalts.empty();
    CHECK(noFalseGap);
    CHECK(rig.core.cursor == 20);
    CHECK(BridgeMetrics::get(rig.metrics.holeVerifications) == 1);
}

TEST_CASE("RotationTest.badFrameFailsOverFirst")
{
    // D-1, H3: a refused frame on one source is a verdict for that source; another source feeds
    Rig rig(2);
    SimRecording &a = rig.archive(0).record();
    appendJournal(rig.archive(0), a, 1, 5);
    Bytes bad = makeFrame(TEMPLATE_TRADE, 6, EPOCH, 64 * 6);
    putLe16(bad, 4, 3); // a schema 3 frame
    rig.archive(0).append(a, bad);
    appendJournal(rig.archive(0), a, 7, 12);
    run(rig.archive(1), 1, 12, false);
    rig.up();
    rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); });
    const BridgeHalt halt = rig.domain->view().halt;
    CHECK(halt == BridgeHalt::NONE);
    CHECK(rig.core.cursor == 12);
    CHECK(rig.metrics.badCount(BadReason::REFUSAL_SCHEMA) == 1);
}

TEST_CASE("RotationTest.fragmentedFrameHalts")
{
    SUBCASE("FRAGMENTED on one source: failover")
    {
        Rig rig(2);
        SimRecording &a = rig.archive(0).record();
        appendJournal(rig.archive(0), a, 1, 5);
        rig.archive(0).append(a, makeFrame(TEMPLATE_TRADE, 6, EPOCH, 64 * 6), 0x80);
        run(rig.archive(1), 1, 12, false);
        rig.up();
        rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); });
        const BridgeHalt oneSource = rig.domain->view().halt;
        CHECK(oneSource == BridgeHalt::NONE);
        CHECK(rig.core.cursor == 12);
    }
    SUBCASE("FRAGMENTED on every source: SOURCE_SCHEMA")
    {
        Rig rig(2);
        for (std::size_t i = 0; i < 2; ++i)
        {
            SimRecording &r = rig.archive(i).record();
            appendJournal(rig.archive(i), r, 1, 5);
            rig.archive(i).append(r, makeFrame(TEMPLATE_TRADE, 6, EPOCH, 64 * 6), 0x80);
        }
        rig.up();
        REQUIRE(rig.runUntil([&] { return halted(rig); }));
        REQUIRE(!rig.core.feedHalts.empty());
        CHECK(rig.core.feedHalts.front().reason == HaltReason::SOURCE_SCHEMA);
        CHECK(rig.core.feedHalts.front().expected == 6);
        CHECK(rig.core.feedHalts.front().seen == 6);
        CHECK(rig.metrics.badCount(BadReason::REFUSAL_FRAGMENTED) == 2);
    }
}

TEST_CASE("RotationTest.schema3RecordHaltsSourceSchema")
{
    // R-B4-3, H4: a schema 3 record on every source halts SOURCE_SCHEMA at e (never skipped like Java's T5)
    Rig rig(2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        SimRecording &r = rig.archive(i).record();
        appendJournal(rig.archive(i), r, 1, 5);
        Bytes bad = makeFrame(TEMPLATE_OUTCOME, 6, EPOCH, 64 * 6);
        putLe16(bad, 4, 3);
        rig.archive(i).append(r, bad);
        appendJournal(rig.archive(i), r, 7, 12);
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(!rig.core.feedHalts.empty());
    const FakeFrame fh = rig.core.feedHalts.front();
    CHECK(fh.reason == HaltReason::SOURCE_SCHEMA);
    CHECK(fh.expected == 6);
    CHECK(rig.core.cursor == 5);
}

TEST_CASE("RotationTest.mixedRotationHaltsSourceSchema")
{
    // Q2 (S9a/README): one source holed at e, the other bad at e: SOURCE_SCHEMA, each verdict logged
    Rig rig(2);
    run(rig.archive(0), 1, 10, true);
    run(rig.archive(0), 13, 20, false);
    SimRecording &b = rig.archive(1).record();
    appendJournal(rig.archive(1), b, 1, 10);
    Bytes bad = makeFrame(TEMPLATE_TRADE, 11, EPOCH, 64 * 11);
    putLe16(bad, 4, 1); // a schema 1 frame
    rig.archive(1).append(b, bad);
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(!rig.core.feedHalts.empty());
    const FakeFrame fh = rig.core.feedHalts.front();
    CHECK(fh.reason == HaltReason::SOURCE_SCHEMA);
    CHECK(fh.expected == 11);
    CHECK(fh.seen == 11);
    VerdictEvent events[16];
    const std::size_t n = rig.domain->drainVerdictEvents(events, 16);
    bool holed = false, badLogged = false;
    for (std::size_t i = 0; i < n; ++i)
    {
        holed = holed || (events[i].source == 0 && events[i].kind == VerdictKind::HOLED && events[i].e == 11);
        badLogged = badLogged || (events[i].source == 1 && events[i].kind == VerdictKind::BAD && events[i].e == 11);
    }
    CHECK(holed);
    CHECK(badLogged);
}

TEST_CASE("RotationTest.secondMemoFreeSeqRegressionIsBad")
{
    // S9a 7 S6: a recording whose seqs regress inside it; the second memo-free regression makes it BAD
    Rig rig(2);
    SimRecording &a = rig.archive(0).record();
    appendJournal(rig.archive(0), a, 1, 8);
    appendJournal(rig.archive(0), a, 4, 12); // 4 after 8 on the same replay
    run(rig.archive(1), 1, 12, false);
    rig.up();
    const bool done = rig.runUntil([&] { return rig.core.cursor == 12 || halted(rig); }, 60'000ms);
    CHECK(done);
    CHECK(rig.core.cursor == 12);
    CHECK(rig.metrics.badCount(BadReason::SEQ_REGRESSION) == 1);
}

TEST_CASE("RotationTest.unreachableSourcesNeverHalt")
{
    // 8.3 row 1: every source unreachable is a degraded state, never a FeedHalt; retried at expiry
    Rig rig(2);
    run(rig.archive(0), 1, 12, false);
    run(rig.archive(1), 1, 12, false);
    rig.archive(0).reachable = false;
    rig.archive(1).reachable = false;
    rig.up();
    rig.runFor(30'000ms);
    const bool noFeedHalt = rig.core.feedHalts.empty();
    CHECK(noFeedHalt);
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    CHECK((rig.domain->view().degraded & bit(Degraded::NO_REACHABLE_SOURCE)) != 0);
    rig.archive(1).reachable = true; // a returning source is tried again
    rig.archive(0).reachable = true;
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 12; }, 60'000ms));
    CHECK((rig.domain->view().degraded & bit(Degraded::NO_REACHABLE_SOURCE)) == 0);
}

TEST_CASE("RotationTest.transientStrikesBecomeUnreachable")
{
    Rig rig(2);
    run(rig.archive(0), 1, 12, false);
    run(rig.archive(1), 1, 12, false);
    rig.archive(0).startError = ArchiveErrorKind::GENERIC; // every startReplay fails: REPLAY_OPEN, a strike each
    rig.up();
    const bool done = rig.runUntil([&] { return rig.core.cursor == 12; }, 60'000ms);
    CHECK(done);
    CHECK(rig.switchesTo(1) >= 1);
}

TEST_CASE("RotationTest.mtuRefusedSourceIsSkipped")
{
    Rig rig(3);
    run(rig.archive(0), 1, 10, true);
    run(rig.archive(0), 13, 30, false);
    rig.archive(1).mtu = 9000; // every recording of B is refused (MTU_MISMATCH): REFUSED_STATIC
    run(rig.archive(1), 1, 30, false);
    run(rig.archive(2), 1, 20, true);
    run(rig.archive(2), 23, 30, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 30 || halted(rig); }, 120'000ms));
    CHECK(rig.core.cursor == 30);
    CHECK(rig.switchesTo(1) == 1); // tried once at e = 11, never again at e = 21
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("RotationTest.foreignPlusUnreachableHaltsSchemaNotEpoch")
{
    SUBCASE("A foreign, B and C unreachable past the grace: SOURCE_SCHEMA {e, e}")
    {
        Rig rig(3);
        rig.core.bound = EPOCH;
        rig.core.cursor = 3;
        SimRecording &a = rig.archive(0).record();
        appendJournal(rig.archive(0), a, 1, 3, EPOCH);
        for (std::int64_t s = 4; s <= 10; ++s)
        {
            rig.archive(0).append(a, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
        }
        run(rig.archive(1), 1, 10, false);
        run(rig.archive(2), 1, 10, false);
        rig.archive(1).reachable = false;
        rig.archive(2).reachable = false;
        rig.up();
        REQUIRE(rig.runUntil([&] { return halted(rig); }, 300'000ms));
        REQUIRE(!rig.core.feedHalts.empty());
        const FakeFrame fh = rig.core.feedHalts.front();
        CHECK(fh.reason == HaltReason::SOURCE_SCHEMA);
        CHECK(fh.expected == 4);
        CHECK(fh.seen == 4);
    }
    SUBCASE("every configured source foreign: EPOCH_MISMATCH")
    {
        Rig rig(3);
        rig.core.bound = EPOCH;
        rig.core.cursor = 3;
        for (std::size_t i = 0; i < 3; ++i)
        {
            SimRecording &r = rig.archive(i).record();
            appendJournal(rig.archive(i), r, 1, 3, EPOCH);
            for (std::int64_t s = 4; s <= 10; ++s)
            {
                rig.archive(i).append(r, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
            }
        }
        rig.up();
        REQUIRE(rig.runUntil([&] { return halted(rig); }));
        REQUIRE(!rig.core.feedHalts.empty());
        CHECK(rig.core.feedHalts.front().reason == HaltReason::EPOCH_MISMATCH);
        CHECK(rig.core.feedHalts.front().meEpochSeen == 9);
    }
}

TEST_CASE("RotationTest.laneAheadPastEPreventsGapHalt")
{
    // FIX-4: every source holed at e, but the confirming report shows another lane delivered e: no FeedHalt
    Rig rig(1);
    run(rig.archive(0), 1, 10, true);
    run(rig.archive(0), 13, 20, false);
    rig.core.beforeReport = [&](StubCore &core) {
        if (rig.domain->waiting() == DecisionRow::GAP && core.cursor == 10)
        {
            core.cursor = 12; // another lane applied 11 and 12
        }
    };
    rig.up();
    rig.runUntil([&] { return rig.core.cursor == 20 || halted(rig); });
    const bool noFeedHalt = rig.core.feedHalts.empty();
    CHECK(noFeedHalt);
    CHECK(rig.core.cursor == 20);
    CHECK(rig.metrics.fenceCount(FenceTrigger::LANE_AHEAD) == 1);
}

TEST_CASE("RotationTest.malformedExtentEverywhereHaltsSchema")
{
    Rig rig(2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        run(rig.archive(i), 1, 10, true);
        SimRecording &bad = rig.archive(i).record();
        appendJournal(rig.archive(i), bad, 11, 12);
        bad.rec.key.termBufferLength = 0; // a malformed extent the walk cannot pass (S9a 7 S8)
    }
    rig.up();
    const bool stopped = rig.runUntil([&] { return halted(rig); }, 120'000ms);
    CHECK(stopped);
    REQUIRE(!rig.core.feedHalts.empty());
    CHECK(rig.core.feedHalts.front().reason == HaltReason::SOURCE_SCHEMA);
    CHECK(rig.metrics.badCount(BadReason::MALFORMED_EXTENT) == 2);
}

TEST_CASE("RotationTest.positionRejectedIsNotAStrike")
{
    Direct d(2);
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.start();
    for (int i = 0; i < 5; ++i)
    {
        SourceFault f;
        f.source = 0;
        f.kind = i % 2 == 0 ? FaultKind::POSITION_REJECTED : FaultKind::RESUME_BELT;
        d.domain->onFault(f);
        d.attach(0); // the follower re-reads memo-free
    }
    const SourceCommand cmd = d.domain->takeSourceCommand();
    CHECK(cmd.kind == SourceCommandKind::NONE);
    CHECK(d.domain->verdicts().at(0).kind == VerdictKind::UNKNOWN);
}

TEST_CASE("RotationTest.rotationWithLaneAheadFencesInsteadOfRotating")
{
    // FIX-16: with C' > last a rotation would resume the next source past e; a sync from C' instead
    Direct d(2);
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.start();
    d.core.cursor = 20; // another lane is at 20
    d.tick(10'001ms);
    REQUIRE(d.domain->view().cursor == 20);
    REQUIRE(d.domain->view().last == 10);
    CHECK(d.rec(TEMPLATE_TRADE, 12) == SinkAction::ABORT); // a hole at 11 on A
    const SourceCommand cmd = d.domain->takeSourceCommand();
    const bool switched = SourceCommandKind::SWITCH == cmd.kind;
    CHECK_FALSE(switched);
    CHECK(d.metrics.fenceCount(FenceTrigger::LANE_AHEAD) == 1);
    d.pump();
    CHECK(d.domain->view().last == 20);
}

TEST_CASE("RotationTest.headRefusalOnEverySourceHaltsSourceSchema")
{
    // review NOTE-6: S9a 4.4 row 7 (HEAD: journalSeq 0 or meEpoch 0) through the domain: a refusal like any other row,
    // BAD at e on each source, SOURCE_SCHEMA {e, e} when every source is BAD
    Rig rig(2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        SimRecording &r = rig.archive(i).record();
        appendJournal(rig.archive(i), r, 1, 5);
        rig.archive(i).append(r, makeFrame(TEMPLATE_TRADE, 0, i == 0 ? EPOCH : 0, 64)); // seq 0 / epoch 0
        appendJournal(rig.archive(i), r, 6, 12);
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return halted(rig); }));
    REQUIRE(!rig.core.feedHalts.empty());
    CHECK(rig.core.feedHalts.front().reason == HaltReason::SOURCE_SCHEMA);
    CHECK(rig.core.feedHalts.front().expected == 6);
    CHECK(rig.core.feedHalts.front().seen == 6);
    CHECK(rig.metrics.badCount(BadReason::REFUSAL_HEAD) == 2);
    CHECK(rig.core.cursor == 5);
}

TEST_CASE("RotationTest.refusalFromAMemoReadIsVerifiedFirst")
{
    // found by the 2026-10-31 property run with post-halt events: a re-journalled recording R1 carries a corrupt copy
    // of 15 while the earlier recording R0 holds every seq. The first (memo-free) read crosses the corrupt copy only
    // after R0 delivered everything (a valid SOURCE_SCHEMA at 31), and proves R1 at 11 on the way. After the leader
    // change loses 12..30, the sync from 11 resumes on R1's proof: that memo read meets the corrupt 15 BEFORE any valid
    // 15. Like a hole (8.6), a refusal on a memo read is verified memo-free once per (source, e) before it becomes BAD:
    // the walk from R0 delivers 12..30. Without the verification the bridge halts SOURCE_SCHEMA at 15 although a
    // memo-free read delivers it (13.10 (c)).
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 11;
    run(rig.archive(0), 1, 30, true);
    SimRecording &rj = rig.archive(0).record();
    appendJournal(rig.archive(0), rj, 8, 14);
    Bytes bad = makeFrame(TEMPLATE_TRADE, 15, EPOCH, 64 * 15);
    putLe16(bad, 4, 3);
    rig.archive(0).append(rj, bad);
    appendJournal(rig.archive(0), rj, 16, 30);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->view().state == State::STREAMING; }));
    rig.core.commitImmediately = false; // nothing commits: the first read's offers are lost with the leader
    REQUIRE(rig.runUntil([&] { return rig.memo->proof(0, rj.rec.key).has_value(); }, 5'000ms));
    rig.runFor(200ms);
    rig.core.elect();
    rig.core.egress.clear();
    rig.core.commitImmediately = true;
    rig.domain->onFence(FenceTrigger::NEW_LEADER, rig.now);
    rig.domain->onSessionUp(rig.now);
    rig.core.commit();
    rig.runUntil([&] { return rig.core.cursor == 30; }, 120'000ms);
    bool falseSchemaAtFifteen = false;
    for (const auto &f : rig.core.feedHalts)
    {
        falseSchemaAtFifteen = falseSchemaAtFifteen || f.expected == 15;
    }
    CHECK_FALSE(falseSchemaAtFifteen);
    CHECK(rig.core.cursor == 30);
    CHECK(BridgeMetrics::get(rig.metrics.refusalVerifications) >= 1);
}

TEST_CASE("RotationTest.streamingHoleStillRotatesFirst")
{
    // RC2 S10' 5.1: while STREAMING nothing changes: a hole on source 0 rotates to source 1 (which has e) at once; no
    // successor walk although source 0 has a later recording
    Rig rig(2);
    SimRecording &a0 = rig.archive(0).record();
    appendJournal(rig.archive(0), a0, 1, 10);
    appendJournal(rig.archive(0), a0, 12, 20);
    rig.archive(0).stop(a0);
    SimRecording &a1 = rig.archive(0).record();
    appendJournal(rig.archive(0), a1, 15, 20); // a later recording that lacks 11 too
    SimRecording &b = rig.archive(1).record();
    appendJournal(rig.archive(1), b, 1, 20);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 20; }, 120'000ms));
    CHECK(BridgeMetrics::get(rig.metrics.sourceFailovers) >= 1);
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    const std::uint64_t relists = rig.metrics.relists(0) + rig.metrics.relists(1);
    CHECK_MESSAGE(relists == 0, "a successor walk while STREAMING");
}
