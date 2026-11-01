// RC2 S12 contract 11.1 ForwarderConfigTest: strict env (section 9, F5): every variable required
// (FWD_INGRESS_CHANNEL included, NOTE-9), both credentials FORWARDER, distinct response channels unless both
// use port 0, 0.0.0.0 refused (S11 validate).
// RC2 S10' 8.3: driverModeIsStrict (RED DRIVER_MODE_DEFAULT_EXTERNAL), driverTimeoutUsesTheSharedParser (RED
// TIMEOUT_CONSTANT), instanceIdRange (table-driven), identityIsTheInstanceIdInBothClientNames.
#include "doctest/doctest.h"

#include <limits>
#include <map>
#include <string>

#include "ForwarderConfig.h"

using namespace cluster_tools::app::forwarder;

namespace {

const std::string SECRET(64, '6');

std::map<std::string, std::string> validEnv()
{
    return {
        { "FWD_CORE_INGRESS", "0=10.0.0.1:9002,1=10.0.0.2:9002,2=10.0.0.3:9002" },
        { "FWD_CORE_RESPONSE_CHANNEL", "aeron:udp?endpoint=10.0.0.5:0" },
        { "AE_AUTH_CREDENTIAL", "6:FORWARDER:" + SECRET },
        { "FWD_ME_INGRESS", "0=10.0.1.1:9002,1=10.0.1.2:9002,2=10.0.1.3:9002" },
        { "FWD_ME_RESPONSE_CHANNEL", "aeron:udp?endpoint=10.0.0.5:0" },
        { "ME_AUTH_CREDENTIAL", "7:FORWARDER:" + SECRET },
        { "FWD_INGRESS_CHANNEL", "aeron:udp?term-length=4m|mtu=8192" },
        { "FWD_DRIVER_MODE", "external" },
        { "AERON_DIR", "/dev/shm/aeron-fwd" },
        { "FWD_DRIVER_TIMEOUT_MS", "5000" },
        { "FWD_INSTANCE_ID", "7" },
        { "FWD_HTTP_BIND", "127.0.0.1:9600" },
    };
}

std::map<std::string, std::string> embeddedEnv()
{
    auto env = validEnv();
    env.erase("AERON_DIR");
    env["FWD_DRIVER_MODE"] = "embedded";
    env["FWD_DRIVER_DIR"] = "/dev/shm/fwd-embedded";
    return env;
}

ConfigResult parse(const std::map<std::string, std::string> &env)
{
    return parseForwarderConfig([&env](const char *name) -> const char * {
        const auto it = env.find(name);
        return it == env.end() ? nullptr : it->second.c_str();
    });
}

bool mentions(const ConfigResult &r, const std::string &text)
{
    for (const std::string &e : r.errors)
    {
        if (e.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

}

TEST_CASE("ForwarderConfigTest.validEnvironmentParses")
{
    const ConfigResult r = parse(validEnv());
    REQUIRE(r.ok());
    CHECK(r.config.httpHost == "127.0.0.1");
    CHECK(r.config.httpPort == 9600);
    CHECK(r.config.coreCredentials.keyId == 6);
    CHECK(r.config.meCredentials.keyId == 7);
    CHECK(coreClientSettings(r.config).ingressChannel == "aeron:udp?term-length=4m|mtu=8192");
    CHECK(meClientSettings(r.config).credentials.size() == 38);
}

TEST_CASE("ForwarderConfigTest.eachVariableMissingRefusesToStart")
{
    for (const auto &[name, value] : validEnv())
    {
        CAPTURE(name);
        auto env = validEnv();
        env.erase(name);
        const ConfigResult r = parse(env);
        CHECK_FALSE(r.ok());
        CHECK(mentions(r, name));
        env[name] = "   ";
        CHECK_FALSE(parse(env).ok()); // blank is missing
    }
}

TEST_CASE("ForwarderConfigTest.aNonForwarderCredentialOnEitherClusterIsRefused")
{
    auto env = validEnv();
    env["AE_AUTH_CREDENTIAL"] = "3:BRIDGE:" + SECRET;
    ConfigResult r = parse(env);
    CHECK_FALSE(r.ok());
    CHECK(mentions(r, "AE_AUTH_CREDENTIAL is a BRIDGE credential"));
    env = validEnv();
    env["ME_AUTH_CREDENTIAL"] = "5:GATEWAY:" + SECRET;
    r = parse(env);
    CHECK_FALSE(r.ok());
    CHECK(mentions(r, "ME_AUTH_CREDENTIAL is a GATEWAY credential"));
    env = validEnv();
    env["ME_AUTH_CREDENTIAL"] = "7:FORWARDER:abc";
    CHECK_FALSE(parse(env).ok());
}

TEST_CASE("ForwarderConfigTest.equalResponseChannelsWithFixedPortsAreRefused")
{
    auto env = validEnv();
    env["FWD_CORE_RESPONSE_CHANNEL"] = "aeron:udp?endpoint=10.0.0.5:41000";
    env["FWD_ME_RESPONSE_CHANNEL"] = "aeron:udp?endpoint=10.0.0.5:41000";
    ConfigResult r = parse(env);
    CHECK_FALSE(r.ok());
    CHECK(mentions(r, "name the same endpoint"));
    env["FWD_ME_RESPONSE_CHANNEL"] = "aeron:udp?endpoint=10.0.0.5:41001";
    CHECK(parse(env).ok());
    // both port 0: the driver picks two ports
    CHECK(parse(validEnv()).ok());
}

TEST_CASE("ForwarderConfigTest.unroutableOrMalformedValuesAreRefused")
{
    auto env = validEnv();
    env["FWD_CORE_RESPONSE_CHANNEL"] = "aeron:udp?endpoint=0.0.0.0:0";
    CHECK(mentions(parse(env), "0.0.0.0"));
    env = validEnv();
    env["FWD_ME_INGRESS"] = "0=10.0.1.1";
    CHECK_FALSE(parse(env).ok());
    env = validEnv();
    env["FWD_INGRESS_CHANNEL"] = "aeron:udp?endpoint=1.2.3.4:5";
    CHECK_FALSE(parse(env).ok());
    for (const char *bind : { "9600", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:70000", "127.0.0.1:96x" })
    {
        CAPTURE(bind);
        env = validEnv();
        env["FWD_HTTP_BIND"] = bind;
        CHECK(mentions(parse(env), "FWD_HTTP_BIND"));
    }
}

TEST_CASE("ForwarderConfigTest.endpointHelpers")
{
    CHECK(endpointOf("aeron:udp?endpoint=1.2.3.4:5|mtu=8192") == "1.2.3.4:5");
    CHECK(endpointPort("aeron:udp?endpoint=1.2.3.4:5|mtu=8192") == 5);
    CHECK(endpointPort("aeron:udp?endpoint=1.2.3.4:0") == 0);
    CHECK(endpointPort("aeron:udp") == -1);
}

TEST_CASE("ForwarderConfigTest.driverModeIsStrict")
{
    // the two valid shapes
    {
        const ConfigResult external = parse(validEnv());
        REQUIRE(external.ok());
        CHECK(external.config.driverMode == DriverMode::EXTERNAL);
        CHECK(external.config.aeronDir == "/dev/shm/aeron-fwd");
        CHECK(external.config.driverDir.empty());
        const ConfigResult embedded = parse(embeddedEnv());
        REQUIRE(embedded.ok());
        CHECK(embedded.config.driverMode == DriverMode::EMBEDDED);
        CHECK(embedded.config.driverDir == "/dev/shm/fwd-embedded");
        CHECK(embedded.config.aeronDir.empty());
    }
    // the six refusals of 8.2 rule 1, each naming its key
    auto env = validEnv();
    env.erase("FWD_DRIVER_MODE");
    const ConfigResult missing = parse(env);
    const bool missingModeRefused = !missing.ok() && mentions(missing, "FWD_DRIVER_MODE is required");
    CHECK_MESSAGE(missingModeRefused, "missing mode parsed"); // DRIVER_MODE_DEFAULT_EXTERNAL RED
    for (const char *mode : { "Embedded", "external ", "both", "1" })
    {
        CAPTURE(mode);
        env = validEnv();
        env["FWD_DRIVER_MODE"] = mode;
        const ConfigResult r = parse(env);
        // trailing blanks are trimmed like every other key; "external " is therefore valid
        if (std::string(mode) == "external ")
        {
            CHECK(r.ok());
            continue;
        }
        CHECK_FALSE(r.ok());
        CHECK(mentions(r, "FWD_DRIVER_MODE must be exactly embedded or external"));
    }
    env = validEnv();
    env.erase("AERON_DIR");
    CHECK(mentions(parse(env), "AERON_DIR is required with FWD_DRIVER_MODE=external"));
    env = validEnv();
    env["FWD_DRIVER_DIR"] = "/dev/shm/x";
    CHECK(mentions(parse(env), "FWD_DRIVER_DIR is set but FWD_DRIVER_MODE=external"));
    env = embeddedEnv();
    env.erase("FWD_DRIVER_DIR");
    CHECK(mentions(parse(env), "FWD_DRIVER_DIR is required with FWD_DRIVER_MODE=embedded"));
    env = embeddedEnv();
    env["AERON_DIR"] = "/dev/shm/aeron-fwd";
    CHECK(mentions(parse(env), "AERON_DIR is set but FWD_DRIVER_MODE=embedded"));
    env = embeddedEnv();
    env["FWD_DRIVER_DIR"] = "relative/dir";
    CHECK(mentions(parse(env), "FWD_DRIVER_DIR must be an absolute path"));
    // an empty AERON_DIR still counts as set in embedded mode (present is present)
    env = embeddedEnv();
    env["AERON_DIR"] = "";
    CHECK(mentions(parse(env), "AERON_DIR is set"));
}

TEST_CASE("ForwarderConfigTest.driverTimeoutUsesTheSharedParser")
{
    auto env = validEnv();
    env.erase("FWD_DRIVER_TIMEOUT_MS");
    const ConfigResult missing = parse(env);
    const bool missingTimeoutRefused = !missing.ok() && mentions(missing, "FWD_DRIVER_TIMEOUT_MS is required");
    CHECK_MESSAGE(missingTimeoutRefused, "5000 used while the key is missing"); // TIMEOUT_CONSTANT RED
    for (const char *bad : { "2999", "60001", "0", "-3000", "+3000", "3000ms", "3e3", "" })
    {
        CAPTURE(bad);
        env = validEnv();
        env["FWD_DRIVER_TIMEOUT_MS"] = bad;
        const ConfigResult r = parse(env);
        CHECK_FALSE(r.ok());
        CHECK(mentions(r, "FWD_DRIVER_TIMEOUT_MS"));
    }
    for (const auto &[text, value] : std::map<std::string, std::int64_t>{ { "3000", 3000 }, { "60000", 60000 }, { "5000", 5000 } })
    {
        CAPTURE(text);
        env = embeddedEnv();
        env["FWD_DRIVER_TIMEOUT_MS"] = text;
        const ConfigResult r = parse(env);
        REQUIRE(r.ok());
        CHECK(r.config.driverTimeoutMs == value);
    }
}

TEST_CASE("ForwarderConfigTest.instanceIdRange")
{
    for (const char *bad : { "0", "-1", "abc", "1.5", "+7", "9223372036854775808", "7 8" })
    {
        for (const bool embedded : { false, true })
        {
            CAPTURE(bad);
            CAPTURE(embedded);
            auto env = embedded ? embeddedEnv() : validEnv();
            env["FWD_INSTANCE_ID"] = bad;
            const ConfigResult r = parse(env);
            CHECK_FALSE(r.ok());
            CHECK(mentions(r, "FWD_INSTANCE_ID must be an integer in 1..9223372036854775807"));
        }
    }
    for (const bool embedded : { false, true })
    {
        auto env = embedded ? embeddedEnv() : validEnv();
        env.erase("FWD_INSTANCE_ID");
        CHECK(mentions(parse(env), "FWD_INSTANCE_ID is required"));
        env["FWD_INSTANCE_ID"] = "9223372036854775807";
        const ConfigResult r = parse(env);
        REQUIRE(r.ok());
        CHECK(r.config.instanceId == std::numeric_limits<std::int64_t>::max());
    }
}

TEST_CASE("ForwarderConfigTest.identityIsTheInstanceIdInBothClientNames")
{
    const ConfigResult r = parse(validEnv());
    REQUIRE(r.ok());
    CHECK(forwarderIdentity(r.config) == 7);
    CHECK(coreClientSettings(r.config).clientName == "ct-forwarder-core identity=7");
    CHECK(meClientSettings(r.config).clientName == "ct-forwarder-me identity=7");
}
