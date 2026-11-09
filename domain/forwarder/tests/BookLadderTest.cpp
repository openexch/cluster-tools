// BookLadderTest (validation in every state, rule U, the market range,
// Trade mark, 4.1 rule 6 session end). Decoded frames (BookFixtures.h); the mirror test drives the ladder
// with a small port of the ME publisher's diff (MP:902-952) until MePublisherModel takes over.
#include "doctest/doctest.h"

#include <functional>
#include <map>
#include <random>
#include <vector>

#include "BookFixtures.h"
#include "cluster_tools/domain/forwarder/BookLadder.h"

using namespace cluster_tools::domain::forwarder;
using namespace cluster_tools::domain::forwarder::book_fixtures;

namespace {

constexpr std::int64_t MS = 1'000'000LL;
constexpr std::int64_t S = 1000 * MS;

// A 20-level book on both sides: bids 2000, 1990, ... 1810; asks 2010, 2020, ... 2200.
BookFrame fullSnapshot(std::int64_t version, int market = 1)
{
    BookFrame f = snapshot(market, version, {}, {});
    for (int i = 0; i < LADDER_LEVELS; i++)
    {
        f.bids[f.nBids++] = BookLevel{ 2000 - 10 * i, 10 + i, 1 };
        f.asks[f.nAsks++] = BookLevel{ 2010 + 10 * i, 10 + i, 1 };
    }
    f.statedCount[0] = f.nBids;
    f.statedCount[1] = f.nAsks;
    return f;
}

BookLadder okLadder(std::int64_t version = 100)
{
    BookLadder l;
    REQUIRE(l.apply(snapshot(1, version, { { 100, 5 }, { 90, 6 } }, { { 110, 7 }, { 120, 8 } }), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    REQUIRE(l.state() == LadderState::OK);
    return l;
}

BookLadder brokenLadder(std::int64_t version = 100)
{
    BookLadder l = okLadder(version);
    REQUIRE(l.apply(delta(1, version + 5, version + 6, { bidUpd(100, 1) }), 0) == LadderOutcome::BROKE);
    REQUIRE(l.state() == LadderState::BROKEN);
    REQUIRE(l.breakCause() == BreakCause::FROM_MISMATCH);
    return l;
}

// A port of the ME publisher's book chain (MP:698-952) for the mirror test: a random book over 40 price levels per
// side, a seqlock-free "collect top 20" per frame, NEW/UPDATE then DELETE per side, bids before asks, the chain
// advancing only when a frame was encoded.
struct PublisherModel
{
    std::map<std::int64_t, BookLevel, std::greater<std::int64_t>> bids;
    std::map<std::int64_t, BookLevel> asks;
    std::int64_t version = 0;
    std::vector<BookLevel> lastBids, lastAsks;
    std::int64_t lastPublished = 0;
    bool sentInitial = false;
    std::mt19937_64 rng{ 7 };

    template <class Side>
    std::vector<BookLevel> top(const Side &side) const
    {
        std::vector<BookLevel> out;
        for (const auto &kv : side)
        {
            if (static_cast<int>(out.size()) == LADDER_LEVELS)
            {
                break;
            }
            out.push_back(kv.second);
        }
        return out;
    }

    void mutate()
    {
        // bids live in 1000..1390, asks in 1400..1790 (never crossed); adds, cancels, fills
        const bool bidSide = (rng() & 1) != 0;
        const std::int64_t price = (bidSide ? 1000 : 1400) + 10 * static_cast<std::int64_t>(rng() % 40);
        const unsigned op = rng() % 3;
        auto apply = [&](auto &side) {
            auto it = side.find(price);
            if (0 == op || it == side.end())
            {
                BookLevel &l = side[price];
                l.price = price;
                l.quantity += 1 + static_cast<std::int64_t>(rng() % 5);
                l.orderCount += 1;
            }
            else if (1 == op)
            {
                side.erase(it);
            }
            else
            {
                it->second.quantity = 1 + static_cast<std::int64_t>(rng() % 9);
            }
        };
        if (bidSide)
        {
            apply(bids);
        }
        else
        {
            apply(asks);
        }
        version++;
    }

    void diffSide(const std::vector<BookLevel> &was, const std::vector<BookLevel> &now, std::uint8_t side, BookFrame &f) const
    {
        auto find = [](const std::vector<BookLevel> &v, std::int64_t p) -> const BookLevel * {
            for (const BookLevel &l : v)
            {
                if (l.price == p)
                {
                    return &l;
                }
            }
            return nullptr;
        };
        for (const BookLevel &l : now)
        {
            const BookLevel *old = find(was, l.price);
            if (nullptr == old)
            {
                f.changes[f.nChanges++] = BookChange{ l.price, l.quantity, l.orderCount, side, BOOK_UPDATE_NEW };
            }
            else if (old->quantity != l.quantity || old->orderCount != l.orderCount)
            {
                f.changes[f.nChanges++] = BookChange{ l.price, l.quantity, l.orderCount, side, BOOK_UPDATE_UPDATE };
            }
        }
        for (const BookLevel &l : was)
        {
            if (nullptr == find(now, l.price))
            {
                f.changes[f.nChanges++] = BookChange{ l.price, 0, 0, side, BOOK_UPDATE_DELETE };
            }
        }
    }

    // false = no frame (nothing visible changed, or an empty book before the first frame: MP:705-708)
    bool frame(BookFrame &f)
    {
        const std::vector<BookLevel> b = top(bids), a = top(asks);
        if (!sentInitial)
        {
            if (b.empty() && a.empty())
            {
                return false;
            }
            f = snapshot(1, version, {}, {});
            for (const BookLevel &l : b) f.bids[f.nBids++] = l;
            for (const BookLevel &l : a) f.asks[f.nAsks++] = l;
            f.statedCount[0] = f.nBids;
            f.statedCount[1] = f.nAsks;
            sentInitial = true;
        }
        else
        {
            if (version == lastPublished)
            {
                return false;
            }
            f = delta(1, lastPublished, version, {});
            diffSide(lastBids, b, BOOK_SIDE_BID, f);
            diffSide(lastAsks, a, BOOK_SIDE_ASK, f);
            f.statedCount[0] = f.nChanges;
            if (0 == f.nChanges)
            {
                return false; // match#115: versions moved below the top, no frame, the next delta spans it
            }
        }
        lastBids = b;
        lastAsks = a;
        lastPublished = version;
        return true;
    }

    void resnapshot() { sentInitial = false; }
};

void expectMirror(const BookLadder &l, const PublisherModel &m)
{
    const auto b = m.top(m.bids), a = m.top(m.asks);
    REQUIRE(l.state() == LadderState::OK);
    REQUIRE(l.version() == m.lastPublished);
    REQUIRE(l.bidCount() == static_cast<int>(b.size()));
    REQUIRE(l.askCount() == static_cast<int>(a.size()));
    for (int i = 0; i < l.bidCount(); i++)
    {
        CHECK(l.bid(i).price == b[i].price);
        CHECK(l.bid(i).quantity == b[i].quantity);
        CHECK(l.bid(i).orderCount == b[i].orderCount);
    }
    for (int i = 0; i < l.askCount(); i++)
    {
        CHECK(l.ask(i).price == a[i].price);
        CHECK(l.ask(i).quantity == a[i].quantity);
        CHECK(l.ask(i).orderCount == a[i].orderCount);
    }
}

} // namespace

TEST_CASE("BookLadderTest.snapshotThenChainedDeltasMirrorTheMe")
{
    PublisherModel m;
    BookLadder l;
    for (int i = 0; i < 30; i++) m.mutate();
    BookFrame f;
    REQUIRE(m.frame(f));
    REQUIRE(f.snapshot);
    REQUIRE(l.apply(f, 0) == LadderOutcome::SNAPSHOT_APPLIED);
    expectMirror(l, m);
    int deltas = 0, snaps = 0, promoted = 0;
    for (int round = 0; round < 3000; round++)
    {
        const unsigned k = 1 + static_cast<unsigned>(m.rng() % 6); // a frame spans 1..6 commands
        for (unsigned j = 0; j < k; j++) m.mutate();
        if (0 == round % 700)
        {
            m.resnapshot(); // the periodic 10 s resnapshot
        }
        if (!m.frame(f))
        {
            continue;
        }
        if (f.snapshot)
        {
            snaps++;
        }
        else
        {
            deltas++;
            int news = 0;
            for (int c = 0; c < f.nChanges; c++) news += BOOK_UPDATE_NEW == f.changes[c].updateType;
            promoted += news;
        }
        const LadderOutcome got = l.apply(f, round * MS);
        REQUIRE(got == (f.snapshot ? LadderOutcome::SNAPSHOT_APPLIED : LadderOutcome::DELTA_APPLIED));
        expectMirror(l, m);
    }
    CHECK(deltas > 1000);
    CHECK(promoted > 100);
    CHECK(snaps >= 4);
    CHECK(l.counters().breaksTotal() == 0);
    CHECK(l.counters().snapshots == static_cast<std::uint64_t>(snaps) + 1);
    CHECK(l.counters().deltas == static_cast<std::uint64_t>(deltas));
}

TEST_CASE("BookLadderTest.deltaWithForeignFromBreaksTheChain")
{
    BookLadder l = okLadder(100);
    CHECK(l.apply(delta(1, 100, 101, { bidUpd(100, 9) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.version() == 101);
    CHECK(l.apply(delta(1, 103, 104, { bidUpd(100, 2) }), 2) == LadderOutcome::BROKE);
    CHECK(l.state() == LadderState::BROKEN);
    CHECK(l.breakCause() == BreakCause::FROM_MISMATCH);
    CHECK(l.counters().breaks(BreakCause::FROM_MISMATCH) == 1);
    CHECK(l.bidCount() == 0); // contents discarded, never emitted
    CHECK(l.askCount() == 0);
    CHECK(l.version() == 101); // the last good version stays readable
    CHECK(l.brokenAgeNs(12) == 10);
}

TEST_CASE("BookLadderTest.brokenLadderIgnoresDeltasUntilASnapshot")
{
    BookLadder l = brokenLadder(100);
    CHECK(l.apply(delta(1, 106, 107, { bidUpd(100, 2) }), 3) == LadderOutcome::IGNORED);
    CHECK(l.apply(delta(1, 100, 108, { bidUpd(100, 2) }), 4) == LadderOutcome::IGNORED); // even a matching from
    CHECK(l.counters().ignored == 2);
    CHECK(l.counters().deltas == 3);
    CHECK(l.state() == LadderState::BROKEN);
    BookLadder e; // rule 4 in EMPTY too
    CHECK(e.apply(delta(1, 0, 1, { bidNew(100, 1) }), 0) == LadderOutcome::IGNORED);
    CHECK(e.state() == LadderState::EMPTY);
    CHECK(e.counters().ignored == 1);
    CHECK(l.apply(snapshot(1, 110, { { 100, 2 } }, {}), 5) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.breakCause() == BreakCause::NONE);
    CHECK(l.counters().recoveries == 1);
    CHECK(l.apply(delta(1, 110, 111, { bidUpd(100, 3) }), 6) == LadderOutcome::DELTA_APPLIED);
}

TEST_CASE("BookLadderTest.snapshotRepairsBrokenTwiceInOneRun")
{
    BookLadder l = okLadder(100);
    // cycle 1
    REQUIRE(l.apply(delta(1, 105, 106, { bidUpd(100, 1) }), 1 * S) == LadderOutcome::BROKE);
    CHECK(l.brokenAgeNs(3 * S) == 2 * S);
    REQUIRE(l.apply(snapshot(1, 120, { { 101, 1 } }, { { 102, 1 } }), 4 * S) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.breakCause() == BreakCause::NONE);
    CHECK(l.brokenAgeNs(5 * S) == 0);
    CHECK(l.counters().brokenNsTotal == 3 * S);
    CHECK(l.counters().recoveries == 1);
    // cycle 2: the second break is a different cause, the second snapshot clears it again
    REQUIRE(l.apply(delta(1, 120, 121, { bidNew(101, 1) }), 6 * S) == LadderOutcome::BROKE);
    CHECK(l.breakCause() == BreakCause::LEVEL_OP);
    CHECK(l.brokenAgeNs(7 * S) == 1 * S);
    CHECK(l.brokenNsIncludingOpen(7 * S) == 4 * S);
    REQUIRE(l.apply(snapshot(1, 130, { { 101, 1 } }, {}), 8 * S) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.breakCause() == BreakCause::NONE);
    CHECK(l.counters().brokenNsTotal == 5 * S);
    CHECK(l.counters().recoveries == 2);
    CHECK(l.counters().breaksTotal() == 2);
    CHECK(l.snapshotAgeNs(9 * S) == 1 * S);
}

TEST_CASE("BookLadderTest.deleteOfLevelOnePromotesLevelTwo")
{
    BookLadder l = okLadder(100);
    REQUIRE(l.apply(delta(1, 100, 101, { bidDel(100) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.bidCount() == 1);
    CHECK(l.bestBid() == 90);
    CHECK(l.bestAsk() == 110);
    REQUIRE(l.apply(delta(1, 101, 102, { askDel(110) }), 2) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.bestAsk() == 120);
    CHECK(l.askCount() == 1);
}

TEST_CASE("BookLadderTest.levelPushedOutOfTop20IsADelete")
{
    BookLadder l;
    REQUIRE(l.apply(fullSnapshot(10), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.bidCount() == 20);
    // a new best bid 2005 enters, the 20th (1810) leaves: the ME lists the NEW before the DELETE
    REQUIRE(l.apply(delta(1, 10, 11, { bidNew(2005, 3), bidDel(1810) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.bidCount() == 20);
    CHECK(l.bestBid() == 2005);
    CHECK(l.bid(19).price == 1820);
    CHECK(l.state() == LadderState::OK);
}

TEST_CASE("BookLadderTest.promotedLevel21IsANew")
{
    BookLadder l;
    REQUIRE(l.apply(fullSnapshot(10), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    // the best ask is taken out; depth 21 (2210) becomes visible as a NEW; the ME lists NEW before DELETE
    REQUIRE(l.apply(delta(1, 10, 11, { askNew(2210, 4), askDel(2010) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.askCount() == 20);
    CHECK(l.bestAsk() == 2020);
    CHECK(l.ask(19).price == 2210);
    CHECK(l.ask(19).quantity == 4);
}

TEST_CASE("BookLadderTest.newBeforeDeleteHolds21LevelsMidFrame")
{
    BookLadder l;
    REQUIRE(l.apply(fullSnapshot(10), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    // 20 visible bids; the frame promotes a level from depth 21 (a NEW while 20 are present) and deletes level 7
    CHECK(l.apply(delta(1, 10, 11, { bidNew(1800, 2), bidDel(1940) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.bidCount() == 20);
    CHECK(l.bid(19).price == 1800);
    // and the same shape with three NEWs before three DELETEs (23 levels between the changes)
    REQUIRE(l.apply(delta(1, 11, 12, { bidNew(1790, 1), bidNew(1780, 1), bidNew(1770, 1), bidDel(2000), bidDel(1990), bidDel(1980) }), 2) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.bidCount() == 20);
    CHECK(l.bestBid() == 1970);
    CHECK(l.bid(19).price == 1770);
}

TEST_CASE("BookLadderTest.newOfPresentPriceBreaks")
{
    BookLadder l = okLadder(100);
    CHECK(l.apply(delta(1, 100, 101, { bidNew(100, 9) }), 1) == LadderOutcome::BROKE);
    CHECK(l.state() == LadderState::BROKEN);
    CHECK(l.breakCause() == BreakCause::LEVEL_OP);
    CHECK(l.counters().breaks(BreakCause::LEVEL_OP) == 1);
    CHECK(l.version() == 100);
}

TEST_CASE("BookLadderTest.updateOrDeleteOfAbsentPriceBreaks")
{
    BookLadder u = okLadder(100);
    CHECK(u.apply(delta(1, 100, 101, { bidUpd(95, 9) }), 1) == LadderOutcome::BROKE);
    CHECK(u.breakCause() == BreakCause::LEVEL_OP);
    BookLadder d = okLadder(100);
    CHECK(d.apply(delta(1, 100, 101, { askDel(115) }), 1) == LadderOutcome::BROKE);
    CHECK(d.breakCause() == BreakCause::LEVEL_OP);
    // an UPDATE of a price present on the OTHER side is absent on its own side
    BookLadder x = okLadder(100);
    CHECK(x.apply(delta(1, 100, 101, { askUpd(100, 1) }), 1) == LadderOutcome::BROKE);
    CHECK(x.breakCause() == BreakCause::LEVEL_OP);
}

TEST_CASE("BookLadderTest.moreThan20LevelsAtFrameEndBreaks")
{
    BookLadder l;
    REQUIRE(l.apply(fullSnapshot(10), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.apply(delta(1, 10, 11, { bidNew(1800, 2) }), 1) == LadderOutcome::BROKE);
    CHECK(l.state() == LadderState::BROKEN);
    CHECK(l.breakCause() == BreakCause::DEPTH);
    CHECK(l.counters().breaks(BreakCause::DEPTH) == 1);
    // 21 asks in a snapshot are refused upstream as COUNT (4.2); a 20-level snapshot is the maximum and stays OK
    BookLadder s;
    CHECK(s.apply(fullSnapshot(10), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(s.state() == LadderState::OK);
}

TEST_CASE("BookLadderTest.crossedResultBreaks")
{
    BookLadder l = okLadder(100);
    CHECK(l.apply(delta(1, 100, 101, { bidNew(115, 1) }), 1) == LadderOutcome::BROKE);
    CHECK(l.breakCause() == BreakCause::CROSSED);
    BookLadder eq = okLadder(100);
    CHECK(eq.apply(delta(1, 100, 101, { askNew(100, 1) }), 1) == LadderOutcome::BROKE); // bestBid == bestAsk is crossed
    CHECK(eq.breakCause() == BreakCause::CROSSED);
    // a crossed snapshot is refused the same way
    BookLadder s;
    CHECK(s.apply(snapshot(1, 5, { { 120, 1 } }, { { 110, 1 } }), 0) == LadderOutcome::BROKE);
    CHECK(s.breakCause() == BreakCause::CROSSED);
    // a one-sided book is never crossed
    BookLadder one;
    CHECK(one.apply(snapshot(1, 5, { { 120, 1 } }, {}), 0) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(one.apply(delta(1, 5, 6, { bidNew(130, 1) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(one.bestBid() == 130);
    CHECK(one.bestAsk() == 0);
}

TEST_CASE("BookLadderTest.malformedFrameRuleInEveryState")
{
    using Make = std::function<BookFrame()>;
    struct Case
    {
        const char *name;
        Make make;
    };
    const std::vector<Case> cases = {
        { "22 header block 35", [] { BookFrame f = snapshot(1, 7, { { 100, 1 } }, {}); f.headerBlockLength = 35; return f; } },
        { "25 header block 43", [] { BookFrame f = delta(1, 100, 101, { bidUpd(100, 1) }); f.headerBlockLength = 43; return f; } },
        { "22 bid count 21", [] { BookFrame f = snapshot(1, 7, { { 100, 1 } }, {}); f.statedCount[0] = 21; return f; } },
        { "25 count 81", [] { BookFrame f = delta(1, 100, 101, { bidUpd(100, 1) }); f.statedCount[0] = 81; return f; } },
        { "22 length short", [] { BookFrame f = snapshot(1, 7, { { 100, 1 } }, {}); f.lengthOk = false; return f; } },
        { "25 length short", [] { BookFrame f = delta(1, 100, 101, { bidUpd(100, 1) }); f.lengthOk = false; return f; } },
        { "22 version -1", [] { return snapshot(1, -1, { { 100, 1 } }, {}); } },
        { "22 version 0 non-empty", [] { return snapshot(1, 0, { { 100, 1 } }, {}); } },
        { "25 version 0", [] { return delta(1, 0, 0, { bidUpd(100, 1) }); } },
        { "25 from -1", [] { return delta(1, -1, 101, { bidUpd(100, 1) }); } },
        { "25 raw side 2", [] { return delta(1, 100, 101, { Ch{ 2, BOOK_UPDATE_UPDATE, 100, 1, 1 } }); } },
        { "25 raw updateType 3", [] { return delta(1, 100, 101, { Ch{ BOOK_SIDE_BID, 3, 100, 1, 1 } }); } },
        { "22 price 0", [] { return snapshot(1, 7, { { 0, 1 } }, {}); } },
        { "22 qty 0", [] { return snapshot(1, 7, { { 100, 0 } }, {}); } },
        { "22 count 0", [] { return snapshot(1, 7, { { 100, 1, 0 } }, {}); } },
        { "25 NEW price 0", [] { return delta(1, 100, 101, { bidNew(0, 1) }); } },
        { "25 UPDATE qty 0", [] { return delta(1, 100, 101, { bidUpd(100, 0) }); } },
        { "25 UPDATE count 0", [] { return delta(1, 100, 101, { bidUpd(100, 1, 0) }); } },
        { "25 DELETE price 0", [] { return delta(1, 100, 101, { bidDel(0) }); } },
        { "22 unordered bids", [] { return snapshot(1, 7, { { 90, 1 }, { 100, 1 } }, {}); } },
        { "22 duplicate ask", [] { return snapshot(1, 7, {}, { { 110, 1 }, { 110, 2 } }); } },
    };
    for (const Case &c : cases)
    {
        INFO("case: ", c.name);
        // EMPTY
        {
            BookLadder l;
            CHECK(l.apply(c.make(), 1) == LadderOutcome::BROKE);
            CHECK(l.state() == LadderState::BROKEN);
            CHECK(l.breakCause() == BreakCause::MALFORMED);
            CHECK(l.counters().breaks(BreakCause::MALFORMED) == 1);
        }
        // OK (the delta cases chain from version 100)
        {
            BookLadder l = okLadder(100);
            CHECK(l.apply(c.make(), 1) == LadderOutcome::BROKE);
            CHECK(l.state() == LadderState::BROKEN);
            CHECK(l.breakCause() == BreakCause::MALFORMED);
            CHECK(l.bidCount() == 0);
        }
        // BROKEN (by from_mismatch): a malformed frame is still malformed, not "ignored"
        {
            BookLadder l = brokenLadder(100);
            CHECK(l.apply(c.make(), 1) == LadderOutcome::BROKE);
            CHECK(l.state() == LadderState::BROKEN);
            CHECK(l.breakCause() == BreakCause::MALFORMED);
            CHECK(l.counters().breaks(BreakCause::MALFORMED) == 1);
            CHECK(l.counters().ignored == 0);
        }
    }
    // without a readable market the frame never reaches a ladder (4.2: forwarder_book_malformed_total)
    BookStitcher st;
    BookFrame nm = snapshot(1, 7, { { 100, 1 } }, {});
    nm.marketReadable = false;
    nm.headerBlockLength = 3;
    CHECK(st.onBookFrame(nm, 1) == 0);
    CHECK(st.counters().malformedNoMarket == 1);
    for (int m = 1; m <= TICK_MARKETS; m++)
    {
        CHECK(st.ladder(m).state() == LadderState::EMPTY);
    }
}

TEST_CASE("BookLadderTest.emptySnapshotIsAnOkEmptyLadder")
{
    BookLadder l;
    CHECK(l.apply(snapshot(1, 500, {}, {}), 1) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.version() == 500);
    CHECK(l.bidCount() == 0);
    CHECK(l.askCount() == 0);
    CHECK(l.bestBid() == 0);
    CHECK(l.bestAsk() == 0);
    CHECK(l.counters().unversioned == 0);
    CHECK(l.apply(delta(1, 500, 501, { bidNew(100, 1) }), 2) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.bestBid() == 100);
}

TEST_CASE("BookLadderTest.allDeleteDeltaEmptiesTheLadder")
{
    BookLadder l = okLadder(100);
    CHECK(l.apply(delta(1, 100, 101, { bidDel(100), bidDel(90), askDel(110), askDel(120) }), 1) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.version() == 101);
    CHECK(l.bidCount() == 0);
    CHECK(l.askCount() == 0);
    CHECK(l.bestBid() == 0);
    CHECK(l.bestAsk() == 0);
}

TEST_CASE("BookLadderTest.unversionedEmptySnapshotIsEmptyNotBrokenInEveryState")
{
    const BookFrame u = snapshot(1, 0, {}, {});
    // from EMPTY
    {
        BookLadder l;
        CHECK(l.apply(u, 1) == LadderOutcome::UNVERSIONED);
        CHECK(l.state() == LadderState::EMPTY);
        CHECK(l.counters().unversioned == 1);
        CHECK(l.counters().breaks(BreakCause::MALFORMED) == 0);
        CHECK(l.snapshotAgeNs(5) == -1);
    }
    // from OK
    {
        BookLadder l = okLadder(100);
        CHECK(l.apply(u, 1) == LadderOutcome::UNVERSIONED);
        CHECK(l.state() == LadderState::EMPTY);
        CHECK(l.version() == 0);
        CHECK(l.bidCount() == 0);
        CHECK(l.counters().unversioned == 1);
        CHECK(l.counters().breaks(BreakCause::MALFORMED) == 0);
    }
    // from BROKEN: the episode closes, no recovery is counted (EMPTY is not OK)
    {
        BookLadder l = brokenLadder(100);
        CHECK(l.apply(u, 3 * S) == LadderOutcome::UNVERSIONED);
        CHECK(l.state() == LadderState::EMPTY);
        CHECK(l.breakCause() == BreakCause::NONE);
        CHECK(l.brokenAgeNs(4 * S) == 0);
        CHECK(l.counters().brokenNsTotal == 3 * S);
        CHECK(l.counters().recoveries == 0);
        CHECK(l.counters().unversioned == 1);
        CHECK(l.counters().breaks(BreakCause::MALFORMED) == 0);
    }
}

TEST_CASE("BookLadderTest.unversionedNonEmptySnapshotIsMalformed")
{
    BookLadder l;
    CHECK(l.apply(snapshot(1, 0, {}, { { 110, 1 } }), 1) == LadderOutcome::BROKE);
    CHECK(l.breakCause() == BreakCause::MALFORMED);
    CHECK(l.counters().unversioned == 0);
    // and a version-0 "empty" snapshot that STATES a count is not empty (the entries were not copied: COUNT / LENGTH)
    BookLadder s;
    BookFrame f = snapshot(1, 0, {}, {});
    f.statedCount[0] = 21;
    CHECK(s.apply(f, 1) == LadderOutcome::BROKE);
    CHECK(s.breakCause() == BreakCause::MALFORMED);
}

TEST_CASE("BookLadderTest.deltaNotAboveFromIsMalformed")
{
    BookLadder a = okLadder(100);
    CHECK(a.apply(delta(1, 100, 100, { bidUpd(100, 1) }), 1) == LadderOutcome::BROKE);
    CHECK(a.breakCause() == BreakCause::MALFORMED);
    BookLadder b = okLadder(100);
    CHECK(b.apply(delta(1, 100, 99, { bidUpd(100, 1) }), 1) == LadderOutcome::BROKE);
    CHECK(b.breakCause() == BreakCause::MALFORMED);
    CHECK(b.version() == 100); // rule 2 never lowers the version
}

TEST_CASE("BookLadderTest.groupBlockLengthBelowEntryIsMalformed")
{
    BookLadder l = okLadder(100);
    BookFrame f22 = snapshot(1, 101, { { 100, 1 } }, {});
    f22.groupBlockLength[1] = 19; // the ask group, even when empty
    CHECK(l.apply(f22, 1) == LadderOutcome::BROKE);
    CHECK(l.breakCause() == BreakCause::MALFORMED);
    BookLadder d = okLadder(100);
    BookFrame f25 = delta(1, 100, 101, { bidUpd(100, 1) });
    f25.groupBlockLength[0] = 21;
    CHECK(d.apply(f25, 1) == LadderOutcome::BROKE);
    CHECK(d.breakCause() == BreakCause::MALFORMED);
    // a LARGER stated group block is the SBE extension rule: the codec skipped it, the ladder accepts the frame
    BookLadder x = okLadder(100);
    BookFrame big = delta(1, 100, 101, { bidUpd(100, 1) });
    big.groupBlockLength[0] = 30;
    CHECK(x.apply(big, 1) == LadderOutcome::DELTA_APPLIED);
}

TEST_CASE("BookLadderTest.foreignMarketIsCountedNotMalformed")
{
    BookStitcher st;
    REQUIRE(st.onBookFrame(snapshot(1, 10, { { 100, 1 } }, {}), 0) == 1);
    CHECK(st.onBookFrame(snapshot(6, 11, { { 100, 1 } }, {}), 1) == 0);
    CHECK(st.onBookFrame(delta(0, 10, 11, { bidUpd(100, 2) }), 2) == 0);
    CHECK(st.onBookFrame(snapshot(-3, 11, {}, {}), 3) == 0);
    CHECK(st.counters().foreignMarket == 3);
    CHECK(st.counters().malformedNoMarket == 0);
    for (int m = 1; m <= TICK_MARKETS; m++)
    {
        CHECK(st.ladder(m).counters().breaksTotal() == 0);
        CHECK(st.ladder(m).counters().snapshots == (1 == m ? 1u : 0u));
    }
    CHECK(st.ladder(1).state() == LadderState::OK);
    CHECK(st.ladder(1).version() == 10);
    // a foreign-market 26 is counted the same way and moves no mark
    CHECK(st.onTradeFrame(trades(7, 55, 100)) == 0);
    CHECK(st.counters().foreignMarket == 4);
    CHECK(st.mark(1).tradeId() == 0);
    // a malformed frame of a foreign market is a foreign market (the range is checked first; no ladder to break)
    BookFrame bad = snapshot(9, -1, { { 100, 1 } }, {});
    CHECK(st.onBookFrame(bad, 4) == 0);
    CHECK(st.counters().foreignMarket == 5);
    CHECK(st.counters().malformedNoMarket == 0);
}

TEST_CASE("BookLadderTest.tradeBatchMalformedRules")
{
    BookStitcher st;
    TradeMark &m = st.mark(1);
    TradeFrame a = trades(1, 5, 100);
    a.headerBlockLength = 11;
    CHECK(st.onTradeFrame(a) == 0);
    TradeFrame b = trades(1, 5, 100);
    b.entryBlockLength = 88;
    CHECK(st.onTradeFrame(b) == 0);
    TradeFrame c = trades(1, 5, 100, 3);
    c.lengthOk = false; // a count whose entries do not fit the frame
    CHECK(st.onTradeFrame(c) == 0);
    TradeFrame d = trades(1, 5, 100, 2);
    d.entryInvalid = true; // an entry with tradeId 0 (the max is still 5)
    CHECK(st.onTradeFrame(d) == 0);
    TradeFrame e = trades(1, 5, 0);
    e.entryInvalid = true; // price 0
    CHECK(st.onTradeFrame(e) == 0);
    CHECK(m.tradeId() == 0);
    CHECK(m.price() == 0);
    CHECK(st.counters().tradeMalformed[0] == 5);
    CHECK(st.counters().tradeBatches[0] == 0);
    CHECK(st.ladder(1).state() == LadderState::EMPTY); // the ladder is unchanged by a trade frame
    // a well-formed batch moves the mark; a LARGER entry block is the extension rule
    TradeFrame ok = trades(1, 5, 100);
    ok.entryBlockLength = 120;
    CHECK(st.onTradeFrame(ok) == 1);
    CHECK(m.tradeId() == 5);
    CHECK(m.price() == 100);
    CHECK(st.counters().tradeBatches[0] == 1);
    // an empty batch is well formed and a no-op
    CHECK(st.onTradeFrame(trades(1, 0, 0, 0)) == 1);
    CHECK(m.tradeId() == 5);
}

TEST_CASE("BookLadderTest.resentTradeEntryIsANoOp")
{
    BookStitcher st;
    REQUIRE(st.onTradeFrame(trades(1, 10, 101)) == 1);
    REQUIRE(st.onTradeFrame(trades(1, 12, 103)) == 1);
    CHECK(st.mark(1).tradeId() == 12);
    CHECK(st.mark(1).price() == 103);
    // a promoted ME leader re-sends warm-standby entries (ACS:388-397): max-by-tradeId ignores them
    CHECK(st.onTradeFrame(trades(1, 10, 101)) == 1);
    CHECK(st.onTradeFrame(trades(1, 12, 103)) == 1);
    CHECK(st.mark(1).tradeId() == 12);
    CHECK(st.mark(1).price() == 103);
    CHECK(st.onTradeFrame(trades(1, 13, 99)) == 1);
    CHECK(st.mark(1).tradeId() == 13);
    CHECK(st.mark(1).price() == 99);
    // marks are per market
    CHECK(st.onTradeFrame(trades(2, 1, 7)) == 2);
    CHECK(st.mark(2).tradeId() == 1);
    CHECK(st.mark(1).tradeId() == 13);
}

TEST_CASE("BookLadderTest.versionRegressingSnapshotIsAppliedAndCounted")
{
    BookLadder l = okLadder(100);
    REQUIRE(l.apply(delta(1, 100, 150, { bidUpd(100, 9) }), 1) == LadderOutcome::DELTA_APPLIED);
    // the new ME leader's service trails the old one's (case V): applied, counted, the chain continues
    CHECK(l.apply(snapshot(1, 140, { { 100, 5 } }, { { 110, 7 } }), 2) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.state() == LadderState::OK);
    CHECK(l.version() == 140);
    CHECK(l.bid(0).quantity == 5);
    CHECK(l.counters().versionRegressions == 1);
    CHECK(l.apply(delta(1, 140, 141, { bidUpd(100, 6) }), 3) == LadderOutcome::DELTA_APPLIED);
    CHECK(l.counters().versionRegressions == 1);
    // an equal version is not a regression, a higher one neither; a snapshot onto a BROKEN ladder neither
    CHECK(l.apply(snapshot(1, 141, { { 100, 6 } }, { { 110, 7 } }), 4) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(l.counters().versionRegressions == 1);
    BookLadder b = brokenLadder(100);
    CHECK(b.apply(snapshot(1, 50, { { 100, 6 } }, {}), 5) == LadderOutcome::SNAPSHOT_APPLIED);
    CHECK(b.counters().versionRegressions == 0);
    CHECK(b.counters().recoveries == 1);
}

TEST_CASE("BookLadderTest.sessionEndResetsEveryLadderAndTradeMark")
{
    BookStitcher st;
    for (int m = 1; m <= TICK_MARKETS; m++)
    {
        REQUIRE(st.onBookFrame(snapshot(m, 10 * m, { { 100, 1 } }, { { 110, 1 } }), 0) == m);
        REQUIRE(st.onTradeFrame(trades(m, 3 * m, 100)) == m);
    }
    REQUIRE(st.onBookFrame(delta(3, 99, 100, { bidUpd(100, 2) }), 1) == 3); // market 3 BROKEN
    REQUIRE(st.ladder(3).state() == LadderState::BROKEN);
    st.onMeSessionEnd(2 * S);
    for (int m = 1; m <= TICK_MARKETS; m++)
    {
        CAPTURE(m);
        const BookLadder &l = st.ladder(m);
        CHECK(l.state() == LadderState::EMPTY);
        CHECK(l.version() == 0);
        CHECK(l.bidCount() == 0);
        CHECK(l.askCount() == 0);
        CHECK(l.breakCause() == BreakCause::NONE);
        CHECK(l.snapshotAgeNs(3 * S) == -1);
        CHECK(l.brokenAgeNs(3 * S) == 0);
        CHECK(st.mark(m).tradeId() == 0);
        CHECK(st.mark(m).price() == 0);
        CHECK(l.counters().snapshots == 1); // counters survive
    }
    CHECK(st.ladder(3).counters().brokenNsTotal == 2 * S - 1);
    // the new session's open snapshot repairs; a delta before it is ignored (rule 4)
    CHECK(st.onBookFrame(delta(1, 10, 11, { bidUpd(100, 3) }), 3 * S) == 1);
    CHECK(st.ladder(1).state() == LadderState::EMPTY);
    CHECK(st.ladder(1).counters().ignored == 1);
    CHECK(st.onBookFrame(snapshot(1, 12, { { 100, 3 } }, {}), 3 * S) == 1);
    CHECK(st.ladder(1).state() == LadderState::OK);
    // second cycle (C4)
    st.onMeSessionEnd(4 * S);
    CHECK(st.ladder(1).state() == LadderState::EMPTY);
    CHECK(st.ladder(1).version() == 0);
}
