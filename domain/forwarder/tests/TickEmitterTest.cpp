// TickEmitterTest (rules E1-E11, the 6.0 reset table with two cycles per field in one
// run). One market (1) through a rig; the ladder is driven with decoded frames (BookFixtures.h) and time is handed in.
#include "doctest/doctest.h"

#include <vector>

#include "BookFixtures.h"
#include "cluster_tools/domain/forwarder/BookLadder.h"
#include "cluster_tools/domain/forwarder/TickEmitter.h"

using namespace cluster_tools::domain::forwarder;
using namespace cluster_tools::domain::forwarder::book_fixtures;

namespace {

constexpr std::int64_t MS = 1'000'000LL;
constexpr std::int64_t S = 1000 * MS;
constexpr std::int64_t T = TickEmitter::T_CAP; // 100 ms

struct Offer
{
    int market;
    std::int64_t bid, ask, tradePrice, tradeId, version;
    TickEdge edge;
};

struct Sink : TickSink
{
    std::vector<Offer> offers;
    bool accept = true;
    bool tryOffer(int market, const TickCandidate &c, TickEdge edge) noexcept override
    {
        if (!accept)
        {
            return false;
        }
        offers.push_back(Offer{ market, c.bid, c.ask, c.tradePrice, c.tradeId, c.version, edge });
        return true;
    }
};

// market 1 only; `e.step(t)` applies nothing, it only runs the emitter at time t
struct Rig
{
    TickEmitter em;
    BookLadder l;
    TradeMark mark;
    Sink sink;
    TickGate gate = TickGate::OPEN;
    std::int64_t now = 0;

