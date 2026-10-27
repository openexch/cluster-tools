// Strict archive settings (RC2 S9a contract 4.1, 9.1): every error reported, never a default.
#include "doctest/doctest.h"

#include "cluster_tools/archive/ArchiveSettings.h"

#include <map>
#include <string>

using namespace cluster_tools::archive;

namespace {

std::map<std::string, std::string> good()
{
    return {
        { "BRIDGE_ME_JOURNAL_ARCHIVES", "10.0.0.1:9010,10.0.0.2:9110,10.0.0.3:9210" },
        { "BRIDGE_JOURNAL_CONTROL_STREAM", "4010" },
        { "BRIDGE_JOURNAL_STREAM", "4001" },
        { "BRIDGE_JOURNAL_CHANNEL", "aeron:ipc" },
        { "BRIDGE_REPLAY_STREAM", "9101" },
        { "BRIDGE_LOCAL_HOST", "10.0.0.9" },
        { "BRIDGE_JOURNAL_MTU", "1408" },
        { "BRIDGE_DRIVER_DIR", "/dev/shm" },
    };
}

ArchiveSettingsResult parse(const std::map<std::string, std::string> &env, SettingsScope scope = SettingsScope::BRIDGE)
{
    return parseArchiveSettings(
        [&](const std::string &key) -> std::optional<std::string> {
            auto it = env.find(key);
            if (it == env.end())
            {
                return std::nullopt;
            }
            return it->second;
        },
        scope);
}

bool mentions(const ArchiveSettingsResult &r, const std::string &needle)
{
    for (const auto &e : r.errors)
    {
        if (e.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

}

TEST_CASE("ArchiveSettingsTest.goodSettingsParse")
{
    const auto r = parse(good());
    REQUIRE(r.ok());
    CHECK(r.errors.empty());
    const ArchiveSettings &s = *r.settings;
    REQUIRE(s.archives.size() == 3);
    CHECK(s.archives[0].host == "10.0.0.1");
    CHECK(s.archives[0].port == 9010);
    CHECK(s.archives[2].text() == "10.0.0.3:9210");
    CHECK(s.controlStreamId == 4010);
    CHECK(s.journalStreamId == 4001);
    CHECK(s.journalChannel == "aeron:ipc");
    CHECK(s.replayStreamId == 9101);
    CHECK(s.localHost == "10.0.0.9");
    CHECK(s.journalMtu == 1408);
    CHECK(s.driverDir == "/dev/shm");
}

TEST_CASE("ArchiveSettingsTest.missingKeyRefuses")
{
    for (const auto &[key, value] : good())
    {
        CAPTURE(key);
        auto env = good();
        env.erase(key);
        const auto r = parse(env);
        CHECK_FALSE(r.ok());
        CHECK(mentions(r, key + " is required"));
        env[key] = "";
        CHECK(mentions(parse(env), key + " is empty"));
    }
    // a tool runs on an external driver: BRIDGE_DRIVER_DIR is the bridge process's key only
    auto env = good();
    env.erase("BRIDGE_DRIVER_DIR");
    CHECK(parse(env, SettingsScope::TOOL).ok());
}

TEST_CASE("ArchiveSettingsTest.haltOnGapPresentRefuses")
{
    auto env = good();
    env["BRIDGE_HALT_ON_GAP"] = "false";
    const auto r = parse(env);
    CHECK_FALSE(r.ok());
    CHECK(mentions(r, "BRIDGE_HALT_ON_GAP"));
}

TEST_CASE("ArchiveSettingsTest.loopbackLocalHostWithRemoteArchiveRefuses")
{
    auto env = good();
    env["BRIDGE_LOCAL_HOST"] = "localhost";
    CHECK(mentions(parse(env), "is loopback"));
    env["BRIDGE_LOCAL_HOST"] = "127.0.0.1";
    CHECK(mentions(parse(env), "is loopback"));
    env["BRIDGE_LOCAL_HOST"] = "::1";
    CHECK(mentions(parse(env), "is loopback"));
    // loopback archives with a loopback local host: fine (one box)
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = "127.0.0.1:9010,localhost:9110";
    env["BRIDGE_LOCAL_HOST"] = "127.0.0.1";
    CHECK(parse(env).ok());
    // a routable local host with loopback archives: fine
    env["BRIDGE_LOCAL_HOST"] = "10.0.0.9";
    CHECK(parse(env).ok());
    // one remote archive among loopback ones is enough to refuse
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = "127.0.0.1:9010,10.0.0.2:9110";
    env["BRIDGE_LOCAL_HOST"] = "127.0.0.5";
    CHECK(mentions(parse(env), "is loopback"));
    CHECK(isLoopbackHost("127.255.0.1"));
    // every 127/8 form (review N-12)
    CHECK(isLoopbackHost("127.1"));
    CHECK(isLoopbackHost("127.0.0"));
    CHECK(isLoopbackHost("0x7f.1"));
    CHECK(isLoopbackHost("2130706433"));
    CHECK(isLoopbackHost("::ffff:127.0.0.1"));
    CHECK(isLoopbackHost("[::1]"));
    CHECK(isLoopbackHost("LOCALHOST"));
    CHECK_FALSE(isLoopbackHost("128.0.0.1"));
    CHECK_FALSE(isLoopbackHost("localhost.example"));
    CHECK_FALSE(isLoopbackHost("::2"));
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = "10.0.0.2:9110";
    env["BRIDGE_LOCAL_HOST"] = "127.1";
    CHECK(mentions(parse(env), "is loopback"));
}

TEST_CASE("ArchiveSettingsTest.journalMtuBounds")
{
    auto env = good();
    for (const char *ok : { "1408", "1152", "8192", "65504" })
    {
        CAPTURE(ok);
        env["BRIDGE_JOURNAL_MTU"] = ok;
        CHECK(parse(env).ok());
    }
    for (const char *bad : { "1112", "1120", "1400", "65536", "0", "-1408", "1408 ", "0x580", "abc" })
    {
        CAPTURE(bad);
        env["BRIDGE_JOURNAL_MTU"] = bad;
        CHECK_FALSE(parse(env).ok());
    }
}

TEST_CASE("ArchiveSettingsTest.archiveListIsStrict")
{
    auto env = good();
    for (const char *bad : { "10.0.0.1", "10.0.0.1:0", "10.0.0.1:65536", ":9010", "10.0.0.1:9010,", "a:1,a:1",
             "10.0.0.1:90x0", "[::1:9010" })
    {
        CAPTURE(bad);
        env["BRIDGE_ME_JOURNAL_ARCHIVES"] = bad;
        CHECK_FALSE(parse(env).ok());
    }
    std::string seventeen;
    for (int i = 0; i < 17; ++i)
    {
        seventeen += (i ? "," : "") + std::string("10.0.0.1:") + std::to_string(9000 + i);
    }
    // duplicates compared in normalised form (review N-12)
    for (const char *dup : { "localhost:9010,127.0.0.1:9010", "127.1:9010,127.0.0.1:9010", "H1:9010,h1:9010",
             "[::1]:9010,[0:0:0:0:0:0:0:1]:9010" })
    {
        CAPTURE(dup);
        env["BRIDGE_ME_JOURNAL_ARCHIVES"] = dup;
        env["BRIDGE_LOCAL_HOST"] = "127.0.0.1";
        CHECK(mentions(parse(env), "twice"));
    }
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = "127.0.0.1:9010,127.0.0.1:9110";
    CHECK(parse(env).ok());
    env["BRIDGE_LOCAL_HOST"] = "10.0.0.9";
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = seventeen;
    CHECK(mentions(parse(env), "1..16"));
    env["BRIDGE_ME_JOURNAL_ARCHIVES"] = "[fd00::1]:9010";
    env["BRIDGE_LOCAL_HOST"] = "fd00::9";
    const auto r = parse(env);
    REQUIRE(r.ok());
    CHECK(r.settings->archives[0].host == "fd00::1");
    CHECK(r.settings->archives[0].text() == "[fd00::1]:9010");
}

TEST_CASE("ArchiveSettingsTest.streamsAndChannel")
{
    auto env = good();
    env["BRIDGE_REPLAY_STREAM"] = "4001";
    CHECK(mentions(parse(env), "must differ"));
    env = good();
    env["BRIDGE_JOURNAL_CHANNEL"] = "ipc";
    CHECK(mentions(parse(env), "must start with aeron:"));
    env = good();
    env["BRIDGE_JOURNAL_STREAM"] = "0";
    CHECK(mentions(parse(env), "int32 > 0"));
    env["BRIDGE_JOURNAL_STREAM"] = "2147483648";
    CHECK(mentions(parse(env), "int32 > 0"));
    env = good();
    env["BRIDGE_DRIVER_DIR"] = "relative/dir";
    CHECK(mentions(parse(env), "absolute"));
}
