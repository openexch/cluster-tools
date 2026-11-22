#include "cluster_tools/adapters/sbe/JournalTradeCodec.h"

#include "com_match_infrastructure_mejournal_generated/MessageHeader.h"
#include "com_match_infrastructure_mejournal_generated/Trade.h"

namespace cluster_tools { namespace sbe_adapter { namespace journal_codec {

namespace jr = com::match::infrastructure::mejournal::generated;

static_assert(jr::MessageHeader::sbeSchemaId() == JOURNAL_SCHEMA_ID);
static_assert(jr::MessageHeader::sbeSchemaVersion() == JOURNAL_VERSION);
static_assert(jr::MessageHeader::encodedLength() == MESSAGE_HEADER_LENGTH);
static_assert(jr::Trade::sbeTemplateId() == TEMPLATE_TRADE);
static_assert(jr::Trade::sbeBlockLength() == TRADE_BLOCK_LENGTH);

namespace {

std::uint16_t le16(const std::uint8_t *p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

}

bool isTradeRecord(const std::uint8_t *record, std::size_t length) noexcept
{
    return nullptr != record && TRADE_RECORD_LENGTH == length && TRADE_BLOCK_LENGTH == le16(record) &&
        TEMPLATE_TRADE == le16(record + 2) && JOURNAL_SCHEMA_ID == le16(record + 4) && JOURNAL_VERSION == le16(record + 6);
}

void readTrade(const std::uint8_t *record, TradeRecord &out, TradeParties *parties) noexcept
{
    jr::Trade d;
    d.wrapForDecode(reinterpret_cast<char *>(const_cast<std::uint8_t *>(record)), MESSAGE_HEADER_LENGTH,
        jr::Trade::sbeBlockLength(), JOURNAL_VERSION, TRADE_RECORD_LENGTH);
    out.journalSeq = d.journalSeq();
    out.meEpoch = d.meEpoch();
    out.tradeId = d.tradeId();
    out.marketId = d.marketId();
    out.price = d.price();
    out.quantity = d.quantity();
    out.takerIsBuyRaw = d.takerIsBuyRaw();
    out.timestamp = d.timestamp();
    if (nullptr != parties)
    {
        parties->egressSeq = d.egressSeq();
        parties->takerOrderId = d.takerOrderId();
        parties->takerUserId = d.takerUserId();
        parties->makerOrderId = d.makerOrderId();
        parties->makerUserId = d.makerUserId();
        parties->takerOmsOrderId = d.takerOmsOrderId();
        parties->makerOmsOrderId = d.makerOmsOrderId();
    }
}

bool decodeTrade(const std::uint8_t *record, std::size_t length, TradeRecord &out, TradeParties *parties) noexcept
{
    if (!isTradeRecord(record, length))
    {
        return false;
    }
    readTrade(record, out, parties);
    return true;
}

}}}
