#ifndef CLUSTER_TOOLS_APP_GENESIS_GENESIS_WIRE_H
#define CLUSTER_TOOLS_APP_GENESIS_GENESIS_WIRE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "genesis/GenesisPlan.h"
#include "genesis/GenesisState.h"

// The genesis tool's wire: the money-schema (core) commands and replies it uses, and the ME EngineConfig and
// ClusterHeartbeat (match order-schema). Every frame is an engine-schema payload (MessageHeader + body); the cluster
// client adds the session header. The only translation unit that includes the generated codecs of the tool; the header
// pulls in none. Decoding never throws: a frame that does not decode is MALFORMED.

namespace cluster_tools { namespace app { namespace genesis { namespace wire {

using Bytes = std::vector<std::uint8_t>;

// AdminAck.result (AdminResult)
inline constexpr std::uint8_t ADMIN_ACCEPTED = 0;
inline constexpr std::uint8_t ADMIN_DUPLICATE = 1;
inline constexpr std::uint8_t ADMIN_REJECTED = 2;
inline constexpr std::uint8_t ADMIN_ADOPTED = 3;
inline constexpr std::uint8_t ADMIN_STALE = 4;
const char *adminResultName(std::uint8_t result) noexcept;
// CoreRejectReason names the AdminAck.reason uses (e.g. 40 FEED_HALTED, 41 ME_LANE_INACTIVE)
const char *coreReasonName(std::uint8_t reason) noexcept;

// Template ids the runner names (money schema).
inline constexpr std::uint16_t T_CORE_CONFIG = 70;
inline constexpr std::uint16_t T_RISK_CONFIG_SET = 71;
inline constexpr std::uint16_t T_ORDER_ENTRY_GATE = 73;
inline constexpr std::uint16_t T_CORE_LIMITS_SET = 77;
inline constexpr std::uint16_t T_DEPOSIT = 1;
inline constexpr std::uint16_t T_SET_MONEY_JOURNAL = 32;
inline constexpr std::uint16_t T_INIT_TRADE_HIGH_WATER = 30; // never encoded by this tool

// ---- encode (core) ----
Bytes setMoneyJournal(std::int64_t correlationId, bool enabled);
Bytes coreConfig(std::int64_t correlationId, std::uint32_t configVersion, std::uint32_t behaviorVersion,
    const GenesisPlan &plan);
Bytes orderEntryGateOpen(std::int64_t correlationId, std::int32_t marketId);
Bytes riskConfigSet(std::int64_t correlationId, std::uint32_t configVersion, const RiskRow &row);
Bytes coreLimitsSet(std::int64_t correlationId, const Limits &limits);
Bytes deposit(std::int64_t correlationId, const Funding &row);
Bytes queryFeedPosition(std::int64_t correlationId);
Bytes requestCoreConfig(std::int64_t correlationId);
Bytes requestBalanceSnapshot(std::int64_t correlationId);
Bytes requestHoldSnapshot(std::int64_t correlationId);
Bytes requestOrderSnapshot(std::int64_t correlationId, std::int64_t fromOrderId);

// ---- encode (ME) ----
// The EngineConfig of the plan (match template 8, wire v12), or an empty vector with `error`.
Bytes engineConfig(const GenesisPlan &plan, std::string &error);

// ---- decode ----
struct CoreFrame
{
    enum class Kind : std::uint8_t
    {
        OTHER,
        MALFORMED,
        ADMIN_ACK,
        DEPOSIT_ACK,
        DEPOSIT_REJECT,
        ORDER_REJECT,
        FEED_POSITION,
        CORE_CONFIG_REPORT,
        BALANCE_ROWS,
        BALANCE_END,
        HOLD_ROWS,
        HOLD_END,
        ORDER_ROWS,
        ORDER_END,
    };
    Kind kind = Kind::OTHER;
    std::uint16_t templateId = 0;
    std::int64_t correlationId = 0;
    // ADMIN_ACK
    std::uint16_t refTemplateId = 0;
    std::uint8_t result = 0;
    std::uint8_t reason = 0;
    std::uint32_t configVersion = 0;
    // DEPOSIT_ACK / DEPOSIT_REJECT
    bool duplicate = false;
    // FEED_POSITION / CORE_CONFIG_REPORT: copied into this
    CoreObserved report;
    // BALANCE_ROWS: (user, asset, available, locked)
    struct Balance
    {
        std::int64_t userId;
        std::int32_t assetId;
        std::int64_t available;
        std::int64_t locked;
    };
    std::vector<Balance> balances;
    // HOLD_ROWS / ORDER_ROWS: the row count; *_END: entryCount, ORDER_END: nextOrderId
    std::int64_t count = 0;
    std::int64_t nextOrderId = 0;
    std::string problem; // MALFORMED
};

CoreFrame decodeCore(const std::uint8_t *data, std::size_t length);

struct MeFrame
{
    enum class Kind : std::uint8_t
    {
        OTHER,
        MALFORMED,
        HEARTBEAT,
    };
    Kind kind = Kind::OTHER;
    std::int64_t meEpoch = 0;
};

MeFrame decodeMe(const std::uint8_t *data, std::size_t length);

// The template id of an engine payload of either schema (0 when shorter than a header). Tests use it.
std::uint16_t templateIdOf(const std::uint8_t *data, std::size_t length) noexcept;

}}}}

#endif
