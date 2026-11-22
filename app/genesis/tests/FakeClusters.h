#ifndef CLUSTER_TOOLS_APP_GENESIS_TESTS_FAKE_CLUSTERS_H
#define CLUSTER_TOOLS_APP_GENESIS_TESTS_FAKE_CLUSTERS_H

// An in-process stand-in for the ME and the core as the genesis tool sees them: the replicated facts it reads (ME
// epoch, journal flag, config ladder, gate, risk rows, limits record, balances) and the answers it waits for
// (AdminAck, DepositAck, the reports and snapshot ends), encoded with the same generated codecs the engines' wire
// uses. The rules are the ones the tool depends on, from the money schema's descriptions: CoreConfig records
// version n+1 (equal = DUPLICATE, else STALE), the global OPEN is refused ME_LANE_INACTIVE until a bridge bound the
// ME epoch, CoreLimitsSet and RiskConfigSet are monotonic, a deposit's requestId is remembered (a replay is a
// duplicate ack), SetMoneyJournal has no reply. A "kill" budget ends every link's sends after n commands (the
// process died; the clusters keep what they applied).

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "genesis/GenesisPlan.h"
#include "genesis/GenesisRunner.h"
#include "genesis/GenesisWire.h"

#include "com_openexchange_core_infrastructure_generated/AdminAck.h"
#include "com_openexchange_core_infrastructure_generated/BalanceSnapshotEnd.h"
#include "com_openexchange_core_infrastructure_generated/BalanceUpdateBatch.h"
#include "com_openexchange_core_infrastructure_generated/CoreConfig.h"
#include "com_openexchange_core_infrastructure_generated/CoreConfigReport.h"
#include "com_openexchange_core_infrastructure_generated/CoreLimitsSet.h"
#include "com_openexchange_core_infrastructure_generated/Deposit.h"
#include "com_openexchange_core_infrastructure_generated/DepositAck.h"
#include "com_openexchange_core_infrastructure_generated/FeedPositionReport.h"
#include "com_openexchange_core_infrastructure_generated/HoldSnapshotEnd.h"
#include "com_openexchange_core_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_core_infrastructure_generated/OrderEntryGate.h"
#include "com_openexchange_core_infrastructure_generated/OrderReject.h"
#include "com_openexchange_core_infrastructure_generated/OrderSnapshotEnd.h"
#include "com_openexchange_core_infrastructure_generated/QueryFeedPosition.h"
#include "com_openexchange_core_infrastructure_generated/RequestBalanceSnapshot.h"
#include "com_openexchange_core_infrastructure_generated/RequestCoreConfig.h"
#include "com_openexchange_core_infrastructure_generated/RequestHoldSnapshot.h"
#include "com_openexchange_core_infrastructure_generated/RequestOrderSnapshot.h"
#include "com_openexchange_core_infrastructure_generated/RiskConfigSet.h"
#include "com_openexchange_core_infrastructure_generated/SetMoneyJournal.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/EngineConfig.h"
#include "com_match_infrastructure_generated/MessageHeader.h"

