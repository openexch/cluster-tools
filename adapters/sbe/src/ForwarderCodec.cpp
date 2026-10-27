#include "cluster_tools/adapters/sbe/ForwarderCodec.h"

#include <cstring>
#include <exception>

#include "com_openexchange_assets_infrastructure_generated/AdmissionState.h"
#include "com_openexchange_assets_infrastructure_generated/MeDispatch.h"
#include "com_openexchange_assets_infrastructure_generated/MeDispatchBatch.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/OrderReject.h"
#include "com_openexchange_assets_infrastructure_generated/PendingDispatchEnd.h"
#include "com_openexchange_assets_infrastructure_generated/RequestPendingDispatch.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/DurableOrderCommand.h"
#include "com_match_infrastructure_generated/MessageHeader.h"
#include "com_match_infrastructure_generated/ReleaseCommandIds.h"

namespace cluster_tools { namespace sbe_adapter { namespace forwarder_codec {

namespace money = com::openexchange::assets::infrastructure::generated;
namespace me = com::match::infrastructure::generated;

// The schemas this codec was written against (contract 5.1/5.2). A schema change that moves a field fails
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
// Review NOTE-7: every 105 entry offset too (readEntry reads both forms at these offsets).
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

}}}
