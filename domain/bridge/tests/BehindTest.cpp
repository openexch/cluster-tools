// BehindTest (RC2 S9b 13.6, Q3 of S9a/README): the BEHIND verdict of 8.4 and rows 2-3 of 8.3.
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

bool halted(Rig &rig)
{
    return rig.domain->view().state == State::HALTED;
}

}

TEST_CASE("BehindTest.deadNodeAtItsHeadFailsOverAfterDwell")
{
    // the 08-12 zombie: A is attached, healthy and at its live head below e while B has e
    Rig rig(2);
    run(rig.archive(0), 1, 10, false);
    run(rig.archive(1), 1, 20, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 10; }));
    const TimePoint stuck = rig.now;
    const bool done = rig.runUntil([&] { return rig.core.cursor == 20; }, 120'000ms);
    CHECK(done);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(rig.now - stuck);
    CHECK(took <= 25'000ms); // 10 s dwell + one probe interval (+ the first probe after the attach)
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("BehindTest.quietFeedIsNotDegradedAndRotatesAtMostOncePerMinute")
{
    Rig rig(2);
    run(rig.archive(0), 1, 10, false);
    run(rig.archive(1), 1, 10, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 10; }));
    bool everDegraded = false;
    for (int i = 0; i < 300; ++i) // 5 minutes of a quiet market
    {
        rig.runFor(1'000ms);
        everDegraded = everDegraded || (rig.domain->view().degraded & bit(Degraded::NO_SOURCE_BEYOND_E)) != 0;
    }
    const std::uint64_t failovers = BridgeMetrics::get(rig.metrics.sourceFailovers);
    CHECK(failovers <= 6); // the first one, then at most one per behindRescan (60 s)
    CHECK_FALSE(everDegraded);
    CHECK(rig.core.feedHalts.empty());
    CHECK(rig.domain->view().halt == BridgeHalt::NONE);
}

TEST_CASE("BehindTest.laneAheadMakesAllBehindDegraded")
{
    Rig rig(2);
    run(rig.archive(0), 1, 10, false);
    run(rig.archive(1), 1, 10, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->waiting() == DecisionRow::ALL_BEHIND; }, 120'000ms));
    rig.core.cursor = 15; // another lane delivered 11..15
    REQUIRE(rig.runUntil([&] { return (rig.domain->view().degraded & bit(Degraded::NO_SOURCE_BEYOND_E)) != 0; },
        30'000ms));
    CHECK(rig.metrics.fenceCount(FenceTrigger::LANE_AHEAD) >= 1);
    CHECK(rig.core.feedHalts.empty());
}

TEST_CASE("BehindTest.holeWithLaggingSourceWaitsThenHalts")
{
    SUBCASE("the lagging source catches up within the grace: no halt")
    {
        Rig rig(2);
        run(rig.archive(0), 1, 10, true);
        run(rig.archive(0), 13, 20, false);
        SimRecording &b = run(rig.archive(1), 1, 8, false);
        rig.up();
        REQUIRE(rig.runUntil([&] { return rig.domain->waiting() == DecisionRow::HOLE_GRACE || halted(rig); }, 60'000ms));
        rig.runFor(30'000ms);
        const bool noHalt = rig.core.feedHalts.empty();
        CHECK(noHalt);
        appendJournal(rig.archive(1), b, 9, 20); // B catches up
        REQUIRE(rig.runUntil([&] { return rig.core.cursor == 20 || halted(rig); }, 60'000ms));
        CHECK(rig.core.cursor == 20);
        CHECK(rig.core.feedHalts.empty());
    }
    SUBCASE("without it, GAP after the grace")
    {
        Rig rig(2);
        run(rig.archive(0), 1, 10, true);
        run(rig.archive(0), 13, 20, false);
        run(rig.archive(1), 1, 8, false);
        rig.up();
        REQUIRE(rig.runUntil([&] { return rig.domain->waiting() == DecisionRow::HOLE_GRACE || halted(rig); }, 60'000ms));
        const TimePoint graceStart = rig.domain->verdicts().firstAt();
        REQUIRE(rig.runUntil([&] { return halted(rig); }, 120'000ms));
        CHECK(rig.now - graceStart >= 60'000ms);
        REQUIRE(!rig.core.feedHalts.empty());
        const FakeFrame fh = rig.core.feedHalts.front();
        CHECK(fh.reason == HaltReason::GAP);
        CHECK(fh.expected == 11);
        CHECK(fh.seen == 13); // the real HOLED source's seen; the BEHIND one counts as holed without a seen
    }
}

TEST_CASE("BehindTest.allBehindParksOnTheLastLiveSource")
{
    // FIX-5: dead node 0, live node 1, a quiet period: parked on 1, not on the first configured
    Rig rig(2);
    run(rig.archive(0), 1, 5, false);
    SimRecording &live = run(rig.archive(1), 1, 10, false);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 10; }, 120'000ms));
    REQUIRE(rig.runUntil([&] { return rig.domain->waiting() == DecisionRow::ALL_BEHIND; }, 120'000ms));
    rig.runFor(5'000ms);
    appendJournal(rig.archive(1), live, 11, 12); // the first post-quiet e
    const TimePoint appended = rig.now;
    rig.runUntil([&] { return rig.core.cursor == 12; }, 120'000ms);
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(rig.now - appended);
    CHECK(latency <= 10'000ms);
    CHECK(rig.core.cursor == 12);
}

TEST_CASE("BehindTest.dwellStartsAfterTheFirstProbe")
{
    // FIX-6: after a switch the backlog gauge still holds the previous source's value (0, at its head) until the
    // new source's first probe; it must not make the new one BEHIND
    Direct d(2, [](BridgeSettings &s) { s.behindDwell = 10'000ms; });
    d.core.bound = EPOCH;
    d.core.cursor = 10;
    d.start();
    d.archiveMetrics.sourceBacklogBytes.store(0);
    d.attach(1, 0);
    for (int i = 0; i < 1200; ++i) // 12 s: the first probe of B would land at 10 s
    {
        d.tick(10ms);
    }
    d.archiveMetrics.sourceBacklogBytes.store(5'000'000); // B's first probe: it has a backlog
    for (int i = 0; i < 300; ++i)
    {
        d.tick(10ms);
    }
    const VerdictKind b = d.domain->verdicts().at(1).kind;
    CHECK(b == VerdictKind::UNKNOWN);
}
