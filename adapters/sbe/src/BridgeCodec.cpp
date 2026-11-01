#include "cluster_tools/adapters/sbe/BridgeCodec.h"

#include "cluster_tools/archive/FramePeek.h"
#include "cluster_tools/domain/bridge/TestSeams.h"

#include <cstring>

#include "com_match_infrastructure_mejournal_generated/CommandOutcome.h"
#include "com_match_infrastructure_mejournal_generated/CommandRelease.h"
#include "com_match_infrastructure_mejournal_generated/EngineConfig.h"
#include "com_match_infrastructure_mejournal_generated/MessageHeader.h"
#include "com_match_infrastructure_mejournal_generated/Terminal.h"
#include "com_match_infrastructure_mejournal_generated/Trade.h"
#include "com_openexchange_assets_infrastructure_generated/AdminAck.h"
#include "com_openexchange_assets_infrastructure_generated/FeedHalt.h"
#include "com_openexchange_assets_infrastructure_generated/FeedPositionReport.h"
#include "com_openexchange_assets_infrastructure_generated/MeCommandOutcome.h"
#include "com_openexchange_assets_infrastructure_generated/MeCommandRelease.h"
#include "com_openexchange_assets_infrastructure_generated/MeEngineConfig.h"
#include "com_openexchange_assets_infrastructure_generated/MeJournalTerminal.h"
#include "com_openexchange_assets_infrastructure_generated/MeJournalTrade.h"
#include "com_openexchange_assets_infrastructure_generated/MeJournalTradeBatch.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/OrderReject.h"
#include "com_openexchange_assets_infrastructure_generated/QueryFeedPosition.h"
#include "com_openexchange_assets_infrastructure_generated/SettleRejected.h"
#include "com_openexchange_assets_infrastructure_generated/SettlementApplied.h"

