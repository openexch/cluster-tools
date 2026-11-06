#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_ME_PUBLISHER_MODEL_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_ME_PUBLISHER_MODEL_H

// RC2 S13 contract 11.2 (W4): the test-side models the StitcherPropertyTest drives the real BookStitcher and
// TickEmitter with. Test only, never linked into a production target.
//
//  - MarketBook      the ME's book of one market under ARRAY semantics (AME:381-400, 431-462, ENG:861-883): a
//                    command mutates the live book, bumps the touched side's version and bookVersion =
//                    max(current + 1, logPosition), then publishes the top-20 with its versions atomically (the
//                    seqlock top). The publisher reads only that published top, so every frame's levels and version
//                    belong together. That is the ARRAY exactness K2 claims (F-S13-10: DIRECT lacks it, carry X8).
//  - MePublisher     a C++ port of MarketPublisher's book chain: the generation reset (MP:540-550), encodeBookSnapshot
//                    with the W-ME2 rules of contract 8.2 switchable back to the ed98477 shape (MP:705-708), the
//                    snapshot / delta decision and the match#115 chain advance (MP:698-772), the diff (MP:902-952:
//                    per side every NEW / UPDATE before every DELETE, bids before asks).
//  - LossyCoreModel  the core's tick apply rule (OE:973-1007) plus the loss of RC-F7: at a core leader change the
//                    last k positively offered ticks are dropped as uncommitted and the state is rebuilt from the
//                    surviving journal. That loss is what E7 (re-offer on registration) exists for.
//
// Nothing here reads a wall clock; the property test hands in every time.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "BookFixtures.h"
#include "cluster_tools/domain/forwarder/MarketData.h"
#include "cluster_tools/domain/forwarder/TickEmitter.h"

namespace cluster_tools { namespace domain { namespace forwarder { namespace me_model {

// The seqlock-published top of one market (AME pubBid*/pubAsk*, pubBidVersion, pubAskVersion, pubBookVersion).
struct PublishedTop
{
    std::vector<BookLevel> bids; // best first, at most LADDER_LEVELS
    std::vector<BookLevel> asks;
    std::int64_t bidVersion = 0;
    std::int64_t askVersion = 0;
    std::int64_t bookVersion = 0;

    bool empty() const { return bids.empty() && asks.empty(); }
    std::int64_t bestBid() const { return bids.empty() ? 0 : bids.front().price; }
    std::int64_t bestAsk() const { return asks.empty() ? 0 : asks.front().price; }
    bool sameLevels(const PublishedTop &o) const
    {
        auto eq = [](const std::vector<BookLevel> &a, const std::vector<BookLevel> &b) {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); i++)
            {
                if (a[i].price != b[i].price || a[i].quantity != b[i].quantity || a[i].orderCount != b[i].orderCount)
                {
                    return false;
                }
            }
            return true;
        };
        return eq(bids, o.bids) && eq(asks, o.asks);
    }
};

enum class Op : std::uint8_t
{
    ADD,    // a resting order at `price` (a new level, or more qty and one more order on it)
    CANCEL, // `qty` off the level at `price` (the level goes when nothing is left)
    FILL    // `qty` off the BEST level of the side (a trade at that price)
};

struct Command
{
    int market = 1;
    Op op = Op::ADD;
    bool bidSide = true;
    std::int64_t price = 0;
    std::int64_t qty = 0;
};

class MarketBook
{
public:
    // Applies one command at `logPosition`. Returns false when the command changed nothing (the ME would have
    // rejected it: nothing is versioned or published). `tradePrice` is set when a FILL traded.
    bool apply(const Command &c, std::int64_t logPosition, std::int64_t &tradePrice)
    {
        tradePrice = 0;
        bool changed = false;
        if (c.bidSide)
        {
            changed = applyOn(m_bids, c, tradePrice);
        }
        else
        {
            changed = applyOn(m_asks, c, tradePrice);
        }
        if (!changed)
        {
            return false;
        }
        (c.bidSide ? m_bidVersion : m_askVersion)++;
        m_bookVersion = std::max(m_bookVersion + 1, logPosition); // ENG:861-866
        publishTopOfBook();                                        // ENG:883
        return true;
    }

