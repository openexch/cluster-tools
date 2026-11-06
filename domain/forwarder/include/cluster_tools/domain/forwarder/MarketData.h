#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_MARKET_DATA_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_MARKET_DATA_H

#include <cstddef>
#include <cstdint>

// MarketData - the value types of the K4 MarketTick producer (RC2 S13 contract section 2) and the one definition of
// "malformed" for the ME book egress (contract 4.2, C7). Pure: no SBE, no Aeron, no allocation.
//
// The codec (adapters/sbe ForwarderCodec) fills a BookFrame / TradeFrame from the wire with RAW reads: every enum
// is a raw byte, the group block lengths and counts are the STATED ones, and `lengthOk` says whether the stated
// groups fit the frame. validateBookFrame / validateTradeFrame then decide malformed-or-not from those facts alone, so
// the ladder (W2) can call the same rule on a frame it was handed without bytes. A malformed frame still carries its
// market when that was readable (the ladder of that market becomes BROKEN, 4.2).

namespace cluster_tools { namespace domain { namespace forwarder {

inline constexpr int TICK_MARKETS = 5;               // markets 1..5: UserRisk.java:11 MARKET_SLOTS - 1 = 5 (the money schema does not carry the count, D-S13-9); K4 ladders
inline constexpr int LADDER_LEVELS = 20;             // MarketPublisher TOP_LEVELS (MP:34)
inline constexpr int LADDER_SLOTS = 40;              // 4.1 rule 2: NEW before DELETE holds 21+ levels mid frame
inline constexpr int DELTA_MAX_CHANGES = 80;         // MAX_BOOK_LEVELS * 4 (MP:835)
inline constexpr std::int64_t T_CAP_NS = 100'000'000; // critic D14 (RC1): leading plus trailing edge per market

// order-schema enums, RAW (OS:21-24, OS:37-41)
inline constexpr std::uint8_t BOOK_SIDE_BID = 0;
inline constexpr std::uint8_t BOOK_SIDE_ASK = 1;
inline constexpr std::uint8_t BOOK_SIDE_MAX = 1;     // a raw side above this is malformed
inline constexpr std::uint8_t BOOK_UPDATE_NEW = 0;
inline constexpr std::uint8_t BOOK_UPDATE_UPDATE = 1;
inline constexpr std::uint8_t BOOK_UPDATE_DELETE = 2;
inline constexpr std::uint8_t BOOK_UPDATE_MAX = 2;   // a raw updateType above this is malformed

// Smallest acceptable stated sizes (contract 4.2; pinned by CodecLayoutTest against the generated codecs).
inline constexpr std::uint16_t SNAPSHOT_BLOCK_MIN = 36;
inline constexpr std::uint16_t DELTA_BLOCK_MIN = 44;
inline constexpr std::uint16_t TRADE_BLOCK_MIN = 12;
inline constexpr std::uint16_t SNAPSHOT_ENTRY_MIN = 20;
inline constexpr std::uint16_t DELTA_ENTRY_MIN = 22;
inline constexpr std::uint16_t TRADE_ENTRY_MIN = 89;

enum class Malformed : std::uint8_t
{
    NONE = 0,
    TEMPLATE,      // the frame is not the template the decoder was asked for
    HEADER_BLOCK,  // header blockLength below the template's
    GROUP_BLOCK,   // a group header blockLength below the entry size
    COUNT,         // a group count above 20 (22), 80 (25)
    LENGTH,        // the frame is shorter than header + block + groups
    VERSION,       // bookVersion / fromVersion rules
    ENUM,          // raw side above 1, raw updateType above 2
    VALUE          // price / qty / count / tradeId out of range
};

const char *malformedName(Malformed m) noexcept;

struct BookLevel
{
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::int32_t orderCount = 0;
};

struct BookChange
{
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::int32_t orderCount = 0;
    std::uint8_t side = 0;        // raw
    std::uint8_t updateType = 0;  // raw
};

// A decoded 22 BookSnapshot (snapshot == true) or 25 BookDelta (snapshot == false). Stored levels are bounded: a stated
// count above the cap is kept in statedCount, the entries are not copied (the frame is malformed).
struct BookFrame
{
    bool snapshot = false;
    bool marketReadable = false;
    bool lengthOk = false;                 // header + block + every stated group fits the frame
    Malformed malformed = Malformed::NONE; // set by the codec after validateBookFrame
    std::int32_t marketId = 0;
    std::int64_t bookVersion = 0;
    std::int64_t fromVersion = 0;          // 25 only
    std::uint16_t headerBlockLength = 0;   // as stated in the message header
    std::uint16_t groupBlockLength[2] = { 0, 0 }; // 22: bids, asks; 25: changes
    std::uint16_t statedCount[2] = { 0, 0 };
    std::uint16_t nBids = 0;               // stored entries (22)
    std::uint16_t nAsks = 0;
    std::uint16_t nChanges = 0;            // stored entries (25)
    BookLevel bids[LADDER_LEVELS];
    BookLevel asks[LADDER_LEVELS];
    BookChange changes[DELTA_MAX_CHANGES];
};

// A decoded 26 TradeExecutionBatch reduced to what K4 needs: the highest tradeId and its price (section 5).
struct TradeFrame
{
    bool marketReadable = false;
    bool lengthOk = false;
    bool entryInvalid = false;             // some entry has tradeId <= 0 or price <= 0
    Malformed malformed = Malformed::NONE;
    std::int32_t marketId = 0;
    std::uint16_t headerBlockLength = 0;
    std::uint16_t entryBlockLength = 0;
    std::uint16_t statedCount = 0;
    std::int64_t maxTradeId = 0;           // 0 when the batch holds no trade
    std::int64_t priceAtMax = 0;
};

// MarketTick 60 (money-schema), the fields of the wire frame.
struct MarketTick
{
    std::int32_t marketId = 0;
    std::int64_t bookVersion = 0;
    std::int64_t bestBid = 0;
    std::int64_t bestAsk = 0;
    std::int64_t lastTradePrice = 0;
    std::int64_t lastTradeId = 0;
    std::int64_t meEpoch = 0;
};

// The one rule of 4.2. NONE = well formed. A 22 with bookVersion 0 and two empty groups is well formed here (rule U is
// the ladder's decision); with a non-empty group it is VERSION.
Malformed validateBookFrame(const BookFrame &frame) noexcept;
// 26: a malformed batch is dropped whole (nothing of it is used).
Malformed validateTradeFrame(const TradeFrame &frame) noexcept;

}}}

#endif