namespace cluster_tools { namespace sbe_adapter { namespace bridge_codec {

namespace money = com::openexchange::assets::infrastructure::generated;
namespace jr = com::match::infrastructure::mejournal::generated;
namespace bridge = domain::bridge;
using bridge::test_seams::Rule;
using bridge::test_seams::disabled;

// ------------------------------------------------------------------ the pins (S9a 6.2, the static-assert unit)

static_assert(money::MessageHeader::sbeSchemaId() == MONEY_SCHEMA_ID);
static_assert(money::MessageHeader::sbeSchemaVersion() == MONEY_VERSION);
static_assert(jr::MessageHeader::sbeSchemaId() == JOURNAL_SCHEMA_ID);
static_assert(jr::MessageHeader::sbeSchemaVersion() == JOURNAL_VERSION);
static_assert(money::MessageHeader::encodedLength() == MESSAGE_HEADER_LENGTH);
static_assert(jr::MessageHeader::encodedLength() == archive::MESSAGE_HEADER_LENGTH);

static_assert(money::MeJournalTrade::sbeTemplateId() == bridge::T_ME_JOURNAL_TRADE);
static_assert(money::MeJournalTradeBatch::sbeTemplateId() == bridge::T_ME_JOURNAL_TRADE_BATCH);
static_assert(money::MeJournalTerminal::sbeTemplateId() == bridge::T_ME_JOURNAL_TERMINAL);
static_assert(money::MeCommandOutcome::sbeTemplateId() == bridge::T_ME_COMMAND_OUTCOME);
static_assert(money::FeedHalt::sbeTemplateId() == bridge::T_FEED_HALT);
static_assert(money::MeCommandRelease::sbeTemplateId() == bridge::T_ME_COMMAND_RELEASE);
static_assert(money::MeEngineConfig::sbeTemplateId() == bridge::T_ME_ENGINE_CONFIG);
static_assert(money::QueryFeedPosition::sbeTemplateId() == bridge::T_QUERY_FEED_POSITION);
static_assert(money::FeedPositionReport::sbeTemplateId() == bridge::T_FEED_POSITION_REPORT);
static_assert(money::AdminAck::sbeTemplateId() == bridge::T_ADMIN_ACK);
static_assert(money::SettleRejected::sbeTemplateId() == bridge::T_SETTLE_REJECTED);
static_assert(jr::Trade::sbeTemplateId() == archive::TEMPLATE_TRADE);
static_assert(jr::Terminal::sbeTemplateId() == archive::TEMPLATE_TERMINAL);
static_assert(jr::CommandOutcome::sbeTemplateId() == archive::TEMPLATE_OUTCOME);
static_assert(jr::CommandRelease::sbeTemplateId() == archive::TEMPLATE_RELEASE);
static_assert(jr::EngineConfig::sbeTemplateId() == archive::TEMPLATE_CONFIG);

// block lengths (5.1): 61 109, 62 0, 63 62, 64 153, 67 44, 68 39; the schema 4 blocks equal them
static_assert(money::MeJournalTrade::sbeBlockLength() == 109 && jr::Trade::sbeBlockLength() == 109);
static_assert(money::MeJournalTradeBatch::sbeBlockLength() == 0);
static_assert(money::MeJournalTradeBatch::Trades::sbeBlockLength() == 109);
static_assert(money::MeJournalTradeBatch::Trades::sbeHeaderSize() == 4);
static_assert(money::MeJournalTerminal::sbeBlockLength() == 62 && jr::Terminal::sbeBlockLength() == 62);
static_assert(money::MeCommandOutcome::sbeBlockLength() == 153 && jr::CommandOutcome::sbeBlockLength() == 153);
static_assert(money::MeCommandRelease::sbeBlockLength() == 44 && jr::CommandRelease::sbeBlockLength() == 44);
static_assert(money::MeCommandRelease::Releases::sbeBlockLength() == 22 && jr::CommandRelease::Releases::sbeBlockLength() == 22);
static_assert(money::MeCommandRelease::Releases::sbeHeaderSize() == 4 && jr::CommandRelease::Releases::sbeHeaderSize() == 4);
static_assert(money::MeEngineConfig::sbeBlockLength() == 39 && jr::EngineConfig::sbeBlockLength() == 39);
static_assert(money::FeedHalt::sbeBlockLength() == 41);
static_assert(money::QueryFeedPosition::sbeBlockLength() == 8);
static_assert(money::FeedPositionReport::sbeBlockLength() == 43);
// the frame bound (S9a 6.3, cpp A5): the largest feed frame + the cluster session header fits the 8192 MTU
static_assert(MESSAGE_HEADER_LENGTH + 4 + 109 * 64 == bridge::MAX_BATCH_FRAME);
static_assert(bridge::MAX_BATCH_FRAME + 32 <= 8192 - 32);

// every pair of the 6.2 tables: same width on both generated codecs, at the table's offsets
#define CT_PAIR(M, J, mf, jf, mo, jo)                                                                          \
    static_assert(money::M::mf##EncodingLength() == jr::J::jf##EncodingLength(), #M "." #mf " width");      \
    static_assert(money::M::mf##EncodingOffset() == (mo), #M "." #mf " offset");                            \
    static_assert(jr::J::jf##EncodingOffset() == (jo), #J "." #jf " offset")

CT_PAIR(MeJournalTrade, Trade, egressSeq, egressSeq, 0, 16);
CT_PAIR(MeJournalTrade, Trade, tradeId, tradeId, 8, 24);
CT_PAIR(MeJournalTrade, Trade, marketId, marketId, 16, 32);
CT_PAIR(MeJournalTrade, Trade, takerOrderId, takerOrderId, 20, 36);
CT_PAIR(MeJournalTrade, Trade, takerUserId, takerUserId, 28, 44);
CT_PAIR(MeJournalTrade, Trade, makerOrderId, makerOrderId, 36, 52);
CT_PAIR(MeJournalTrade, Trade, makerUserId, makerUserId, 44, 60);
CT_PAIR(MeJournalTrade, Trade, price, price, 52, 68);
CT_PAIR(MeJournalTrade, Trade, quantity, quantity, 60, 76);
CT_PAIR(MeJournalTrade, Trade, takerIsBuy, takerIsBuy, 68, 84);
CT_PAIR(MeJournalTrade, Trade, meTimestamp, timestamp, 69, 85);
CT_PAIR(MeJournalTrade, Trade, takerOmsOrderId, takerOmsOrderId, 77, 93);
CT_PAIR(MeJournalTrade, Trade, makerOmsOrderId, makerOmsOrderId, 85, 101);
CT_PAIR(MeJournalTrade, Trade, meEpoch, meEpoch, 93, 8);
CT_PAIR(MeJournalTrade, Trade, journalSeq, journalSeq, 101, 0);

// review NOTE-5: every one of the 15 entry fields of the 62 against the journal Trade (width and offset)
CT_PAIR(MeJournalTradeBatch::Trades, Trade, egressSeq, egressSeq, 0, 16);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, tradeId, tradeId, 8, 24);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, marketId, marketId, 16, 32);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, takerOrderId, takerOrderId, 20, 36);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, takerUserId, takerUserId, 28, 44);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, makerOrderId, makerOrderId, 36, 52);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, makerUserId, makerUserId, 44, 60);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, price, price, 52, 68);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, quantity, quantity, 60, 76);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, takerIsBuy, takerIsBuy, 68, 84);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, meTimestamp, timestamp, 69, 85);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, takerOmsOrderId, takerOmsOrderId, 77, 93);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, makerOmsOrderId, makerOmsOrderId, 85, 101);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, meEpoch, meEpoch, 93, 8);
CT_PAIR(MeJournalTradeBatch::Trades, Trade, journalSeq, journalSeq, 101, 0);

