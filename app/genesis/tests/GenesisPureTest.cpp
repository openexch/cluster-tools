// ct-genesis pure parts: the strict input file, the strict settings, and the judgement of a state (emptiness on
// replicated facts, the phase lines, the exact-prefix rule and its refusal texts).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "genesis/GenesisPlan.h"
#include "genesis/GenesisSettings.h"
#include "genesis/GenesisState.h"
#include "genesis/Json.h"

namespace gn = cluster_tools::app::genesis;

namespace {

std::string example()
{
    std::ifstream in(CT_GENESIS_EXAMPLE);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string replaced(std::string text, const std::string &from, const std::string &to)
{
    const auto at = text.find(from);
    REQUIRE(std::string::npos != at);
    return text.replace(at, from.size(), to);
}

bool anyContains(const std::vector<std::string> &errors, const std::string &needle)
{
    for (const std::string &e : errors)
    {
        if (std::string::npos != e.find(needle))
        {
            return true;
        }
    }
    return false;
}

gn::GenesisPlan plan()
{
    gn::PlanResult r = gn::parsePlan(example());
    REQUIRE_MESSAGE(r.ok(), (r.errors.empty() ? "" : r.errors.front()));
    return r.plan;
}

// The observed state after each phase of `p`, as the clusters would report it.
gn::Observed after(const gn::GenesisPlan &p, gn::Phase last)
{
    gn::Observed o;
    o.core.gateReasons = gn::GATE_REASON_GENESIS_BIT;
    const auto at = static_cast<int>(last);
    if (at >= static_cast<int>(gn::Phase::ME_GENESIS))
    {
        o.meEpoch = p.genesisId;
    }
    if (at >= static_cast<int>(gn::Phase::ARM_MONEY_JOURNAL))
    {
        o.core.journalEnabled = true;
    }
    if (at >= static_cast<int>(gn::Phase::CORE_GENESIS))
    {
        o.core.configVersion = 3;
        o.core.behaviorVersion = 3;
        o.core.genesisId = p.genesisId;
        o.core.gateState = gn::GATE_OPEN;
        o.core.gateReasons = 0;
        o.core.feedMeEpoch = p.genesisId;
        o.core.lastAppliedJournalSeq = 1;
        for (const auto &m : p.markets)
        {
            o.core.markets.push_back({ m.marketId, 0, 0, 0, 0, 0 });
        }
    }
    if (at >= static_cast<int>(gn::Phase::CONFIGURE_RISK))
    {
        for (auto &m : o.core.markets)
        {
            for (const auto &r : p.risk)
            {
                if (r.marketId == m.marketId)
                {
                    m = { m.marketId, p.riskConfigVersion, r.minQuantity, r.maxQuantity, r.minNotional, r.maxNotional };
                }
            }
        }
        o.core.limitsVersion = p.limits.configVersion;
        o.core.requestTtlMs = p.limits.requestTtlMs;
        o.core.requestPerUserCap = p.limits.requestPerUserCap;
        o.core.orderEventRingCapacity = p.limits.orderEventRingCapacity;
    }
    if (at >= static_cast<int>(gn::Phase::FUND))
    {
        for (const auto &[key, amount] : gn::expectedBalances(p))
        {
            o.core.balances[key] = { amount, 0 };
        }
    }
    if (at >= static_cast<int>(gn::Phase::ACTIVATE))
    {
        o.core.configVersion = 4;
        o.core.behaviorVersion = 4;
        o.activation.present = true;
        o.activation.genesisId = p.genesisId;
    }
    return o;
}

}

// ------------------------------------------------------------------ json

TEST_CASE("json: integers only, duplicate keys refused, offsets named")
{
    CHECK(gn::json::parse("{\"a\": 1, \"b\": [true, null, \"x\"]}").ok());
    CHECK_FALSE(gn::json::parse("{\"a\": 1.5}").ok());
    CHECK_FALSE(gn::json::parse("{\"a\": 1e3}").ok());
    CHECK_FALSE(gn::json::parse("{\"a\": 01}").ok());
    CHECK_FALSE(gn::json::parse("{\"a\": -0}").ok());
    CHECK_FALSE(gn::json::parse("{\"a\": 9223372036854775808}").ok());
    CHECK(gn::json::parse("{\"a\": -9223372036854775808}").ok());
    const auto dup = gn::json::parse("{\"a\": 1, \"a\": 2}");
    REQUIRE_FALSE(dup.ok());
    CHECK(std::string::npos != dup.error.find("duplicate key \"a\""));
    CHECK(std::string::npos != gn::json::parse("{} x").error.find("at byte 3"));
}

// ------------------------------------------------------------------ the input file

TEST_CASE("plan: the example input parses, limits default to the demo request cap")
{
    const gn::GenesisPlan p = plan();
    CHECK(7301000000000001 == p.genesisId);
    CHECK(p.me.genesisId == p.genesisId);
    CHECK(5 == p.markets.size());
    CHECK(6 == p.assets.size());
    CHECK(5 == p.risk.size());
    CHECK(6 == p.funding.size());
    CHECK(2048 == p.limits.requestPerUserCap);

    std::string text = example();
    const auto at = text.find("  \"limits\"");
    const auto end = text.find("},", at);
    text.erase(at, end + 3 - at);
    gn::PlanResult r = gn::parsePlan(text);
    REQUIRE_MESSAGE(r.ok(), (r.errors.empty() ? "" : r.errors.front()));
    CHECK(2048 == r.plan.limits.requestPerUserCap);
    CHECK(1 == r.plan.limits.configVersion);
    CHECK(900000 == r.plan.limits.requestTtlMs);
    CHECK(65536 == r.plan.limits.orderEventRingCapacity);
}

TEST_CASE("plan: an input that asks for InitTradeHighWater is refused with the exact text")
{
    for (const std::string key : { "InitTradeHighWater", "initTradeHighWater", "coreInitTradeHighWaterTradeId" })
    {
        const std::string text = replaced(example(), "\"funding\"", "\"" + key + "\": {\"tradeId\": 7}, \"funding\"");
        gn::PlanResult r = gn::parsePlan(text);
        REQUIRE(1 == r.errors.size());
        CHECK(std::string("InitTradeHighWater is not part of a genesis") == r.errors.front());
    }
    // nested, too
    const std::string nested = replaced(example(), "\"assets\"", "\"initTradeHighWater\": 1, \"assets\"");
    CHECK(std::string(gn::INIT_TRADE_HIGH_WATER_REFUSAL) == gn::parsePlan(nested).errors.front());
}

TEST_CASE("plan: unknown, missing and out-of-range keys are named")
{
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"genesisId\"", "\"genesisld\"")).errors,
        "the genesis input.genesisId is required"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"genesisId\"", "\"extra\": 1, \"genesisId\"")).errors,
        "unknown key \"extra\""));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"behaviorVersion\": 2", "\"behaviorVersion\": 3")).errors,
        "me.behaviorVersion must be in [2, 2]"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"maxWireVersion\": 12", "\"maxWireVersion\": 11")).errors,
        "me.maxWireVersion must be in [12, 12]"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"genesisId\": 7301000000000001", "\"genesisId\": 0")).errors,
        "genesisId must be in [1,"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"requestPerUserCap\": 2048", "\"requestPerUserCap\": 0")).errors,
        "limits.requestPerUserCap must be in [1,"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"requestIdLow\": 2", "\"requestIdLow\": 1")).errors,
        "appears twice"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"assetId\": 1,\n      \"amount\"", "\"assetId\": 9,\n      \"amount\"")).errors,
        "is not a core asset"));
    CHECK(anyContains(gn::parsePlan(replaced(example(), "\"symbol\": \"BTC-USD\",\n        \"minPrice\": 1000000",
                                       "\"symbol\": \"BTC-EUR\",\n        \"minPrice\": 1000000")).errors,
        "ME market 1 is \"BTC-EUR\" at the ME"));
    CHECK(anyContains(gn::parsePlan("{\"genesisId\": 5}").errors, "the genesis input.me is required"));
    CHECK(anyContains(gn::parsePlan("[1]").errors, "must be an object"));
    CHECK(anyContains(gn::parsePlan("{").errors, "not valid JSON"));
}

