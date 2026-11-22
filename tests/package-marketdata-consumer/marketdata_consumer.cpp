// Links the market data components of the installed package: a hand-built 22 BookSnapshot goes through the forwarder
// codec into the book ladder, a hand-built ME journal record 1 through the record decode (public fields only), and
// the stall verdict helpers of the archive follow library answer. Exit 0 and "market data consumer OK" = pass.
#include "cluster_tools/adapters/sbe/ForwarderCodec.h"
#include "cluster_tools/adapters/sbe/JournalTradeCodec.h"
#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/domain/forwarder/BookLadder.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace fc = cluster_tools::sbe_adapter::forwarder_codec;
namespace jc = cluster_tools::sbe_adapter::journal_codec;
namespace fwd = cluster_tools::domain::forwarder;
namespace ar = cluster_tools::archive;

namespace
{

int failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "market data consumer: FAILED %s\n", what);
        failures++;
    }
}

template<typename T>
void put(std::vector<std::uint8_t> &b, std::size_t at, T v)
{
    std::memcpy(b.data() + at, &v, sizeof v); // little-endian hosts only, like the codecs
}

// order schema 1 v12, template 22: block 36 (marketId 0, timestamp 4, bidVersion 12, askVersion 20, bookVersion 28),
// then the bid and the ask group (entry 20: price 0, quantity 8, orderCount 16)
std::vector<std::uint8_t> snapshot(std::int32_t market, std::int64_t version)
{
    const std::int64_t bids[][3] = { { 100, 5, 1 }, { 99, 3, 2 } };
    const std::int64_t asks[][3] = { { 101, 4, 1 } };
    std::vector<std::uint8_t> b(8 + 36 + 4 + 2 * 20 + 4 + 1 * 20, 0);
    put<std::uint16_t>(b, 0, 36);
    put<std::uint16_t>(b, 2, fc::T_BOOK_SNAPSHOT);
    put<std::uint16_t>(b, 4, fc::MATCH_SCHEMA_ID);
    put<std::uint16_t>(b, 6, fc::MATCH_VERSION);
    put<std::int32_t>(b, 8, market);
    put<std::int64_t>(b, 8 + 4, 1700000000000LL);
    put<std::int64_t>(b, 8 + 12, version);
    put<std::int64_t>(b, 8 + 20, version);
    put<std::int64_t>(b, 8 + 28, version);
    std::size_t at = 8 + 36;
    put<std::uint16_t>(b, at, 20);
    put<std::uint16_t>(b, at + 2, 2);
    at += 4;
    for (const auto &l : bids)
    {
        put<std::int64_t>(b, at, l[0]);
        put<std::int64_t>(b, at + 8, l[1]);
        put<std::int32_t>(b, at + 16, static_cast<std::int32_t>(l[2]));
        at += 20;
    }
    put<std::uint16_t>(b, at, 20);
    put<std::uint16_t>(b, at + 2, 1);
    at += 4;
    for (const auto &l : asks)
    {
        put<std::int64_t>(b, at, l[0]);
        put<std::int64_t>(b, at + 8, l[1]);
        put<std::int32_t>(b, at + 16, static_cast<std::int32_t>(l[2]));
        at += 20;
    }
    return b;
}

// ME journal schema 4 v1, record 1: block 109 at the schema's offsets
std::vector<std::uint8_t> journalTrade()
{
    std::vector<std::uint8_t> b(jc::TRADE_RECORD_LENGTH, 0);
    put<std::uint16_t>(b, 0, jc::TRADE_BLOCK_LENGTH);
    put<std::uint16_t>(b, 2, jc::TEMPLATE_TRADE);
    put<std::uint16_t>(b, 4, jc::JOURNAL_SCHEMA_ID);
    put<std::uint16_t>(b, 6, jc::JOURNAL_VERSION);
    put<std::int64_t>(b, 8 + 0, 41);           // journalSeq
    put<std::int64_t>(b, 8 + 8, 3);            // meEpoch
    put<std::int64_t>(b, 8 + 16, 900);         // egressSeq
    put<std::int64_t>(b, 8 + 24, 77);          // tradeId
    put<std::int32_t>(b, 8 + 32, 2);           // marketId
    put<std::int64_t>(b, 8 + 44, 5001);        // takerUserId
    put<std::int64_t>(b, 8 + 68, 100);         // price
    put<std::int64_t>(b, 8 + 76, 4);           // quantity
    b[8 + 84] = 1;                             // takerIsBuy
    put<std::int64_t>(b, 8 + 85, 1700000000123LL); // timestamp
    return b;
}

}