CT_PAIR(MeJournalTerminal, Terminal, egressSeq, egressSeq, 0, 16);
CT_PAIR(MeJournalTerminal, Terminal, orderId, orderId, 8, 24);
CT_PAIR(MeJournalTerminal, Terminal, userId, userId, 16, 32);
CT_PAIR(MeJournalTerminal, Terminal, marketId, marketId, 24, 40);
CT_PAIR(MeJournalTerminal, Terminal, status, status, 28, 44);
CT_PAIR(MeJournalTerminal, Terminal, meTimestamp, timestamp, 29, 45);
CT_PAIR(MeJournalTerminal, Terminal, omsOrderId, omsOrderId, 37, 53);
CT_PAIR(MeJournalTerminal, Terminal, meEpoch, meEpoch, 45, 8);
CT_PAIR(MeJournalTerminal, Terminal, journalSeq, journalSeq, 53, 0);
CT_PAIR(MeJournalTerminal, Terminal, rejectReason, rejectReason, 61, 61);

CT_PAIR(MeCommandOutcome, CommandOutcome, egressSeq, egressSeq, 0, 16);
CT_PAIR(MeCommandOutcome, CommandOutcome, commandIdHigh, commandIdHigh, 8, 24);
CT_PAIR(MeCommandOutcome, CommandOutcome, commandIdLow, commandIdLow, 16, 32);
CT_PAIR(MeCommandOutcome, CommandOutcome, userId, userId, 24, 40);
CT_PAIR(MeCommandOutcome, CommandOutcome, omsOrderId, omsOrderId, 32, 48);
CT_PAIR(MeCommandOutcome, CommandOutcome, oldOrderId, oldOrderId, 40, 56);
CT_PAIR(MeCommandOutcome, CommandOutcome, price, price, 48, 64);
CT_PAIR(MeCommandOutcome, CommandOutcome, quantity, quantity, 56, 72);
CT_PAIR(MeCommandOutcome, CommandOutcome, budget, budget, 64, 80);
CT_PAIR(MeCommandOutcome, CommandOutcome, marketId, marketId, 72, 88);
CT_PAIR(MeCommandOutcome, CommandOutcome, commandKind, commandKind, 76, 92);
CT_PAIR(MeCommandOutcome, CommandOutcome, orderType, orderType, 77, 93);
CT_PAIR(MeCommandOutcome, CommandOutcome, orderSide, orderSide, 78, 94);
CT_PAIR(MeCommandOutcome, CommandOutcome, appliedPosition, appliedPosition, 79, 95);
CT_PAIR(MeCommandOutcome, CommandOutcome, meTimestamp, timestamp, 87, 103);
CT_PAIR(MeCommandOutcome, CommandOutcome, orderId, orderId, 95, 111);
CT_PAIR(MeCommandOutcome, CommandOutcome, status, status, 103, 119);
CT_PAIR(MeCommandOutcome, CommandOutcome, reason, reason, 107, 123);
CT_PAIR(MeCommandOutcome, CommandOutcome, oldCancelled, oldCancelled, 111, 127);
CT_PAIR(MeCommandOutcome, CommandOutcome, result, result, 112, 128);
CT_PAIR(MeCommandOutcome, CommandOutcome, meEpoch, meEpoch, 116, 8);
CT_PAIR(MeCommandOutcome, CommandOutcome, journalSeq, journalSeq, 124, 0);
CT_PAIR(MeCommandOutcome, CommandOutcome, timeInForce, timeInForce, 132, 132);
CT_PAIR(MeCommandOutcome, CommandOutcome, expectedOldRemaining, expectedOldRemaining, 133, 133);
CT_PAIR(MeCommandOutcome, CommandOutcome, ledgerSize, ledgerSize, 141, 141);
CT_PAIR(MeCommandOutcome, CommandOutcome, ledgerCapacity, ledgerCapacity, 145, 145);
CT_PAIR(MeCommandOutcome, CommandOutcome, behaviorVersion, behaviorVersion, 149, 149);