// ------------------------------------------------------------------ settings

namespace {

std::map<std::string, std::string> baseEnv()
{
    return { { "GENESIS_DRIVER_MODE", "external" }, { "AERON_DIR", "/dev/shm/aeron-host" },
        { "GENESIS_CORE_INGRESS", "0=localhost:22002,1=localhost:22102,2=localhost:22202" },
        { "GENESIS_ME_INGRESS", "0=localhost:23002,1=localhost:23102,2=localhost:23202" },
        { "GENESIS_RESPONSE_CHANNEL", "aeron:udp?endpoint=127.0.0.1:0" },
        { "GENESIS_CORE_ADMIN_CREDENTIAL", "1:ADMIN:" + std::string(64, '1') },
        { "GENESIS_CORE_TOOL_CREDENTIAL", "2:TOOL:" + std::string(64, '2') },
        { "GENESIS_ME_ADMIN_CREDENTIAL", "4:ADMIN:" + std::string(64, '4') },
        { "GENESIS_ACTIVATION_RECORD", "/var/lib/genesis/activation.json" } };
}

gn::SettingsResult parse(const std::map<std::string, std::string> &env, bool tool = false)
{
    cluster_tools::runtime::DirProbe probe;
    probe.isDirectory = [](const std::string &p) { return p != "/missing"; };
    probe.isWritable = [](const std::string &) { return true; };
    return gn::parseGenesisSettings(
        [&env](const char *key) -> const char * {
            const auto it = env.find(key);
            return env.end() == it ? nullptr : it->second.c_str();
        },
        tool, probe);
}

}

