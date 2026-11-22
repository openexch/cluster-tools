#include "genesis/GenesisWire.h"

#include <cstring>
#include <exception>

#include "cluster_tools/adapters/sbe/EngineConfigCodec.h"

#include "com_openexchange_core_infrastructure_generated/AdminAck.h"
#include "com_openexchange_core_infrastructure_generated/BalanceSnapshotEnd.h"
#include "com_openexchange_core_infrastructure_generated/BalanceUpdate.h"
#include "com_openexchange_core_infrastructure_generated/BalanceUpdateBatch.h"
#include "com_openexchange_core_infrastructure_generated/CoreConfig.h"
#include "com_openexchange_core_infrastructure_generated/CoreConfigReport.h"
#include "com_openexchange_core_infrastructure_generated/CoreLimitsSet.h"
#include "com_openexchange_core_infrastructure_generated/Deposit.h"
#include "com_openexchange_core_infrastructure_generated/DepositAck.h"
#include "com_openexchange_core_infrastructure_generated/DepositReject.h"
#include "com_openexchange_core_infrastructure_generated/FeedPositionReport.h"
#include "com_openexchange_core_infrastructure_generated/HoldSnapshotBatch.h"
#include "com_openexchange_core_infrastructure_generated/HoldSnapshotEnd.h"
#include "com_openexchange_core_infrastructure_generated/HoldSnapshotEntry.h"
#include "com_openexchange_core_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_core_infrastructure_generated/OrderEntryGate.h"
#include "com_openexchange_core_infrastructure_generated/OrderReject.h"
#include "com_openexchange_core_infrastructure_generated/OrderSnapshotBatch.h"
#include "com_openexchange_core_infrastructure_generated/OrderSnapshotEnd.h"
#include "com_openexchange_core_infrastructure_generated/QueryFeedPosition.h"
#include "com_openexchange_core_infrastructure_generated/RequestBalanceSnapshot.h"
#include "com_openexchange_core_infrastructure_generated/RequestCoreConfig.h"
#include "com_openexchange_core_infrastructure_generated/RequestHoldSnapshot.h"
#include "com_openexchange_core_infrastructure_generated/RequestOrderSnapshot.h"
#include "com_openexchange_core_infrastructure_generated/RiskConfigSet.h"
#include "com_openexchange_core_infrastructure_generated/SetMoneyJournal.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/MessageHeader.h"

