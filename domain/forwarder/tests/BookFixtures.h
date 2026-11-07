#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_BOOK_FIXTURES_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_BOOK_FIXTURES_H

#include <cstdint>
#include <initializer_list>

#include "cluster_tools/domain/forwarder/MarketData.h"

// Decoded-frame builders for the S13 domain tests (BookLadderTest, TickEmitterTest): the shapes the codec (W1)
// produces for well-formed 22 / 25 / 26 frames, with every "malformed" knob of contract 4.2 reachable by editing the
// returned struct. No bytes: the ladder is tested on the decoded view, the codec on the goldens.

namespace cluster_tools { namespace domain { namespace forwarder { namespace book_fixtures {

struct Lv
{
    std::int64_t price;
    std::int64_t qty;
    std::int32_t count = 1;
};

struct Ch
{
    std::uint8_t side;       // BOOK_SIDE_BID / BOOK_SIDE_ASK (raw)
    std::uint8_t type;       // BOOK_UPDATE_NEW / UPDATE / DELETE (raw)
    std::int64_t price;
    std::int64_t qty = 0;
    std::int32_t count = 0;
};

inline BookFrame snapshot(std::int32_t market, std::int64_t version, std::initializer_list<Lv> bids,
    std::initializer_list<Lv> asks)
{
    BookFrame f;
    f.snapshot = true;
    f.marketReadable = true;
    f.lengthOk = true;
    f.marketId = market;
    f.bookVersion = version;
    f.headerBlockLength = SNAPSHOT_BLOCK_MIN;
    f.groupBlockLength[0] = SNAPSHOT_ENTRY_MIN;
    f.groupBlockLength[1] = SNAPSHOT_ENTRY_MIN;
    for (const Lv &l : bids)
    {
        f.bids[f.nBids++] = BookLevel{ l.price, l.qty, l.count };
    }
    for (const Lv &l : asks)
    {
        f.asks[f.nAsks++] = BookLevel{ l.price, l.qty, l.count };
    }
    f.statedCount[0] = f.nBids;
    f.statedCount[1] = f.nAsks;
    return f;
}

inline BookFrame delta(std::int32_t market, std::int64_t from, std::int64_t to, std::initializer_list<Ch> changes)
{
    BookFrame f;
    f.snapshot = false;
    f.marketReadable = true;
    f.lengthOk = true;
    f.marketId = market;
    f.bookVersion = to;
    f.fromVersion = from;
    f.headerBlockLength = DELTA_BLOCK_MIN;
    f.groupBlockLength[0] = DELTA_ENTRY_MIN;
    for (const Ch &c : changes)
    {
        BookChange &out = f.changes[f.nChanges++];
        out.price = c.price;
        out.quantity = c.qty;
        out.orderCount = c.count;
        out.side = c.side;
        out.updateType = c.type;
    }
    f.statedCount[0] = f.nChanges;
    return f;
}

// short forms
inline Ch bidNew(std::int64_t p, std::int64_t q, std::int32_t c = 1) { return Ch{ BOOK_SIDE_BID, BOOK_UPDATE_NEW, p, q, c }; }
inline Ch bidUpd(std::int64_t p, std::int64_t q, std::int32_t c = 1) { return Ch{ BOOK_SIDE_BID, BOOK_UPDATE_UPDATE, p, q, c }; }
inline Ch bidDel(std::int64_t p) { return Ch{ BOOK_SIDE_BID, BOOK_UPDATE_DELETE, p, 0, 0 }; }
inline Ch askNew(std::int64_t p, std::int64_t q, std::int32_t c = 1) { return Ch{ BOOK_SIDE_ASK, BOOK_UPDATE_NEW, p, q, c }; }
inline Ch askUpd(std::int64_t p, std::int64_t q, std::int32_t c = 1) { return Ch{ BOOK_SIDE_ASK, BOOK_UPDATE_UPDATE, p, q, c }; }
inline Ch askDel(std::int64_t p) { return Ch{ BOOK_SIDE_ASK, BOOK_UPDATE_DELETE, p, 0, 0 }; }

// A well-formed 26 reduced to the mark: the highest tradeId with its price. count 0 = an empty batch.
inline TradeFrame trades(std::int32_t market, std::int64_t maxTradeId, std::int64_t price, std::uint16_t count = 1)
{
    TradeFrame t;
    t.marketReadable = true;
    t.lengthOk = true;
    t.marketId = market;
    t.headerBlockLength = TRADE_BLOCK_MIN;
    t.entryBlockLength = TRADE_ENTRY_MIN;
    t.statedCount = count;
    t.maxTradeId = maxTradeId;
    t.priceAtMax = price;
    return t;
}

}}}}

#endif
