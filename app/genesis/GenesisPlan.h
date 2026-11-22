#ifndef CLUSTER_TOOLS_APP_GENESIS_GENESIS_PLAN_H
#define CLUSTER_TOOLS_APP_GENESIS_GENESIS_PLAN_H

#include <cstdint>
#include <string>
#include <vector>

#include "cluster_tools/domain/EngineConfig.h"

// The genesis input file: one JSON document that says everything the phases send. Strict (adapters/json house
// style): every key is required unless stated, unknown and duplicate keys are refused by name, numbers are integers.
//
//   {
//     "genesisId": 7301...,                 // [1, 2^63-1], the ME meEpoch and the core genesisId (one value)
//     "me": { the EngineConfig of adapters/json without "genesisId" (configVersion, impl, bookCapacity,
//             maxMatchesPerOrder, maxOrdersPerLevel, behaviorVersion, commandLedgerCapacity, maxWireVersion,
//             markets[{marketId, symbol, minPrice, maxPrice, tickSize}]) },
//     "core": { "markets": [{marketId, symbol, baseAssetId, quoteAssetId, tickSize, minPrice, maxPrice, lotSize}],
//               "assets":  [{assetId, code, scale}] },
//     "risk": { "configVersion": 1,
//               "markets": [{marketId, minQuantity, maxQuantity, minNotional, maxNotional, priceCollarBps,
//                            maxOrdersPerSec, maxOrdersPerMin, maxOpenOrders, maxPositionPerMarket,
//                            marketBuySlippageBps}] },
//     "limits": { "configVersion": 1, "requestTtlMs": 900000, "requestPerUserCap": 2048,
//                 "orderEventRingCapacity": 65536 },          // optional block, each key optional (these defaults)
//     "funding": [{userId, assetId, amount, requestIdHigh, requestIdLow}]
//   }
//
// The core ladder is fixed: CoreConfig 1, 2, 3 (configVersion = behaviorVersion = n, the genesisId, the core tables),
// then, after the phases, one more CoreConfig at ACTIVATION_BEHAVIOR with the same tables. The ME record is the one
// the cut's genesis block names: behaviour 2, maxWireVersion 12. InitTradeHighWater is never part of a genesis: a key
// that names it anywhere in the document is refused with INIT_TRADE_HIGH_WATER_REFUSAL.

namespace cluster_tools { namespace app { namespace genesis {

inline constexpr const char *INIT_TRADE_HIGH_WATER_REFUSAL = "InitTradeHighWater is not part of a genesis";

inline constexpr std::uint32_t CORE_LADDER_TOP = 3;       // CoreConfig 1 -> 2 -> 3
inline constexpr std::uint32_t ACTIVATION_BEHAVIOR = 4;   // the step after the phases
inline constexpr std::int64_t ME_GENESIS_BEHAVIOR = 2;
inline constexpr std::int64_t ME_MAX_WIRE_VERSION = 12;

// The limits record defaults: the core's genesis record (CoreLimitsSet description) with the demo request cap.
inline constexpr std::uint32_t DEFAULT_LIMITS_VERSION = 1;
inline constexpr std::int64_t DEFAULT_REQUEST_TTL_MS = 900'000;
inline constexpr std::int32_t DEFAULT_REQUEST_PER_USER_CAP = 2048;
inline constexpr std::int32_t DEFAULT_ORDER_EVENT_RING_CAPACITY = 65'536;
inline constexpr std::int32_t MAX_ORDER_EVENT_RING_CAPACITY = 12'859'183; // CoreLimitsSet: the core refuses above

struct CoreMarket
{
    std::int32_t marketId = 0;
    std::string symbol;
    std::int32_t baseAssetId = 0;
    std::int32_t quoteAssetId = 0;
    std::int64_t tickSize = 0;
    std::int64_t minPrice = 0;
    std::int64_t maxPrice = 0;
    std::int64_t lotSize = 0;
};

struct CoreAsset
{
    std::int32_t assetId = 0;
    std::string code;
    std::uint8_t scale = 0;
};

struct RiskRow
{
    std::int32_t marketId = 0;
    std::int64_t minQuantity = 0;
    std::int64_t maxQuantity = 0;
    std::int64_t minNotional = 0;
    std::int64_t maxNotional = 0;
    std::uint32_t priceCollarBps = 0;
    std::int32_t maxOrdersPerSec = 0;
    std::int32_t maxOrdersPerMin = 0;
    std::int32_t maxOpenOrders = 0;
    std::int64_t maxPositionPerMarket = 0;
    std::uint32_t marketBuySlippageBps = 0;
};

struct Limits
{
    std::uint32_t configVersion = DEFAULT_LIMITS_VERSION;
    std::int64_t requestTtlMs = DEFAULT_REQUEST_TTL_MS;
    std::int32_t requestPerUserCap = DEFAULT_REQUEST_PER_USER_CAP;
    std::int32_t orderEventRingCapacity = DEFAULT_ORDER_EVENT_RING_CAPACITY;
};

struct Funding
{
    std::int64_t userId = 0;
    std::int32_t assetId = 0;
    std::int64_t amount = 0;
    std::int64_t requestIdHigh = 0;
    std::int64_t requestIdLow = 0;
};

struct GenesisPlan
{
    std::int64_t genesisId = 0;
    domain::EngineConfig me;          // normalized, genesisId = genesisId
    std::vector<CoreMarket> markets;  // ascending marketId
    std::vector<CoreAsset> assets;    // ascending assetId
    std::uint32_t riskConfigVersion = 0;
    std::vector<RiskRow> risk;        // ascending marketId
    Limits limits;
    std::vector<Funding> funding;     // in document order (the order the deposits are sent)
};

struct PlanResult
{
    GenesisPlan plan;
    std::vector<std::string> errors; // every problem found, in document order
    bool ok() const { return errors.empty(); }
};

PlanResult parsePlan(const std::string &text);

}}}

#endif