TEST_CASE("settings: the driver mode is required, both modes take their own keys, timeouts default")
{
    auto env = baseEnv();
    gn::SettingsResult ok = parse(env);
    REQUIRE_MESSAGE(ok.ok(), (ok.errors.empty() ? "" : ok.errors.front()));
    CHECK(gn::DriverMode::EXTERNAL == ok.settings.driverMode);
    CHECK(600 == ok.settings.timeouts.gateOpenRetries);
    CHECK(100 == ok.settings.timeouts.gateOpenRetryIntervalMs);
    CHECK(30000 == ok.settings.timeouts.replyMs);
    CHECK(60000 == ok.settings.timeouts.connectMs);
    CHECK(30000 == ok.settings.timeouts.meEpochMs);
    CHECK(30000 == ok.settings.timeouts.journalArmMs);
    CHECK(10000 == ok.settings.driverTimeoutMs);
    CHECK(std::string("aeron:udp") == ok.settings.ingressChannel);

    env.erase("GENESIS_DRIVER_MODE");
    CHECK(anyContains(parse(env).errors, "GENESIS_DRIVER_MODE is required (no default)"));
    env["GENESIS_DRIVER_MODE"] = "both";
    CHECK(anyContains(parse(env).errors, "GENESIS_DRIVER_MODE must be exactly embedded or external, got 'both'"));

    env = baseEnv();
    env.erase("AERON_DIR");
    CHECK(anyContains(parse(env).errors, "AERON_DIR is required with GENESIS_DRIVER_MODE=external"));
    env["AERON_DIR"] = "/missing";
    CHECK(anyContains(parse(env).errors, "is not an existing directory"));

    env = baseEnv();
    env["GENESIS_DRIVER_MODE"] = "embedded";
    CHECK(anyContains(parse(env).errors, "GENESIS_DRIVER_DIR is required with GENESIS_DRIVER_MODE=embedded"));
    CHECK(anyContains(parse(env).errors, "AERON_DIR is set but GENESIS_DRIVER_MODE=embedded"));
    env.erase("AERON_DIR");
    env["GENESIS_DRIVER_DIR"] = "/dev/shm";
    gn::SettingsResult embedded = parse(env);
    REQUIRE_MESSAGE(embedded.ok(), (embedded.errors.empty() ? "" : embedded.errors.front()));
    CHECK(gn::DriverMode::EMBEDDED == embedded.settings.driverMode);
    CHECK(1 == embedded.settings.instanceId);
}