int main()
{
    // the book: 22 -> codec -> ladder
    fwd::BookStitcher stitcher;
    const auto frame = snapshot(2, 7);
    fc::Header header;
    expect(fc::readHeader(frame.data(), frame.size(), header) && header.templateId == fc::T_BOOK_SNAPSHOT,
        "the 22 header reads");
    fwd::BookFrame book;
    expect(fc::decodeBookSnapshot(frame.data(), frame.size(), header, book), "the 22 decodes");
    fwd::LadderOutcome outcome = fwd::LadderOutcome::IGNORED;
    expect(stitcher.onBookFrame(book, 1, &outcome) == 2 && outcome == fwd::LadderOutcome::SNAPSHOT_APPLIED,
        "the snapshot reaches market 2's ladder");
    const fwd::BookLadder &ladder = stitcher.ladder(2);
    expect(ladder.state() == fwd::LadderState::OK && ladder.version() == 7, "the ladder is OK at version 7");
    expect(ladder.bidCount() == 2 && ladder.askCount() == 1 && ladder.bestBid() == 100 && ladder.bestAsk() == 101 &&
            ladder.bid(1).orderCount == 2, "the ladder holds the levels with their order counts");
    const auto foreign = snapshot(6, 1);
    fwd::BookFrame other;
    expect(fc::readHeader(foreign.data(), foreign.size(), header) &&
            fc::decodeBookSnapshot(foreign.data(), foreign.size(), header, other) && stitcher.onBookFrame(other, 2) == 0 &&
            stitcher.counters().foreignMarket == 1, "a market outside 1..5 is counted foreign");
    stitcher.onMeSessionEnd(3);
    expect(stitcher.ladder(2).state() == fwd::LadderState::EMPTY, "a session end empties the ladders");

    // the tape: record 1, public fields only
    const auto record = journalTrade();
    jc::TradeRecord trade;
    expect(jc::decodeTrade(record.data(), record.size(), trade), "record 1 decodes");
    expect(trade.journalSeq == 41 && trade.meEpoch == 3 && trade.tradeId == 77 && trade.marketId == 2 &&
            trade.price == 100 && trade.quantity == 4 && trade.takerIsBuyRaw == 1 && trade.timestamp == 1700000000123LL,
        "record 1 carries its public fields");
    expect(!jc::decodeTrade(record.data(), record.size() - 1, trade), "a short record is refused");

    // the stall verdicts
    expect(ar::stallReasonOf(ar::FaultKind::STALL) == ar::StallReason::STALL &&
            !ar::stallReasonOf(ar::FaultKind::CONNECT_FAILED).has_value(), "the stall mapping answers");
    ar::StallVerdicts verdicts;
    const ar::TimePoint t0 = ar::Clock::now();
    expect(!ar::checkedClean(verdicts, t0, std::chrono::seconds(20)), "never checked is not clean");
    verdicts.lastPassAt = t0;
    expect(ar::checkedClean(verdicts, t0 + std::chrono::seconds(5), std::chrono::seconds(20)), "a recent pass is clean");
    verdicts.sincePass = ar::StallReason::IMAGE_LOST;
    expect(!ar::checkedClean(verdicts, t0 + std::chrono::seconds(5), std::chrono::seconds(20)),
        "a verdict after the pass is not clean");
    expect(std::strcmp(ar::stallLabel(ar::StallReason::IMAGE_LOST), "image_lost") == 0, "the stall label");

    if (0 != failures)
    {
        return 1;
    }
    std::printf("market data consumer OK\n");
    return 0;
}
