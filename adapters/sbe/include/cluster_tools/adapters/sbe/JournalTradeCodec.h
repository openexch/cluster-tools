#ifndef CLUSTER_TOOLS_ADAPTERS_SBE_JOURNAL_TRADE_CODEC_H
#define CLUSTER_TOOLS_ADAPTERS_SBE_JOURNAL_TRADE_CODEC_H

// The decode of ME journal record 1 (Trade, schema id 4, header version exactly 1, block 109, frame 117), the one
// copy of it: the settlement bridge reads its rows through readTrade, and a reader of the public tape (market data)
// through decodeTrade. The generated journal codec is used inside the translation unit only; this header pulls in
// no generated code.
//
// The record splits in two: TradeRecord holds the public fields of a trade; TradeParties holds the order and user
// ids and the egress position. The parties are read only when the caller passes somewhere to put them, so a reader
// that must never read them (the public tape) passes nothing and those bytes are never touched.
//
// takerIsBuy is the raw byte of the schema's BooleanType (0 FALSE, 1 TRUE; any other byte is handed over as is):
// no generated enum getter, which throws on an unknown value. Nothing here throws, allocates or keeps state.

#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace sbe_adapter { namespace journal_codec {

inline constexpr std::uint16_t JOURNAL_SCHEMA_ID = 4;
inline constexpr std::uint16_t JOURNAL_VERSION = 1;
inline constexpr std::uint16_t TEMPLATE_TRADE = 1;
inline constexpr std::size_t MESSAGE_HEADER_LENGTH = 8;
inline constexpr std::uint16_t TRADE_BLOCK_LENGTH = 109;
inline constexpr std::size_t TRADE_RECORD_LENGTH = MESSAGE_HEADER_LENGTH + TRADE_BLOCK_LENGTH; // 117

struct TradeRecord
{
    std::int64_t journalSeq = 0;
    std::int64_t meEpoch = 0;
    std::int64_t tradeId = 0;
    std::int32_t marketId = 0;
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::uint8_t takerIsBuyRaw = 0;
    std::int64_t timestamp = 0; // the ME's epoch milliseconds
};

struct TradeParties
{
    std::int64_t egressSeq = 0;
    std::int64_t takerOrderId = 0;
    std::int64_t takerUserId = 0;
    std::int64_t makerOrderId = 0;
    std::int64_t makerUserId = 0;
    std::int64_t takerOmsOrderId = 0;
    std::int64_t makerOmsOrderId = 0;
};

// True when `record` is a whole record 1 of this schema: the header names schema 4, version 1, template 1 and
// block 109, and the length is exactly 117.
bool isTradeRecord(const std::uint8_t *record, std::size_t length) noexcept;

// Reads a record the caller has already admitted as a record 1 (isTradeRecord, or the archive frame gate and an
// exact length): `record` points at the 8-byte header of 117 readable bytes. No check of its own.
void readTrade(const std::uint8_t *record, TradeRecord &out, TradeParties *parties = nullptr) noexcept;

// isTradeRecord, then readTrade. False = not a record 1 (out and *parties are left untouched).
bool decodeTrade(const std::uint8_t *record, std::size_t length, TradeRecord &out,
    TradeParties *parties = nullptr) noexcept;

}}}

#endif