TEST_CASE("settings: a bad value names its key; the TOOL key is required by fund only; classes are asserted")
{
    auto env = baseEnv();
    env["GENESIS_GATE_OPEN_RETRIES"] = "0";
    CHECK(anyContains(parse(env).errors, "GENESIS_GATE_OPEN_RETRIES must be an integer in 1..1000000, got '0'"));
    env["GENESIS_GATE_OPEN_RETRIES"] = "25";
    env["GENESIS_REPLY_TIMEOUT_MS"] = "fast";
    gn::SettingsResult r = parse(env);
    CHECK(anyContains(r.errors, "GENESIS_REPLY_TIMEOUT_MS must be an integer"));
    env.erase("GENESIS_REPLY_TIMEOUT_MS");
    r = parse(env);
    REQUIRE(r.ok());
    CHECK(25 == r.settings.timeouts.gateOpenRetries);

    env.erase("GENESIS_CORE_TOOL_CREDENTIAL");
    CHECK(parse(env, false).ok());
    CHECK(anyContains(parse(env, true).errors, "GENESIS_CORE_TOOL_CREDENTIAL is required"));
    env["GENESIS_CORE_TOOL_CREDENTIAL"] = "2:GATEWAY:" + std::string(64, '2');
    CHECK(anyContains(parse(env, true).errors, "GENESIS_CORE_TOOL_CREDENTIAL must be a TOOL key, got GATEWAY"));
    env = baseEnv();
    env["GENESIS_DRIVER_TIMEOUT_MS"] = "100";
    CHECK(anyContains(parse(env).errors, "GENESIS_DRIVER_TIMEOUT_MS must be an integer in 3000..60000"));
    env = baseEnv();
    env["GENESIS_ACTIVATION_RECORD"] = "/missing/activation.json";
    CHECK(anyContains(parse(env).errors, "the directory /missing does not exist"));
}

// ------------------------------------------------------------------ the judgement

TEST_CASE("state: emptiness is judged on replicated facts, each one alone makes the cluster not empty")
{
    const gn::GenesisPlan p = plan();
    gn::Observed empty;
    empty.core.gateReasons = gn::GATE_REASON_GENESIS_BIT;
    gn::Assessment a = gn::assess(p, empty);
    CHECK(gn::Overall::EMPTY == a.overall);
    CHECK(gn::Phase::ME_GENESIS == *a.next);
    CHECK_FALSE(gn::admit(gn::Phase::ME_GENESIS, a).refused());

    using Mutate = void (*)(gn::Observed &);
    const std::pair<const char *, Mutate> facts[] = {
        { "ME epoch", [](gn::Observed &o) { o.meEpoch = 99; } },
        { "journal", [](gn::Observed &o) { o.core.journalEnabled = true; } },
        { "config", [](gn::Observed &o) { o.core.configVersion = 1; } },
        { "gate open", [](gn::Observed &o) { o.core.gateState = gn::GATE_OPEN; o.core.gateReasons = 0; } },
        { "gate admin", [](gn::Observed &o) { o.core.gateReasons = 1; } },
        { "limits", [](gn::Observed &o) { o.core.limitsVersion = 1; } },
        { "balance", [](gn::Observed &o) { o.core.balances[{ 1, 0 }] = { 5, 0 }; } },
        { "hold", [](gn::Observed &o) { o.core.holds = 1; } },
        { "order", [](gn::Observed &o) { o.core.orders = 1; } },
        { "trade", [](gn::Observed &o) { o.core.lastAppliedTradeId = 1; } },
        { "feed epoch", [](gn::Observed &o) { o.core.feedMeEpoch = 5; } },
        { "feed cursor", [](gn::Observed &o) { o.core.lastAppliedJournalSeq = 1; } },
        { "halt", [](gn::Observed &o) { o.core.feedHalted = true; } },
    };
    for (const auto &[name, mutate] : facts)
    {
        CAPTURE(name);
        gn::Observed o = empty;
        mutate(o);
        const gn::Assessment n = gn::assess(p, o);
        CHECK(gn::Overall::EMPTY != n.overall);
        const gn::Refusal r = gn::admit(gn::Phase::ME_GENESIS, n);
        CHECK(std::string("cluster is not empty: genesis refused") == r.line);
    }
}