CT_PAIR(MeCommandRelease, CommandRelease, egressSeq, egressSeq, 0, 16);
CT_PAIR(MeCommandRelease, CommandRelease, journalSeq, journalSeq, 8, 0);
CT_PAIR(MeCommandRelease, CommandRelease, meEpoch, meEpoch, 16, 8);
CT_PAIR(MeCommandRelease, CommandRelease, releaseSeq, releaseSeq, 24, 24);
CT_PAIR(MeCommandRelease, CommandRelease, ledgerSize, ledgerSize, 32, 32);
CT_PAIR(MeCommandRelease, CommandRelease, ledgerCapacity, ledgerCapacity, 36, 36);
CT_PAIR(MeCommandRelease, CommandRelease, tombstoneRanges, tombstoneRanges, 40, 40);
CT_PAIR(MeCommandRelease::Releases, CommandRelease::Releases, commandIdHigh, commandIdHigh, 0, 0);
CT_PAIR(MeCommandRelease::Releases, CommandRelease::Releases, idLowFloor, idLowFloor, 8, 8);
CT_PAIR(MeCommandRelease::Releases, CommandRelease::Releases, scope, scope, 16, 16);
CT_PAIR(MeCommandRelease::Releases, CommandRelease::Releases, result, result, 17, 17);
CT_PAIR(MeCommandRelease::Releases, CommandRelease::Releases, removed, removed, 18, 18);

CT_PAIR(MeEngineConfig, EngineConfig, egressSeq, egressSeq, 0, 16);
CT_PAIR(MeEngineConfig, EngineConfig, journalSeq, journalSeq, 8, 0);
CT_PAIR(MeEngineConfig, EngineConfig, meEpoch, meEpoch, 16, 8);
CT_PAIR(MeEngineConfig, EngineConfig, configVersion, configVersion, 24, 24);
CT_PAIR(MeEngineConfig, EngineConfig, behaviorVersion, behaviorVersion, 28, 28);
CT_PAIR(MeEngineConfig, EngineConfig, ledgerCapacity, ledgerCapacity, 32, 32);
CT_PAIR(MeEngineConfig, EngineConfig, maxWireVersion, maxWireVersion, 36, 36);
CT_PAIR(MeEngineConfig, EngineConfig, decision, decision, 38, 38);
#undef CT_PAIR

// the 62 entry is exactly the 61 row layout
static_assert(money::MeJournalTradeBatch::Trades::journalSeqEncodingOffset() == money::MeJournalTrade::journalSeqEncodingOffset());
static_assert(money::MeJournalTradeBatch::Trades::takerIsBuyEncodingOffset() == money::MeJournalTrade::takerIsBuyEncodingOffset());
static_assert(money::MeJournalTradeBatch::Trades::makerOmsOrderIdEncodingOffset() == money::MeJournalTrade::makerOmsOrderIdEncodingOffset());

