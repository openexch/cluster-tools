// The stall verdicts of one follower as an owner that is not the bridge reads them (FollowPolicy::stallVerdicts,
// checkedClean, stallReasonOf), driven through FollowerLoop over the simulated archive on a manual clock.
#include "doctest/doctest.h"

#include "SimArchive.h"

using namespace cluster_tools::archive;
using namespace ct_archive_test;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t EPOCH = 9;
constexpr std::chrono::milliseconds TWO_PROBES = 20'000ms; // two probe intervals of the default settings

bool clean(Harness &h)
{
    return checkedClean(h.policy->stallVerdicts(), h.now, TWO_PROBES);
}

}

TEST_CASE("StallVerdictsTest.theMappingIsTheCountersMapping")
{
    CHECK(stallReasonOf(FaultKind::NEVER_CONNECTED) == StallReason::NEVER_CONNECTED);
    CHECK(stallReasonOf(FaultKind::IMAGE_LOST) == StallReason::IMAGE_LOST);
    CHECK(stallReasonOf(FaultKind::ARCHIVE_GONE) == StallReason::ARCHIVE_GONE);
    CHECK(stallReasonOf(FaultKind::STALL) == StallReason::STALL);
    CHECK(stallReasonOf(FaultKind::STALL_AFTER_STOP) == StallReason::STALL_AFTER_STOP);
    CHECK(stallReasonOf(FaultKind::STALL_BOUNDED_STUCK) == StallReason::BOUNDED_STUCK);
    CHECK(stallReasonOf(FaultKind::EARLY_EOF) == StallReason::EARLY_EOF);
    CHECK(stallReasonOf(FaultKind::ARCHIVE_ERROR) == StallReason::ARCHIVE_ERROR);
    for (const FaultKind kind : { FaultKind::NONE, FaultKind::FATAL, FaultKind::CONNECT_FAILED, FaultKind::LIST_FAILED,
             FaultKind::MALFORMED_EXTENT, FaultKind::REPLAY_STORAGE, FaultKind::REPLAY_ENDPOINT_UNRESOLVED,
             FaultKind::REPLAY_OPEN, FaultKind::POSITION_REJECTED, FaultKind::SCHEMA_REFUSED,
             FaultKind::SEQ_REGRESSION_IN_RECORDING, FaultKind::RESUME_BELT })
    {
        CAPTURE(faultName(kind));
        CHECK_FALSE(stallReasonOf(kind).has_value());
    }
}

TEST_CASE("StallVerdictsTest.aHealthyLiveFollowPassesEveryProbe")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    CHECK_FALSE(h.policy->stallVerdicts().lastPassAt.has_value());
    CHECK_FALSE(clean(h)); // never checked
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    REQUIRE(h.runUntil([&] { return h.policy->stallVerdicts().lastPassAt.has_value(); }, 400, 50ms));
    for (int i = 0; i < 6; ++i) // a minute of probes, each one clean
    {
        h.run(200, 50ms);
        CHECK(clean(h));
    }
    CHECK(h.policy->stallVerdicts().count == 0);
    CHECK_FALSE(h.policy->stallVerdicts().last.has_value());
    // The owner's clock moves on with no check (the duty thread stopped): the verdict ages out.
    const TimePoint checkedAt = *h.policy->stallVerdicts().lastPassAt;
    CHECK_FALSE(checkedClean(h.policy->stallVerdicts(), checkedAt + TWO_PROBES + 1ms, TWO_PROBES));
    CHECK(checkedClean(h.policy->stallVerdicts(), checkedAt + TWO_PROBES, TWO_PROBES));
}

// The 07-10 shape: the recording grows, the replay stays put. Nothing reads clean again until a later check passes:
// not the re-attach, not the first probe after it (it only sets the baseline), so a source that stays stuck never
// reads clean between its stall verdicts.
TEST_CASE("StallVerdictsTest.aStuckReplayIsNotCleanUntilALaterCheckPasses")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.deliverNothing = true;
    h.archive.appendTrades(r, 4, 2, EPOCH);
    REQUIRE(h.runUntil([&] { return h.observer.sawFault(FaultKind::STALL); }, 600, 50ms));
    const StallVerdicts &v = h.policy->stallVerdicts();
    REQUIRE(v.sincePass.has_value());
    CHECK(*v.sincePass == StallReason::STALL);
    CHECK(v.last == StallReason::STALL);
    CHECK(v.count == 1);
    CHECK(v.lastAt == h.now);
    CHECK(h.metrics.stalls(StallReason::STALL) == 1);
    // Back off, re-list, re-attach: FOLLOWING again, the source still delivers nothing. Through the next probe (the
    // baseline) and up to the second stall verdict the follower never reads clean.
    const TimePoint stalledAt = h.now;
    bool everClean = false;
    bool refollowed = false;
    REQUIRE(h.runUntil([&] {
        everClean = everClean || clean(h);
        refollowed = refollowed || h.policy->phase() == Phase::FOLLOWING;
        return h.policy->stallVerdicts().count == 2;
    }, 1000, 50ms));
    CHECK(refollowed);
    CHECK(h.now - stalledAt >= 20'000ms); // two probes after the re-attach
    CHECK_FALSE(everClean);
    CHECK(h.policy->stallVerdicts().sincePass == StallReason::STALL);
    // The source heals: the next check passes and the follower reads clean.
    h.archive.deliverNothing = false;
    REQUIRE(h.runUntil([&] { return h.sink.seqs.back() == 5; }, 400, 50ms)); // a re-attach may re-read from its proof
    REQUIRE(h.runUntil([&] { return clean(h); }, 800, 50ms));
    CHECK(h.policy->stallVerdicts().last == StallReason::STALL); // the history stays
}

TEST_CASE("StallVerdictsTest.aBoundedReplayIsCheckedWithoutAnArchiveCall")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 5, EPOCH);
    h.archive.stop(r);
    h.archive.deliverNothing = true;
    h.attach();
    REQUIRE(h.runUntil([&] { return h.policy->phase() == Phase::FOLLOWING; }, 50));
    REQUIRE(h.runUntil([&] { return h.observer.sawFault(FaultKind::STALL_BOUNDED_STUCK); }, 600, 50ms));
    CHECK(h.policy->stallVerdicts().sincePass == StallReason::BOUNDED_STUCK);
    CHECK_FALSE(clean(h));
    CHECK(h.archive.probeCalls == 0);
}

TEST_CASE("StallVerdictsTest.aPausedFollowerPassesOnlyWhenCaughtUp")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.run(1200, 50ms, 0); // paused and caught up for 60 s
    CHECK(clean(h));
    h.archive.appendTrades(r, 4, 20, EPOCH); // a backlog the paused owner does not read: the stuck rules cannot judge
    h.run(1200, 50ms, 0);
    CHECK_FALSE(clean(h));
    CHECK(h.observer.faults.empty()); // and it is no stall either (the pause rule)
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 23; }, 100));
    REQUIRE(h.runUntil([&] { return clean(h); }, 800, 50ms));
}

TEST_CASE("StallVerdictsTest.aLostArchiveIsAVerdictOnTheProbe")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    REQUIRE(h.runUntil([&] { return clean(h); }, 400, 50ms));
    h.archive.probeFails = true;
    REQUIRE(h.runUntil([&] { return h.observer.sawFault(FaultKind::ARCHIVE_GONE); }, 400, 50ms));
    CHECK(h.policy->stallVerdicts().sincePass == StallReason::ARCHIVE_GONE);
    CHECK_FALSE(clean(h));
}
