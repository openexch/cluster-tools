#ifndef CLUSTER_TOOLS_DOMAIN_ENGINE_CONFIG_H
#define CLUSTER_TOOLS_DOMAIN_ENGINE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

// EngineConfig — the engine-creation config a `send-config` command puts on the wire
// (match order-schema v10, template 8), as a pure domain value with deterministic
// validation. This header knows nothing about JSON, SBE, or Aeron: parsing a config
// file into this type is an input adapter's job, encoding it onto the wire is the SBE
// adapter's job, and THIS is where the rules live.
//
// The rules mirror match's engine-side validation (EngineConfigState.invalidReason on
// match main) so the tool refuses, before anything touches the network, exactly what
// the cluster would deterministically REJECT — one vocabulary, same reasons:
//
//   impl                is array or direct (never unknown)
//   maxMatchesPerOrder  in [1, INT32_MAX]
//   impl=array          bookCapacity in [1, INT32_MAX], maxOrdersPerLevel == 0
//                       (the array book has no per-level cap)
//   impl=direct         bookCapacity == 0, maxOrdersPerLevel in [1, INT32_MAX]
//                       (the direct book has no book-wide capacity knob)
//   markets             at least one; per market: marketId > 0, unique, symbol 1..16
//                       printable-ASCII chars, tickSize > 0, minPrice > 0,
//                       maxPrice > minPrice, and (maxPrice-minPrice)/tickSize+1
//                       price levels must fit an int32
//
// One rule is TOOL-SIDE ONLY and deliberately stricter than the engine:
//
//   configVersion       in [1, UINT32_MAX]. The engine (as of match cd85640) records
//                       whatever uint32 arrives, 0 included. This tool refuses 0 —
//                       on a uint32 wire field, 0 is indistinguishable from "field
//                       never set", and an ambiguous config generation is exactly the
//                       kind of silent default that hides errors. The upper bound is
//                       wire representability: a value that cannot ride a uint32
//                       must fail here, not truncate silently in the encoder.
//
// Like the engine, validation runs on the NORMALIZED view: the engine sorts markets by
// marketId the moment a command is decoded (EngineConfigState.of), so wire order can
// never change an outcome — invalidReason() normalizes internally for the same reason.
// Callers still encode `normalized(config)` so the bytes sent match the view validated
// and the indices in any reason text.

namespace cluster_tools { namespace domain {

// Order-book implementation selector — the wire's EngineImpl vocabulary plus UNKNOWN
// for "did not parse/decode to either", which validation rejects like the engine does.
enum class EngineImpl : std::uint8_t
{
    ARRAY = 0,
    DIRECT = 1,
    UNKNOWN = 255
};

// One market's engine-creation values. Prices are fixed-point with 8 decimals, the
// convention of every price field on this wire.
struct MarketDef
{
    std::int32_t marketId = 0;
    std::string symbol;             // 1..16 printable-ASCII chars on a valid config
    std::int64_t minPrice = 0;      // the engine's basePrice
    std::int64_t maxPrice = 0;
    std::int64_t tickSize = 0;
};

// The full engine-creation config. uint32 wire fields are carried as int64 so any
// parsed/decoded value is representable without truncation; range is validation's job.
struct EngineConfig
{
    std::int64_t configVersion = 0;
    EngineImpl impl = EngineImpl::UNKNOWN;
    std::int64_t bookCapacity = 0;
    std::int64_t maxMatchesPerOrder = 0;
    std::int64_t maxOrdersPerLevel = 0;
    std::vector<MarketDef> markets;
};

// The canonical form: markets sorted ascending by marketId (stable, mirroring the
// engine's normalization). Encode THIS, so the wire carries what was validated.
EngineConfig normalized(EngineConfig config);

// Deterministic structural validation of the normalized view — a pure function of the
// config's fields. Returns the empty string when the config is valid, otherwise a
// human-readable reason the engine would REJECT it (or, for the configVersion rule,
// why this tool refuses to send it).
std::string invalidReason(const EngineConfig &config);

// "array" / "direct" / "unknown" — the engine's impl vocabulary, for messages.
const char *implName(EngineImpl impl) noexcept;

}}

#endif