namespace {

char *asChars(const std::uint8_t *p) noexcept
{
    return reinterpret_cast<char *>(const_cast<std::uint8_t *>(p));
}

std::uint16_t le16(const std::uint8_t *p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// S9a 6.1: an enum field written raw (C++ setters take E::Value; every generated enum here spans 0..255 because it
// lists NULL_VALUE 255, so every raw byte is a representable value); E::get is never called. The RED shape
// CODEC_ENUM_GET goes through E::get and clamps an unknown byte (a throw inside noexcept would only crash).
template<typename E>
typename E::Value raw(std::uint8_t b) noexcept
{
    if (disabled(Rule::CODEC_ENUM_GET))
    {
        try
        {
            return E::get(b);
        }
        catch (...)
        {
            return E::NULL_VALUE;
        }
    }
    return static_cast<typename E::Value>(b);
}

// One schema 4 Trade at `record` (header included) into a 61 row (the same encoder calls for the 62 entry).
template<typename Out>
void tradeRow(const std::uint8_t *record, Out &e) noexcept
{
    jr::Trade d;
    d.wrapForDecode(asChars(record), MESSAGE_HEADER_LENGTH, jr::Trade::sbeBlockLength(), JOURNAL_VERSION,
        bridge::TRADE_RECORD_LENGTH);
    const bool swap = disabled(Rule::CODEC_SWAP_TAKER_MAKER_ORDER);
    const bool head = disabled(Rule::CODEC_HEAD_NOT_PERMUTED);
    e.egressSeq(head ? d.journalSeq() : d.egressSeq())
        .tradeId(d.tradeId())
        .marketId(d.marketId())
        .takerOrderId(swap ? d.makerOrderId() : d.takerOrderId())
        .takerUserId(d.takerUserId())
        .makerOrderId(swap ? d.takerOrderId() : d.makerOrderId())
        .makerUserId(d.makerUserId())
        .price(d.price())
        .quantity(d.quantity())
        .takerIsBuy(raw<money::BoolFlag>(d.takerIsBuyRaw()))
        .meTimestamp(d.timestamp())
        .takerOmsOrderId(d.takerOmsOrderId())
        .makerOmsOrderId(d.makerOmsOrderId())
        .meEpoch(d.meEpoch())
        .journalSeq(head ? d.egressSeq() : d.journalSeq());
}

}

std::size_t BridgeCodec::frameLengthFor(const std::uint8_t *record, std::size_t length) noexcept
{
    if (nullptr == record || length < archive::MESSAGE_HEADER_LENGTH)
    {
        return 0;
    }
    const std::uint16_t block = le16(record);
    const std::uint16_t templateId = le16(record + 2);
    if (le16(record + 4) != JOURNAL_SCHEMA_ID || le16(record + 6) != JOURNAL_VERSION ||
        block != archive::blockLengthFor(templateId))
    {
        return 0;
    }
    switch (templateId)
    {
        case archive::TEMPLATE_TRADE:
            return length == MESSAGE_HEADER_LENGTH + 109 ? MESSAGE_HEADER_LENGTH + 109 : 0;
        case archive::TEMPLATE_TERMINAL:
            return length == MESSAGE_HEADER_LENGTH + 62 ? MESSAGE_HEADER_LENGTH + 62 : 0;
        case archive::TEMPLATE_OUTCOME:
            return length == MESSAGE_HEADER_LENGTH + 153 ? MESSAGE_HEADER_LENGTH + 153 : 0;
        case archive::TEMPLATE_CONFIG:
            return length == MESSAGE_HEADER_LENGTH + 39 ? MESSAGE_HEADER_LENGTH + 39 : 0;
        case archive::TEMPLATE_RELEASE:
        {
            if (length < archive::RELEASE_MIN_FRAME)
            {
                return 0;
            }
            const std::uint8_t *g = record + MESSAGE_HEADER_LENGTH + 44;
            const std::uint16_t entryBlock = le16(g);
            const std::uint16_t n = le16(g + 2);
            if (entryBlock != 22 || n > archive::RELEASE_MAX_ENTRIES || length != archive::RELEASE_MIN_FRAME + 22u * n)
            {
                return 0;
            }
            return length;
        }
        default:
            return 0;
    }
}

std::size_t BridgeCodec::encodeRecord(const std::uint8_t *record, std::size_t length, std::uint8_t *out,
    std::size_t capacity) noexcept
{
    const std::size_t frame = frameLengthFor(record, length);
    if (0 == frame || capacity < frame)
    {
        return 0; // behind the gate this cannot happen: the domain treats it as Fatal::ENCODER_OVERFLOW
    }
    char *buf = reinterpret_cast<char *>(out);
    const bool head = disabled(Rule::CODEC_HEAD_NOT_PERMUTED);
    switch (le16(record + 2))
    {
        case archive::TEMPLATE_TRADE:
        {
            money::MeJournalTrade e;
            e.wrapAndApplyHeader(buf, 0, capacity);
            tradeRow(record, e);
            return MESSAGE_HEADER_LENGTH + e.encodedLength();
        }
        case archive::TEMPLATE_TERMINAL:
        {
            jr::Terminal d;
            d.wrapForDecode(asChars(record), MESSAGE_HEADER_LENGTH, jr::Terminal::sbeBlockLength(), JOURNAL_VERSION, length);
            money::MeJournalTerminal e;
            e.wrapAndApplyHeader(buf, 0, capacity)
                .egressSeq(head ? d.journalSeq() : d.egressSeq())
                .orderId(d.orderId())
                .userId(d.userId())
                .marketId(d.marketId())
                .status(raw<money::OrderStatus>(d.statusRaw()))
                .meTimestamp(d.timestamp())
                .omsOrderId(d.omsOrderId())
                .meEpoch(d.meEpoch())
                .journalSeq(head ? d.egressSeq() : d.journalSeq())
                .rejectReason(d.rejectReason()); // a plain uint8 on both schemas (review FIX-14): no null check
            return MESSAGE_HEADER_LENGTH + e.encodedLength();
        }
        case archive::TEMPLATE_OUTCOME:
        {
            jr::CommandOutcome d;
            d.wrapForDecode(asChars(record), MESSAGE_HEADER_LENGTH, jr::CommandOutcome::sbeBlockLength(), JOURNAL_VERSION,
                length);
            money::MeCommandOutcome e;
            e.wrapAndApplyHeader(buf, 0, capacity)
                .egressSeq(head ? d.journalSeq() : d.egressSeq())
                .commandIdHigh(d.commandIdHigh())
                .commandIdLow(d.commandIdLow())
                .userId(d.userId())
                .omsOrderId(d.omsOrderId())
                .oldOrderId(d.oldOrderId())
                .price(d.price())
                .quantity(d.quantity())
                .budget(d.budget())
                .marketId(d.marketId())
                .commandKind(d.commandKind())
                .orderType(d.orderType())
                .orderSide(d.orderSide())
                .appliedPosition(d.appliedPosition())
                .meTimestamp(d.timestamp())
                .orderId(d.orderId())
                .status(d.status())
                .reason(d.reason())
                .oldCancelled(d.oldCancelled())
                .result(d.result())
                .meEpoch(d.meEpoch())
                .journalSeq(head ? d.egressSeq() : d.journalSeq())
                .timeInForce(d.timeInForce())
                .expectedOldRemaining(d.expectedOldRemaining())
                .ledgerSize(d.ledgerSize())
                .ledgerCapacity(d.ledgerCapacity())
                .behaviorVersion(d.behaviorVersion());
            return MESSAGE_HEADER_LENGTH + e.encodedLength();
        }
        case archive::TEMPLATE_RELEASE:
        {
            jr::CommandRelease d;
            d.wrapForDecode(asChars(record), MESSAGE_HEADER_LENGTH, jr::CommandRelease::sbeBlockLength(), JOURNAL_VERSION,
                length);
            money::MeCommandRelease e;
            e.wrapAndApplyHeader(buf, 0, capacity)
                .egressSeq(head ? d.journalSeq() : d.egressSeq())
                .journalSeq(head ? d.egressSeq() : d.journalSeq())
                .meEpoch(d.meEpoch())
                .releaseSeq(d.releaseSeq())
                .ledgerSize(d.ledgerSize())
                .ledgerCapacity(d.ledgerCapacity())
                .tombstoneRanges(d.tombstoneRanges());
            auto &in = d.releases();
            auto &group = e.releasesCount(static_cast<std::uint16_t>(in.count()));
            while (in.hasNext())
            {
                in.next();
                group.next()
                    .commandIdHigh(in.commandIdHigh())
                    .idLowFloor(in.idLowFloor())
                    .scope(raw<money::ReleaseScope>(in.scopeRaw()))
                    .result(raw<money::ReleaseResult>(in.resultRaw()))
                    .removed(in.removed());
            }
            return MESSAGE_HEADER_LENGTH + e.encodedLength();
        }
        case archive::TEMPLATE_CONFIG:
        {
            jr::EngineConfig d;
            d.wrapForDecode(asChars(record), MESSAGE_HEADER_LENGTH, jr::EngineConfig::sbeBlockLength(), JOURNAL_VERSION,
                length);
            money::MeEngineConfig e;
            e.wrapAndApplyHeader(buf, 0, capacity)
                .egressSeq(head ? d.journalSeq() : d.egressSeq())
                .journalSeq(head ? d.egressSeq() : d.journalSeq())
                .meEpoch(d.meEpoch())
                .configVersion(d.configVersion())
                .behaviorVersion(d.behaviorVersion())
                .ledgerCapacity(d.ledgerCapacity())
                .maxWireVersion(d.maxWireVersion())
                .decision(raw<money::ConfigDecision>(d.decisionRaw()));
            return MESSAGE_HEADER_LENGTH + e.encodedLength();
        }
        default:
            return 0;
    }
}

std::size_t BridgeCodec::encodeTradeBatch(const StagedTrade *trades, std::size_t n, std::uint8_t *out,
    std::size_t capacity) noexcept
{
    if (nullptr == trades || n < 2 || n > 64 || capacity < MESSAGE_HEADER_LENGTH + 4 + 109 * n)
    {
        return 0;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        if (0 == frameLengthFor(trades[i].record, bridge::TRADE_RECORD_LENGTH) ||
            archive::TEMPLATE_TRADE != le16(trades[i].record + 2))
        {
            return 0;
        }
    }
    money::MeJournalTradeBatch b;
    b.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity);
    auto &group = b.tradesCount(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i)
    {
        tradeRow(trades[i].record, group.next());
    }
    return MESSAGE_HEADER_LENGTH + b.encodedLength();
}