    const PublishedTop &published() const { return m_pub; }
    std::int64_t bookVersion() const { return m_bookVersion; }
    bool liveEmpty() const { return m_bids.empty() && m_asks.empty(); }

private:
    template <class Side>
    static bool applyOn(Side &side, const Command &c, std::int64_t &tradePrice)
    {
        switch (c.op)
        {
            case Op::ADD:
            {
                BookLevel &l = side[c.price];
                l.price = c.price;
                l.quantity += c.qty;
                l.orderCount += 1;
                return true;
            }
            case Op::CANCEL:
            {
                auto it = side.find(c.price);
                if (it == side.end())
                {
                    return false;
                }
                if (c.qty >= it->second.quantity)
                {
                    side.erase(it);
                }
                else
                {
                    it->second.quantity -= c.qty;
                    if (it->second.orderCount > 1)
                    {
                        it->second.orderCount--;
                    }
                }
                return true;
            }
            case Op::FILL:
            {
                if (side.empty())
                {
                    return false;
                }
                auto it = side.begin();
                tradePrice = it->first;
                if (c.qty >= it->second.quantity)
                {
                    side.erase(it);
                }
                else
                {
                    it->second.quantity -= c.qty;
                }
                return true;
            }
        }
        return false;
    }

    template <class Side>
    static void collect(const Side &side, std::vector<BookLevel> &out)
    {
        out.clear();
        for (const auto &kv : side)
        {
            if (static_cast<int>(out.size()) == LADDER_LEVELS)
            {
                break;
            }
            out.push_back(kv.second);
        }
    }

    // AME publishTopOfBook: the dirty gate on the side versions, then one consistent copy of the top.
    void publishTopOfBook()
    {
        if (m_bidVersion == m_lastPubBidVersion && m_askVersion == m_lastPubAskVersion)
        {
            return;
        }
        m_lastPubBidVersion = m_bidVersion;
        m_lastPubAskVersion = m_askVersion;
        collect(m_bids, m_pub.bids);
        collect(m_asks, m_pub.asks);
        m_pub.bidVersion = m_bidVersion;
        m_pub.askVersion = m_askVersion;
        m_pub.bookVersion = m_bookVersion;
    }

    std::map<std::int64_t, BookLevel, std::greater<std::int64_t>> m_bids;
    std::map<std::int64_t, BookLevel> m_asks;
    std::int64_t m_bidVersion = 0;
    std::int64_t m_askVersion = 0;
    std::int64_t m_bookVersion = 0;
    std::int64_t m_lastPubBidVersion = -1;
    std::int64_t m_lastPubAskVersion = -1;
    PublishedTop m_pub;
};

// One MarketPublisher's book chain (one per node and market). flush() is one scheduler tick of MP:522-567 reduced
// to the book: the generation check, then encodeBookSnapshot. Returns true when a frame was encoded into `out`.
class MePublisher
{
public:
    bool emptyBookFrames = true;       // W-ME2 (8.2) on; false = the ed98477 early return MP:705-708
    bool emptyBookVersionGuard = true; // the 8.2 guard; false = a never-touched market sends a version-0 empty 22 (rule U)

    bool flush(std::int64_t generation, int market, const PublishedTop &top, BookFrame &out)
    {
        if (generation != m_lastResnapshotGen) // MP:540-550
        {
            m_lastResnapshotGen = generation;
            m_sentInitialSnapshot = false;
            m_lastBidVersion = -1;
            m_lastAskVersion = -1;
            m_lastPublishedBookVersion = 0;
            m_lastBids.clear();
            m_lastAsks.clear();
        }
        return encodeBookSnapshot(market, top, out);
    }

