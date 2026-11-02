#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_BOOK_LADDER_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_BOOK_LADDER_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/MarketData.h"

// BookLadder - the per-market top-20 ladder the forwarder stitches from the ME's BookSnapshot 22 / BookDelta 25
// egress (RC2 S13 contract section 4), the per-market TradeMark from TradeExecutionBatch 26 (section 5), and the
// BookStitcher that routes decoded frames to the market's ladder (4.2 market range, malformed without a market) and
// resets everything on an ME session end (4.1 rule 6). Pure: no Aeron, no SBE, no allocation after construction, no
// wall clock (every `nowNs` is handed in by the duty cycle).
//
// K1: heap only, a restart starts EMPTY. K2: an OK ladder equals the ME's visible top-20 at `version`; a ladder that
// cannot prove that is BROKEN and emits nothing. 4.2: the frame validation rule (validateBookFrame /
// validateTradeFrame, MarketData.cpp) is applied in every state before any state rule. 4.1 rule 5: BROKEN is never a
// latch, the next well-formed snapshot of the market clears it.

namespace cluster_tools { namespace domain { namespace forwarder {

enum class LadderState : std::uint8_t
{
    EMPTY = 0, // no versioned state known (start, session end, rule U)
    OK = 1,    // equals the ME's visible top-20 at `version`
    BROKEN = 2 // the chain cannot be trusted; only a snapshot leaves it
};

enum class BreakCause : std::uint8_t
{
    NONE = 0,
    FROM_MISMATCH, // 4.1 rule 3: a delta whose fromVersion is not the ladder's version
    LEVEL_OP,      // 4.1 rule 2: NEW of a present price, UPDATE / DELETE of an absent one
    DEPTH,         // 4.1 rule 2: more than 20 levels on a side at the frame end (or no slot left mid frame)
    CROSSED,       // 4.1 rule 2 / E9: bestBid >= bestAsk with both sides non-empty
    MALFORMED,     // 4.2: a malformed 22 / 25 of this market (also an unordered snapshot)
    COUNT
};

const char *nameOf(LadderState state) noexcept;
const char *nameOf(BreakCause cause) noexcept;

// What a frame did to a ladder (the caller's touched set and the per-template counters of 10.1 come from it).
enum class LadderOutcome : std::uint8_t
{
    SNAPSHOT_APPLIED,  // rule 1: the ladder is OK at the frame's bookVersion
    DELTA_APPLIED,     // rule 2
    UNVERSIONED,       // rule U: the ladder is EMPTY
    IGNORED,           // rule 4: a delta while EMPTY or BROKEN
    BROKE              // rules 2, 3 or 4.2: the ladder is BROKEN (breakCause says why)
};

struct LadderCounters
{
    std::uint64_t snapshots = 0;                                      // forwarder_book_frames_total{template="22"}
    std::uint64_t deltas = 0;                                         // forwarder_book_frames_total{template="25"}
    std::uint64_t ignored = 0;                                        // forwarder_book_frames_ignored_total
    std::uint64_t chainBreaks[static_cast<unsigned>(BreakCause::COUNT)] = {}; // forwarder_book_chain_breaks_total{cause}
    std::uint64_t recoveries = 0;                                     // forwarder_book_recoveries_total (BROKEN -> OK)
    std::uint64_t versionRegressions = 0;                             // forwarder_book_version_regressions_total
    std::uint64_t unversioned = 0;                                    // forwarder_book_unversioned_total (rule U)
    std::int64_t brokenNsTotal = 0;                                   // forwarder_book_broken_seconds_total (closed episodes)

    std::uint64_t breaks(BreakCause c) const noexcept { return chainBreaks[static_cast<unsigned>(c)]; }
    std::uint64_t breaksTotal() const noexcept
    {
        std::uint64_t sum = 0;
        for (auto v : chainBreaks)
        {
            sum += v;
        }
        return sum;
    }
};

class BookLadder
{
public:
    BookLadder() noexcept = default;

    // A decoded 22 or 25 of THIS market (the stitcher routed it; the frame's marketId is not re-checked here).
    // Validation (4.2) first in every state, then the rule of 4.1 for the frame's template and the ladder's state.
    LadderOutcome apply(const BookFrame &frame, std::int64_t nowNs) noexcept;

    // 4.1 rule 6 (ME session end): EMPTY, contents and version gone, counters kept. Closes a BROKEN episode.
    void reset(std::int64_t nowNs) noexcept;

    // E9 (TickEmitter): a candidate the stitcher itself produced failed validation. The ladder becomes BROKEN with
    // `cause`; contents discarded; counted as a chain break like any other.
    void breakFor(BreakCause cause, std::int64_t nowNs) noexcept;

    LadderState state() const noexcept { return m_state; }
    BreakCause breakCause() const noexcept { return m_cause; }
    std::int64_t version() const noexcept { return m_version; } // 0 when EMPTY; the last good version while BROKEN
    int bidCount() const noexcept { return m_bidCount; }
    int askCount() const noexcept { return m_askCount; }
    const BookLevel &bid(int i) const noexcept { return m_bids[i]; } // i < bidCount(), best first
    const BookLevel &ask(int i) const noexcept { return m_asks[i]; }
    std::int64_t bestBid() const noexcept { return m_bidCount > 0 ? m_bids[0].price : 0; }
    std::int64_t bestAsk() const noexcept { return m_askCount > 0 ? m_asks[0].price : 0; }

