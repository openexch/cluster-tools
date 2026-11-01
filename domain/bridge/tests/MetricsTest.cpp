// MetricsTest (RC2 S9b 13.9, section 12, S9a 11a.13).
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

#include <cstring>

using namespace ct_bridge_test;

TEST_CASE("MetricsTest.recordingStartStepBackIsCounted")
{
    // S9a 11a.13: a later recording of a source whose first seq is below an earlier recording's first seq
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 2;
    SimRecording &a = rig.archive(0).record();
    for (std::int64_t s = 3; s <= 10; ++s)
    {
        rig.archive(0).append(a, makeFrame(TEMPLATE_TRADE, s, EPOCH, 64 * s));
    }
    rig.archive(0).stop(a);
    SimRecording &b = rig.archive(0).record();
    for (std::int64_t s = 2; s <= 20; ++s) // starts at 2, below the earlier recording's first seq 3
    {
        rig.archive(0).append(b, makeFrame(TEMPLATE_TRADE, s, EPOCH, 64 * s));
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 20; }));
    const std::uint64_t stepbacks = BridgeMetrics::get(rig.metrics.recordingStartStepBack);
    CHECK(stepbacks == 1);
}

TEST_CASE("MetricsTest.everySeriesHasItsFixedLabels")
{
    for (unsigned i = 0; i < static_cast<unsigned>(FrameKind::COUNT); ++i)
    {
        CHECK(std::strcmp(frameKindLabel(static_cast<FrameKind>(i)), "?") != 0);
    }
    for (unsigned i = 0; i < static_cast<unsigned>(FenceTrigger::COUNT); ++i)
    {
        CHECK(std::strcmp(fenceTriggerName(static_cast<FenceTrigger>(i)), "?") != 0);
    }
    for (unsigned i = 0; i < static_cast<unsigned>(BadReason::COUNT); ++i)
    {
        CHECK(std::strcmp(badReasonLabel(static_cast<BadReason>(i)), "?") != 0);
    }
    for (unsigned i = 0; i < static_cast<unsigned>(DeferReason::COUNT); ++i)
    {
        CHECK(std::strcmp(deferReasonLabel(static_cast<DeferReason>(i)), "?") != 0);
    }
    for (unsigned i = 0; i <= 5; ++i)
    {
        CHECK(std::strcmp(haltReasonName(static_cast<HaltReason>(i)), "?") != 0);
    }
    BridgeMetrics m;
    CHECK(m.forwarded.size() == 6);
    CHECK(m.skipped.size() == 5);
    CHECK(m.feedHaltsSent.size() == 6);
    CHECK(m.fences.size() == 9);
    CHECK(m.settleRejected.size() == 10);
    CHECK(m.sourceVerdicts.size() == MAX_SOURCES);
    CHECK(frameKindOf(61) == FrameKind::K61);
    CHECK(frameKindOf(68) == FrameKind::K68);
    CHECK(frameKindOf(66) == FrameKind::COUNT);
    // a run moves the series it owns
    Rig rig(1);
    SimRecording &r = rig.archive(0).record();
    appendJournal(rig.archive(0), r, 1, 6);
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.core.cursor == 6; }));
    rig.runFor(11'000ms); // the next periodic report
    CHECK(rig.metrics.forwardedFrames(FrameKind::K68) == 1);
    CHECK(BridgeMetrics::value(rig.metrics.coreCursor) == 6);
    CHECK(BridgeMetrics::value(rig.metrics.lastOfferedSeq) == 6);
    CHECK(BridgeMetrics::value(rig.metrics.meEpoch) == EPOCH);
    CHECK(BridgeMetrics::get(rig.metrics.epochs) == 1);
    CHECK(BridgeMetrics::get(rig.metrics.reportQueries) >= 1);
    CHECK(BridgeMetrics::value(rig.metrics.halted) == 0);
}

// RC2 S9c W2a (2026-10-31): BridgeView carries the domain's time of its last accepted report and the follower's
// consumed position as the domain last saw it; a skip walk moves the position without a forward; a session loss
// resets the report time.
TEST_CASE("MetricsTest.viewCarriesReportTimeAndConsumedPosition")
{
    Rig rig(1);
    rig.core.bound = EPOCH;
    rig.core.cursor = 30; // the core already applied 1..30: the walk over them is skips only
    SimRecording &a = rig.archive(0).record();
    appendJournal(rig.archive(0), a, 1, 40);
    CHECK(rig.domain->view().lastReportAt == TimePoint{});
    rig.up();
    std::int64_t lastPosition = rig.domain->view().consumedPosition;
    bool movedWithoutForward = false;
    REQUIRE(rig.runUntil([&] {
        const BridgeView v = rig.domain->view();
        if (v.consumedPosition != lastPosition && v.lastForwardAt == TimePoint{})
        {
            movedWithoutForward = true;
        }
        lastPosition = v.consumedPosition;
        return rig.core.cursor == 40;
    }));
    CHECK(movedWithoutForward); // the skip walk over 1..30 advanced the consumed position, no forward yet
    rig.runFor(std::chrono::milliseconds(500));
    const BridgeView v = rig.domain->view();
    CHECK(v.lastReportAt != TimePoint{});
    CHECK(v.lastReportAt <= rig.now);
    CHECK(v.consumedPosition > 0);
    CHECK(v.lastForwardAt != TimePoint{});
    // the next periodic report moves lastReportAt forward
    const TimePoint first = v.lastReportAt;
    rig.runFor(std::chrono::milliseconds(11'000));
    CHECK(rig.domain->view().lastReportAt > first);
    rig.domain->onSessionDown(rig.now);
    CHECK(rig.domain->view().lastReportAt == TimePoint{});
}