std::size_t BridgeCodec::encodeFeedHalt(std::int64_t correlationId, HaltReason reason, std::int64_t meEpochSeen,
    std::int64_t expected, std::int64_t seen, std::int64_t identity, std::uint8_t *out, std::size_t capacity) noexcept
{
    if (capacity < FEED_HALT_FRAME_LENGTH)
    {
        return 0;
    }
    money::FeedHalt e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity)
        .correlationId(correlationId)
        .reason(static_cast<money::FeedHaltReason::Value>(static_cast<std::uint8_t>(reason)))
        .meEpochSeen(meEpochSeen)
        .expectedJournalSeq(expected)
        .seenJournalSeq(seen)
        .bridgeIdentity(identity);
    return MESSAGE_HEADER_LENGTH + e.encodedLength();
}

std::size_t BridgeCodec::encodeQuery(std::int64_t correlationId, std::uint8_t *out, std::size_t capacity) noexcept
{
    if (capacity < QUERY_FRAME_LENGTH)
    {
        return 0;
    }
    money::QueryFeedPosition e;
    e.wrapAndApplyHeader(reinterpret_cast<char *>(out), 0, capacity).correlationId(correlationId);
    return MESSAGE_HEADER_LENGTH + e.encodedLength();
}

EgressKind BridgeCodec::decodeEgress(const std::uint8_t *frame, std::size_t length, EgressEvent &out) noexcept
{
    out = EgressEvent{};
    if (nullptr == frame || length < MESSAGE_HEADER_LENGTH)
    {
        out.kind = EgressKind::MALFORMED;
        return out.kind;
    }
    const std::uint16_t block = le16(frame);
    const std::uint16_t templateId = le16(frame + 2);
    const std::uint16_t schemaId = le16(frame + 4);
    const std::uint16_t version = le16(frame + 6);
    out.templateId = templateId;
    if (MONEY_SCHEMA_ID != schemaId || (bridge::T_FEED_POSITION_REPORT != templateId &&
                                        bridge::T_ADMIN_ACK != templateId && bridge::T_SETTLE_REJECTED != templateId))
    {
        out.kind = EgressKind::IGNORED;
        return out.kind;
    }
    const bool versionOk = disabled(Rule::EGRESS_VERSION_FLOOR) ? version >= 6 : MONEY_VERSION == version;
    std::size_t need = 0;
    switch (templateId)
    {
        case bridge::T_FEED_POSITION_REPORT: need = money::FeedPositionReport::sbeBlockLength(); break;
        case bridge::T_ADMIN_ACK: need = money::AdminAck::sbeBlockLength(); break;
        default: need = money::SettleRejected::sbeBlockLength(); break;
    }
    // RED shape EGRESS_VERSION_FLOOR: an older block is read as far as it goes (the v7 tail then reads zeros)
    const bool blockOk = disabled(Rule::EGRESS_VERSION_FLOOR) ? block >= 8 : block >= need;
    if (!versionOk || !blockOk || length < MESSAGE_HEADER_LENGTH + block)
    {
        out.kind = EgressKind::MALFORMED;
        return out.kind;
    }
    std::uint8_t padded[MESSAGE_HEADER_LENGTH + 64]{};
    const std::uint8_t *body = frame;
    if (block < need) // only reachable with the seam: a short block decoded over zeros
    {
        std::memcpy(padded, frame, MESSAGE_HEADER_LENGTH + block);
        body = padded;
    }
    const std::size_t bodyLength = MESSAGE_HEADER_LENGTH + (block < need ? need : block);
    switch (templateId)
    {
        case bridge::T_FEED_POSITION_REPORT:
        {
            money::FeedPositionReport d;
            d.wrapForDecode(asChars(body), MESSAGE_HEADER_LENGTH, block < need ? need : block, version, bodyLength);
            FeedPositionReport &r = out.report;
            r.correlationId = d.correlationId();
            r.consumePosition = d.consumePosition();
            r.lastAppliedTradeId = d.lastAppliedTradeId();
            r.journalEnabled = 1 == d.journalEnabledRaw();
            r.meEpoch = d.meEpoch();
            r.lastAppliedJournalSeq = d.lastAppliedJournalSeq();
            r.feedHaltedRaw = d.feedHaltedRaw();
            r.feedHalted = 1 == r.feedHaltedRaw;
            r.haltReasonRaw = d.haltReasonRaw();
            out.kind = EgressKind::REPORT;
            return out.kind;
        }
        case bridge::T_ADMIN_ACK:
        {
            money::AdminAck d;
            d.wrapForDecode(asChars(body), MESSAGE_HEADER_LENGTH, block < need ? need : block, version, bodyLength);
            out.ack.correlationId = d.correlationId();
            out.ack.refTemplateId = d.refTemplateId();
            out.ack.resultRaw = d.resultRaw();
            out.ack.reason = d.reason();
            out.kind = EgressKind::ADMIN_ACK;
            return out.kind;
        }
        default:
        {
            money::SettleRejected d;
            d.wrapForDecode(asChars(body), MESSAGE_HEADER_LENGTH, block < need ? need : block, version, bodyLength);
            out.reject.tradeId = d.tradeId();
            out.reject.marketId = d.marketId();
            out.reject.journalPosition = d.journalPosition();
            out.reject.reasonRaw = d.reasonRaw();
            out.reject.detail = d.detail();
            out.kind = EgressKind::SETTLE_REJECTED;
            return out.kind;
        }
    }
}