TEST_CASE("state: after each phase k, status names k and the next verb is admitted, every other verb refused")
{
    const gn::GenesisPlan p = plan();
    const gn::Phase stateful[] = { gn::Phase::ME_GENESIS, gn::Phase::ARM_MONEY_JOURNAL, gn::Phase::CORE_GENESIS,
        gn::Phase::CONFIGURE_RISK, gn::Phase::FUND };
    for (std::size_t k = 0; k < 5; ++k)
    {
        CAPTURE(gn::phaseName(stateful[k]));
        const gn::Assessment a = gn::assess(p, after(p, stateful[k]));
        REQUIRE(gn::Overall::PREFIX == a.overall);
        CHECK(stateful[k] == *a.lastComplete);
        const gn::Phase next = k < 4 ? stateful[k + 1] : gn::Phase::VERIFY_FUNDED;
        CHECK(next == *a.next);
        for (gn::Phase verb : gn::ALL_PHASES)
        {
            const gn::Refusal r = gn::admit(verb, a);
            const bool allowed = verb == next || (4 == k && gn::Phase::ACTIVATE == verb);
            CAPTURE(gn::phaseName(verb));
            CHECK(allowed == !r.refused());
            if (r.refused() && gn::Phase::ME_GENESIS != verb)
            {
                CHECK(0 == r.line.rfind("genesis state is not a prefix of the plan: ", 0));
            }
        }
    }
    const gn::Assessment done = gn::assess(p, after(p, gn::Phase::ACTIVATE));
    CHECK(gn::Overall::COMPLETE == done.overall);
    CHECK(gn::Phase::ACTIVATE == *done.lastComplete);
    CHECK(std::string("cluster is not empty: genesis refused") == gn::admit(gn::Phase::ME_GENESIS, done).line);
}

TEST_CASE("state: a partial phase is not a prefix; the refusal names it and the recovery is wipe and redo")
{
    const gn::GenesisPlan p = plan();
    struct Case
    {
        const char *name;
        gn::Phase verb;
        gn::Observed state;
    };
    std::vector<Case> cases;
    {
        gn::Observed o = after(p, gn::Phase::ARM_MONEY_JOURNAL);
        o.core.configVersion = 1;
        o.core.behaviorVersion = 1;
        o.core.genesisId = p.genesisId;
        cases.push_back({ "core_genesis stopped after CoreConfig 1", gn::Phase::CORE_GENESIS, o });
        o.core.configVersion = 3;
        cases.push_back({ "core_genesis stopped before the gate OPEN", gn::Phase::CONFIGURE_RISK, o });
    }
    {
        gn::Observed o = after(p, gn::Phase::CORE_GENESIS);
        o.core.markets[0].riskConfigVersion = p.riskConfigVersion;
        o.core.markets[0].maxQuantity = p.risk[0].maxQuantity;
        o.core.markets[0].minQuantity = p.risk[0].minQuantity;
        o.core.markets[0].maxNotional = p.risk[0].maxNotional;
        cases.push_back({ "configure_risk after one row", gn::Phase::CONFIGURE_RISK, o });
    }
    {
        gn::Observed o = after(p, gn::Phase::CONFIGURE_RISK);
        o.core.balances[{ 1, 0 }] = { p.funding[0].amount, 0 };
        cases.push_back({ "fund in the middle of the deposits", gn::Phase::FUND, o });
    }
    {
        gn::Observed o = after(p, gn::Phase::FUND);
        o.meEpoch = p.genesisId + 2;
        cases.push_back({ "a foreign ME genesis", gn::Phase::VERIFY_FUNDED, o });
    }
    {
        gn::Observed o = after(p, gn::Phase::FUND);
        o.core.lastAppliedTradeId = 4;
        cases.push_back({ "trading during a genesis", gn::Phase::ACTIVATE, o });
    }
    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        const gn::Assessment a = gn::assess(p, c.state);
        CHECK(gn::Overall::NOT_A_PREFIX == a.overall);
        const gn::Refusal r = gn::admit(c.verb, a);
        CHECK(0 == r.line.rfind("genesis state is not a prefix of the plan: ", 0));
        CHECK(std::string::npos != r.recovery.find("wipe both clusters and redo"));
        CHECK(std::string("cluster is not empty: genesis refused") == gn::admit(gn::Phase::ME_GENESIS, a).line);
    }
}

