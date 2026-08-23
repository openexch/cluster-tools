#include "cluster_tools/domain/EngineConfig.h"

#include <algorithm>
#include <climits>

namespace cluster_tools { namespace domain {

namespace
{

// uint32 wire bounds; INT32_MAX is the engine's cap for the count-like fields
// (EngineConfigState.invalidReason bounds them to Integer.MAX_VALUE).
constexpr std::int64_t UINT32_MAX_VALUE = 4294967295LL;
constexpr std::int64_t INT32_MAX_VALUE = INT32_MAX;

std::string marketAt(std::size_t index, const MarketDef &market)
{
    return "markets[" + std::to_string(index) + "] (marketId=" +
        std::to_string(market.marketId) + ")";
}

}

EngineConfig normalized(EngineConfig config)
{
    // Stable, like the engine's normalization (EngineConfigState.of): equal marketIds
    // keep their relative order, so the duplicate check names the same entry.
    std::stable_sort(config.markets.begin(), config.markets.end(),
        [](const MarketDef &a, const MarketDef &b)
        {
            return a.marketId < b.marketId;
        });
    return config;
}

std::string invalidReason(const EngineConfig &config)
{
    // TOOL-SIDE rule (see the header): the engine records any uint32, 0 included; this
    // tool refuses the ambiguous 0 and anything a uint32 cannot carry.
    if (config.configVersion < 1 || config.configVersion > UINT32_MAX_VALUE)
    {
        return "configVersion=" + std::to_string(config.configVersion) +
            " must be in [1, " + std::to_string(UINT32_MAX_VALUE) +
            "] (tool-side rule: 0 is indistinguishable from unset on the uint32 wire field)";
    }

    // From here on: the engine's own rules, in the engine's order.
    if (EngineImpl::ARRAY != config.impl && EngineImpl::DIRECT != config.impl)
    {
        return "impl is missing or unknown (must be array or direct)";
    }
    if (config.maxMatchesPerOrder < 1 || config.maxMatchesPerOrder > INT32_MAX_VALUE)
    {
        return "maxMatchesPerOrder=" + std::to_string(config.maxMatchesPerOrder) +
            " must be in [1, " + std::to_string(INT32_MAX_VALUE) + "]";
    }
    if (EngineImpl::ARRAY == config.impl)
    {
        if (config.bookCapacity < 1 || config.bookCapacity > INT32_MAX_VALUE)
        {
            return "bookCapacity=" + std::to_string(config.bookCapacity) +
                " must be in [1, " + std::to_string(INT32_MAX_VALUE) +
                "] for impl=array (its single capacity bound)";
        }
        if (0 != config.maxOrdersPerLevel)
        {
            return "maxOrdersPerLevel=" + std::to_string(config.maxOrdersPerLevel) +
                " must be 0 for impl=array (the array book has no per-level cap)";
        }
    }
    else // direct
    {
        if (0 != config.bookCapacity)
        {
            return "bookCapacity=" + std::to_string(config.bookCapacity) +
                " must be 0 for impl=direct (the direct book has no book-wide capacity knob)";
        }
        if (config.maxOrdersPerLevel < 1 || config.maxOrdersPerLevel > INT32_MAX_VALUE)
        {
            return "maxOrdersPerLevel=" + std::to_string(config.maxOrdersPerLevel) +
                " must be in [1, " + std::to_string(INT32_MAX_VALUE) + "] for impl=direct";
        }
    }
    if (config.markets.empty())
    {
        return "markets is empty — a cluster with no markets cannot create engines";
    }

    // Market checks run on the normalized (sorted) view, exactly like the engine: it
    // sorts on decode before validating, so wire order can never change the outcome.
    const std::vector<MarketDef> markets = normalized(config).markets;
    for (std::size_t i = 0; i < markets.size(); i++)
    {
        const MarketDef &market = markets[i];
        const std::string at = marketAt(i, market);
        if (market.marketId <= 0)
        {
            return at + ": marketId must be positive";
        }
        if (i > 0 && markets[i - 1].marketId == market.marketId)
        {
            return at + ": duplicate marketId";
        }
        if (market.symbol.empty() || market.symbol.size() > 16)
        {
            return at + ": symbol must be 1..16 ASCII chars (was \"" + market.symbol + "\")";
        }
        for (std::size_t c = 0; c < market.symbol.size(); c++)
        {
            // unsigned compare: a UTF-8 byte in a (signed) char must land in the
            // reject branch, not wrap negative past it. 0x21..0x7E = printable ASCII
            // minus space; NUL specifically is the wire's padding byte.
            const unsigned char ch = static_cast<unsigned char>(market.symbol[c]);
            if (ch < 0x21 || ch > 0x7E)
            {
                return at + ": symbol contains a non-printable or non-ASCII char at index " +
                    std::to_string(c);
            }
        }
        if (market.tickSize <= 0)
        {
            return at + ": tickSize=" + std::to_string(market.tickSize) + " must be positive";
        }
        if (market.minPrice <= 0)
        {
            return at + ": minPrice=" + std::to_string(market.minPrice) + " must be positive";
        }
        if (market.maxPrice <= market.minPrice)
        {
            return at + ": maxPrice=" + std::to_string(market.maxPrice) +
                " must exceed minPrice=" + std::to_string(market.minPrice);
        }
        // No overflow: 0 < minPrice < maxPrice implies the difference fits int64, and
        // tickSize >= 1 keeps the quotient at or below it.
        const std::int64_t levels = (market.maxPrice - market.minPrice) / market.tickSize + 1;
        if (levels > INT32_MAX_VALUE)
        {
            return at + ": (maxPrice-minPrice)/tickSize+1 = " + std::to_string(levels) +
                " price levels does not fit an int";
        }
    }
    return std::string();
}

const char *implName(EngineImpl impl) noexcept
{
    switch (impl)
    {
        case EngineImpl::ARRAY: return "array";
        case EngineImpl::DIRECT: return "direct";
        case EngineImpl::UNKNOWN: return "unknown";
    }
    return "?";
}

}}
