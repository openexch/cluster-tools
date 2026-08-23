#ifndef CLUSTER_TOOLS_ADAPTERS_JSON_ENGINE_CONFIG_JSON_H
#define CLUSTER_TOOLS_ADAPTERS_JSON_ENGINE_CONFIG_JSON_H

#include <string>

#include "cluster_tools/domain/EngineConfig.h"
#include "cluster_tools/ports/Result.h"

// EngineConfigJson — the input adapter that turns an operator's engine-config JSON
// document into the domain value type. Schema-directed and hand-rolled (~250 lines):
// this repo takes no dependencies, and the document has exactly one known shape, so a
// generic JSON DOM would be more code, not less. Builds and tests with zero
// infrastructure — an adapter, but not a transport one.
//
// The document (all fields REQUIRED — refusing ambiguity beats defaulting it):
//
//   {
//     "configVersion": 1,
//     "impl": "array",                // or "direct"
//     "bookCapacity": 131072,         // impl=array only; 0 for direct
//     "maxMatchesPerOrder": 10000,
//     "maxOrdersPerLevel": 0,         // impl=direct only; 0 for array
//     "markets": [
//       { "marketId": 1, "symbol": "BTC-USD",
//         "minPrice": 5000000000000, "maxPrice": 15000000000000,
//         "tickSize": 100000000 }     // prices fixed-point, 8 decimals, raw int64
//     ]
//   }
//
// Strictness contract (the strict-config house style — a silent default or a skipped
// typo'd key hides an error for hours):
//   - every key above is required; a missing one is an error naming it
//   - unknown and duplicate keys are errors naming the key
//   - numeric fields are JSON integers: floats, exponents and leading zeros refused
//   - impl must be exactly "array" or "direct"
//   - anything after the closing brace except whitespace is an error
//
// SHAPE only. Semantic validity (ranges, market rules) is the domain's
// invalidReason(); this parser never second-guesses it. Errors carry a byte offset.
// No exception crosses the boundary — failures come back as Result.

namespace cluster_tools { namespace json_adapter {

ports::Result<domain::EngineConfig> parseEngineConfig(const std::string &text);

}}

#endif
