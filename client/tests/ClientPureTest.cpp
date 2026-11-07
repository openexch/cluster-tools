// The pure client parts: ingress endpoint parsing, core credentials (byte-compatible with
// assets CoreCredentials), backoff, settings validation. Runs in the no-aeron build.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "cluster_tools/client/Backoff.h"
#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/Credentials.h"
#include "cluster_tools/client/IngressEndpoints.h"
#include "cluster_tools/client/Protocol.h"

using namespace cluster_tools::client;

TEST_CASE("ingress endpoints: the AeronCluster form, strictly")
{
    const ParsedEndpoints ok = parseIngressEndpoints("0=10.0.0.1:9302,1=10.0.0.2:9302,2=h3:9302");
    REQUIRE(ok.ok());
    REQUIRE(ok.members.size() == 3);
    CHECK(ok.members[2].id == 2);
    CHECK(ok.members[2].endpoint == "h3:9302");
    CHECK(ok.find(1)->endpoint == "10.0.0.2:9302");
    CHECK(nullptr == ok.find(7));

    CHECK_FALSE(parseIngressEndpoints("").ok());
    CHECK_FALSE(parseIngressEndpoints("0=a:1,").ok());          // trailing empty token
    CHECK_FALSE(parseIngressEndpoints("0=a:1,,1=b:2").ok());
    CHECK_FALSE(parseIngressEndpoints("x=a:1").ok());
    CHECK_FALSE(parseIngressEndpoints("-1=a:1").ok());
    CHECK_FALSE(parseIngressEndpoints("0=a").ok());              // no port
    CHECK_FALSE(parseIngressEndpoints("0=a:1,0=b:2").ok());      // duplicate id
    CHECK_FALSE(parseIngressEndpoints("0a=a:1").ok());
}

TEST_CASE("ingress channel: endpoint appended per member, base validated")
{
    CHECK(ingressChannelFor("aeron:udp", "h:1") == "aeron:udp?endpoint=h:1");
    CHECK(ingressChannelFor("aeron:udp?term-length=4m|mtu=8192", "h:1") ==
        "aeron:udp?term-length=4m|mtu=8192|endpoint=h:1");
    CHECK(validateIngressBaseChannel("aeron:udp?mtu=8192").empty());
    CHECK_FALSE(validateIngressBaseChannel("aeron:ipc").empty());
    CHECK_FALSE(validateIngressBaseChannel("aeron:udp?endpoint=h:1").empty());
}

TEST_CASE("core credentials: byte-identical to assets CoreCredentials.encode()")
{
    // TestAuth FORWARDER key: "6:FORWARDER:" + "66" x 32 (assets TestAuth.java).
    const CredentialsResult r = parseCoreCredentials("6:FORWARDER:" + std::string(64, '6'));
    REQUIRE(r.ok());
    const std::vector<std::uint8_t> bytes = r.credentials.encode();
    REQUIRE(bytes.size() == 38);
    CHECK(bytes[0] == 1);    // version
    CHECK(bytes[1] == 2);    // FORWARDER
    CHECK(bytes[2] == 6);    // keyId little endian
    CHECK(bytes[3] == 0);
    CHECK(bytes[4] == 0);
    CHECK(bytes[5] == 0);
    for (std::size_t i = 6; i < 38; i++)
    {
        CHECK(bytes[i] == 0x66);
    }
    // class by code and case-insensitively; negative key ids encode two's complement LE
    CHECK((parseCoreCredentials("3:bridge:" + std::string(64, 'a')).credentials.sessionClass == SessionClass::BRIDGE));
    CHECK((parseCoreCredentials("3:4:" + std::string(64, 'A')).credentials.sessionClass == SessionClass::ADMIN));
    const auto negative = parseCoreCredentials("-2:TOOL:" + std::string(64, '0')).credentials.encode();
    CHECK(negative[2] == 0xFE);
    CHECK(negative[5] == 0xFF);
}

TEST_CASE("core credentials: every malformed spec is refused")
{
    CHECK_FALSE(parseCoreCredentials("").ok());
    CHECK_FALSE(parseCoreCredentials("6:FORWARDER").ok());
    CHECK_FALSE(parseCoreCredentials("6:FORWARDER:aa:bb").ok());
    CHECK_FALSE(parseCoreCredentials("x:FORWARDER:" + std::string(64, '6')).ok());
    CHECK_FALSE(parseCoreCredentials("6:UNSET:" + std::string(64, '6')).ok());
    CHECK_FALSE(parseCoreCredentials("6:0:" + std::string(64, '6')).ok());
    CHECK_FALSE(parseCoreCredentials("6:9:" + std::string(64, '6')).ok());
    CHECK_FALSE(parseCoreCredentials("6:FORWARDER:" + std::string(62, '6')).ok());
    CHECK_FALSE(parseCoreCredentials("6:FORWARDER:" + std::string(63, '6') + "g").ok());
}

TEST_CASE("credentials from the environment: missing refuses, the class is asserted")
{
    ::unsetenv("CT_TEST_CREDENTIAL");
    CHECK_FALSE(credentialsFromEnvironment(SessionClass::FORWARDER, "CT_TEST_CREDENTIAL").ok());
    ::setenv("CT_TEST_CREDENTIAL", ("3:BRIDGE:" + std::string(64, '3')).c_str(), 1);
    const CredentialsResult wrong = credentialsFromEnvironment(SessionClass::FORWARDER, "CT_TEST_CREDENTIAL");
    CHECK_FALSE(wrong.ok());
    CHECK(wrong.error.find("BRIDGE") != std::string::npos);
    CHECK(credentialsFromEnvironment(SessionClass::BRIDGE, "CT_TEST_CREDENTIAL").ok());
    CHECK(credentialsFromEnvironment(SessionClass::UNSET, "CT_TEST_CREDENTIAL").ok());
    ::unsetenv("CT_TEST_CREDENTIAL");
}

