#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include "cluster_tools/adapters/json/EngineConfigJson.h"

// Shape tests for the strict engine-config parser. Semantic rules (ranges, market
// rules) live in the domain and are tested there — here we prove the parser accepts
// exactly the documented document and refuses ambiguity loudly.

using cluster_tools::domain::EngineConfig;
using cluster_tools::domain::EngineImpl;
using cluster_tools::json_adapter::parseEngineConfig;

namespace
{

const char *const VALID =
    "{\n"
    "  \"configVersion\": 1,\n"
    "  \"impl\": \"array\",\n"
    "  \"bookCapacity\": 131072,\n"
    "  \"maxMatchesPerOrder\": 10000,\n"
    "  \"maxOrdersPerLevel\": 0,\n"
    "  \"markets\": [\n"
    "    { \"marketId\": 2, \"symbol\": \"ETH-USD\", \"minPrice\": 100000000000,\n"
    "      \"maxPrice\": 1000000000000, \"tickSize\": 50000000 },\n"
    "    { \"marketId\": 1, \"symbol\": \"BTC-USD\", \"minPrice\": 5000000000000,\n"
    "      \"maxPrice\": 15000000000000, \"tickSize\": 100000000 }\n"
    "  ]\n"
    "}\n";

}

TEST_CASE("a valid document parses into the domain value, wire order preserved")
{
    const auto result = parseEngineConfig(VALID);
    REQUIRE(result.ok());
    const EngineConfig &config = result.value();
    CHECK(config.configVersion == 1);
    CHECK(config.impl == EngineImpl::ARRAY);
    CHECK(config.bookCapacity == 131072);
    CHECK(config.maxMatchesPerOrder == 10000);
    CHECK(config.maxOrdersPerLevel == 0);
    REQUIRE(config.markets.size() == 2);
    // The parser is shape-only: normalization (sorting) is the caller's explicit step.
    CHECK(config.markets[0].marketId == 2);
    CHECK(config.markets[0].symbol == "ETH-USD");
    CHECK(config.markets[1].marketId == 1);
    CHECK(config.markets[1].minPrice == 5000000000000LL);
    CHECK(config.markets[1].maxPrice == 15000000000000LL);
    CHECK(config.markets[1].tickSize == 100000000LL);
}

TEST_CASE("impl accepts exactly array and direct")
{
    const auto direct = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"direct\",\"bookCapacity\":0,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":64,\"markets\":[]}");
    REQUIRE(direct.ok());
    CHECK(direct.value().impl == EngineImpl::DIRECT);

    const auto bad = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"ARRAY\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().find("impl must be \"array\" or \"direct\"") != std::string::npos);
}

TEST_CASE("every root key is required and named when missing")
{
    const auto result = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":131072,"
        "\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("missing required key(s): maxMatchesPerOrder") != std::string::npos);
}

TEST_CASE("unknown and duplicate keys are refused loudly")
{
    const auto unknown = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookcapacity\":131072,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().find("unknown key 'bookcapacity'") != std::string::npos);

    const auto duplicate = parseEngineConfig(
        "{\"configVersion\":1,\"configVersion\":2,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error().find("duplicate key 'configVersion'") != std::string::npos);
}

TEST_CASE("market objects get the same strictness")
{
    const auto missing = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":["
        "{\"marketId\":1,\"symbol\":\"BTC-USD\",\"minPrice\":1,\"maxPrice\":2}]}");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().find("market object is missing required key(s): tickSize")
        != std::string::npos);

    const auto unknown = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":["
        "{\"marketId\":1,\"symbol\":\"BTC-USD\",\"minPrice\":1,\"maxPrice\":2,"
        "\"tick\":3}]}");
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().find("unknown market key 'tick'") != std::string::npos);
}

TEST_CASE("numeric fields are strict integers")
{
    const auto floatCapacity = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1.5,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(floatCapacity.ok());
    CHECK(floatCapacity.error().find("not a float") != std::string::npos);

    const auto quoted = parseEngineConfig(
        "{\"configVersion\":\"1\",\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(quoted.ok());
    CHECK(quoted.error().find("expected an integer") != std::string::npos);

    const auto leadingZero = parseEngineConfig(
        "{\"configVersion\":01,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(leadingZero.ok());
    CHECK(leadingZero.error().find("leading zeros") != std::string::npos);

    const auto tooWide = parseEngineConfig(
        "{\"configVersion\":9223372036854775808,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE_FALSE(tooWide.ok());
    CHECK(tooWide.error().find("does not fit int64") != std::string::npos);

    // Negative values are SHAPE-valid (the domain rejects them semantically): the
    // parser must carry them through unchanged, not mangle or refuse them.
    const auto negative = parseEngineConfig(
        "{\"configVersion\":-2,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE(negative.ok());
    CHECK(negative.value().configVersion == -2);
}

TEST_CASE("marketId must fit int32 at parse time")
{
    const auto result = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":["
        "{\"marketId\":2147483648,\"symbol\":\"X\",\"minPrice\":1,\"maxPrice\":2,"
        "\"tickSize\":1}]}");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("marketId does not fit int32") != std::string::npos);
}

TEST_CASE("trailing garbage and truncation are refused")
{
    const auto trailing = parseEngineConfig(std::string(VALID) + "x");
    REQUIRE_FALSE(trailing.ok());
    CHECK(trailing.error().find("unexpected content after the closing '}'") != std::string::npos);

    const std::string valid(VALID);
    const auto truncated = parseEngineConfig(valid.substr(0, valid.size() / 2));
    CHECK_FALSE(truncated.ok());
}

TEST_CASE("string escapes: the simple ones decode, \\u is refused")
{
    const auto escaped = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":["
        "{\"marketId\":1,\"symbol\":\"A\\\\B\",\"minPrice\":1,\"maxPrice\":2,"
        "\"tickSize\":1}]}");
    REQUIRE(escaped.ok());
    CHECK(escaped.value().markets[0].symbol == "A\\B");

    const auto unicode = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":["
        "{\"marketId\":1,\"symbol\":\"\\u0041\",\"minPrice\":1,\"maxPrice\":2,"
        "\"tickSize\":1}]}");
    REQUIRE_FALSE(unicode.ok());
    CHECK(unicode.error().find("\\u escapes are not supported") != std::string::npos);
}

TEST_CASE("an empty markets array is shape-valid (the domain rejects it, not the parser)")
{
    const auto result = parseEngineConfig(
        "{\"configVersion\":1,\"impl\":\"array\",\"bookCapacity\":1,"
        "\"maxMatchesPerOrder\":1,\"maxOrdersPerLevel\":0,\"markets\":[]}");
    REQUIRE(result.ok());
    CHECK(result.value().markets.empty());
}
