#include "cluster_tools/domain/forwarder/BookLadder.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

// RC2 S13 contract section 4 (rules 1-7, 4.2 in every state, rule U), section 5 (the trade mark through the
// stitcher) and 4.1 rule 6 (session end). Every "RED shape" branch is a test seam that compiles to nothing in the
// production library (TestSeams.h).

namespace cluster_tools { namespace domain { namespace forwarder {

namespace seams = test_seams;

const char *nameOf(LadderState state) noexcept
{
    switch (state)
    {
        case LadderState::EMPTY: return "EMPTY";
        case LadderState::OK: return "OK";
        case LadderState::BROKEN: return "BROKEN";
    }
    return "?";
}

const char *nameOf(BreakCause cause) noexcept
{
    switch (cause)
    {
        case BreakCause::NONE: return "none";
        case BreakCause::FROM_MISMATCH: return "from_mismatch";
        case BreakCause::LEVEL_OP: return "level_op";
        case BreakCause::DEPTH: return "depth";
        case BreakCause::CROSSED: return "crossed";
        case BreakCause::MALFORMED: return "malformed";
        case BreakCause::COUNT: break;
    }
    return "?";
}

// ---- BookLadder

LadderOutcome BookLadder::apply(const BookFrame &f, std::int64_t nowNs) noexcept
{
    // 4.2 first, in every state: one rule (MarketData.cpp), the ladder only reacts to its verdict
    if (validateBookFrame(f) != Malformed::NONE)
    {
        (f.snapshot ? m_counters.snapshots : m_counters.deltas)++;
        return enterBroken(BreakCause::MALFORMED, nowNs);
    }
    return f.snapshot ? applySnapshot(f, nowNs) : applyDelta(f, nowNs);
}

LadderOutcome BookLadder::applySnapshot(const BookFrame &f, std::int64_t nowNs) noexcept
{
    m_counters.snapshots++;

    // rule U (4.2): bookVersion 0 with two empty groups = "no versioned state": EMPTY from any state, never BROKEN
    if (0 == f.bookVersion && 0 == f.nBids && 0 == f.nAsks && 0 == f.statedCount[0] && 0 == f.statedCount[1])
    {
        if (LadderState::BROKEN == m_state)
        {
            leaveBroken(nowNs);
        }
        m_bidCount = 0;
        m_askCount = 0;
        m_version = 0;
        m_state = LadderState::EMPTY;
        m_cause = BreakCause::NONE;
        m_lastSnapshotNs = -1;
        m_counters.unversioned++;
        return LadderOutcome::UNVERSIONED;
    }

    // a snapshot is checked like a delta's result: ordered sides (strict), not crossed (values by 4.2)
    if (!sideOrdered(f.bids, f.nBids, true) || !sideOrdered(f.asks, f.nAsks, false))
    {
        return enterBroken(BreakCause::MALFORMED, nowNs);
    }
    if (f.nBids > 0 && f.nAsks > 0 && f.bids[0].price >= f.asks[0].price)
    {
        return enterBroken(BreakCause::CROSSED, nowNs);
    }

    // rule 1: replace both sides
    const bool wasOk = LadderState::OK == m_state;
    const bool wasBroken = LadderState::BROKEN == m_state;
    if (wasOk && f.bookVersion < m_version)
    {
        m_counters.versionRegressions++; // section 9 case V: safe only because of the step 4 stale-term filter
    }
    for (int i = 0; i < f.nBids; i++)
    {
        m_bids[i] = f.bids[i];
    }
    for (int i = 0; i < f.nAsks; i++)
    {
        m_asks[i] = f.asks[i];
    }
    m_bidCount = f.nBids;
    m_askCount = f.nAsks;
    m_version = f.bookVersion;
    if (wasBroken)
    {
        leaveBroken(nowNs);
        m_counters.recoveries++;
    }
    m_state = LadderState::OK;
    m_cause = BreakCause::NONE;
    m_lastSnapshotNs = nowNs;
    return LadderOutcome::SNAPSHOT_APPLIED;
}

LadderOutcome BookLadder::applyDelta(const BookFrame &f, std::int64_t nowNs) noexcept
{
    m_counters.deltas++;

    // rule 4: only a snapshot leaves EMPTY or BROKEN
    if (LadderState::OK != m_state)
    {
        m_counters.ignored++;
        return LadderOutcome::IGNORED;
    }
    // rule 3: the chain
    if (f.fromVersion != m_version && !seams::disabled(seams::Rule::STITCH_CHAIN_CHECK))
    {
        return enterBroken(BreakCause::FROM_MISMATCH, nowNs);
    }
    // rule 2: frame order on 40 slots, the 20-level check at the frame end
    for (int i = 0; i < f.nChanges; i++)
    {
        if (!applyChange(f.changes[i]))
        {
            return enterBroken(m_pendingCause, nowNs);
        }
    }
    if (!seams::disabled(seams::Rule::STITCH_DEPTH_CHECK) && (m_bidCount > LADDER_LEVELS || m_askCount > LADDER_LEVELS))
    {
        return enterBroken(BreakCause::DEPTH, nowNs);
    }
    if (crossed())
    {
        return enterBroken(BreakCause::CROSSED, nowNs);
    }
    m_version = f.bookVersion;
    return LadderOutcome::DELTA_APPLIED;
}

// One change of a 25 in frame order. Sides are kept sorted best first; a NEW is inserted at its rank. false = the
// frame is defective: m_pendingCause names LEVEL_OP (NEW of a present price, UPDATE / DELETE of an absent one, an
// unknown raw side or type that the seam let through) or DEPTH (no slot left).
bool BookLadder::applyChange(const BookChange &c) noexcept
{
    const bool strict = !seams::disabled(seams::Rule::STITCH_STRICT_LEVEL_OPS);
    if (c.side > BOOK_SIDE_MAX || c.updateType > BOOK_UPDATE_MAX)
    {
        m_pendingCause = BreakCause::LEVEL_OP;
        return false;
    }
    const bool bid = BOOK_SIDE_BID == c.side;
    BookLevel *levels = bid ? m_bids : m_asks;
    int &count = bid ? m_bidCount : m_askCount;

    // rank: the first index whose price is worse than c.price (bids descending, asks ascending)
    int at = 0;
    while (at < count && (bid ? levels[at].price > c.price : levels[at].price < c.price))
    {
        at++;
    }
    const bool present = at < count && levels[at].price == c.price;

    if (BOOK_UPDATE_NEW == c.updateType)
    {
        if (present)
        {
            if (strict)
            {
                m_pendingCause = BreakCause::LEVEL_OP;
                return false;
            }
            levels[at].quantity = c.quantity;
            levels[at].orderCount = c.orderCount;
            return true;
        }
        // CL-N-11: the RED shape caps the effective capacity at 20 by this bound check, never by a smaller array
        const int capacity = seams::disabled(seams::Rule::STITCH_40_SLOTS) ? LADDER_LEVELS : LADDER_SLOTS;
        if (count >= capacity)
        {
            m_pendingCause = BreakCause::DEPTH;
            return false;
        }
        for (int i = count; i > at; i--)
        {
            levels[i] = levels[i - 1];
        }
        levels[at] = BookLevel{ c.price, c.quantity, c.orderCount };
        count++;
        return true;
    }
    if (BOOK_UPDATE_UPDATE == c.updateType)
    {
        if (!present)
        {
            if (strict)
            {
                m_pendingCause = BreakCause::LEVEL_OP;
                return false;
            }
            if (count >= LADDER_SLOTS)
            {
                m_pendingCause = BreakCause::DEPTH;
                return false;
            }
            for (int i = count; i > at; i--)
            {
                levels[i] = levels[i - 1];
            }
            levels[at] = BookLevel{ c.price, c.quantity, c.orderCount };
            count++;
            return true;
        }
        levels[at].quantity = c.quantity;
        levels[at].orderCount = c.orderCount;
        return true;
    }
    // DELETE
    if (!present)
    {
        if (strict)
        {
            m_pendingCause = BreakCause::LEVEL_OP;
            return false;
        }
        return true;
    }
    for (int i = at; i + 1 < count; i++)
    {
        levels[i] = levels[i + 1];
    }
    count--;
    return true;
}

LadderOutcome BookLadder::enterBroken(BreakCause cause, std::int64_t nowNs) noexcept
{
    m_counters.chainBreaks[static_cast<unsigned>(cause)]++;
    if (LadderState::BROKEN != m_state)
    {
        m_brokenSinceNs = nowNs;
    }
    m_state = LadderState::BROKEN;
    m_cause = cause;
    m_bidCount = 0; // contents discarded (rule E1: never emitted); m_version stays the last good version
    m_askCount = 0;
    return LadderOutcome::BROKE;
}

void BookLadder::leaveBroken(std::int64_t nowNs) noexcept
{
    const std::int64_t age = nowNs - m_brokenSinceNs;
    m_counters.brokenNsTotal += age > 0 ? age : 0;
    m_brokenSinceNs = 0;
}

void BookLadder::reset(std::int64_t nowNs) noexcept
{
    if (seams::disabled(seams::Rule::LADDER_RESET_ON_SESSION_END))
    {
        return; // RED shape: the ladder survives the session end
    }
    if (LadderState::BROKEN == m_state)
    {
        leaveBroken(nowNs);
    }
    m_bidCount = 0;
    m_askCount = 0;
    m_version = 0;
    m_state = LadderState::EMPTY;
    m_cause = BreakCause::NONE;
    m_lastSnapshotNs = -1;
}

void BookLadder::breakFor(BreakCause cause, std::int64_t nowNs) noexcept
{
    enterBroken(cause, nowNs);
}

std::int64_t BookLadder::brokenAgeNs(std::int64_t nowNs) const noexcept
{
    if (LadderState::BROKEN != m_state)
    {
        return 0;
    }
    const std::int64_t age = nowNs - m_brokenSinceNs;
    return age > 0 ? age : 0;
}

std::int64_t BookLadder::snapshotAgeNs(std::int64_t nowNs) const noexcept
{
    if (m_lastSnapshotNs < 0)
    {
        return -1;
    }
    const std::int64_t age = nowNs - m_lastSnapshotNs;
    return age > 0 ? age : 0;
}

std::int64_t BookLadder::brokenNsIncludingOpen(std::int64_t nowNs) const noexcept
{
    return m_counters.brokenNsTotal + brokenAgeNs(nowNs);
}

bool BookLadder::sideOrdered(const BookLevel *levels, int n, bool descending) noexcept
{
    for (int i = 1; i < n; i++)
    {
        if (descending ? levels[i].price >= levels[i - 1].price : levels[i].price <= levels[i - 1].price)
        {
            return false;
        }
    }
    return true;
}

bool BookLadder::crossed() const noexcept
{
    return m_bidCount > 0 && m_askCount > 0 && m_bids[0].price >= m_asks[0].price;
}

#ifdef CT_TEST_SEAMS
void BookLadder::adoptUnchecked(const BookLevel *bids, int nBids, const BookLevel *asks, int nAsks, std::int64_t version) noexcept
{
    for (int i = 0; i < nBids && i < LADDER_SLOTS; i++)
    {
        m_bids[i] = bids[i];
    }
    for (int i = 0; i < nAsks && i < LADDER_SLOTS; i++)
    {
        m_asks[i] = asks[i];
    }
    m_bidCount = nBids < LADDER_SLOTS ? nBids : LADDER_SLOTS;
    m_askCount = nAsks < LADDER_SLOTS ? nAsks : LADDER_SLOTS;
    m_version = version;
    m_state = LadderState::OK;
    m_cause = BreakCause::NONE;
    m_lastSnapshotNs = 0;
}
#endif

// ---- BookStitcher

int BookStitcher::onBookFrame(const BookFrame &f, std::int64_t nowNs, LadderOutcome *outcome) noexcept
{
    if (!f.marketReadable)
    {
        m_counters.malformedNoMarket++; // 4.2: malformed without a readable market
        return 0;
    }
    if (!marketInRange(f.marketId))
    {
        m_counters.foreignMarket++; // RC-N6: a deployment mismatch, no ladder changes
        return 0;
    }
    const LadderOutcome got = m_ladders[f.marketId - 1].apply(f, nowNs);
    if (nullptr != outcome)
    {
        *outcome = got;
    }
    return f.marketId;
}

int BookStitcher::onTradeFrame(const TradeFrame &f) noexcept
{
    if (!f.marketReadable)
    {
        m_counters.malformedNoMarket++;
        return 0;
    }
    if (!marketInRange(f.marketId))
    {
        m_counters.foreignMarket++;
        return 0;
    }
    const int slot = f.marketId - 1;
    if (validateTradeFrame(f) != Malformed::NONE)
    {
        m_counters.tradeMalformed[slot]++; // dropped whole: mark and ladder unchanged
        return 0;
    }
    m_counters.tradeBatches[slot]++;
    m_marks[slot].onBatch(f);
    return f.marketId;
}

void BookStitcher::onMeSessionEnd(std::int64_t nowNs) noexcept
{
    for (int i = 0; i < TICK_MARKETS; i++)
    {
        m_ladders[i].reset(nowNs);
        if (!seams::disabled(seams::Rule::LADDER_RESET_ON_SESSION_END))
        {
            m_marks[i].reset();
        }
    }
}

}}}