    // 0 when not BROKEN; else nowNs - the entry into BROKEN.
    std::int64_t brokenAgeNs(std::int64_t nowNs) const noexcept;
    // -1 when no snapshot was ever applied (also after reset); else nowNs - the last applied snapshot.
    std::int64_t snapshotAgeNs(std::int64_t nowNs) const noexcept;
    // brokenNsTotal plus the open BROKEN episode, for the render (10.1 forwarder_book_broken_seconds_total).
    std::int64_t brokenNsIncludingOpen(std::int64_t nowNs) const noexcept;
    const LadderCounters &counters() const noexcept { return m_counters; }

#ifdef CT_TEST_SEAMS
    // Test builds only: load an arbitrary (possibly defective) OK ladder without validation, so the emitter's own
    // validation (E9) can be exercised against a state the ladder's rules would never produce.
    void adoptUnchecked(const BookLevel *bids, int nBids, const BookLevel *asks, int nAsks, std::int64_t version) noexcept;
#endif

private:
    LadderOutcome applySnapshot(const BookFrame &f, std::int64_t nowNs) noexcept;
    LadderOutcome applyDelta(const BookFrame &f, std::int64_t nowNs) noexcept;
    bool applyChange(const BookChange &c) noexcept; // false = LEVEL_OP or DEPTH (m_pendingCause set)
    LadderOutcome enterBroken(BreakCause cause, std::int64_t nowNs) noexcept;
    void leaveBroken(std::int64_t nowNs) noexcept;
    static bool sideOrdered(const BookLevel *levels, int n, bool descending) noexcept;
    bool crossed() const noexcept;

    BookLevel m_bids[LADDER_SLOTS] = {};
    BookLevel m_asks[LADDER_SLOTS] = {};
    int m_bidCount = 0;
    int m_askCount = 0;
    std::int64_t m_version = 0;
    LadderState m_state = LadderState::EMPTY;
    BreakCause m_cause = BreakCause::NONE;
    BreakCause m_pendingCause = BreakCause::NONE;
    std::int64_t m_brokenSinceNs = 0;
    std::int64_t m_lastSnapshotNs = -1;
    LadderCounters m_counters;
};

// Section 5: the highest tradeId seen in the market with its price. Rides on ticks only.
class TradeMark
{
public:
    std::int64_t tradeId() const noexcept { return m_tradeId; }
    std::int64_t price() const noexcept { return m_price; }

    // A well-formed 26 (the stitcher validated it): max by tradeId; a re-sent entry is a no-op (RC-N2).
    void onBatch(const TradeFrame &frame) noexcept
    {
        if (frame.maxTradeId > m_tradeId)
        {
            m_tradeId = frame.maxTradeId;
            m_price = frame.priceAtMax;
        }
    }
    void reset() noexcept
    {
        m_tradeId = 0;
        m_price = 0;
    }

private:
    std::int64_t m_tradeId = 0;
    std::int64_t m_price = 0;
};

// The five ladders and trade marks of markets 1..5 (TICK_MARKETS), the routing of 4.2 and the reset of 4.1 rule 6.
class BookStitcher
{
public:
    struct Counters
    {
        std::uint64_t foreignMarket = 0;                 // forwarder_book_foreign_market_total (marketId outside 1..5)
        std::uint64_t malformedNoMarket = 0;             // forwarder_book_malformed_total (no readable market)
        std::uint64_t tradeMalformed[TICK_MARKETS] = {}; // forwarder_trade_malformed_total{market}
        std::uint64_t tradeBatches[TICK_MARKETS] = {};   // well-formed 26 applied to the mark
    };

    static bool marketInRange(std::int32_t marketId) noexcept { return marketId >= 1 && marketId <= TICK_MARKETS; }

    // Routes a decoded 22 / 25. Returns the 1-based market whose ladder was touched (applied, broke, unversioned or
    // ignored), or 0 when the frame was dropped before any ladder (foreign market, no readable market).
    int onBookFrame(const BookFrame &frame, std::int64_t nowNs, LadderOutcome *outcome = nullptr) noexcept;
    // Routes a decoded 26: a malformed batch is dropped whole (counted), a foreign market is counted, else the mark moves.
    // Returns the 1-based market whose mark was touched, or 0.
    int onTradeFrame(const TradeFrame &frame) noexcept;
    // 4.1 rule 6: every ladder EMPTY, every mark 0.
    void onMeSessionEnd(std::int64_t nowNs) noexcept;

    BookLadder &ladder(int market) noexcept { return m_ladders[market - 1]; }
    const BookLadder &ladder(int market) const noexcept { return m_ladders[market - 1]; }
    TradeMark &mark(int market) noexcept { return m_marks[market - 1]; }
    const TradeMark &mark(int market) const noexcept { return m_marks[market - 1]; }
    const Counters &counters() const noexcept { return m_counters; }

private:
    BookLadder m_ladders[TICK_MARKETS];
    TradeMark m_marks[TICK_MARKETS];
    Counters m_counters;
};

}}}

#endif