// ------------------------------------------------------------------ RC2 S9c: the app's egress reads and the O3/O4 getter

static_assert(money::OrderReject::sbeTemplateId() == T_ORDER_REJECT);
static_assert(money::SettlementApplied::sbeTemplateId() == T_SETTLEMENT_APPLIED);
static_assert(static_cast<std::uint8_t>(money::CoreRejectReason::INVALID) == CORE_REJECT_INVALID);
static_assert(static_cast<std::uint8_t>(money::CoreRejectReason::UNAUTHORIZED_CLASS) == CORE_REJECT_UNAUTHORIZED_CLASS);
static_assert(static_cast<std::uint8_t>(money::CoreRejectReason::UNREGISTERED_SESSION) == CORE_REJECT_UNREGISTERED_SESSION);
// the 61 root block and the 62 entry carry tradeId and meTimestamp at the same offsets (S9b NOTE-5 pins every field)
static_assert(money::MeJournalTrade::tradeIdEncodingOffset() == money::MeJournalTradeBatch::Trades::tradeIdEncodingOffset());
static_assert(money::MeJournalTrade::meTimestampEncodingOffset() ==
    money::MeJournalTradeBatch::Trades::meTimestampEncodingOffset());

namespace {

bool moneyHeaderIs(const std::uint8_t *frame, std::size_t length, std::uint16_t templateId, std::uint16_t need,
    std::uint16_t &block, std::uint16_t &version) noexcept
{
    if (nullptr == frame || length < MESSAGE_HEADER_LENGTH)
    {
        return false;
    }
    block = le16(frame);
    version = le16(frame + 6);
    if (MONEY_SCHEMA_ID != le16(frame + 4) || templateId != le16(frame + 2))
    {
        return false;
    }
    const bool versionOk = disabled(Rule::EGRESS_VERSION_FLOOR) ? version >= 6 : MONEY_VERSION == version;
    return versionOk && block >= need && length >= MESSAGE_HEADER_LENGTH + block;
}

std::int64_t rd64(const std::uint8_t *p) noexcept
{
    std::int64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

}

bool BridgeCodec::decodeOrderReject(const std::uint8_t *frame, std::size_t length, OrderRejectView &out) noexcept
{
    std::uint16_t block = 0, version = 0;
    if (!moneyHeaderIs(frame, length, T_ORDER_REJECT, money::OrderReject::sbeBlockLength(), block, version))
    {
        return false;
    }
    money::OrderReject d;
    d.wrapForDecode(asChars(frame), MESSAGE_HEADER_LENGTH, block, version, length);
    out.correlationId = d.correlationId();
    out.refTemplateId = d.refTemplateId();
    out.reasonRaw = d.reasonRaw();
    return true;
}

bool BridgeCodec::decodeSettlementApplied(const std::uint8_t *frame, std::size_t length, std::int64_t &tradeId) noexcept
{
    std::uint16_t block = 0, version = 0;
    if (!moneyHeaderIs(frame, length, T_SETTLEMENT_APPLIED, money::SettlementApplied::sbeBlockLength(), block, version))
    {
        return false;
    }
    money::SettlementApplied d;
    d.wrapForDecode(asChars(frame), MESSAGE_HEADER_LENGTH, block, version, length);
    tradeId = d.tradeId();
    return true;
}

std::size_t BridgeCodec::visitTrades(const std::uint8_t *frame, std::size_t length, TradeVisitor visitor,
    void *context) noexcept
{
    if (nullptr == frame || nullptr == visitor || length < MESSAGE_HEADER_LENGTH || MONEY_SCHEMA_ID != le16(frame + 4))
    {
        return 0;
    }
    constexpr std::size_t ID = money::MeJournalTrade::tradeIdEncodingOffset();
    constexpr std::size_t TS = money::MeJournalTrade::meTimestampEncodingOffset();
    constexpr std::size_t BLOCK = money::MeJournalTrade::sbeBlockLength();
    const std::uint16_t templateId = le16(frame + 2);
    if (bridge::T_ME_JOURNAL_TRADE == templateId)
    {
        if (length < MESSAGE_HEADER_LENGTH + BLOCK)
        {
            return 0;
        }
        const std::uint8_t *b = frame + MESSAGE_HEADER_LENGTH;
        visitor(context, rd64(b + ID), rd64(b + TS));
        return 1;
    }
    if (bridge::T_ME_JOURNAL_TRADE_BATCH != templateId || length < MESSAGE_HEADER_LENGTH + 4)
    {
        return 0;
    }
    const std::uint8_t *g = frame + MESSAGE_HEADER_LENGTH + le16(frame); // the root block is 0
    const std::uint16_t entryBlock = le16(g);
    const std::uint16_t n = le16(g + 2);
    if (entryBlock < BLOCK || g + 4 + static_cast<std::size_t>(entryBlock) * n > frame + length)
    {
        return 0;
    }
    for (std::uint16_t i = 0; i < n; ++i)
    {
        const std::uint8_t *e = g + 4 + static_cast<std::size_t>(entryBlock) * i;
        visitor(context, rd64(e + ID), rd64(e + TS));
    }
    return n;
}

}}}
