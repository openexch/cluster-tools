// LaggedEgressTest (the 2026-10-31 review): the halt path under a realistic egress latency.
// A real core answers a QueryFeedPosition only after a Raft commit and the egress hop, i.e. after the duty loop has
// polled the sources again. "Confirm before halting" must then send ONE query per
// (row, e) and wait for it with the source paused; re-querying on every redelivery of the aborted frame turned every
// answer stale, so the bridge never halted and committed a QueryFeedPosition per duty iteration (the review probe:
// 29,995 queries in 30 s, no FeedHalt). The Rig delivers egress `lag` iterations late (1 ms iterations).
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

#include <algorithm>

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

struct Result
{
    int queries = 0;
    int confirmingQueries = 0; // queries offered while a confirmation was outstanding (its own included)
    int confirmations = 0;
    int feedHalts = 0;
    State state = State::DOWN;
    BridgeHalt halt = BridgeHalt::NONE;
    std::uint64_t stale = 0;
};

Result holedEverywhere(std::size_t sources, int lag)
{
    Rig rig(sources);
    rig.lag = lag;
    for (std::size_t i = 0; i < sources; ++i)
    {
        run(rig.archive(i), 1, 10, true);
        run(rig.archive(i), 12 + static_cast<std::int64_t>(i), 20, false); // every source holed at 11
    }
    rig.up();
    Result r;
    bool was = false;
    int lastOffered = 0;
    for (int i = 0; i < 30'000; ++i) // 30 s of 1 ms duty iterations
    {
        rig.step(1ms);
        const bool confirming = rig.domain->confirming();
        const int offered = static_cast<int>(std::count_if(rig.core.offered.begin(), rig.core.offered.end(),
            [](const FakeFrame &f) { return T_QUERY_FEED_POSITION == f.moneyTemplate; }));
        if (confirming && !was)
        {
            ++r.confirmations;
        }
        if (confirming || was)
        {
            r.confirmingQueries += offered - lastOffered;
        }
        lastOffered = offered;
        was = confirming;
    }
    r.queries = static_cast<int>(std::count_if(rig.core.offered.begin(), rig.core.offered.end(),
        [](const FakeFrame &f) { return T_QUERY_FEED_POSITION == f.moneyTemplate; }));
    r.feedHalts = static_cast<int>(rig.core.feedHalts.size());
    r.state = rig.domain->view().state;
    r.halt = rig.domain->view().halt;
    r.stale = BridgeMetrics::get(rig.metrics.reportStale);
    MESSAGE("sources " << sources << " lag " << lag << ": queries " << r.queries << ", confirmations " << r.confirmations
            << ", queries while confirming " << r.confirmingQueries << ", FeedHalts " << r.feedHalts << ", state "
            << stateName(r.state) << ", stale " << r.stale);
    return r;
}

}

TEST_CASE("LaggedEgressTest.holeOnEverySourceHaltsGapUnderEgressLag")
{
    for (std::size_t sources : { std::size_t(1), std::size_t(2) })
    {
        for (int lag : { 1, 5, 50 })
        {
            CAPTURE(sources);
            CAPTURE(lag);
            const Result r = holedEverywhere(sources, lag);
            const bool halted = r.state == State::HALTED && r.halt == BridgeHalt::GAP;
            CHECK(halted);
            CHECK(r.feedHalts == 1);
            CHECK(r.confirmations == 1);
            CHECK(r.confirmingQueries == 1); // exactly one query per confirmation
            CHECK(r.stale == 0);             // no answer goes stale in the steady state
            CHECK(r.queries < 20);           // the sync, the confirmation, the 5 s halted cadence: bounded
        }
    }
}

TEST_CASE("LaggedEgressTest.gapRescanUnderALongLagIssuesABoundedNumberOfQueries")
{
    Rig rig(1);
    run(rig.archive(0), 1, 10, true);
    run(rig.archive(0), 12, 20, false);
    rig.up();
    for (int i = 0; i < 5'000; ++i)
    {
        rig.step(1ms);
    }
    REQUIRE(rig.domain->view().state == State::HALTED);
    REQUIRE(rig.domain->view().halt == BridgeHalt::GAP);
    rig.lag = 2'000; // 2 s: longer than the 500 ms resend, every query is re-sent a few times
    const int before = rig.core.queries;
    for (int i = 0; i < 180'000; ++i) // three minutes: the re-scans at 60 s and 120 s, the 5 s halted queries
    {
        rig.step(1ms);
    }
    const int queries = rig.core.queries - before;
    MESSAGE("HALTED(GAP), lag 2 s for 180 s: queries " << queries << ", stale " << BridgeMetrics::get(rig.metrics.reportStale)
            << ", state " << stateName(rig.domain->view().state));
    CHECK(queries < 300);
    CHECK(rig.domain->view().halt == BridgeHalt::GAP);
    CHECK(rig.core.feedHalts.size() >= 1);
}
