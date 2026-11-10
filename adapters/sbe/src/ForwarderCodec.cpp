#include "cluster_tools/adapters/sbe/ForwarderCodec.h"

#include <cstring>
#include <exception>

#include "com_openexchange_core_infrastructure_generated/AdmissionState.h"
#include "com_openexchange_core_infrastructure_generated/MeDispatch.h"
#include "com_openexchange_core_infrastructure_generated/MarketTick.h"
#include "com_openexchange_core_infrastructure_generated/MeDispatchBatch.h"
#include "com_openexchange_core_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_core_infrastructure_generated/OrderReject.h"
#include "com_openexchange_core_infrastructure_generated/PendingDispatchEnd.h"
#include "com_openexchange_core_infrastructure_generated/RequestPendingDispatch.h"
#include "com_match_infrastructure_generated/BookDelta.h"
#include "com_match_infrastructure_generated/BookSnapshot.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/DurableOrderCommand.h"
#include "com_match_infrastructure_generated/MessageHeader.h"
#include "com_match_infrastructure_generated/OpenOrdersSnapshot.h"
#include "com_match_infrastructure_generated/OrderStatusBatch.h"
#include "com_match_infrastructure_generated/ReleaseCommandIds.h"
#include "com_match_infrastructure_generated/TradeExecutionBatch.h"
#include "com_match_infrastructure_generated/TradesBatch.h"

