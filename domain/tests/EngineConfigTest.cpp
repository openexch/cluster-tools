#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <climits>
#include <cstdint>
#include <string>

#include "cluster_tools/domain/EngineConfig.h"

// The validation matrix for the engine-creation config: one case per rule (each rule
// violated once), a valid config of each impl, and the normalization contract. The
// rules mirror match's EngineConfigState.invalidReason (engine side), plus the ONE
// deliberately tool-side rule (configVersion) — see the header for why.

using cluster_tools::domain::EngineConfig;
using cluster_tools::domain::EngineImpl;
using cluster_tools::domain::MarketDef;
using cluster_tools::domain::implName;
using cluster_tools::domain::invalidReason;
using cluster_tools::domain::normalized;

namespace
{

MarketDef btc()
{
    return { 1, "BTC-USD", 5'000'000'000'000LL, 15'000'000'000'000LL, 100'000'000LL };
}

MarketDef eth()
{
    return { 2, "ETH-USD", 100'000'000'000LL, 1'000'000'000'000LL, 50'000'000LL };
}

// A valid array-impl config (the demo shape: today's compiled market table's head).
EngineConfig validArray()
{
    EngineConfig config;
    config.configVersion = 1;
    config.impl = EngineImpl::ARRAY;
    config.bookCapacity = 131072;
    config.maxMatchesPerOrder = 10000;
    config.maxOrdersPerLevel = 0;
    config.behaviorVersion = 1;
    config.commandLedgerCapacity = 100000;
    config.genesisId = 0x0102030405060708LL;
    config.maxWireVersion = 12;
    config.markets = { btc(), eth() };
    return config;
}

EngineConfig validDirect()
{
    EngineConfig config;
    config.configVersion = 7;
    config.impl = EngineImpl::DIRECT;
    config.bookCapacity = 0;
    config.maxMatchesPerOrder = 10000;
    config.maxOrdersPerLevel = 64;
    config.behaviorVersion = 2;
    config.commandLedgerCapacity = 1;
    config.genesisId = -1;
    config.maxWireVersion = 11;
    config.markets = { btc() };
    return config;
}

}

TEST_CASE("a valid config of each impl has no invalid reason")
{
    CHECK(invalidReason(validArray()) == "");
    CHECK(invalidReason(validDirect()) == "");
}

TEST_CASE("configVersion must be a positive uint32 (tool-side rule)")
{
    // The engine records any uint32 configVersion, 0 included; the TOOL refuses 0
    // (indistinguishable from unset) and anything a uint32 cannot carry.
    EngineConfig config = validArray();

    config.configVersion = 0;
    CHECK(invalidReason(config).find("configVersion=0") == 0);

    config.configVersion = -1;
    CHECK_FALSE(invalidReason(config).empty());

    config.configVersion = 4294967295LL; // uint32 max is representable...
    CHECK(invalidReason(config) == "");

    config.configVersion = 4294967296LL; // ...its successor is not
    CHECK_FALSE(invalidReason(config).empty());
}

TEST_CASE("behaviorVersion must be in [1, UINT32_MAX] (tool-side, RC2 S1)")
{
    EngineConfig config = validArray();
    config.behaviorVersion = 0;
    CHECK(invalidReason(config).find("behaviorVersion=0") == 0);
    config.behaviorVersion = -1;
    CHECK_FALSE(invalidReason(config).empty());
    config.behaviorVersion = 4294967295LL;
    CHECK(invalidReason(config) == "");
    config.behaviorVersion = 4294967296LL;
    CHECK(invalidReason(config).find("behaviorVersion=") == 0);
}

TEST_CASE("commandLedgerCapacity must be in [1, UINT32_MAX] (tool-side, RC2 S1)")
{
    EngineConfig config = validArray();
    config.commandLedgerCapacity = 0;
    CHECK(invalidReason(config).find("commandLedgerCapacity=0") == 0);
    config.commandLedgerCapacity = -1;
    CHECK_FALSE(invalidReason(config).empty());
    config.commandLedgerCapacity = 1;
    CHECK(invalidReason(config) == "");
    config.commandLedgerCapacity = 4294967295LL;
    CHECK(invalidReason(config) == "");
    config.commandLedgerCapacity = 4294967296LL;
    CHECK(invalidReason(config).find("commandLedgerCapacity=") == 0);
}

TEST_CASE("genesisId must be non-zero; any other int64 is a valid nonce (tool-side, RC2 S1)")
{
    EngineConfig config = validArray();
    config.genesisId = 0;
    CHECK(invalidReason(config).find("genesisId must be non-zero") == 0);
    config.genesisId = 1;
    CHECK(invalidReason(config) == "");
    config.genesisId = -1;
    CHECK(invalidReason(config) == "");
    config.genesisId = INT64_MIN;
    CHECK(invalidReason(config) == "");
}

TEST_CASE("maxWireVersion must be in [11, 12] (tool-side, RC2 S1)")
{
    EngineConfig config = validArray();
    config.maxWireVersion = 0; // what a pre-12 frame decodes as; never a valid ceiling to send
    CHECK(invalidReason(config).find("maxWireVersion=0") == 0);
    config.maxWireVersion = 10;
    CHECK_FALSE(invalidReason(config).empty());
    config.maxWireVersion = 11;
    CHECK(invalidReason(config) == "");
    config.maxWireVersion = 12;
    CHECK(invalidReason(config) == "");
    config.maxWireVersion = 13;
    CHECK(invalidReason(config).find("maxWireVersion=13") == 0);
}

TEST_CASE("impl must be array or direct")
{
    EngineConfig config = validArray();
    config.impl = EngineImpl::UNKNOWN;
    CHECK(invalidReason(config) == "impl is missing or unknown (must be array or direct)");
}

TEST_CASE("maxMatchesPerOrder must be in [1, INT32_MAX]")
{
    EngineConfig config = validArray();

    config.maxMatchesPerOrder = 0;
    CHECK(invalidReason(config).find("maxMatchesPerOrder=0") == 0);

    config.maxMatchesPerOrder = static_cast<std::int64_t>(INT32_MAX) + 1;
    CHECK_FALSE(invalidReason(config).empty());

    config.maxMatchesPerOrder = INT32_MAX;
    CHECK(invalidReason(config) == "");
}

TEST_CASE("impl=array wants bookCapacity in [1, INT32_MAX] and maxOrdersPerLevel == 0")
{
    EngineConfig config = validArray();

    config.bookCapacity = 0;
    CHECK(invalidReason(config).find("bookCapacity=0") == 0);

    config.bookCapacity = static_cast<std::int64_t>(INT32_MAX) + 1;
    CHECK_FALSE(invalidReason(config).empty());

    config = validArray();
    config.maxOrdersPerLevel = 64; // a per-level cap on the array book is a mistake
    CHECK(invalidReason(config).find("maxOrdersPerLevel=64 must be 0 for impl=array") == 0);
}

TEST_CASE("impl=direct wants bookCapacity == 0 and maxOrdersPerLevel in [1, INT32_MAX]")
{
    EngineConfig config = validDirect();

    config.bookCapacity = 131072; // a book-wide capacity on the direct book is a mistake
    CHECK(invalidReason(config).find("bookCapacity=131072 must be 0 for impl=direct") == 0);

    config = validDirect();
    config.maxOrdersPerLevel = 0;
    CHECK(invalidReason(config).find("maxOrdersPerLevel=0") == 0);

    config = validDirect();
    config.maxOrdersPerLevel = static_cast<std::int64_t>(INT32_MAX) + 1;
    CHECK_FALSE(invalidReason(config).empty());
}

TEST_CASE("at least one market is required")
{
    EngineConfig config = validArray();
    config.markets.clear();
    CHECK(invalidReason(config) ==
        "markets is empty — a cluster with no markets cannot create engines");
}

TEST_CASE("marketId must be positive")
{
    EngineConfig config = validArray();
    config.markets[0].marketId = 0;
    CHECK(invalidReason(config).find("marketId must be positive") != std::string::npos);

    config.markets[0].marketId = -3;
    CHECK(invalidReason(config).find("marketId must be positive") != std::string::npos);
}

TEST_CASE("duplicate marketIds are rejected even when the input order hides the pair")
{
    // 2, 1, 2 — the duplicates are not adjacent as given; normalization makes them
    // adjacent, exactly like the engine's decode-then-sort path.
    EngineConfig config = validArray();
    MarketDef dupe = eth();
    dupe.symbol = "ETH-USD-B";
    config.markets = { eth(), btc(), dupe };
    CHECK(invalidReason(config).find("duplicate marketId") != std::string::npos);
}

TEST_CASE("symbol must be 1..16 printable-ASCII chars")
{
    EngineConfig config = validArray();

    config.markets[0].symbol = "";
    CHECK(invalidReason(config).find("symbol must be 1..16 ASCII chars") != std::string::npos);

    config.markets[0].symbol = "ABCDEFGHIJKLMNOPQ"; // 17 chars
    CHECK(invalidReason(config).find("symbol must be 1..16 ASCII chars") != std::string::npos);

    config.markets[0].symbol = "ABCDEFGHIJKLMNOP"; // exactly 16 is fine
    CHECK(invalidReason(config) == "");

    config.markets[0].symbol = "BTC USD"; // space (0x20) is below the printable band
    CHECK(invalidReason(config).find("non-printable or non-ASCII char at index 3")
        != std::string::npos);

    config.markets[0].symbol = "BTC\xC3\x9CUSD"; // a UTF-8 byte must not wrap past the check
    CHECK(invalidReason(config).find("non-printable or non-ASCII") != std::string::npos);
}

TEST_CASE("price band and tick rules")
{
    EngineConfig config = validArray();

    config.markets[0].tickSize = 0;
    CHECK(invalidReason(config).find("tickSize=0 must be positive") != std::string::npos);

    config = validArray();
    config.markets[0].minPrice = 0;
    CHECK(invalidReason(config).find("minPrice=0 must be positive") != std::string::npos);

    config = validArray();
    config.markets[0].maxPrice = config.markets[0].minPrice; // equal is as wrong as inverted
    CHECK(invalidReason(config).find("must exceed minPrice") != std::string::npos);
}

TEST_CASE("the price-level count must fit an int32")
{
    EngineConfig config = validArray();
    config.markets[0].minPrice = 1;
    config.markets[0].maxPrice = 5'000'000'000LL; // 5e9 levels at tick 1
    config.markets[0].tickSize = 1;
    CHECK(invalidReason(config).find("price levels does not fit an int") != std::string::npos);
}

TEST_CASE("normalized sorts markets ascending by marketId and validation is order-blind")
{
    EngineConfig config = validArray();
    config.markets = { eth(), btc() }; // wire order reversed

    const EngineConfig sorted = normalized(config);
    REQUIRE(sorted.markets.size() == 2);
    CHECK(sorted.markets[0].marketId == 1);
    CHECK(sorted.markets[1].marketId == 2);

    // Same outcome for both orders: the engine sorts before validating, so must we.
    CHECK(invalidReason(config) == "");
    CHECK(invalidReason(sorted) == "");
}

TEST_CASE("implName names every impl")
{
    CHECK(std::string(implName(EngineImpl::ARRAY)) == "array");
    CHECK(std::string(implName(EngineImpl::DIRECT)) == "direct");
    CHECK(std::string(implName(EngineImpl::UNKNOWN)) == "unknown");
}