TEST_CASE("backoff: doubles from the initial delay up to the cap, reset restarts")
{
    ExponentialBackoff backoff(100, 1000);
    CHECK(backoff.next() == 100);
    CHECK(backoff.next() == 200);
    CHECK(backoff.next() == 400);
    CHECK(backoff.next() == 800);
    CHECK(backoff.next() == 1000);
    CHECK(backoff.next() == 1000);
    backoff.reset();
    CHECK(backoff.next() == 100);
}

TEST_CASE("settings: strict, no silent defaults for endpoints, channel or credentials")
{
    ClientSettings s;
    CHECK_FALSE(validate(s).empty()); // no endpoints
    s.ingressEndpoints = "0=10.0.0.1:9302";
    CHECK(validate(s).find("responseChannel") != std::string::npos);
    s.responseChannel = "aeron:udp?endpoint=10.0.0.9:0";
    CHECK(validate(s).find("credentials") != std::string::npos);
    s.credentials = { 1 };
    CHECK(validate(s).empty());
    s.maxConnectAttempts = 0;
    CHECK_FALSE(validate(s).empty());
    s.maxConnectAttempts = 1;
    s.keepAliveIntervalNs = 0;
    CHECK_FALSE(validate(s).empty());
    s.disableKeepAliveForTest = true;
    s.keepAliveIntervalNs = 1'000'000'000LL;
    CHECK(validate(s).empty());
    s.egressStreamId = s.ingressStreamId;
    CHECK_FALSE(validate(s).empty());
}

TEST_CASE("vocabulary names are stable (they appear in logs and metrics)")
{
    CHECK(std::string(nameOf(ClientState::AWAIT_NEW_LEADER_CONNECTION)) == "AWAIT_NEW_LEADER_CONNECTION");
    CHECK(std::string(nameOf(OfferStatus::BACK_PRESSURED)) == "BACK_PRESSURED");
    CHECK(std::string(nameOf(CloseReason::NEW_LEADER_TIMEOUT)) == "NEW_LEADER_TIMEOUT");
    CHECK(std::string(nameOf(ConnectError::AUTHENTICATION_REJECTED)) == "AUTHENTICATION_REJECTED");
    CHECK(std::string(nameOf(SessionClass::FORWARDER)) == "FORWARDER");
}

// ---- added by the S11 independent review (notebook .../RC2-me-link/S11/review.md) ----

namespace
{
std::string hexOf(const std::vector<std::uint8_t> &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes)
    {
        out += digits[b >> 4];
        out += digits[b & 0x0F];
    }
    return out;
}
}

TEST_CASE("review: core credentials equal Java CoreCredentials.fromSpec(spec).encode() byte for byte")
{
    // Goldens produced by assets a9a865d CoreCredentials (assets-cluster.jar of the IT run):
    // java -cp assets-cluster.jar:. CredGolden, one line per spec below. A non-uniform secret and
    // multi-byte key ids, so byte order, nibble order and the secret position are all pinned.
    struct Golden { std::string spec; std::string hex; };
    const Golden goldens[] = {
        { "16909060:FORWARDER:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
          "010204030201000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f" },
        { "-2:bridge:FFEEDDCCBBAA99887766554433221100F0E1D2C3B4A5968778695A4B3C2D1E0F",
          "0103feffffffffeeddccbbaa99887766554433221100f0e1d2c3b4a5968778695a4b3c2d1e0f" },
        { " 7 : 6 : 0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF ",
          "0106070000000123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" },
    };
    for (std::size_t i = 0; i < sizeof(goldens) / sizeof(goldens[0]); i++)
    {
        const CredentialsResult r = parseCoreCredentials(goldens[i].spec);
        REQUIRE_MESSAGE(r.ok(), goldens[i].spec << ": " << r.error);
        CHECK(hexOf(r.credentials.encode()) == goldens[i].hex);
    }
    std::string a5;
    for (int i = 0; i < 32; i++)
    {
        a5 += "a5";
    }
    const CredentialsResult maxKey = parseCoreCredentials("2147483647:4:" + a5);
    REQUIRE(maxKey.ok());
    CHECK(hexOf(maxKey.credentials.encode()) == "0104ffffff7f" + a5);
}

TEST_CASE("review: a timeout large enough to overflow now + timeout is refused")
{
    // Review F2 (was RED): validate() bounds every timeout at MAX_TIMEOUT_NS (1 h).
    ClientSettings s;
    s.ingressEndpoints = "0=10.0.0.1:9302";
    s.responseChannel = "aeron:udp?endpoint=10.0.0.9:0";
    s.credentials = { 1 };
    REQUIRE(validate(s).empty());
    s.messageTimeoutNs = INT64_MAX;
    CHECK_FALSE(validate(s).empty());
    s.messageTimeoutNs = 5'000'000'000LL;
    s.newLeaderTimeoutNs = INT64_MAX;
    CHECK_FALSE(validate(s).empty());
    s.newLeaderTimeoutNs = 0;
    s.connectBackoffMaxNs = INT64_MAX;
    CHECK_FALSE(validate(s).empty());
    s.connectBackoffMaxNs = MAX_TIMEOUT_NS;
    CHECK(validate(s).empty());
    s.keepAliveRetryIntervalNs = MAX_TIMEOUT_NS + 1;
    CHECK_FALSE(validate(s).empty());
}