namespace cluster_tools { namespace app { namespace genesis { namespace fake {

namespace money = com::openexchange::core::infrastructure::generated;
namespace mewire = com::match::infrastructure::generated;
using Bytes = std::vector<std::uint8_t>;

enum class Principal : std::uint8_t
{
    CORE_ADMIN,
    CORE_TOOL,
    ME_ADMIN,
};

struct Clusters
{
    // ---- ME
    std::int64_t meEpoch = 0;
    int engineConfigs = 0;
    // ---- core
    bool journal = false;
    std::uint32_t configVersion = 0;
    std::uint32_t behaviorVersion = 0;
    std::int64_t genesisId = 0;
    std::vector<std::pair<std::int32_t, std::string>> markets; // recorded CoreConfig markets
    std::size_t assets = 0;
    std::uint8_t gate = 1;            // SUSPENDED
    std::uint32_t gateReasons = 1u << 5; // GENESIS
    struct Risk
    {
        std::uint32_t version = 0;
        std::int64_t minQ = 0, maxQ = 0, minN = 0, maxN = 0;
    };
    std::map<std::int32_t, Risk> risk;
    std::uint32_t limitsVersion = 0;
    std::int64_t ttl = 900'000;
    std::int32_t cap = 256;
    std::int32_t ring = 65'536;
    std::map<std::pair<std::int64_t, std::int32_t>, std::int64_t> balances;
    std::set<std::pair<std::int64_t, std::int64_t>> requestIds;
    std::size_t orders = 0;
    std::int64_t lastTrade = 0;
    // ---- the bridge
    bool bridgeRunning = true;
    int bindAfterOpenTries = 3; // the OPEN tries refused before the bridge's 68 is in the log
    int openTries = 0;
    std::int64_t feedMeEpoch = 0;
    std::int64_t journalSeq = 0;
    // ---- what was sent, in order (template ids, per cluster) and the kill budget
    std::vector<std::uint16_t> coreCommands;
    std::vector<std::uint16_t> meCommands;
    std::int64_t sendBudget = -1; // -1 = unlimited; 0 = every later send fails (the process was killed)
    std::int64_t sends = 0;

    void bindIfBridged()
    {
        if (bridgeRunning && 0 != meEpoch && 0 == feedMeEpoch)
        {
            feedMeEpoch = meEpoch;
            journalSeq = 1;
        }
    }
};

inline Bytes adminAck(std::int64_t corr, std::uint16_t ref, std::uint8_t result, std::uint8_t reason, std::uint32_t version)
{
    Bytes b(64);
    money::AdminAck m;
    m.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(corr)
        .refTemplateId(ref)
        .result(money::AdminResult::get(result))
        .reason(reason)
        .configVersion(version);
    b.resize(money::MessageHeader::encodedLength() + m.encodedLength());
    return b;
}

inline Bytes orderReject(std::int64_t corr, std::uint16_t ref, std::uint8_t reason)
{
    Bytes b(128);
    money::OrderReject m;
    m.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
        .correlationId(corr)
        .requestIdHigh(0)
        .requestIdLow(0)
        .userId(0)
        .orderId(0)
        .marketId(0)
        .refTemplateId(ref)
        .reason(money::CoreRejectReason::get(reason))
        .meReason(255)
        .detail(0)
        .rejectedAtMs(0);
    b.resize(money::MessageHeader::encodedLength() + m.encodedLength());
    return b;
}

class FakeLink : public Link
{
public:
    FakeLink(Clusters &c, Principal p) : m_c(c), m_p(p) {}

    bool open(std::int64_t, std::string &) override
    {
        return true;
    }

    bool send(const wire::Bytes &frame, std::int64_t, std::string &error) override
    {
        if (0 == m_c.sendBudget)
        {
            error = "the process was killed (test)";
            return false;
        }
        if (m_c.sendBudget > 0)
        {
            m_c.sendBudget--;
        }
        m_c.sends++;
        handle(frame);
        return true;
    }

    bool pumpUntil(const FrameHandler &onFrame, std::int64_t timeoutMs, std::string &error) override
    {
        if (Principal::ME_ADMIN == m_p)
        {
            m_inbox.push_back(heartbeat()); // the keep-warm of an idle ME
        }
        while (!m_inbox.empty())
        {
            const Bytes f = std::move(m_inbox.front());
            m_inbox.pop_front();
            if (onFrame(f.data(), f.size()))
            {
                return true;
            }
        }
        if (0 != timeoutMs)
        {
            error = "fake: nothing more to read";
        }
        return false;
    }

    void pause(std::int64_t) override {}
    void close() override {}

    std::deque<Bytes> &inbox() { return m_inbox; }

private:
    Bytes heartbeat() const
    {
        Bytes b(64);
        mewire::ClusterHeartbeat m;
        m.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size()).nodeId(0).timestamp(1).meEpoch(m_c.meEpoch);
        b.resize(mewire::MessageHeader::encodedLength() + m.encodedLength());
        return b;
    }

