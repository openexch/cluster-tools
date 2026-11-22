// JournalTradeCodecTest: the shared decode of ME journal record 1, graded against the bytes the ME's own Java
// encoder wrote (archive/tests/fixtures/mejournal/java-trade.bin, pinned by SHA256SUMS), never against bytes this
// codec produced. The bridge reads its rows through the same readTrade, so the bridge codec suite grades it too.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "cluster_tools/adapters/sbe/JournalTradeCodec.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace jc = cluster_tools::sbe_adapter::journal_codec;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes javaTrade()
{
    const std::string path = std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/java-trade.bin";
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot read " + path);
    }
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void put16(Bytes &b, std::size_t at, std::uint16_t v)
{
    b[at] = static_cast<std::uint8_t>(v & 0xff);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// the values the Java encoder wrote into the fixture
void checkPublic(const jc::TradeRecord &t)
{
    CHECK(t.journalSeq == 2);
    CHECK(t.meEpoch == 0x123456789abcdef1LL);
    CHECK(t.tradeId == 1);
    CHECK(t.marketId == 1);
    CHECK(t.price == 12345678901234LL);
    CHECK(t.quantity == 123456789LL);
    CHECK(t.takerIsBuyRaw == 1);
    CHECK(t.timestamp == 1700000000123LL);
}

}

TEST_CASE("JournalTradeCodecTest.theJavaTradeDecodesFieldByField")
{
    const Bytes b = javaTrade();
    REQUIRE(b.size() == jc::TRADE_RECORD_LENGTH);
    CHECK(jc::isTradeRecord(b.data(), b.size()));
    jc::TradeRecord t;
    jc::TradeParties p;
    REQUIRE(jc::decodeTrade(b.data(), b.size(), t, &p));
    checkPublic(t);
    CHECK(p.egressSeq == 128);
    CHECK(p.takerOrderId == 91);
    CHECK(p.takerUserId == 11);
    CHECK(p.makerOrderId == 92);
    CHECK(p.makerUserId == 12);
    CHECK(p.takerOmsOrderId == 101);
    CHECK(p.makerOmsOrderId == 102);
}

TEST_CASE("JournalTradeCodecTest.withoutPartiesOnlyThePublicFieldsAreRead")
{
    const Bytes b = javaTrade();
    jc::TradeRecord t;
    REQUIRE(jc::decodeTrade(b.data(), b.size(), t));
    checkPublic(t);

    jc::TradeRecord viaRead;
    jc::readTrade(b.data(), viaRead);
    checkPublic(viaRead);
}

TEST_CASE("JournalTradeCodecTest.anythingButAWholeRecordOneIsRefusedAndNothingIsWritten")
{
    const Bytes good = javaTrade();
    struct Case
    {
        const char *name;
        std::size_t at;
        std::uint16_t value;
    };
    const Case cases[] = {
        { "block 108", 0, 108 },
        { "template 2", 2, 2 },
        { "schema 1", 4, 1 },
        { "version 2", 6, 2 },
    };
    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        Bytes b = good;
        put16(b, c.at, c.value);
        CHECK_FALSE(jc::isTradeRecord(b.data(), b.size()));
        jc::TradeRecord t;
        t.tradeId = -7;
        jc::TradeParties p;
        p.takerUserId = -7;
        CHECK_FALSE(jc::decodeTrade(b.data(), b.size(), t, &p));
        CHECK(t.tradeId == -7);
        CHECK(p.takerUserId == -7);
    }
    Bytes longer = good;
    longer.push_back(0);
    CHECK_FALSE(jc::isTradeRecord(longer.data(), longer.size()));
    CHECK_FALSE(jc::isTradeRecord(good.data(), good.size() - 1));
    CHECK_FALSE(jc::isTradeRecord(nullptr, jc::TRADE_RECORD_LENGTH));
}

TEST_CASE("JournalTradeCodecTest.takerIsBuyIsHandedOverRaw")
{
    Bytes b = javaTrade();
    for (const std::uint8_t raw : { std::uint8_t{ 0 }, std::uint8_t{ 2 }, std::uint8_t{ 254 }, std::uint8_t{ 255 } })
    {
        b[8 + 84] = raw; // takerIsBuy: block offset 84 (schema 4 Trade)
        jc::TradeRecord t;
        REQUIRE(jc::decodeTrade(b.data(), b.size(), t));
        CHECK(t.takerIsBuyRaw == raw);
    }
}