TEST_CASE("state: behaviour 4 without the activation record resumes activate; a record alone is foreign")
{
    const gn::GenesisPlan p = plan();
    gn::Observed o = after(p, gn::Phase::ACTIVATE);
    o.activation = {};
    gn::Assessment a = gn::assess(p, o);
    CHECK(gn::Overall::PREFIX == a.overall);
    CHECK(gn::Phase::ACTIVATE == *a.next);
    CHECK(gn::Phase::VERIFY_FUNDED == *a.lastComplete);
    CHECK_FALSE(gn::admit(gn::Phase::ACTIVATE, a).refused());
    CHECK(gn::admit(gn::Phase::FUND, a).refused());

    gn::Observed record = after(p, gn::Phase::FUND);
    record.activation.present = true;
    record.activation.genesisId = p.genesisId;
    CHECK(gn::Overall::NOT_A_PREFIX == gn::assess(p, record).overall);

    // a record of an earlier genesis does not count, and does not block a new one
    gn::Observed stale;
    stale.core.gateReasons = gn::GATE_REASON_GENESIS_BIT;
    stale.activation.present = true;
    stale.activation.genesisId = 42;
    CHECK(gn::Overall::EMPTY == gn::assess(p, stale).overall);
}

TEST_CASE("state: an activated cluster is COMPLETE while it trades; funding is not judged after the activation")
{
    const gn::GenesisPlan p = plan();
    gn::Observed o = after(p, gn::Phase::ACTIVATE);
    o.core.orders = 12;
    o.core.lastAppliedTradeId = 40;
    o.core.lastAppliedJournalSeq = 300;
    o.core.balances.clear();
    o.core.balances[{ 1, 0 }] = { 3, 9 };
    const gn::Assessment a = gn::assess(p, o);
    CHECK(gn::Overall::COMPLETE == a.overall);
    CHECK(std::string("cluster is not empty: genesis refused") == gn::admit(gn::Phase::ME_GENESIS, a).line);
}

TEST_CASE("state: the prefix and emptiness switches (the negative-control seams) admit anything")
{
    const gn::GenesisPlan p = plan();
    gn::Observed o = after(p, gn::Phase::CONFIGURE_RISK);
    o.core.balances[{ 1, 0 }] = { 1, 0 };
    const gn::Assessment a = gn::assess(p, o);
    CHECK(gn::admit(gn::Phase::FUND, a).refused());
    CHECK_FALSE(gn::admit(gn::Phase::FUND, a, true, false).refused());
    CHECK(gn::admit(gn::Phase::ME_GENESIS, a).refused());
    CHECK_FALSE(gn::admit(gn::Phase::ME_GENESIS, a, false, true).refused());
}
