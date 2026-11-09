// BridgeConfigTest: the strict environment (every key required, every problem named in
// table order, the driver mode strict in both directions, the port formula and the channels the Java bridge used).
#include "doctest/doctest.h"

#include <map>
#include <set>
#include <string>

#include "BridgeConfig.h"

using namespace cluster_tools::app::bridge;

namespace {

const std::string SECRET(64, '3');

using Env = std::map<std::string, std::string>;

Env external()
{
    return {
        { "BRIDGE_ME_JOURNAL_ARCHIVES", "10.0.0.1:9010,10.0.0.2:9110" },
        { "BRIDGE_JOURNAL_CONTROL_STREAM", "4010" },
        { "BRIDGE_JOURNAL_STREAM", "4001" },
        { "BRIDGE_JOURNAL_CHANNEL", "aeron:ipc" },
        { "BRIDGE_REPLAY_STREAM", "9101" },
        { "BRIDGE_JOURNAL_MTU", "1408" },
        { "BRIDGE_LOCAL_HOST", "10.0.0.9" },
        { "BRIDGE_DRIVER_MODE", "external" },
        { "AERON_DIR", "/dev/shm/aeron-host" },
        { "BRIDGE_DRIVER_TIMEOUT_MS", "5000" },
        { "BRIDGE_AE_CLUSTER_ADDRESSES", "10.0.1.1,10.0.1.2,10.0.1.3" },
        { "BRIDGE_AE_PORT_BASE", "19300" },
        { "CLUSTER_PORT_STRIDE", "100" },
        { "BRIDGE_AE_EGRESS_ENDPOINT", "10.0.0.9:0" },
        { "AE_AUTH_CREDENTIAL", "3:BRIDGE:" + SECRET },
        { "BRIDGE_INSTANCE_ID", "1" },
        { "BRIDGE_HTTP_BIND", "127.0.0.1:9600" },
    };
}

Env embedded()
{
    Env env = external();
    env.erase("AERON_DIR");
    env["BRIDGE_DRIVER_MODE"] = "embedded";
    env["BRIDGE_DRIVER_DIR"] = "/dev/shm";
    return env;
}

std::set<std::string> g_dirs = { "/dev/shm", "/dev/shm/aeron-host" };
std::set<std::string> g_writable = { "/dev/shm", "/dev/shm/aeron-host" };

DirProbe fakeProbe()
{
    DirProbe p;
    p.isDirectory = [](const std::string &d) { return g_dirs.count(d) > 0; };
    p.isWritable = [](const std::string &d) { return g_writable.count(d) > 0; };
    return p;
}

ConfigResult parse(const Env &env)
{
    return parseBridgeConfig(
        [&env](const char *name) -> const char * {
            const auto it = env.find(name);
            return it == env.end() ? nullptr : it->second.c_str();
        },
        fakeProbe());
}

bool mentions(const ConfigResult &r, const std::string &needle)
{
    for (const std::string &e : r.errors)
    {
        if (e.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string all(const ConfigResult &r)
{
    std::string out;
    for (const std::string &e : r.errors)
    {
        out += e + "\n";
    }
    return out;
}

}

TEST_CASE("BridgeConfigTest.everyKeyMissingIsRefusedAndNamed")
{
    for (const Env &good : { external(), embedded() })
    {
        const ConfigResult ok = parse(good);
        CAPTURE(all(ok));
        REQUIRE(ok.ok());
        for (const auto &[key, value] : good)
        {
            CAPTURE(key);
            Env env = good;
            env.erase(key);
            const ConfigResult missing = parse(env);
            CAPTURE(all(missing));
            const bool refused = !missing.ok();
            CHECK(refused); // CONFIG_DEFAULTS RED: BRIDGE_AE_PORT_BASE is filled by a default
            CHECK(missing.errors.size() == 1);
            CHECK(mentions(missing, key));
        }
    }
    const ConfigResult r = parse(external());
    REQUIRE(r.ok());
    const BridgeConfig &c = r.config;
    CHECK(DriverMode::EXTERNAL == c.driverMode);
    CHECK(c.aeronDir == "/dev/shm/aeron-host");
    CHECK(c.driverDir.empty());
    CHECK(c.driverTimeoutMs == 5000);
    CHECK(c.archive.archives.size() == 2);
    CHECK(c.archive.journalMtu == 1408);
    CHECK(c.corePortBase == 19300);
    CHECK(c.portStride == 100);
    CHECK(c.instanceId == 1);
    CHECK(c.httpHost == "127.0.0.1");
    CHECK(c.httpPort == 9600);
    CHECK(c.credentials.sessionClass == cluster_tools::client::SessionClass::BRIDGE);
    CHECK(configKeys().size() == 19);
}

TEST_CASE("BridgeConfigTest.haltOnGapPresentIsRefused")
{
    for (const char *value : { "false", "true" })
    {
        CAPTURE(value);
        Env env = external();
        env["BRIDGE_HALT_ON_GAP"] = value;
        const ConfigResult r = parse(env);
        const bool haltOnGapRefused = !r.ok() && mentions(r, "BRIDGE_HALT_ON_GAP");
        CHECK(haltOnGapRefused); // CONFIG_HALT_ON_GAP_IGNORED RED
    }
}

TEST_CASE("BridgeConfigTest.loopbackLocalHostWithRemoteArchiveRefused")
{
    Env env = external();
    env["BRIDGE_LOCAL_HOST"] = "127.0.0.1";
    const ConfigResult r = parse(env);
    CHECK_FALSE(r.ok());
    CHECK(mentions(r, "BRIDGE_LOCAL_HOST 127.0.0.1 is loopback"));
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = "localhost:9010,127.0.0.1:9110";
    env["BRIDGE_AE_CLUSTER_ADDRESSES"] = "localhost";
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "localhost:0";
    CHECK(parse(env).ok()); // an all-loopback deployment is fine
}

TEST_CASE("BridgeConfigTest.loopbackEgressEndpointWithRemoteCoreRefused")
{
    Env env = external();
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "127.0.0.1:0";
    const ConfigResult r = parse(env);
    CHECK_FALSE(r.ok());
    CHECK(mentions(r, "BRIDGE_AE_EGRESS_ENDPOINT 127.0.0.1:0 is loopback but core member 10.0.1.1 is not"));
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "localhost:40123";
    CHECK(mentions(parse(env), "is loopback but core member"));
}

TEST_CASE("BridgeConfigTest.instanceIdRange")
{
    for (const char *bad : { "0", "-1", "abc", "1x", "+5", "9223372036854775808" })
    {
        CAPTURE(bad);
        Env env = external();
        env["BRIDGE_INSTANCE_ID"] = bad;
        const ConfigResult r = parse(env);
        CHECK_FALSE(r.ok());
        CHECK(mentions(r, "BRIDGE_INSTANCE_ID must be an integer in 1..9223372036854775807"));
    }
    Env env = external();
    env["BRIDGE_INSTANCE_ID"] = "9223372036854775807";
    const ConfigResult r = parse(env);
    REQUIRE(r.ok());
    CHECK(r.config.instanceId == INT64_MAX);
}

TEST_CASE("BridgeConfigTest.credentialClassMustBeBridge")
{
    Env env = external();
    env["AE_AUTH_CREDENTIAL"] = "6:FORWARDER:" + SECRET;
    const ConfigResult r = parse(env);
    const bool classRefused = !r.ok() && mentions(r, "is a FORWARDER credential; ct-bridge needs BRIDGE");
    CHECK(classRefused); // CONFIG_CLASS_CHECK_OFF RED: the FORWARDER key passes
    env["AE_AUTH_CREDENTIAL"] = "3:BRIDGE";
    CHECK(mentions(parse(env), "AE_AUTH_CREDENTIAL: credential spec must be keyId:class:secretHex"));
}

TEST_CASE("BridgeConfigTest.egressEndpointNotWildcard")
{
    Env env = external();
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "0.0.0.0:0";
    CHECK(mentions(parse(env), "BRIDGE_AE_EGRESS_ENDPOINT host 0.0.0.0 is not routable"));
    for (const char *bad : { "10.0.0.9", "10.0.0.9:", ":40000", "10.0.0.9:70000", "10.0.0.9:-1" })
    {
        CAPTURE(bad);
        env["BRIDGE_AE_EGRESS_ENDPOINT"] = bad;
        CHECK(mentions(parse(env), "BRIDGE_AE_EGRESS_ENDPOINT must be host:port"));
    }
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "10.0.0.9:40123";
    CHECK(parse(env).ok());
}

TEST_CASE("BridgeConfigTest.httpBindIsHostPort")
{
    Env env = external();
    env["BRIDGE_HTTP_BIND"] = "0.0.0.0:9600"; // allowed only when written out
    const ConfigResult wildcard = parse(env);
    REQUIRE(wildcard.ok());
    CHECK(wildcard.config.httpHost == "0.0.0.0");
    env["BRIDGE_HTTP_BIND"] = "127.0.0.1:0"; // ephemeral, for tests
    CHECK(parse(env).ok());
    for (const char *bad : { "9600", ":9600", "127.0.0.1:", "127.0.0.1:65536" })
    {
        CAPTURE(bad);
        env["BRIDGE_HTTP_BIND"] = bad;
        CHECK(mentions(parse(env), "BRIDGE_HTTP_BIND must be host:port"));
    }
    env["BRIDGE_HTTP_BIND"] = "bridge.example:9600";
    CHECK(mentions(parse(env), "BRIDGE_HTTP_BIND host 'bridge.example' is not an IPv4 address"));
}

TEST_CASE("BridgeConfigTest.driverDirMustBeWritable")
{
    Env env = embedded();
    env["BRIDGE_DRIVER_DIR"] = "/nonexistent";
    CHECK(mentions(parse(env), "BRIDGE_DRIVER_DIR /nonexistent is not an existing directory"));
    g_dirs.insert("/readonly");
    env["BRIDGE_DRIVER_DIR"] = "/readonly";
    CHECK(mentions(parse(env), "BRIDGE_DRIVER_DIR /readonly is not writable"));
    env["BRIDGE_DRIVER_DIR"] = "relative";
    CHECK(mentions(parse(env), "BRIDGE_DRIVER_DIR must be an absolute path"));
    Env ext = external();
    ext["AERON_DIR"] = "/nonexistent";
    CHECK(mentions(parse(ext), "AERON_DIR /nonexistent is not an existing directory"));
}

TEST_CASE("BridgeConfigTest.driverModeIsStrict")
{
    Env env = external();
    env.erase("BRIDGE_DRIVER_MODE");
    const ConfigResult missing = parse(env);
    const bool missingRefused = !missing.ok() && mentions(missing, "BRIDGE_DRIVER_MODE is required (no default)");
    CHECK(missingRefused); // CONFIG_DRIVER_MODE_DEFAULT RED: a missing mode becomes embedded
    env["BRIDGE_DRIVER_MODE"] = "Embedded";
    CHECK(mentions(parse(env), "BRIDGE_DRIVER_MODE must be exactly embedded or external, got 'Embedded'"));
    env["BRIDGE_DRIVER_MODE"] = "both";
    CHECK(mentions(parse(env), "got 'both'"));

    Env e = embedded();
    e["AERON_DIR"] = "/dev/shm/aeron-host";
    CHECK(mentions(parse(e), "AERON_DIR is set but BRIDGE_DRIVER_MODE=embedded"));
    Env x = external();
    x["BRIDGE_DRIVER_DIR"] = "/dev/shm";
    CHECK(mentions(parse(x), "BRIDGE_DRIVER_DIR is set but the bridge runs on an external driver"));
    x.erase("BRIDGE_DRIVER_DIR");
    x.erase("AERON_DIR");
    CHECK(mentions(parse(x), "AERON_DIR is required with BRIDGE_DRIVER_MODE=external"));

    const ConfigResult okEmbedded = parse(embedded());
    REQUIRE(okEmbedded.ok());
    CHECK(DriverMode::EMBEDDED == okEmbedded.config.driverMode);
    CHECK(okEmbedded.config.driverDir == "/dev/shm");
    CHECK(okEmbedded.config.aeronDir.empty());
    CHECK(parse(external()).ok());
}

TEST_CASE("BridgeConfigTest.driverTimeoutIsStrict")
{
    // An env key per process, missing = refuse (78), not a constant
    // The floor is 3000 ms (two driver heartbeats and a margin); below it is refused, named
    for (const char *bad : { "999", "1000", "2999", "60001", "5s", "-5000", "+5000", "" })
    {
        CAPTURE(bad);
        Env env = external();
        env["BRIDGE_DRIVER_TIMEOUT_MS"] = bad;
        CHECK(mentions(parse(env), "BRIDGE_DRIVER_TIMEOUT_MS"));
    }
    {
        Env env = external();
        env["BRIDGE_DRIVER_TIMEOUT_MS"] = "2999";
        CHECK(mentions(parse(env), "must be an integer in 3000..60000"));
    }
    for (const char *good : { "3000", "5000", "60000" })
    {
        CAPTURE(good);
        Env env = external();
        env["BRIDGE_DRIVER_TIMEOUT_MS"] = good;
        CHECK(parse(env).ok());
    }
}

TEST_CASE("BridgeConfigTest.ingressEndpointsEqualJava")
{
    Env env = external();
    env["BRIDGE_AE_CLUSTER_ADDRESSES"] = "a,b,c";
    env["BRIDGE_AE_PORT_BASE"] = "19300";
    env["CLUSTER_PORT_STRIDE"] = "100";
    const ConfigResult r = parse(env);
    CAPTURE(all(r));
    REQUIRE(r.ok());
    CHECK(r.config.ingressEndpoints == "0=a:19302,1=b:19402,2=c:19502"); // CONFIG_PORT_FORMULA RED (offset 0)
    CHECK(ingressPort(19300, 100, 2) == 19502);
    env["CLUSTER_PORT_STRIDE"] = "0";
    const ConfigResult distinctHosts = parse(env);
    REQUIRE(distinctHosts.ok()); // stride 0 is fine on distinct hosts
    CHECK(distinctHosts.config.ingressEndpoints == "0=a:19302,1=b:19302,2=c:19302");
    env["BRIDGE_AE_CLUSTER_ADDRESSES"] = "a,a";
    CHECK(mentions(parse(env), "members share the ingress endpoint a:19302"));
    env["BRIDGE_AE_CLUSTER_ADDRESSES"] = "localhost,127.0.0.1";
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "localhost:0";
    CHECK(mentions(parse(env), "members share the ingress endpoint"));
    env["BRIDGE_AE_CLUSTER_ADDRESSES"] = "a,b";
    env["BRIDGE_AE_EGRESS_ENDPOINT"] = "10.0.0.9:0";
    env["BRIDGE_AE_PORT_BASE"] = "65534";
    env["CLUSTER_PORT_STRIDE"] = "10";
    CHECK(mentions(parse(env), "is outside 1..65535"));
}

TEST_CASE("BridgeConfigTest.channelsAreTheJavaBridgeLiterals")
{
    CHECK(ingressChannel() == "aeron:udp?term-length=4m|mtu=8192"); // CONFIG_MTU_DEFAULT RED (the 09-02 B shape)
    CHECK(responseChannel("10.0.0.9:0") == "aeron:udp?endpoint=10.0.0.9:0|term-length=4m");
    const ConfigResult r = parse(external());
    REQUIRE(r.ok());
    const cluster_tools::client::ClientSettings s = coreClientSettings(r.config);
    CHECK(s.ingressChannel == "aeron:udp?term-length=4m|mtu=8192");
    CHECK(s.responseChannel == "aeron:udp?endpoint=10.0.0.9:0|term-length=4m");
    CHECK(s.keepAliveIntervalNs == 1'000'000'000LL);
    CHECK(s.messageTimeoutNs == 5'000'000'000LL);
    CHECK(s.maxConnectAttempts == 10);
    CHECK(s.clientName == "ct-bridge");
    CHECK(s.ingressEndpoints == "0=10.0.1.1:19302,1=10.0.1.2:19402,2=10.0.1.3:19502");
    CHECK(s.credentials.size() == 38);
}

TEST_CASE("BridgeConfigTest.everyProblemReportedInOrder")
{
    Env env = external();
    env["BRIDGE_HTTP_BIND"] = "9600";            // 17
    env["BRIDGE_JOURNAL_MTU"] = "1000";          // 5
    env["BRIDGE_INSTANCE_ID"] = "0";             // 16
    env.erase("BRIDGE_ME_JOURNAL_ARCHIVES");     // 0
    env["CLUSTER_PORT_STRIDE"] = "x";            // 13
    const ConfigResult r = parse(env);
    CAPTURE(all(r));
    REQUIRE(r.errors.size() == 5);
    CHECK(r.errors[0].find("BRIDGE_ME_JOURNAL_ARCHIVES") == 0);
    CHECK(r.errors[1].find("BRIDGE_JOURNAL_MTU") == 0);
    CHECK(r.errors[2].find("CLUSTER_PORT_STRIDE") == 0);
    CHECK(r.errors[3].find("BRIDGE_INSTANCE_ID") == 0);
    CHECK(r.errors[4].find("BRIDGE_HTTP_BIND") == 0);
}