namespace cluster_tools { namespace sbe_adapter { namespace forwarder_codec {

namespace money = com::openexchange::core::infrastructure::generated;
namespace me = com::match::infrastructure::generated;

// The schemas this codec was written against. A schema change that moves a field fails
// the build here, not on the wire.
static_assert(money::MessageHeader::sbeSchemaId() == MONEY_SCHEMA_ID);
static_assert(money::MessageHeader::sbeSchemaVersion() == MONEY_VERSION);
static_assert(me::MessageHeader::sbeSchemaId() == MATCH_SCHEMA_ID);
static_assert(me::MessageHeader::sbeSchemaVersion() == MATCH_VERSION);
static_assert(money::MessageHeader::encodedLength() == MESSAGE_HEADER_LENGTH);
static_assert(money::MeDispatch::sbeTemplateId() == T_ME_DISPATCH);
static_assert(money::MeDispatchBatch::sbeTemplateId() == T_ME_DISPATCH_BATCH);
static_assert(money::PendingDispatchEnd::sbeTemplateId() == T_PENDING_DISPATCH_END);
static_assert(money::AdmissionState::sbeTemplateId() == T_ADMISSION_STATE);
static_assert(money::OrderReject::sbeTemplateId() == T_ORDER_REJECT);
static_assert(money::RequestPendingDispatch::sbeTemplateId() == T_REQUEST_PENDING_DISPATCH);
static_assert(me::DurableOrderCommand::sbeTemplateId() == T_DURABLE_ORDER_COMMAND);
static_assert(me::ReleaseCommandIds::sbeTemplateId() == T_RELEASE_COMMAND_IDS);
static_assert(me::ClusterHeartbeat::sbeTemplateId() == T_CLUSTER_HEARTBEAT);
static_assert(money::MeDispatch::sbeBlockLength() == DISPATCH_BLOCK_V7);
static_assert(money::MeDispatchBatch::Dispatches::sbeBlockLength() == DISPATCH_BLOCK_V7);
static_assert(money::MeDispatchBatch::sbeBlockLength() == 0);
static_assert(money::MeDispatchBatch::Dispatches::sbeHeaderSize() == 4);
static_assert(MESSAGE_HEADER_LENGTH + me::DurableOrderCommand::sbeBlockLength() == DURABLE_COMMAND_FRAME_LENGTH);
static_assert(MESSAGE_HEADER_LENGTH + money::RequestPendingDispatch::sbeBlockLength() == REQUEST_PENDING_FRAME_LENGTH);
static_assert(me::ReleaseCommandIds::Releases::sbeBlockLength() == RELEASE_ENTRY_LENGTH);
static_assert(MESSAGE_HEADER_LENGTH + me::ReleaseCommandIds::sbeBlockLength() + 4 == releaseFrameLength(0));

// 5.2 offsets of the 104 block (and of each 105 entry): the raw reads below use exactly these.
static_assert(money::MeDispatch::dispatchSeqEncodingOffset() == 0);
static_assert(money::MeDispatch::commandIdHighEncodingOffset() == 8);
static_assert(money::MeDispatch::commandIdLowEncodingOffset() == 16);
static_assert(money::MeDispatch::kindEncodingOffset() == 24);
static_assert(money::MeDispatch::userIdEncodingOffset() == 25);
static_assert(money::MeDispatch::omsOrderIdEncodingOffset() == 33);
static_assert(money::MeDispatch::oldOrderIdEncodingOffset() == 41);
static_assert(money::MeDispatch::marketIdEncodingOffset() == 49);
static_assert(money::MeDispatch::orderTypeEncodingOffset() == 53);
static_assert(money::MeDispatch::sideEncodingOffset() == 54);
static_assert(money::MeDispatch::priceEncodingOffset() == 55);
static_assert(money::MeDispatch::quantityEncodingOffset() == 63);
static_assert(money::MeDispatch::budgetEncodingOffset() == 71);
static_assert(money::MeDispatch::timeInForceEncodingOffset() == 79);
static_assert(money::MeDispatch::releaseScopeEncodingOffset() == 80);
static_assert(money::MeDispatch::expectedOldRemainingEncodingOffset() == 81);
// Every 105 entry offset too (readEntry reads both forms at these offsets).
static_assert(money::MeDispatchBatch::Dispatches::dispatchSeqEncodingOffset() == 0);
static_assert(money::MeDispatchBatch::Dispatches::commandIdHighEncodingOffset() == 8);
static_assert(money::MeDispatchBatch::Dispatches::commandIdLowEncodingOffset() == 16);
static_assert(money::MeDispatchBatch::Dispatches::kindEncodingOffset() == 24);
static_assert(money::MeDispatchBatch::Dispatches::userIdEncodingOffset() == 25);
static_assert(money::MeDispatchBatch::Dispatches::omsOrderIdEncodingOffset() == 33);
static_assert(money::MeDispatchBatch::Dispatches::oldOrderIdEncodingOffset() == 41);
static_assert(money::MeDispatchBatch::Dispatches::marketIdEncodingOffset() == 49);
static_assert(money::MeDispatchBatch::Dispatches::orderTypeEncodingOffset() == 53);
static_assert(money::MeDispatchBatch::Dispatches::sideEncodingOffset() == 54);
static_assert(money::MeDispatchBatch::Dispatches::priceEncodingOffset() == 55);
static_assert(money::MeDispatchBatch::Dispatches::quantityEncodingOffset() == 63);
static_assert(money::MeDispatchBatch::Dispatches::budgetEncodingOffset() == 71);
static_assert(money::MeDispatchBatch::Dispatches::timeInForceEncodingOffset() == 79);
static_assert(money::MeDispatchBatch::Dispatches::releaseScopeEncodingOffset() == 80);
static_assert(money::MeDispatchBatch::Dispatches::expectedOldRemainingEncodingOffset() == 81);

// The book egress layouts the raw reads below use. A schema change that moves a field
// fails the build here, not on the wire. (CodecLayoutTest pins the same numbers from the test side.)
static_assert(me::BookSnapshot::sbeTemplateId() == T_BOOK_SNAPSHOT);
static_assert(me::BookDelta::sbeTemplateId() == T_BOOK_DELTA);
static_assert(me::TradeExecutionBatch::sbeTemplateId() == T_TRADE_EXECUTION_BATCH);
static_assert(me::TradesBatch::sbeTemplateId() == T_TRADES_BATCH);
static_assert(me::OrderStatusBatch::sbeTemplateId() == T_ORDER_STATUS_BATCH);
static_assert(me::OpenOrdersSnapshot::sbeTemplateId() == T_OPEN_ORDERS_SNAPSHOT);
static_assert(money::MarketTick::sbeTemplateId() == T_MARKET_TICK);
static_assert(me::BookSnapshot::sbeBlockLength() == domain::forwarder::SNAPSHOT_BLOCK_MIN);
static_assert(me::BookDelta::sbeBlockLength() == domain::forwarder::DELTA_BLOCK_MIN);
static_assert(me::TradeExecutionBatch::sbeBlockLength() == domain::forwarder::TRADE_BLOCK_MIN);
static_assert(me::BookSnapshot::Bids::sbeBlockLength() == domain::forwarder::SNAPSHOT_ENTRY_MIN);
static_assert(me::BookSnapshot::Asks::sbeBlockLength() == domain::forwarder::SNAPSHOT_ENTRY_MIN);
static_assert(me::BookDelta::Changes::sbeBlockLength() == domain::forwarder::DELTA_ENTRY_MIN);
static_assert(me::TradeExecutionBatch::Trades::sbeBlockLength() == domain::forwarder::TRADE_ENTRY_MIN);
static_assert(me::BookSnapshot::Bids::sbeHeaderSize() == 4);
static_assert(me::BookSnapshot::Asks::sbeHeaderSize() == 4);
static_assert(me::BookDelta::Changes::sbeHeaderSize() == 4);
static_assert(me::TradeExecutionBatch::Trades::sbeHeaderSize() == 4);
static_assert(me::BookSnapshot::marketIdEncodingOffset() == 0);
static_assert(me::BookSnapshot::bookVersionEncodingOffset() == 28);
static_assert(me::BookSnapshot::Bids::priceEncodingOffset() == 0);
static_assert(me::BookSnapshot::Bids::quantityEncodingOffset() == 8);
static_assert(me::BookSnapshot::Bids::orderCountEncodingOffset() == 16);
static_assert(me::BookSnapshot::Asks::priceEncodingOffset() == 0);
static_assert(me::BookSnapshot::Asks::quantityEncodingOffset() == 8);
static_assert(me::BookSnapshot::Asks::orderCountEncodingOffset() == 16);
static_assert(me::BookDelta::marketIdEncodingOffset() == 0);
static_assert(me::BookDelta::bookVersionEncodingOffset() == 28);
static_assert(me::BookDelta::fromVersionEncodingOffset() == 36);
static_assert(me::BookDelta::Changes::priceEncodingOffset() == 0);
static_assert(me::BookDelta::Changes::quantityEncodingOffset() == 8);
static_assert(me::BookDelta::Changes::orderCountEncodingOffset() == 16);
static_assert(me::BookDelta::Changes::sideEncodingOffset() == 20);
static_assert(me::BookDelta::Changes::updateTypeEncodingOffset() == 21);
static_assert(me::TradeExecutionBatch::marketIdEncodingOffset() == 0);
static_assert(me::TradeExecutionBatch::Trades::tradeIdEncodingOffset() == 0);
static_assert(me::TradeExecutionBatch::Trades::priceEncodingOffset() == 40);
static_assert(MESSAGE_HEADER_LENGTH + money::MarketTick::sbeBlockLength() == MARKET_TICK_FRAME_LENGTH);
static_assert(money::MarketTick::marketIdEncodingOffset() == 0);
static_assert(money::MarketTick::bookVersionEncodingOffset() == 4);
static_assert(money::MarketTick::bestBidEncodingOffset() == 12);
static_assert(money::MarketTick::bestAskEncodingOffset() == 20);
static_assert(money::MarketTick::lastTradePriceEncodingOffset() == 28);
static_assert(money::MarketTick::lastTradeIdEncodingOffset() == 36);
static_assert(money::MarketTick::meEpochEncodingOffset() == 44);

namespace {

template<typename T>
T read(const std::uint8_t *p) noexcept
{
    T v;
    std::memcpy(&v, p, sizeof(T)); // SBE is little endian; so are both deployment targets (x86-64, aarch64)
    return v;
}

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "raw SBE reads assume a little-endian host");

void readEntry(const std::uint8_t *b, Dispatch &d) noexcept
{
    d.dispatchSeq = read<std::int64_t>(b + 0);
    d.commandIdHigh = read<std::int64_t>(b + 8);
    d.commandIdLow = read<std::int64_t>(b + 16);
    d.kind = b[24];
    d.userId = read<std::int64_t>(b + 25);
    d.omsOrderId = read<std::int64_t>(b + 33);
    d.oldOrderId = read<std::int64_t>(b + 41);
    d.marketId = read<std::int32_t>(b + 49);
    d.orderType = b[53];
    d.side = b[54];
    d.price = read<std::int64_t>(b + 55);
    d.quantity = read<std::int64_t>(b + 63);
    d.budget = read<std::int64_t>(b + 71);
    d.timeInForce = b[79];
    d.releaseScope = b[80];
    d.expectedOldRemaining = read<std::int64_t>(b + 81);
}

}

bool readHeader(const std::uint8_t *frame, std::size_t length, Header &header) noexcept
{
    if (nullptr == frame || length < MESSAGE_HEADER_LENGTH)
    {
        return false;
    }
    header.blockLength = read<std::uint16_t>(frame + 0);
    header.templateId = read<std::uint16_t>(frame + 2);
    header.schemaId = read<std::uint16_t>(frame + 4);
    header.version = read<std::uint16_t>(frame + 6);
    return true;
}

bool decodeDispatch(const std::uint8_t *frame, std::size_t length, const Header &header, Dispatch &out,
    bool *seqBelowOne) noexcept
{
    if (nullptr != seqBelowOne)
    {
        *seqBelowOne = false;
    }
    if (T_ME_DISPATCH != header.templateId || header.blockLength < DISPATCH_BLOCK_V7 ||
        length < MESSAGE_HEADER_LENGTH + header.blockLength)
    {
        return false;
    }
    readEntry(frame + MESSAGE_HEADER_LENGTH, out);
    if (out.dispatchSeq < 1)
    {
        if (nullptr != seqBelowOne)
        {
            *seqBelowOne = true;
        }
        return false;
    }
    return true;
}

bool decodeDispatchBatch(const std::uint8_t *frame, std::size_t length, const Header &header, DispatchSink sink,
    void *context, bool *seqBelowOne) noexcept
{
    if (nullptr != seqBelowOne)
    {
        *seqBelowOne = false;
    }
    if (T_ME_DISPATCH_BATCH != header.templateId || nullptr == sink)
    {
        return false;
    }
    const std::size_t groupOffset = MESSAGE_HEADER_LENGTH + header.blockLength;
    if (length < groupOffset + 4)
    {
        return false;
    }
    const std::size_t entryBlock = read<std::uint16_t>(frame + groupOffset);
    const std::size_t count = read<std::uint16_t>(frame + groupOffset + 2);
    if (entryBlock < DISPATCH_BLOCK_V7 || entryBlock * count > length - groupOffset - 4)
    {
        return false;
    }
    const std::uint8_t *entry = frame + groupOffset + 4;
    for (std::size_t i = 0; i < count; i++)
    {
        if (read<std::int64_t>(entry + i * entryBlock) < 1) // no partial send: refused before any entry is delivered
        {
            if (nullptr != seqBelowOne)
            {
                *seqBelowOne = true;
            }
            return false;
        }
    }
    Dispatch d;
    for (std::size_t i = 0; i < count; i++)
    {
        readEntry(entry, d);
        sink(context, d);
        entry += entryBlock;
    }
    return true;
}

bool decodePendingEnd(const std::uint8_t *frame, std::size_t length, const Header &header, PendingEnd &out) noexcept
{
    static_assert(money::PendingDispatchEnd::correlationIdEncodingOffset() == 0);
    static_assert(money::PendingDispatchEnd::countEncodingOffset() == 8);
    static_assert(money::PendingDispatchEnd::headDispatchSeqEncodingOffset() == 12);
    if (T_PENDING_DISPATCH_END != header.templateId || header.blockLength < 20 ||
        length < MESSAGE_HEADER_LENGTH + header.blockLength)
    {
        return false;
    }
    const std::uint8_t *b = frame + MESSAGE_HEADER_LENGTH;
    out.correlationId = read<std::int64_t>(b);
    out.count = read<std::int32_t>(b + 8);
    out.headDispatchSeq = read<std::int64_t>(b + 12);
    return true;
}

bool decodeAdmission(const std::uint8_t *frame, std::size_t length, const Header &header, Admission &out) noexcept
{
    static_assert(money::AdmissionState::marketIdEncodingOffset() == 0);
    static_assert(money::AdmissionState::gateStateEncodingOffset() == 4);
    static_assert(money::AdmissionState::gateReasonsEncodingOffset() == 23);
    static_assert(money::AdmissionState::sbeBlockLength() == 27);
    if (T_ADMISSION_STATE != header.templateId || header.blockLength < 27 ||
        length < MESSAGE_HEADER_LENGTH + header.blockLength)
    {
        return false;
    }
    const std::uint8_t *b = frame + MESSAGE_HEADER_LENGTH;
    out.marketId = read<std::int32_t>(b);
    out.gateState = b[4];
    out.gateReasons = read<std::uint32_t>(b + 23);
    return true;
}

bool decodeOrderReject(const std::uint8_t *frame, std::size_t length, const Header &header, OrderRejectView &out) noexcept
{
    static_assert(money::OrderReject::correlationIdEncodingOffset() == 0);
    static_assert(money::OrderReject::refTemplateIdEncodingOffset() == 44);
    static_assert(money::OrderReject::reasonEncodingOffset() == 46);
    if (T_ORDER_REJECT != header.templateId || header.blockLength < 47 ||
        length < MESSAGE_HEADER_LENGTH + header.blockLength)
    {
        return false;
    }
    const std::uint8_t *b = frame + MESSAGE_HEADER_LENGTH;
    out.correlationId = read<std::int64_t>(b);
    out.refTemplateId = read<std::uint16_t>(b + 44);
    out.reason = b[46];
    return true;
}

std::size_t encodeRequestPendingDispatch(std::uint8_t *out, std::size_t capacity, const PendingRequest &request) noexcept
{
    if (nullptr == out || capacity < REQUEST_PENDING_FRAME_LENGTH)
    {
        return 0;
    }
    try
    {
        money::RequestPendingDispatch e;
        e.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity)
            .correlationId(request.correlationId)
            .fromDispatchSeq(request.fromDispatchSeq)
            .maxEntries(request.maxEntries);
        return MESSAGE_HEADER_LENGTH + e.encodedLength();
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

bool decodeHeartbeatEpoch(const std::uint8_t *frame, std::size_t length, const Header &header, std::int64_t &meEpoch) noexcept
{
    static_assert(me::ClusterHeartbeat::meEpochEncodingOffset() == 9);
    if (T_CLUSTER_HEARTBEAT != header.templateId || header.blockLength < 17 ||
        length < MESSAGE_HEADER_LENGTH + header.blockLength)
    {
        return false;
    }
    meEpoch = read<std::int64_t>(frame + MESSAGE_HEADER_LENGTH + 9);
    return true;
}

std::size_t encodeDurableCommand(std::uint8_t *out, std::size_t capacity, const DurableCommand &c) noexcept
{
    if (nullptr == out || capacity < DURABLE_COMMAND_FRAME_LENGTH)
    {
        return 0;
    }
    try
    {
        me::DurableOrderCommand e;
        e.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity)
            .commandIdHigh(c.commandIdHigh)
            .commandIdLow(c.commandIdLow)
            .userId(c.userId)
            .omsOrderId(c.omsOrderId)
            .oldOrderId(c.oldOrderId)
            .price(c.price)
            .quantity(c.quantity)
            .budget(c.budget)
            .marketId(c.marketId)
            .commandKind(c.commandKind)
            .orderType(c.orderType)
            .orderSide(c.orderSide)
            .timeInForce(c.timeInForce)
            .expectedOldRemaining(c.expectedOldRemaining);
        return MESSAGE_HEADER_LENGTH + e.encodedLength();
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

std::size_t encodeReleaseCommandIds(std::uint8_t *out, std::size_t capacity, std::int64_t releaseSeq,
    const ReleaseEntry *entries, std::uint32_t n) noexcept
{
    if (nullptr == out || nullptr == entries || 0 == n || n > 0xFFFF || capacity < releaseFrameLength(n))
    {
        return 0;
    }
    try
    {
        me::ReleaseCommandIds e;
        e.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity).releaseSeq(releaseSeq);
        auto &group = e.releasesCount(static_cast<std::uint16_t>(n));
        for (std::uint32_t i = 0; i < n; i++)
        {
            // scope is written RAW: an RX entry (scope 2) travels exactly as the core minted it (5.4)
            group.next().commandIdHigh(entries[i].commandIdHigh).idLowFloor(entries[i].idLowFloor);
            std::memcpy(out + MESSAGE_HEADER_LENGTH + me::ReleaseCommandIds::sbeBlockLength() + 4 +
                    i * RELEASE_ENTRY_LENGTH + me::ReleaseCommandIds::Releases::scopeEncodingOffset(),
                &entries[i].scope, 1);
        }
        return MESSAGE_HEADER_LENGTH + e.encodedLength();
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

namespace {

using domain::forwarder::BookLevel;
using domain::forwarder::LADDER_LEVELS;
using domain::forwarder::DELTA_MAX_CHANGES;
using domain::forwarder::Malformed;

void resetBook(BookFrame &out, bool snapshot) noexcept
{
    // scalars only: the entry arrays are delimited by nBids / nAsks / nChanges, never read past them
    out.snapshot = snapshot;
    out.marketReadable = false;
    out.lengthOk = false;
    out.malformed = Malformed::NONE;
    out.marketId = 0;
    out.bookVersion = 0;
    out.fromVersion = 0;
    out.headerBlockLength = 0;
    out.groupBlockLength[0] = out.groupBlockLength[1] = 0;
    out.statedCount[0] = out.statedCount[1] = 0;
    out.nBids = out.nAsks = out.nChanges = 0;
}

void readMarket(const std::uint8_t *frame, std::size_t length, const Header &header, std::int32_t &market, bool &readable) noexcept
{
    // marketId is the first field (offset 0, 4 bytes) of 22, 25 and 26
    if (header.blockLength >= 4 && length >= MESSAGE_HEADER_LENGTH + 4)
    {
        market = read<std::int32_t>(frame + MESSAGE_HEADER_LENGTH);
        readable = true;
    }
}

// Reads one group header at `at` and checks that `count` entries of the STATED block length fit. False = the group
// header is missing or the entries do not fit (the frame is short). A stated block below `entryMin` or a count above
// `cap` is not a "short frame": the caller sees the raw numbers and the validator names the rule; no entry is read.
bool groupHeader(const std::uint8_t *frame, std::size_t length, std::size_t at, std::uint16_t &block, std::uint16_t &count) noexcept
{
    if (length < at + 4 || at + 4 < at)
    {
        return false;
    }
    block = read<std::uint16_t>(frame + at);
    count = read<std::uint16_t>(frame + at + 2);
    return true;
}

bool entriesFit(std::size_t length, std::size_t first, std::uint16_t block, std::uint16_t count) noexcept
{
    return first <= length && static_cast<std::size_t>(block) * count <= length - first;
}

}

bool decodeBookSnapshot(const std::uint8_t *frame, std::size_t length, const Header &header, BookFrame &out) noexcept
{
    resetBook(out, true);
    if (T_BOOK_SNAPSHOT != header.templateId)
    {
        out.malformed = Malformed::TEMPLATE;
        return false;
    }
    out.headerBlockLength = header.blockLength;
    readMarket(frame, length, header, out.marketId, out.marketReadable);
    if (header.blockLength >= domain::forwarder::SNAPSHOT_BLOCK_MIN && length >= MESSAGE_HEADER_LENGTH + domain::forwarder::SNAPSHOT_BLOCK_MIN)
    {
        out.bookVersion = read<std::int64_t>(frame + MESSAGE_HEADER_LENGTH + 28);
    }
    std::size_t at = MESSAGE_HEADER_LENGTH + header.blockLength;
    bool fits = length >= at;
    BookLevel *sides[2] = { out.bids, out.asks };
    std::uint16_t *stored[2] = { &out.nBids, &out.nAsks };
    for (unsigned g = 0; g < 2 && fits; g++)
    {
        fits = groupHeader(frame, length, at, out.groupBlockLength[g], out.statedCount[g]);
        if (!fits)
        {
            break;
        }
        at += 4;
        const std::uint16_t block = out.groupBlockLength[g];
        const std::uint16_t count = out.statedCount[g];
        fits = entriesFit(length, at, block, count);
        if (fits && block >= domain::forwarder::SNAPSHOT_ENTRY_MIN && count <= LADDER_LEVELS)
        {
            for (std::uint16_t i = 0; i < count; i++)
            {
                const std::uint8_t *e = frame + at + static_cast<std::size_t>(i) * block;
                sides[g][i].price = read<std::int64_t>(e + 0);
                sides[g][i].quantity = read<std::int64_t>(e + 8);
                sides[g][i].orderCount = read<std::int32_t>(e + 16);
            }
            *stored[g] = count;
        }
        at += static_cast<std::size_t>(block) * count; // only reached when the entries fit
    }
    out.lengthOk = fits;
    out.malformed = domain::forwarder::validateBookFrame(out);
    return Malformed::NONE == out.malformed;
}

bool decodeBookDelta(const std::uint8_t *frame, std::size_t length, const Header &header, BookFrame &out) noexcept
{
    resetBook(out, false);
    if (T_BOOK_DELTA != header.templateId)
    {
        out.malformed = Malformed::TEMPLATE;
        return false;
    }
    out.headerBlockLength = header.blockLength;
    readMarket(frame, length, header, out.marketId, out.marketReadable);
    if (header.blockLength >= domain::forwarder::DELTA_BLOCK_MIN && length >= MESSAGE_HEADER_LENGTH + domain::forwarder::DELTA_BLOCK_MIN)
    {
        out.bookVersion = read<std::int64_t>(frame + MESSAGE_HEADER_LENGTH + 28);
        out.fromVersion = read<std::int64_t>(frame + MESSAGE_HEADER_LENGTH + 36);
    }
    std::size_t at = MESSAGE_HEADER_LENGTH + header.blockLength;
    bool fits = length >= at && groupHeader(frame, length, at, out.groupBlockLength[0], out.statedCount[0]);
    if (fits)
    {
        at += 4;
        const std::uint16_t block = out.groupBlockLength[0];
        const std::uint16_t count = out.statedCount[0];
        fits = entriesFit(length, at, block, count);
        if (fits && block >= domain::forwarder::DELTA_ENTRY_MIN && count <= DELTA_MAX_CHANGES)
        {
            for (std::uint16_t i = 0; i < count; i++)
            {
                const std::uint8_t *e = frame + at + static_cast<std::size_t>(i) * block;
                out.changes[i].price = read<std::int64_t>(e + 0);
                out.changes[i].quantity = read<std::int64_t>(e + 8);
                out.changes[i].orderCount = read<std::int32_t>(e + 16);
                out.changes[i].side = e[20];       // raw: no generated enum getter (it throws on an unknown value)
                out.changes[i].updateType = e[21]; // raw
            }
            out.nChanges = count;
        }
    }
    out.lengthOk = fits;
    out.malformed = domain::forwarder::validateBookFrame(out);
    return Malformed::NONE == out.malformed;
}

bool decodeTradeBatch(const std::uint8_t *frame, std::size_t length, const Header &header, TradeFrame &out) noexcept
{
    out.marketReadable = false;
    out.lengthOk = false;
    out.entryInvalid = false;
    out.malformed = Malformed::NONE;
    out.marketId = 0;
    out.headerBlockLength = header.blockLength;
    out.entryBlockLength = 0;
    out.statedCount = 0;
    out.maxTradeId = 0;
    out.priceAtMax = 0;
    if (T_TRADE_EXECUTION_BATCH != header.templateId)
    {
        out.malformed = Malformed::TEMPLATE;
        return false;
    }
    readMarket(frame, length, header, out.marketId, out.marketReadable);
    std::size_t at = MESSAGE_HEADER_LENGTH + header.blockLength;
    bool fits = length >= at && groupHeader(frame, length, at, out.entryBlockLength, out.statedCount);
    if (fits)
    {
        at += 4;
        fits = entriesFit(length, at, out.entryBlockLength, out.statedCount);
        if (fits && out.entryBlockLength >= domain::forwarder::TRADE_ENTRY_MIN)
        {
            for (std::uint16_t i = 0; i < out.statedCount; i++)
            {
                const std::uint8_t *e = frame + at + static_cast<std::size_t>(i) * out.entryBlockLength;
                const std::int64_t tradeId = read<std::int64_t>(e + 0);
                const std::int64_t price = read<std::int64_t>(e + 40);
                if (tradeId <= 0 || price <= 0)
                {
                    out.entryInvalid = true;
                }
                if (tradeId > out.maxTradeId)
                {
                    out.maxTradeId = tradeId;
                    out.priceAtMax = price;
                }
            }
        }
    }
    out.lengthOk = fits;
    out.malformed = domain::forwarder::validateTradeFrame(out);
    if (Malformed::NONE != out.malformed)
    {
        out.maxTradeId = 0; // dropped whole: nothing of it is used
        out.priceAtMax = 0;
        return false;
    }
    return true;
}

std::size_t encodeMarketTick(std::uint8_t *out, std::size_t capacity, const MarketTick &t) noexcept
{
    if (nullptr == out || capacity < MARKET_TICK_FRAME_LENGTH)
    {
        return 0;
    }
    try
    {
        money::MarketTick e;
        e.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity)
            .marketId(t.marketId)
            .bookVersion(t.bookVersion)
            .bestBid(t.bestBid)
            .bestAsk(t.bestAsk)
            .lastTradePrice(t.lastTradePrice)
            .lastTradeId(t.lastTradeId)
            .meEpoch(t.meEpoch);
        return MESSAGE_HEADER_LENGTH + e.encodedLength();
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

}}}