    void step(std::int64_t t)
    {
        now = t;
        em.step(1, l, mark, now, gate, sink);
    }
    // step 5a for a market WITHOUT a frame this iteration: only when the emitter says it is due
    void stepIfDue(std::int64_t t)
    {
        if (em.due(1, t))
        {
            step(t);
        }
    }
    LadderOutcome book(const BookFrame &f, std::int64_t t)
    {
        now = t;
        return l.apply(f, t);
    }
    void trade(std::int64_t id, std::int64_t price) { mark.onBatch(trades(1, id, price)); }
    const TickCandidate &lastOffered() const { return em.lastOffered(1); }
    std::int64_t pendingAgeNs(std::int64_t t) const { return em.pendingAgeNs(1, t); }
    const TickSlotCounters &counters() const { return em.counters(1); }
    bool due(std::int64_t t) const { return em.due(1, t); }
};

// OK ladder at `version` with top 100 / 110 and a second level on each side, offered at t = 0 (one leading offer).
void primed(Rig &e, std::int64_t version = 1)
{
    REQUIRE(e.book(snapshot(1, version, { { 100, 5 }, { 90, 6 } }, { { 110, 7 }, { 120, 8 } }), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(0);
    REQUIRE(e.sink.offers.size() == 1);
    REQUIRE(e.sink.offers[0].edge == TickEdge::LEADING);
    REQUIRE(e.sink.offers[0].bid == 100);
    REQUIRE(e.sink.offers[0].ask == 110);
    REQUIRE(e.sink.offers[0].version == version);
}

// a top-of-book move: the best bid becomes `price` (a NEW above the current best, below the ask)
BookFrame topMove(std::int64_t from, std::int64_t to, std::int64_t price)
{
    return delta(1, from, to, { bidNew(price, 1) });
}

} // namespace

TEST_CASE("TickEmitterTest.isolatedChangeIsOfferedAtOnce")
{
    Rig e;
    auto &offers = e.sink.offers;
    REQUIRE(e.book(snapshot(1, 1, { { 100, 5 } }, { { 110, 7 } }), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(0);
    CHECK(offers.size() == 1);
    REQUIRE(!offers.empty());
    CHECK(offers[0].market == 1);
    CHECK(offers[0].bid == 100);
    CHECK(offers[0].ask == 110);
    CHECK(offers[0].version == 1);
    CHECK(offers[0].tradeId == 0);
    CHECK(offers[0].edge == TickEdge::LEADING);
    CHECK(e.em.windowEndNs(1) == T);
    CHECK(e.lastOffered().version == 1);
    CHECK(e.counters().offeredOn(TickEdge::LEADING) == 1);
    CHECK(e.pendingAgeNs(0) == 0);
    // quiet for 500 ms, then one isolated change: offered at once again
    REQUIRE(e.book(topMove(1, 2, 101), 500 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(500 * MS);
    CHECK(offers.size() == 2);
    CHECK(offers.back().bid == 101);
    CHECK(offers.back().version == 2);
    CHECK(offers.back().edge == TickEdge::LEADING);
    CHECK(e.em.windowEndNs(1) == 500 * MS + T);
}

TEST_CASE("TickEmitterTest.burstEndsWithTheNewestState")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    REQUIRE(e.book(topMove(1, 2, 101), 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(10 * MS);
    CHECK(offers.size() == 1);
    CHECK(e.em.trailingDue(1));
    CHECK(e.pendingAgeNs(20 * MS) == 10 * MS);
    const std::int64_t newestBid = 102;
    REQUIRE(e.book(topMove(2, 3, newestBid), 60 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(60 * MS);
    CHECK(offers.size() == 1);
    CHECK_FALSE(e.due(99 * MS));
    CHECK(e.due(100 * MS));
    e.step(100 * MS);
    REQUIRE(offers.size() == 2);
    CHECK(offers.back().bid == newestBid);
    CHECK(offers.back().version == 3);
    CHECK(offers.back().edge == TickEdge::TRAILING);
    CHECK_FALSE(e.em.trailingDue(1));
    CHECK(e.em.windowEndNs(1) == 200 * MS); // the window restarts at the trailing offer
    CHECK(e.pendingAgeNs(100 * MS) == 0);
    CHECK(e.counters().offeredOn(TickEdge::TRAILING) == 1);
}

TEST_CASE("TickEmitterTest.deeperLevelChangeEmitsNothing")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    REQUIRE(e.book(delta(1, 1, 2, { bidUpd(90, 9), askNew(130, 1) }), 500 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(500 * MS);
    CHECK(offers.size() == 1);
    CHECK_FALSE(e.em.trailingDue(1));
    CHECK(e.pendingAgeNs(500 * MS) == 0);
    CHECK(e.lastOffered().version == 1);
    // inside a window too: a deeper change sets no trailing flag
    REQUIRE(e.book(delta(1, 2, 3, { bidUpd(90, 3) }), 520 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(520 * MS);
    CHECK(offers.size() == 1);
    CHECK_FALSE(e.em.trailingDue(1));
}

TEST_CASE("TickEmitterTest.tradeOnlyChangeEmitsNothing")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    e.trade(7, 105);
    e.step(500 * MS);
    CHECK(offers.size() == 1);
    CHECK(e.lastOffered().tradeId == 0);
    CHECK(e.pendingAgeNs(500 * MS) == 0);
    CHECK_FALSE(e.em.trailingDue(1));
    CHECK(e.counters().suppressedFor(TickSuppressed::VERSION_NOT_ABOVE) == 0); // not a change, so not "suppressed"
}

TEST_CASE("TickEmitterTest.tradeWithVersionMoveRidesOnTheTick")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    e.trade(7, 105);
    REQUIRE(e.book(delta(1, 1, 2, { bidUpd(90, 1) }), 500 * MS) == LadderOutcome::DELTA_APPLIED); // top unchanged
    e.step(500 * MS);
    REQUIRE(offers.size() == 2);
    CHECK(offers.back().bid == 100);
    CHECK(offers.back().ask == 110);
    CHECK(offers.back().tradeId == 7);
    CHECK(offers.back().tradePrice == 105);
    CHECK(offers.back().version == 2);
    CHECK(e.lastOffered().tradeId == 7);
    // the same trade again with another version move is not a change (not newer)
    REQUIRE(e.book(delta(1, 2, 3, { bidUpd(90, 2) }), 1 * S) == LadderOutcome::DELTA_APPLIED);
    e.step(1 * S);
    CHECK(offers.size() == 2);
}

TEST_CASE("TickEmitterTest.brokenLadderOffersNothingAndTheTrailingFlagWaits")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    REQUIRE(e.book(topMove(1, 2, 101), 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(10 * MS);
    REQUIRE(e.em.trailingDue(1));
    REQUIRE(e.book(delta(1, 7, 8, { bidUpd(101, 2) }), 20 * MS) == LadderOutcome::BROKE);
    REQUIRE(e.l.state() == LadderState::BROKEN);
    e.step(20 * MS);
    offers.clear();
    e.step(100 * MS); // the trailing edge is due, the ladder is BROKEN
    CHECK(offers.empty());
    CHECK(e.em.trailingDue(1));
    CHECK(e.counters().suppressedFor(TickSuppressed::BROKEN) >= 1);
    CHECK(e.pendingAgeNs(100 * MS) == 0);
    e.step(150 * MS);
    CHECK(offers.empty());
    REQUIRE(e.book(snapshot(1, 3, { { 103, 1 } }, { { 110, 7 } }), 180 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(180 * MS);
    REQUIRE(offers.size() == 1);
    CHECK(offers.back().bid == 103);
    CHECK(offers.back().version == 3);
    CHECK(offers.back().edge == TickEdge::TRAILING);
    CHECK_FALSE(e.em.trailingDue(1));
}

TEST_CASE("TickEmitterTest.snapshotClearsThePendingTrailingEdge")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    REQUIRE(e.book(topMove(1, 2, 101), 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(10 * MS);
    REQUIRE(e.em.trailingDue(1));
    REQUIRE(e.pendingAgeNs(15 * MS) == 5 * MS);
    // a snapshot inside the window brings the top back to the offered state: nothing is pending any more
    REQUIRE(e.book(snapshot(1, 3, { { 100, 5 } }, { { 110, 7 } }), 30 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(30 * MS);
    CHECK_FALSE(e.em.trailingDue(1));
    CHECK(e.pendingAgeNs(30 * MS) == 0);
    CHECK_FALSE(e.due(100 * MS));
    e.step(100 * MS);
    CHECK(offers.size() == 1);
    // a snapshot inside the window with another top keeps the flag; the trailing offer carries the snapshot state
    REQUIRE(e.book(topMove(3, 4, 104), 110 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(110 * MS);
    REQUIRE(offers.size() == 2); // the window had ended at 100 ms: a leading edge
    REQUIRE(e.book(topMove(4, 5, 105), 120 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(120 * MS);
    REQUIRE(e.em.trailingDue(1));
    REQUIRE(e.book(snapshot(1, 6, { { 106, 1 } }, { { 110, 7 } }), 150 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(150 * MS);
    CHECK(e.em.trailingDue(1));
    CHECK(offers.size() == 2);
    e.step(210 * MS);
    REQUIRE(offers.size() == 3);
    CHECK(offers.back().bid == 106);
    CHECK(offers.back().version == 6);
    CHECK(offers.back().edge == TickEdge::TRAILING);
}

TEST_CASE("TickEmitterTest.crossedCandidateIsRefusedAndBreaks")
{
    Rig e;
    auto &offers = e.sink.offers;
    const BookLevel bids[] = { { 120, 1, 1 } };
    const BookLevel asks[] = { { 110, 1, 1 } };
    e.l.adoptUnchecked(bids, 1, asks, 1, 5);
    REQUIRE(e.l.state() == LadderState::OK);
    e.step(0);
    CHECK(offers.empty());
    CHECK(e.counters().refused == 1);
    CHECK(e.l.state() == LadderState::BROKEN);
    CHECK(e.l.breakCause() == BreakCause::CROSSED);
    CHECK(e.l.counters().breaks(BreakCause::CROSSED) == 1);
    CHECK_FALSE(e.em.haveOffered(1));
    CHECK(e.pendingAgeNs(0) == 0);
    // a negative price is refused too (not crossed: the ladder breaks as malformed)
    Rig n;
    const BookLevel neg[] = { { -5, 1, 1 } };
    n.l.adoptUnchecked(neg, 1, nullptr, 0, 5);
    n.step(0);
    CHECK(n.sink.offers.empty());
    CHECK(n.counters().refused == 1);
    CHECK(n.l.state() == LadderState::BROKEN);
    CHECK(n.l.breakCause() == BreakCause::MALFORMED);
    // the next snapshot repairs and the candidate goes out
    REQUIRE(e.book(snapshot(1, 6, { { 100, 1 } }, { { 110, 1 } }), 10 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(10 * MS);
    CHECK(offers.size() == 1);
}

TEST_CASE("TickEmitterTest.versionNotAboveLastOfferedIsSuppressed")
{
    Rig e;
    primed(e, 100);
    auto &offers = e.sink.offers;
    // the new ME leader's service trails (case V): a lower version with another top is applied but never offered
    REQUIRE(e.book(snapshot(1, 90, { { 95, 1 } }, { { 110, 7 } }), 500 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(500 * MS);
    CHECK(offers.size() == 1);
    CHECK(e.counters().suppressedFor(TickSuppressed::VERSION_NOT_ABOVE) == 1);
    CHECK(e.pendingAgeNs(500 * MS) == 0);
    CHECK_FALSE(e.em.trailingDue(1));
    // equal version: suppressed as well
    REQUIRE(e.book(snapshot(1, 100, { { 96, 1 } }, { { 110, 7 } }), 600 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(600 * MS);
    CHECK(offers.size() == 1);
    CHECK(e.counters().suppressedFor(TickSuppressed::VERSION_NOT_ABOVE) == 2);
    // once the version passes the offered one, the change goes out
    REQUIRE(e.book(topMove(100, 101, 97), 700 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(700 * MS);
    REQUIRE(offers.size() == 2);
    CHECK(offers.back().version == 101);
    CHECK(offers.back().bid == 97);
}

TEST_CASE("TickEmitterTest.failedOfferKeepsTheCandidateAndRetriesNextIteration")
{
    Rig e;
    auto &offers = e.sink.offers;
    REQUIRE(e.book(snapshot(1, 1, { { 100, 5 } }, { { 110, 7 } }), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    e.sink.accept = false;
    e.step(0);
    CHECK(offers.empty());
    CHECK(e.counters().offerFailures == 1);
    CHECK(e.counters().offeredTotal() == 0);
    CHECK(e.lastOffered().version == 0);
    CHECK_FALSE(e.em.haveOffered(1));
    CHECK(e.em.windowEndNs(1) == 0);
    CHECK(e.em.retryDue(1));
    CHECK(e.due(1 * MS));
    CHECK(e.pendingAgeNs(1 * MS) == 1 * MS);
    // the next iteration retries the NEWEST state (a change arrived meanwhile), not after T_cap
    REQUIRE(e.book(topMove(1, 2, 101), 1 * MS) == LadderOutcome::DELTA_APPLIED);
    e.sink.accept = true;
    e.step(1 * MS);
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].bid == 101);
    CHECK(offers[0].version == 2);
    CHECK(offers[0].edge == TickEdge::RETRY);
    CHECK(e.lastOffered().version == 2);
    CHECK(e.em.windowEndNs(1) == 1 * MS + T);
    CHECK_FALSE(e.em.retryDue(1));
    CHECK(e.pendingAgeNs(1 * MS) == 0);
    CHECK(e.counters().offeredOn(TickEdge::RETRY) == 1);
}

TEST_CASE("TickEmitterTest.reofferAfterRegistrationEvenWhenEqual")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    e.em.onRegistered();
    CHECK(e.em.reofferDue(1));
    CHECK(e.due(50 * MS));
    e.step(50 * MS); // inside the window: a re-offer is exempt from the rate (E10)
    REQUIRE(offers.size() == 2);
    CHECK(offers.back().bid == 100);
    CHECK(offers.back().ask == 110);
    CHECK(offers.back().version == 1); // equal version: E5 waived
    CHECK(offers.back().edge == TickEdge::REOFFER);
    CHECK_FALSE(e.em.reofferDue(1));
    CHECK(e.counters().offeredOn(TickEdge::REOFFER) == 1);
    // a never-offered market has nothing to re-offer: the registration flags nothing there
    CHECK_FALSE(e.em.reofferDue(2));
    // (6.0, E10): the re-offer does not restart the window. The window of the t = 0 offer ends at 100 ms, so
    // a real top change 50 ms after the re-offer is a LEADING offer at once, not a trailing edge 100 ms later.
    REQUIRE(e.book(topMove(1, 2, 105), 100 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(100 * MS);
    CHECK(offers.size() == 3);
    if (offers.size() == 3)
    {
        CHECK(offers.back().edge == TickEdge::LEADING);
        CHECK(offers.back().bid == 105);
        CHECK(offers.back().version == 2);
    }
    CHECK(e.counters().offeredOn(TickEdge::TRAILING) == 0);
}

TEST_CASE("TickEmitterTest.reofferTwiceInOneRun")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    // cycle 1: a core leader change, re-registration
    e.em.onRegistered();
    e.step(1 * S);
    CHECK(offers.size() == 2);
    CHECK(offers.back().edge == TickEdge::REOFFER);
    // cycle 2: the ladder is BROKEN at the re-registration; the flag waits for the repair (6.0)
    REQUIRE(e.book(delta(1, 7, 8, { bidUpd(100, 2) }), 2 * S) == LadderOutcome::BROKE);
    e.em.onRegistered();
    e.step(2 * S);
    CHECK(offers.size() == 2);
    CHECK(e.em.reofferDue(1));
    CHECK(e.counters().suppressedFor(TickSuppressed::BROKEN) == 1);
    REQUIRE(e.book(snapshot(1, 2, { { 100, 5 } }, { { 110, 7 } }), 3 * S) == LadderOutcome::SNAPSHOT_APPLIED); // same top
    e.step(3 * S);
    CHECK(offers.size() == 3);
    CHECK(offers.back().edge == TickEdge::REOFFER);
    CHECK(offers.back().version == 2);
    CHECK_FALSE(e.em.reofferDue(1));
    CHECK(e.counters().offeredOn(TickEdge::REOFFER) == 2);
}

TEST_CASE("TickEmitterTest.atMostOneOfferPerTcapPerMarket")
{
    Rig e;
    auto &offers = e.sink.offers;
    REQUIRE(e.book(snapshot(1, 1, { { 100, 5 } }, { { 1'000'000, 7 } }), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(0);
    REQUIRE(offers.size() == 1);
    // 10 s of 1 ms top changes (the best bid climbs by one tick per ms)
    for (std::int64_t ms = 1; ms <= 10'000; ms++)
    {
        REQUIRE(e.book(delta(1, ms, ms + 1, { bidNew(100 + ms, 1), bidDel(100 + ms - 1) }), ms * MS) == LadderOutcome::DELTA_APPLIED);
        e.step(ms * MS);
    }
    CHECK(offers.size() <= 101);
    CHECK(offers.size() >= 99);
    for (std::size_t i = 1; i < offers.size(); i++)
    {
        CHECK(offers[i].version > offers[i - 1].version);
    }
    CHECK(e.counters().offeredOn(TickEdge::LEADING) == 1);
    CHECK(e.counters().offeredOn(TickEdge::TRAILING) + 1 == offers.size());
}

TEST_CASE("TickEmitterTest.slotSurvivesTwoMeSessionEnds")
{
    Rig e;
    primed(e, 100);
    auto &offers = e.sink.offers;
    for (int cycle = 1; cycle <= 2; cycle++)
    {
        CAPTURE(cycle);
        const std::int64_t t0 = cycle * 10 * S;
        const std::int64_t v = e.l.version();
        REQUIRE(e.book(topMove(v, v + 1, 101), t0 - 5 * S) == LadderOutcome::DELTA_APPLIED);
        e.step(t0 - 5 * S); // leading
        REQUIRE(e.book(topMove(v + 1, v + 2, 102), t0 - 5 * S + 10 * MS) == LadderOutcome::DELTA_APPLIED);
        e.step(t0 - 5 * S + 10 * MS);
        REQUIRE(e.em.trailingDue(1)); // a change inside the window, then the session ends
        e.l.reset(t0); // 4.1 rule 6 (the stitcher's onMeSessionEnd)
        e.step(t0);
        CHECK_FALSE(e.em.trailingDue(1)); // EMPTY clears the trailing edge (6.0)
        CHECK(e.pendingAgeNs(t0) == 0);
        const std::int64_t kept = e.lastOffered().version;
        CHECK(kept >= 100);
        CHECK(e.em.haveOffered(1));
        // the new session's open snapshot from a trailing service: below the offered version, suppressed
        const std::size_t before = offers.size();
        REQUIRE(e.book(snapshot(1, kept - 5, { { 95, 1 } }, { { 110, 1 } }), t0 + 1 * S) == LadderOutcome::SNAPSHOT_APPLIED);
        e.step(t0 + 1 * S);
        CHECK(offers.size() == before);
        CHECK(e.counters().suppressedFor(TickSuppressed::VERSION_NOT_ABOVE) == static_cast<std::uint64_t>(cycle));
        // above it: offered
        REQUIRE(e.book(snapshot(1, kept + 5, { { 96, 1 } }, { { 110, 1 } }), t0 + 2 * S) == LadderOutcome::SNAPSHOT_APPLIED);
        e.step(t0 + 2 * S);
        REQUIRE(offers.size() == before + 1);
        CHECK(offers.back().version == kept + 5);
        CHECK(offers.back().bid == 96);
    }
}

TEST_CASE("TickEmitterTest.trailingEdgeTwiceInOneRun")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    // burst 1
    REQUIRE(e.book(topMove(1, 2, 101), 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(10 * MS);
    e.stepIfDue(100 * MS);
    CHECK(offers.size() == 2);
    CHECK(offers.back().edge == TickEdge::TRAILING);
    // burst 2, one second later: leading, then a change inside the window, a break, and the repair inside the window
    REQUIRE(e.book(topMove(2, 3, 102), 1000 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(1000 * MS);
    CHECK(offers.size() == 3);
    CHECK(offers.back().edge == TickEdge::LEADING);
    REQUIRE(e.book(topMove(3, 4, 103), 1010 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(1010 * MS);
    CHECK(e.em.trailingDue(1));
    REQUIRE(e.book(delta(1, 9, 10, { bidUpd(103, 2) }), 1020 * MS) == LadderOutcome::BROKE);
    e.step(1020 * MS);
    REQUIRE(e.book(snapshot(1, 6, { { 104, 1 } }, { { 110, 7 } }), 1050 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(1050 * MS);
    CHECK(offers.size() == 3); // still inside the window
    CHECK(e.em.trailingDue(1));
    e.stepIfDue(1100 * MS);
    CHECK(offers.size() == 4);
    CHECK(offers.back().edge == TickEdge::TRAILING);
    CHECK(offers.back().bid == 104);
    CHECK(offers.back().version == 6);
    CHECK(e.counters().offeredOn(TickEdge::TRAILING) == 2);
    CHECK_FALSE(e.em.trailingDue(1));
}

TEST_CASE("TickEmitterTest.suppressedEpisodeCountsOncePerEpisodeTwice")
{
    Rig e;
    auto &offers = e.sink.offers;
    REQUIRE(e.book(snapshot(1, 1, { { 100, 5 } }, { { 110, 7 } }), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    // episode 1: unbound for three iterations
    e.gate = TickGate::UNBOUND;
    e.step(0);
    e.step(1 * MS);
    e.step(2 * MS);
    CHECK(offers.empty());
    CHECK(e.counters().suppressedFor(TickSuppressed::UNBOUND) == 1);
    CHECK(e.pendingAgeNs(2 * MS) == 0);
    // the gate holds: the change goes out and the episode ends
    e.gate = TickGate::OPEN;
    e.step(3 * MS);
    REQUIRE(offers.size() == 1);
    // episode 2
    REQUIRE(e.book(topMove(1, 2, 101), 500 * MS) == LadderOutcome::DELTA_APPLIED);
    e.gate = TickGate::UNBOUND;
    e.step(500 * MS);
    e.step(501 * MS);
    const std::uint64_t suppressedUnbound = e.counters().suppressedFor(TickSuppressed::UNBOUND);
    CHECK(suppressedUnbound == 2);
    CHECK(offers.size() == 1);
    // the other reason has its own episode flag
    e.gate = TickGate::UNREGISTERED;
    e.step(502 * MS);
    e.step(503 * MS);
    CHECK(e.counters().suppressedFor(TickSuppressed::UNREGISTERED) == 1);
    e.gate = TickGate::OPEN;
    e.step(504 * MS);
    CHECK(offers.size() == 2);
    e.gate = TickGate::UNREGISTERED;
    e.step(505 * MS);
    CHECK(e.counters().suppressedFor(TickSuppressed::UNREGISTERED) == 2);
    CHECK(e.counters().suppressedFor(TickSuppressed::UNBOUND) == 2);
}

TEST_CASE("TickEmitterTest.pendingAgeIsZeroWhileBroken")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    REQUIRE(e.book(topMove(1, 2, 101), 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(10 * MS);
    REQUIRE(e.pendingAgeNs(20 * MS) == 10 * MS);
    REQUIRE(e.book(delta(1, 7, 8, { bidUpd(101, 2) }), 30 * MS) == LadderOutcome::BROKE);
    e.step(30 * MS);
    for (std::int64_t now = 1 * S; now <= 9 * S; now += 1 * S)
    {
        e.step(now);
        CHECK(e.pendingAgeNs(now) == 0);
    }
    CHECK(offers.size() == 1);
    CHECK(e.em.trailingDue(1));
    REQUIRE(e.book(snapshot(1, 3, { { 102, 1 } }, { { 110, 7 } }), 9500 * MS) == LadderOutcome::SNAPSHOT_APPLIED);
    e.step(9500 * MS);
    CHECK(offers.size() == 2);
    CHECK(e.pendingAgeNs(9500 * MS) == 0);
    // EMPTY (rule U / session end) reads 0 as well
    REQUIRE(e.book(topMove(3, 4, 103), 9510 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(9510 * MS);
    REQUIRE(e.pendingAgeNs(9520 * MS) == 10 * MS);
    e.l.reset(9520 * MS);
    e.step(9520 * MS);
    CHECK(e.pendingAgeNs(9600 * MS) == 0);
}

TEST_CASE("TickEmitterTest.pendingAgeIgnoresGateLoss")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    e.gate = TickGate::UNREGISTERED; // a core election
    REQUIRE(e.book(topMove(1, 2, 101), 1 * S) == LadderOutcome::DELTA_APPLIED);
    for (std::int64_t now = 1 * S; now <= 4 * S; now += 1 * S)
    {
        e.step(now);
        CHECK(e.pendingAgeNs(now) == 0);
    }
    CHECK(offers.size() == 1);
    // the gate holds again at 4 s; the core publication is back-pressured for 5 ms: the clock starts at 4 s
    e.gate = TickGate::OPEN;
    e.sink.accept = false;
    e.step(4 * S);
    CHECK(e.pendingAgeNs(4 * S + 5 * MS) == 5 * MS);
    e.sink.accept = true;
    e.step(4 * S + 5 * MS);
    REQUIRE(offers.size() == 2);
    CHECK(offers.back().bid == 101);
    CHECK(e.pendingAgeNs(4 * S + 5 * MS) == 0);
    // losing the gate with a pending change clears a running clock
    REQUIRE(e.book(topMove(2, 3, 102), 4 * S + 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(4 * S + 10 * MS);
    REQUIRE(e.pendingAgeNs(4 * S + 20 * MS) == 10 * MS);
    e.gate = TickGate::UNBOUND;
    e.step(4 * S + 20 * MS);
    CHECK(e.pendingAgeNs(4 * S + 30 * MS) == 0);
}

TEST_CASE("TickEmitterTest.pendingAgeClearsWhenTheCandidateRevertsToLastOffered")
{
    Rig e;
    primed(e);
    auto &offers = e.sink.offers;
    REQUIRE(e.book(topMove(1, 2, 101), 10 * MS) == LadderOutcome::DELTA_APPLIED);
    e.step(10 * MS);
    REQUIRE(e.pendingAgeNs(20 * MS) == 10 * MS);
    REQUIRE(e.book(delta(1, 2, 3, { bidDel(101) }), 30 * MS) == LadderOutcome::DELTA_APPLIED); // back to 100 / 110
    e.step(30 * MS);
    CHECK(e.pendingAgeNs(30 * MS) == 0);
    CHECK_FALSE(e.em.trailingDue(1));
    e.step(100 * MS);
    CHECK(offers.size() == 1);
    CHECK(e.lastOffered().version == 1);
}
