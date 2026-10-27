// RC2 S12 contract 11.1 ForwarderConfigTest: strict env (section 9, F5): every variable required
// (FWD_INGRESS_CHANNEL included, NOTE-9), both credentials FORWARDER, distinct response channels unless both
// use port 0, 0.0.0.0 refused (S11 validate).
#include "doctest/doctest.h"

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
        { "AERON_DIR", "/dev/shm/aeron-fwd" },
        { "FWD_HTTP_BIND", "127.0.0.1:9600" },
    };
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