    std::int64_t lastPublishedBookVersion() const { return m_lastPublishedBookVersion; }
    bool sentInitialSnapshot() const { return m_sentInitialSnapshot; }

private:
    bool encodeBookSnapshot(int market, const PublishedTop &top, BookFrame &out)
    {
        const int bidCount = static_cast<int>(top.bids.size());
        const int askCount = static_cast<int>(top.asks.size());
        if (0 == bidCount && 0 == askCount)
        {
            if (!emptyBookFrames)
            {
                return false; // MP:705-708 before W-ME2: an emptied book is invisible (F-S13-1)
            }
            const bool known = !emptyBookVersionGuard || top.bookVersion > 0;
            const bool shows = !m_sentInitialSnapshot || m_lastBids.size() + m_lastAsks.size() > 0;
            if (!known || !shows)
            {
                return false;
            }
        }
        const bool changed = top.bidVersion != m_lastBidVersion || top.askVersion != m_lastAskVersion;
        if (!changed)
        {
            return false;
        }
        m_lastBidVersion = top.bidVersion;
        m_lastAskVersion = top.askVersion;

        bool encoded = false;
        if (!m_sentInitialSnapshot)
        {
            out = book_fixtures::snapshot(market, top.bookVersion, {}, {});
            for (const BookLevel &l : top.bids)
            {
                out.bids[out.nBids++] = l;
            }
            for (const BookLevel &l : top.asks)
            {
                out.asks[out.nAsks++] = l;
            }
            out.statedCount[0] = out.nBids;
            out.statedCount[1] = out.nAsks;
            m_sentInitialSnapshot = true;
            encoded = true;
        }
        else
        {
            out = book_fixtures::delta(market, m_lastPublishedBookVersion, top.bookVersion, {});
            computeLevelChanges(m_lastBids, top.bids, BOOK_SIDE_BID, out);
            computeLevelChanges(m_lastAsks, top.asks, BOOK_SIDE_ASK, out);
            out.statedCount[0] = out.nChanges;
            encoded = out.nChanges > 0;
        }
        if (encoded)
        {
            m_lastPublishedBookVersion = top.bookVersion; // match#115: only when a frame was encoded
        }
        m_lastBids = top.bids; // the last state is stored either way (MP:758-768)
        m_lastAsks = top.asks;
        return encoded;
    }

    // MP:902-952: every NEW / UPDATE in the new side's order, then one DELETE per vanished old price. Both sides are
    // sorted best first, so one merge pass finds the pairs (the Java scans; the output is the same).
    static void computeLevelChanges(const std::vector<BookLevel> &was, const std::vector<BookLevel> &now,
        std::uint8_t side, BookFrame &f)
    {
        const bool desc = BOOK_SIDE_BID == side;
        auto before = [desc](std::int64_t a, std::int64_t b) { return desc ? a > b : a < b; };
        std::size_t i = 0, j = 0;
        BookChange deletes[LADDER_LEVELS];
        int nDeletes = 0;
        while (i < was.size() || j < now.size())
        {
            if (j == now.size() || (i < was.size() && before(was[i].price, now[j].price)))
            {
                deletes[nDeletes++] = BookChange{ was[i].price, 0, 0, side, BOOK_UPDATE_DELETE };
                i++;
            }
            else if (i == was.size() || before(now[j].price, was[i].price))
            {
                f.changes[f.nChanges++] = BookChange{ now[j].price, now[j].quantity, now[j].orderCount, side, BOOK_UPDATE_NEW };
                j++;
            }
            else
            {
                if (was[i].quantity != now[j].quantity || was[i].orderCount != now[j].orderCount)
                {
                    f.changes[f.nChanges++] = BookChange{ now[j].price, now[j].quantity, now[j].orderCount, side, BOOK_UPDATE_UPDATE };
                }
                i++;
                j++;
            }
        }
        for (int d = 0; d < nDeletes; d++)
        {
            f.changes[f.nChanges++] = deletes[d];
        }
    }

    std::int64_t m_lastResnapshotGen = 0;
    bool m_sentInitialSnapshot = false;
    std::int64_t m_lastBidVersion = -1;
    std::int64_t m_lastAskVersion = -1;
    std::int64_t m_lastPublishedBookVersion = 0;
    std::vector<BookLevel> m_lastBids;
    std::vector<BookLevel> m_lastAsks;
};

// OE:973-1007 (after the epoch checks, which the forwarder's gate makes moot here) with the RC-F7 loss.
class LossyCoreModel
{
public:
    enum class Verdict : std::uint8_t
    {
        APPLIED,
        STALE,
        INVALID
    };