namespace cluster_tools { namespace app { namespace genesis { namespace wire {

namespace money = com::openexchange::core::infrastructure::generated;
namespace me = com::match::infrastructure::generated;

static_assert(money::CoreConfig::sbeTemplateId() == T_CORE_CONFIG);
static_assert(money::RiskConfigSet::sbeTemplateId() == T_RISK_CONFIG_SET);
static_assert(money::OrderEntryGate::sbeTemplateId() == T_ORDER_ENTRY_GATE);
static_assert(money::CoreLimitsSet::sbeTemplateId() == T_CORE_LIMITS_SET);
static_assert(money::Deposit::sbeTemplateId() == T_DEPOSIT);
static_assert(money::SetMoneyJournal::sbeTemplateId() == T_SET_MONEY_JOURNAL);
static_assert(money::MessageHeader::sbeSchemaId() == 2);
static_assert(money::MessageHeader::sbeSchemaVersion() >= 9); // the genesisId on CoreConfigReport (v9)
static_assert(me::MessageHeader::sbeSchemaId() == 1);
static_assert(money::GateState::OPEN == GATE_OPEN && money::GateState::SUSPENDED == GATE_SUSPENDED);
static_assert(money::MessageHeader::encodedLength() == me::MessageHeader::encodedLength());

const char *adminResultName(std::uint8_t result) noexcept
{
    switch (result)
    {
        case ADMIN_ACCEPTED: return "ACCEPTED";
        case ADMIN_DUPLICATE: return "DUPLICATE";
        case ADMIN_REJECTED: return "REJECTED";
        case ADMIN_ADOPTED: return "ADOPTED";
        case ADMIN_STALE: return "STALE";
        default: return "UNKNOWN";
    }
}

const char *coreReasonName(std::uint8_t reason) noexcept
{
    switch (reason)
    {
        case 0: return "NONE";
        case 15: return "INVALID";
        case 18: return "GATE_CLOSED";
        case 20: return "UNAUTHORIZED_CLASS";
        case 40: return "FEED_HALTED";
        case 41: return "ME_LANE_INACTIVE";
        default: break;
    }
    try
    {
        return money::CoreRejectReason::c_str(money::CoreRejectReason::get(reason));
    }
    catch (const std::exception &)
    {
        return "UNKNOWN";
    }
}

namespace {

constexpr std::size_t HEADER = money::MessageHeader::encodedLength();

template<typename Message>
Bytes finish(Bytes &buffer, Message &m)
{
    buffer.resize(HEADER + m.encodedLength());
    return buffer;
}

template<typename Message>
Message wrapOut(Bytes &buffer, std::size_t capacity)
{
    buffer.assign(capacity, 0);
    Message m;
    m.wrapAndApplyHeader(reinterpret_cast<char *>(buffer.data()), 0, buffer.size());
    return m;
}

}

Bytes setMoneyJournal(std::int64_t correlationId, bool enabled)
{
    Bytes b;
    auto m = wrapOut<money::SetMoneyJournal>(b, 64);
    m.correlationId(correlationId).enabled(enabled ? money::BoolFlag::TRUE : money::BoolFlag::FALSE);
    return finish(b, m);
}

Bytes coreConfig(std::int64_t correlationId, std::uint32_t configVersion, std::uint32_t behaviorVersion,
    const GenesisPlan &plan)
{
    Bytes b;
    auto m = wrapOut<money::CoreConfig>(b, 256 + 64 * plan.markets.size() + 32 * plan.assets.size());
    m.correlationId(correlationId).configVersion(configVersion).behaviorVersion(behaviorVersion).genesisId(plan.genesisId);
    auto &markets = m.marketsCount(static_cast<std::uint16_t>(plan.markets.size()));
    for (const CoreMarket &row : plan.markets)
    {
        markets.next()
            .marketId(row.marketId)
            .putSymbol(std::string_view(row.symbol))
            .baseAssetId(row.baseAssetId)
            .quoteAssetId(row.quoteAssetId)
            .tickSize(row.tickSize)
            .minPrice(row.minPrice)
            .maxPrice(row.maxPrice)
            .lotSize(row.lotSize);
    }
    auto &assets = m.assetsCount(static_cast<std::uint16_t>(plan.assets.size()));
    for (const CoreAsset &row : plan.assets)
    {
        assets.next().assetId(row.assetId).putCode(std::string_view(row.code)).scale(row.scale);
    }
    return finish(b, m);
}

Bytes orderEntryGateOpen(std::int64_t correlationId, std::int32_t marketId)
{
    Bytes b;
    auto m = wrapOut<money::OrderEntryGate>(b, 64);
    m.correlationId(correlationId).marketId(marketId).state(money::GateState::OPEN).reason(money::GateReason::ADMIN);
    return finish(b, m);
}

Bytes riskConfigSet(std::int64_t correlationId, std::uint32_t configVersion, const RiskRow &r)
{
    Bytes b;
    auto m = wrapOut<money::RiskConfigSet>(b, 128);
    m.correlationId(correlationId)
        .configVersion(configVersion)
        .marketId(r.marketId)
        .minQuantity(r.minQuantity)
        .maxQuantity(r.maxQuantity)
        .minNotional(r.minNotional)
        .maxNotional(r.maxNotional)
        .priceCollarBps(r.priceCollarBps)
        .maxOrdersPerSec(r.maxOrdersPerSec)
        .maxOrdersPerMin(r.maxOrdersPerMin)
        .maxOpenOrders(r.maxOpenOrders)
        .maxPositionPerMarket(r.maxPositionPerMarket)
        .marketBuySlippageBps(r.marketBuySlippageBps);
    return finish(b, m);
}

Bytes coreLimitsSet(std::int64_t correlationId, const Limits &l)
{
    Bytes b;
    auto m = wrapOut<money::CoreLimitsSet>(b, 64);
    m.correlationId(correlationId)
        .configVersion(l.configVersion)
        .requestTtlMs(l.requestTtlMs)
        .requestPerUserCap(l.requestPerUserCap)
        .orderEventRingCapacity(l.orderEventRingCapacity);
    return finish(b, m);
}

Bytes deposit(std::int64_t correlationId, const Funding &d)
{
    Bytes b;
    auto m = wrapOut<money::Deposit>(b, 64);
    m.correlationId(correlationId)
        .userId(d.userId)
        .assetId(d.assetId)
        .amount(d.amount)
        .requestIdHigh(d.requestIdHigh)
        .requestIdLow(d.requestIdLow);
    return finish(b, m);
}

Bytes queryFeedPosition(std::int64_t correlationId)
{
    Bytes b;
    auto m = wrapOut<money::QueryFeedPosition>(b, 32);
    m.correlationId(correlationId);
    return finish(b, m);
}

Bytes requestCoreConfig(std::int64_t correlationId)
{
    Bytes b;
    auto m = wrapOut<money::RequestCoreConfig>(b, 32);
    m.correlationId(correlationId);
    return finish(b, m);
}

Bytes requestBalanceSnapshot(std::int64_t correlationId)
{
    Bytes b;
    auto m = wrapOut<money::RequestBalanceSnapshot>(b, 32);
    m.correlationId(correlationId);
    return finish(b, m);
}

Bytes requestHoldSnapshot(std::int64_t correlationId)
{
    Bytes b;
    auto m = wrapOut<money::RequestHoldSnapshot>(b, 32);
    m.correlationId(correlationId);
    return finish(b, m);
}

Bytes requestOrderSnapshot(std::int64_t correlationId, std::int64_t fromOrderId)
{
    Bytes b;
    auto m = wrapOut<money::RequestOrderSnapshot>(b, 64);
    m.correlationId(correlationId).userId(0).fromOrderId(fromOrderId);
    return finish(b, m);
}

Bytes engineConfig(const GenesisPlan &plan, std::string &error)
{
    auto encoded = sbe_adapter::encodeEngineConfig(plan.me);
    if (!encoded.ok())
    {
        error = encoded.error();
        return {};
    }
    return encoded.value();
}

std::uint16_t templateIdOf(const std::uint8_t *data, std::size_t length) noexcept
{
    if (length < HEADER)
    {
        return 0;
    }
    std::uint16_t id = 0;
    std::memcpy(&id, data + 2, sizeof(id)); // MessageHeader: blockLength, templateId, schemaId, version (LE)
    return id;
}

namespace {

template<typename Message>
Message wrapIn(const std::uint8_t *data, std::size_t length, const money::MessageHeader &h)
{
    Message m;
    m.wrapForDecode(const_cast<char *>(reinterpret_cast<const char *>(data)), HEADER, h.blockLength(), h.version(), length);
    return m;
}

}

CoreFrame decodeCore(const std::uint8_t *data, std::size_t length)
{
    CoreFrame f;
    if (length < HEADER)
    {
        f.kind = CoreFrame::Kind::MALFORMED;
        f.problem = "shorter than a message header";
        return f;
    }
    try
    {
        money::MessageHeader h(const_cast<char *>(reinterpret_cast<const char *>(data)), length);
        f.templateId = h.templateId();
        if (h.schemaId() != money::MessageHeader::sbeSchemaId())
        {
            return f; // OTHER: another schema on this session
        }
        switch (h.templateId())
        {
            case money::AdminAck::sbeTemplateId():
            {
                auto m = wrapIn<money::AdminAck>(data, length, h);
                f.kind = CoreFrame::Kind::ADMIN_ACK;
                f.correlationId = m.correlationId();
                f.refTemplateId = m.refTemplateId();
                f.result = m.resultRaw();
                f.reason = m.reason();
                f.configVersion = m.configVersion();
                break;
            }
            case money::DepositAck::sbeTemplateId():
            {
                auto m = wrapIn<money::DepositAck>(data, length, h);
                f.kind = CoreFrame::Kind::DEPOSIT_ACK;
                f.correlationId = m.correlationId();
                f.duplicate = money::BoolFlag::TRUE == m.duplicateRaw();
                break;
            }
            case money::DepositReject::sbeTemplateId():
            {
                auto m = wrapIn<money::DepositReject>(data, length, h);
                f.kind = CoreFrame::Kind::DEPOSIT_REJECT;
                f.correlationId = m.correlationId();
                f.reason = m.reasonRaw();
                break;
            }
            case money::OrderReject::sbeTemplateId():
            {
                auto m = wrapIn<money::OrderReject>(data, length, h);
                f.kind = CoreFrame::Kind::ORDER_REJECT;
                f.correlationId = m.correlationId();
                f.refTemplateId = m.refTemplateId();
                f.reason = m.reasonRaw();
                break;
            }
            case money::FeedPositionReport::sbeTemplateId():
            {
                auto m = wrapIn<money::FeedPositionReport>(data, length, h);
                f.kind = CoreFrame::Kind::FEED_POSITION;
                f.correlationId = m.correlationId();
                f.report.lastAppliedTradeId = m.lastAppliedTradeId();
                f.report.journalEnabled = money::BoolFlag::TRUE == m.journalEnabledRaw();
                f.report.feedMeEpoch = m.meEpoch();
                f.report.lastAppliedJournalSeq = m.lastAppliedJournalSeq();
                f.report.feedHalted = money::BoolFlag::TRUE == m.feedHaltedRaw();
                f.report.haltReason = m.haltReasonRaw();
                break;
            }
            case money::CoreConfigReport::sbeTemplateId():
            {
                auto m = wrapIn<money::CoreConfigReport>(data, length, h);
                f.kind = CoreFrame::Kind::CORE_CONFIG_REPORT;
                f.correlationId = m.correlationId();
                CoreObserved &r = f.report;
                r.configVersion = m.configVersion();
                r.behaviorVersion = m.behaviorVersion();
                r.limitsVersion = m.limitsVersion();
                r.requestTtlMs = m.requestTtlMs();
                r.requestPerUserCap = m.requestPerUserCap();
                r.orderEventRingCapacity = m.orderEventRingCapacity();
                r.gateState = m.gateStateRaw();
                r.gateReasons = m.gateReasons().rawValue();
                r.genesisId = m.genesisId();
                auto &markets = m.markets();
                while (markets.hasNext())
                {
                    markets.next();
                    MarketReport row;
                    row.marketId = markets.marketId();
                    row.riskConfigVersion = markets.riskConfigVersion();
                    row.minQuantity = markets.minQuantity();
                    row.maxQuantity = markets.maxQuantity();
                    row.minNotional = markets.minNotional();
                    row.maxNotional = markets.maxNotional();
                    r.markets.push_back(row);
                }
                r.assetCount = m.assets().count();
                break;
            }
            case money::BalanceUpdate::sbeTemplateId():
            {
                auto m = wrapIn<money::BalanceUpdate>(data, length, h);
                f.kind = CoreFrame::Kind::BALANCE_ROWS;
                f.balances.push_back({ m.userId(), m.assetId(), m.available(), m.locked() });
                break;
            }
            case money::BalanceUpdateBatch::sbeTemplateId():
            {
                auto m = wrapIn<money::BalanceUpdateBatch>(data, length, h);
                f.kind = CoreFrame::Kind::BALANCE_ROWS;
                auto &u = m.updates();
                while (u.hasNext())
                {
                    u.next();
                    f.balances.push_back({ u.userId(), u.assetId(), u.available(), u.locked() });
                }
                break;
            }
            case money::BalanceSnapshotEnd::sbeTemplateId():
            {
                auto m = wrapIn<money::BalanceSnapshotEnd>(data, length, h);
                f.kind = CoreFrame::Kind::BALANCE_END;
                f.correlationId = m.correlationId();
                f.count = m.entryCount();
                break;
            }
            case money::HoldSnapshotEntry::sbeTemplateId():
                f.kind = CoreFrame::Kind::HOLD_ROWS;
                f.count = 1;
                break;
            case money::HoldSnapshotBatch::sbeTemplateId():
            {
                auto m = wrapIn<money::HoldSnapshotBatch>(data, length, h);
                f.kind = CoreFrame::Kind::HOLD_ROWS;
                f.count = static_cast<std::int64_t>(m.holds().count());
                break;
            }
            case money::HoldSnapshotEnd::sbeTemplateId():
            {
                auto m = wrapIn<money::HoldSnapshotEnd>(data, length, h);
                f.kind = CoreFrame::Kind::HOLD_END;
                f.correlationId = m.correlationId();
                f.count = m.entryCount();
                break;
            }
            case money::OrderSnapshotBatch::sbeTemplateId():
            {
                auto m = wrapIn<money::OrderSnapshotBatch>(data, length, h);
                f.kind = CoreFrame::Kind::ORDER_ROWS;
                f.count = static_cast<std::int64_t>(m.orders().count());
                break;
            }
            case money::OrderSnapshotEnd::sbeTemplateId():
            {
                auto m = wrapIn<money::OrderSnapshotEnd>(data, length, h);
                f.kind = CoreFrame::Kind::ORDER_END;
                f.correlationId = m.correlationId();
                f.count = m.entryCount();
                f.nextOrderId = m.nextOrderId();
                break;
            }
            default:
                break;
        }
    }
    catch (const std::exception &e)
    {
        f.kind = CoreFrame::Kind::MALFORMED;
        f.problem = e.what();
    }
    return f;
}

MeFrame decodeMe(const std::uint8_t *data, std::size_t length)
{
    MeFrame f;
    if (length < HEADER)
    {
        f.kind = MeFrame::Kind::MALFORMED;
        return f;
    }
    try
    {
        me::MessageHeader h(const_cast<char *>(reinterpret_cast<const char *>(data)), length);
        if (h.schemaId() != me::MessageHeader::sbeSchemaId() || h.templateId() != me::ClusterHeartbeat::sbeTemplateId())
        {
            return f;
        }
        me::ClusterHeartbeat m;
        m.wrapForDecode(const_cast<char *>(reinterpret_cast<const char *>(data)), HEADER, h.blockLength(), h.version(),
            length);
        f.kind = MeFrame::Kind::HEARTBEAT;
        f.meEpoch = m.meEpoch();
    }
    catch (const std::exception &)
    {
        f.kind = MeFrame::Kind::MALFORMED;
    }
    return f;
}

}}}}