    void handle(const wire::Bytes &frame)
    {
        const std::uint16_t t = wire::templateIdOf(frame.data(), frame.size());
        char *buf = const_cast<char *>(reinterpret_cast<const char *>(frame.data()));
        if (Principal::ME_ADMIN == m_p)
        {
            m_c.meCommands.push_back(t);
            mewire::MessageHeader h(buf, frame.size());
            if (mewire::EngineConfig::sbeTemplateId() == t)
            {
                mewire::EngineConfig m;
                m.wrapForDecode(buf, mewire::MessageHeader::encodedLength(), h.blockLength(), h.version(), frame.size());
                m_c.engineConfigs++;
                if (0 == m_c.meEpoch)
                {
                    m_c.meEpoch = m.genesisId();
                }
            }
            return;
        }
        m_c.coreCommands.push_back(t);
        money::MessageHeader h(buf, frame.size());
        const auto off = money::MessageHeader::encodedLength();
        auto in = [&](auto &m) { m.wrapForDecode(buf, off, h.blockLength(), h.version(), frame.size()); };
        const bool admin = Principal::CORE_ADMIN == m_p;
        switch (t)
        {
            case money::SetMoneyJournal::sbeTemplateId():
                if (admin)
                {
                    money::SetMoneyJournal m;
                    in(m);
                    m_c.journal = money::BoolFlag::TRUE == m.enabledRaw();
                }
                return;
            case money::CoreConfig::sbeTemplateId():
            {
                money::CoreConfig m;
                in(m);
                if (!admin)
                {
                    m_inbox.push_back(orderReject(m.correlationId(), t, 20));
                    return;
                }
                std::uint8_t result = 4; // STALE
                if (m.configVersion() == m_c.configVersion + 1)
                {
                    result = 0;
                    m_c.configVersion = m.configVersion();
                    m_c.behaviorVersion = m.behaviorVersion();
                    m_c.genesisId = m.genesisId();
                    m_c.markets.clear();
                    auto &markets = m.markets();
                    while (markets.hasNext())
                    {
                        markets.next();
                        m_c.markets.emplace_back(markets.marketId(), markets.getSymbolAsString());
                    }
                    m_c.assets = m.assets().count();
                }
                else if (m.configVersion() == m_c.configVersion)
                {
                    result = 1;
                }
                m_inbox.push_back(adminAck(m.correlationId(), t, result, 0, m_c.configVersion));
                return;
            }
            case money::OrderEntryGate::sbeTemplateId():
            {
                money::OrderEntryGate m;
                in(m);
                m_c.openTries++;
                if (m_c.openTries > m_c.bindAfterOpenTries)
                {
                    m_c.bindIfBridged();
                }
                if (0 == m_c.feedMeEpoch || m_c.configVersion < 3)
                {
                    m_inbox.push_back(adminAck(m.correlationId(), t, 2, 41, 0)); // REJECTED ME_LANE_INACTIVE
                    return;
                }
                const bool already = 0 == m_c.gate;
                m_c.gate = 0;
                m_c.gateReasons = 0;
                m_inbox.push_back(adminAck(m.correlationId(), t, already ? 1 : 0, 0, 0));
                return;
            }
            case money::RiskConfigSet::sbeTemplateId():
            {
                money::RiskConfigSet m;
                in(m);
                const bool known = std::any_of(m_c.markets.begin(), m_c.markets.end(),
                    [&](const auto &row) { return row.first == m.marketId(); });
                Clusters::Risk &r = m_c.risk[m.marketId()];
                std::uint8_t result = 4;
                if (!known)
                {
                    result = 2;
                }
                else if (m.configVersion() > r.version)
                {
                    result = 0;
                    r = { m.configVersion(), m.minQuantity(), m.maxQuantity(), m.minNotional(), m.maxNotional() };
                }
                else if (m.configVersion() == r.version)
                {
                    result = 1;
                }
                m_inbox.push_back(adminAck(m.correlationId(), t, result, 0, r.version));
                return;
            }
            case money::CoreLimitsSet::sbeTemplateId():
            {
                money::CoreLimitsSet m;
                in(m);
                std::uint8_t result = 4;
                if (m.configVersion() > m_c.limitsVersion)
                {
                    result = 0;
                    m_c.limitsVersion = m.configVersion();
                    m_c.ttl = m.requestTtlMs();
                    m_c.cap = m.requestPerUserCap();
                    m_c.ring = m.orderEventRingCapacity();
                }
                else if (m.configVersion() == m_c.limitsVersion)
                {
                    result = 1;
                }
                m_inbox.push_back(adminAck(m.correlationId(), t, result, 0, m_c.limitsVersion));
                return;
            }
            case money::Deposit::sbeTemplateId():
            {
                money::Deposit m;
                in(m);
                const bool duplicate = !m_c.requestIds.insert({ m.requestIdHigh(), m.requestIdLow() }).second;
                if (!duplicate)
                {
                    m_c.balances[{ m.userId(), m.assetId() }] += m.amount();
                }
                Bytes b(96);
                money::DepositAck a;
                a.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
                    .correlationId(m.correlationId())
                    .userId(m.userId())
                    .assetId(m.assetId())
                    .amount(m.amount())
                    .newAvailable(m_c.balances[{ m.userId(), m.assetId() }])
                    .duplicate(duplicate ? money::BoolFlag::TRUE : money::BoolFlag::FALSE);
                b.resize(money::MessageHeader::encodedLength() + a.encodedLength());
                m_inbox.push_back(b);
                return;
            }
            case money::QueryFeedPosition::sbeTemplateId():
            {
                money::QueryFeedPosition q;
                in(q);
                Bytes b(128);
                money::FeedPositionReport r;
                r.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
                    .correlationId(q.correlationId())
                    .consumePosition(0)
                    .lastAppliedTradeId(m_c.lastTrade)
                    .journalEnabled(m_c.journal ? money::BoolFlag::TRUE : money::BoolFlag::FALSE)
                    .meEpoch(m_c.feedMeEpoch)
                    .lastAppliedJournalSeq(m_c.journalSeq)
                    .feedHalted(money::BoolFlag::FALSE)
                    .haltReason(money::FeedHaltReason::NONE);
                b.resize(money::MessageHeader::encodedLength() + r.encodedLength());
                m_inbox.push_back(b);
                return;
            }
            case money::RequestCoreConfig::sbeTemplateId():
            {
                money::RequestCoreConfig q;
                in(q);
                Bytes b(4096);
                money::CoreConfigReport r;
                r.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size())
                    .correlationId(q.correlationId())
                    .configVersion(m_c.configVersion)
                    .behaviorVersion(m_c.behaviorVersion)
                    .limitsVersion(m_c.limitsVersion)
                    .requestTtlMs(m_c.ttl)
                    .requestPerUserCap(m_c.cap)
                    .orderEventRingCapacity(m_c.ring)
                    .gateState(money::GateState::get(m_c.gate));
                r.gateReasons().rawValue(m_c.gateReasons);
                r.asOfLogPosition(1).genesisId(m_c.genesisId);
                auto &markets = r.marketsCount(static_cast<std::uint16_t>(m_c.markets.size()));
                for (const auto &[id, symbol] : m_c.markets)
                {
                    const Clusters::Risk risk = m_c.risk.count(id) ? m_c.risk.at(id) : Clusters::Risk{};
                    markets.next()
                        .marketId(id)
                        .putSymbol(std::string_view(symbol))
                        .baseAssetId(id)
                        .quoteAssetId(0)
                        .tickSize(1)
                        .minPrice(1)
                        .maxPrice(2)
                        .lotSize(0)
                        .minQuantity(risk.minQ)
                        .maxQuantity(risk.maxQ)
                        .minNotional(risk.minN)
                        .maxNotional(risk.maxN)
                        .riskConfigVersion(risk.version)
                        .gateState(money::GateState::get(m_c.gate));
                    markets.gateReasons().rawValue(0);
                    markets.circuitBreaker(money::BoolFlag::FALSE);
                }
                auto &assets = r.assetsCount(static_cast<std::uint16_t>(m_c.assets));
                for (std::size_t i = 0; i < m_c.assets; ++i)
                {
                    assets.next().assetId(static_cast<std::int32_t>(i)).putCode(std::string_view("A")).scale(8);
                }
                b.resize(money::MessageHeader::encodedLength() + r.encodedLength());
                m_inbox.push_back(b);
                return;
            }
            case money::RequestBalanceSnapshot::sbeTemplateId():
            {
                money::RequestBalanceSnapshot q;
                in(q);
                Bytes b(64 + 28 * m_c.balances.size());
                money::BalanceUpdateBatch batch;
                batch.wrapAndApplyHeader(reinterpret_cast<char *>(b.data()), 0, b.size());
                auto &u = batch.updatesCount(static_cast<std::uint16_t>(m_c.balances.size()));
                for (const auto &[key, amount] : m_c.balances)
                {
                    u.next().userId(key.first).assetId(key.second).available(amount).locked(0);
                }
                b.resize(money::MessageHeader::encodedLength() + batch.encodedLength());
                if (!m_c.balances.empty())
                {
                    m_inbox.push_back(b);
                }
                Bytes e(64);
                money::BalanceSnapshotEnd end;
                end.wrapAndApplyHeader(reinterpret_cast<char *>(e.data()), 0, e.size())
                    .correlationId(q.correlationId())
                    .entryCount(static_cast<std::int32_t>(m_c.balances.size()));
                e.resize(money::MessageHeader::encodedLength() + end.encodedLength());
                m_inbox.push_back(e);
                return;
            }
            case money::RequestHoldSnapshot::sbeTemplateId():
            {
                money::RequestHoldSnapshot q;
                in(q);
                Bytes e(64);
                money::HoldSnapshotEnd end;
                end.wrapAndApplyHeader(reinterpret_cast<char *>(e.data()), 0, e.size()).correlationId(q.correlationId()).entryCount(0);
                e.resize(money::MessageHeader::encodedLength() + end.encodedLength());
                m_inbox.push_back(e);
                return;
            }
            case money::RequestOrderSnapshot::sbeTemplateId():
            {
                money::RequestOrderSnapshot q;
                in(q);
                Bytes e(64);
                money::OrderSnapshotEnd end;
                end.wrapAndApplyHeader(reinterpret_cast<char *>(e.data()), 0, e.size())
                    .correlationId(q.correlationId())
                    .entryCount(static_cast<std::int32_t>(m_c.orders))
                    .nextOrderId(0)
                    .asOfLogPosition(1);
                e.resize(money::MessageHeader::encodedLength() + end.encodedLength());
                m_inbox.push_back(e);
                return;
            }
            default:
                return;
        }
    }

    Clusters &m_c;
    Principal m_p;
    std::deque<Bytes> m_inbox;
};

class FakeLinks : public LinkFactory
{
public:
    explicit FakeLinks(Clusters &c) : m_c(c) {}
    std::unique_ptr<Link> coreAdmin() override { return std::make_unique<FakeLink>(m_c, Principal::CORE_ADMIN); }
    std::unique_ptr<Link> coreTool() override { return std::make_unique<FakeLink>(m_c, Principal::CORE_TOOL); }
    std::unique_ptr<Link> meAdmin() override { return std::make_unique<FakeLink>(m_c, Principal::ME_ADMIN); }

private:
    Clusters &m_c;
};

class MemoryStore : public ActivationStore
{
public:
    ActivationSeen read() override
    {
        ActivationSeen s;
        if (text.empty())
        {
            return s;
        }
        const auto at = text.find("\"genesisId\": ");
        s.present = std::string::npos != at;
        if (s.present)
        {
            s.genesisId = std::stoll(text.substr(at + 13));
        }
        return s;
    }
    bool write(const std::string &t, std::string &) override
    {
        text = t;
        writes++;
        return true;
    }
    std::string text;
    int writes = 0;
};

}}}}

#endif