    struct Market
    {
        std::int64_t bookVersion = 0;
        std::int64_t bestBid = 0;
        std::int64_t bestAsk = 0;
        std::int64_t lastTradeId = 0;
        std::int64_t lastTradePrice = 0;
    };

    struct Entry
    {
        int market;
        TickCandidate c;
    };

    // A positively offered tick: journaled, then applied by the OE rule.
    Verdict offer(int market, const TickCandidate &c)
    {
        m_journal.push_back(Entry{ market, c });
        const Verdict v = applyOne(market, c);
        switch (v)
        {
            case Verdict::APPLIED: m_applied++; break;
            case Verdict::STALE: m_stale++; break;
            case Verdict::INVALID: m_invalid++; break;
        }
        return v;
    }

    // RC-F7: the last k journaled ticks were never committed. Returns the markets whose state changed by the loss.
    std::vector<int> leaderChange(int k)
    {
        Market before[TICK_MARKETS + 1];
        for (int m = 1; m <= TICK_MARKETS; m++)
        {
            before[m] = m_markets[m];
        }
        const std::size_t keep = m_journal.size() > static_cast<std::size_t>(k) ? m_journal.size() - k : 0;
        m_journal.resize(keep);
        for (int m = 1; m <= TICK_MARKETS; m++)
        {
            m_markets[m] = Market{};
            m_appliedVersions[m].clear();
        }
        for (const Entry &e : m_journal)
        {
            applyOne(e.market, e.c);
        }
        std::vector<int> changed;
        for (int m = 1; m <= TICK_MARKETS; m++)
        {
            const Market &a = m_markets[m], &b = before[m];
            if (a.bookVersion != b.bookVersion || a.bestBid != b.bestBid || a.bestAsk != b.bestAsk)
            {
                changed.push_back(m);
            }
        }
        m_leaderChanges++;
        return changed;
    }

    const Market &market(int m) const { return m_markets[m]; }
    std::uint64_t applied() const { return m_applied; }
    std::uint64_t stale() const { return m_stale; }
    std::uint64_t invalid() const { return m_invalid; }
    std::uint64_t leaderChanges() const { return m_leaderChanges; }
    // S6: true if a version was ever applied twice on the surviving journal (impossible under the OE rule)
    bool duplicateApplied() const { return m_duplicateApplied; }

private:
    Verdict applyOne(int market, const TickCandidate &c)
    {
        if (market < 1 || market > TICK_MARKETS)
        {
            return Verdict::INVALID;
        }
        if (c.bid < 0 || c.ask < 0 || c.tradePrice < 0 || c.tradeId < 0 || (c.bid > 0 && c.ask > 0 && c.bid >= c.ask))
        {
            return Verdict::INVALID;
        }
        Market &m = m_markets[market];
        if (c.version <= m.bookVersion)
        {
            return Verdict::STALE;
        }
        if (!m_appliedVersions[market].insert(c.version).second)
        {
            m_duplicateApplied = true;
        }
        m.bookVersion = c.version;
        m.bestBid = c.bid;
        m.bestAsk = c.ask;
        if (c.tradeId > m.lastTradeId && c.tradePrice > 0)
        {
            m.lastTradeId = c.tradeId;
            m.lastTradePrice = c.tradePrice;
        }
        return Verdict::APPLIED;
    }

    Market m_markets[TICK_MARKETS + 1];
    std::vector<Entry> m_journal;
    std::set<std::int64_t> m_appliedVersions[TICK_MARKETS + 1];
    bool m_duplicateApplied = false;
    std::uint64_t m_applied = 0;
    std::uint64_t m_stale = 0;
    std::uint64_t m_invalid = 0;
    std::uint64_t m_leaderChanges = 0;
};

}}}}

#endif
