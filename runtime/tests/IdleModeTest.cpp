// IdleModeTest (runtime.idle_mode): the idle vocabulary (three words, an unknown or empty value refused naming the key
// and the allowed values, unset = backoff), the duty policies per mode (backoff = the process's own policy, unchanged),
// the IdleStrategy phases (spin, yield, park; busy spin never parks), the embedded driver's Aeron mapping, and the
// shipped idle overlays (deploy/media-driver/driver-idle-<mode>.properties) against that same mapping: only idle keys,
// every agent role, the values driverIdleFor() gives.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <time.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cluster_tools/runtime/IdleMode.h"
#include "cluster_tools/runtime/IdleStrategy.h"

using namespace cluster_tools::runtime;

namespace {

std::int64_t nowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// key -> value of a properties file (comments and blank lines skipped); `bad` collects malformed lines.
std::map<std::string, std::string> readProperties(const std::string &path, std::vector<std::string> &bad)
{
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t first = line.find_first_not_of(" \t");
        if (std::string::npos == first || '#' == line[first] || '!' == line[first])
        {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (std::string::npos == eq)
        {
            bad.push_back(line);
            continue;
        }
        std::string key = line.substr(first, eq - first);
        while (!key.empty() && (' ' == key.back() || '\t' == key.back()))
        {
            key.pop_back();
        }
        std::string value = line.substr(eq + 1);
        const std::size_t v0 = value.find_first_not_of(" \t");
        value = std::string::npos == v0 ? std::string() : value.substr(v0);
        while (!value.empty() && (' ' == value.back() || '\t' == value.back() || '\r' == value.back()))
        {
            value.pop_back();
        }
        if (out.count(key))
        {
            bad.push_back("duplicate " + key);
        }
        out[key] = value;
    }
    return out;
}

}

TEST_CASE("IdleModeTest.threeWordsParseAndNothingElse")
{
    CHECK(parseIdleMode("busy_spin") == IdleMode::BUSY_SPIN);
    CHECK(parseIdleMode("backoff") == IdleMode::BACKOFF);
    CHECK(parseIdleMode("sleep") == IdleMode::SLEEP);
    for (const char *bad : { "", "BACKOFF", "Sleep", "busy-spin", "busyspin", "spin", "noop", "yield", "sleeping",
             "sleep-ns", " sleep", "sleep ", "backoff,sleep" })
    {
        CAPTURE(bad);
        CHECK_FALSE(parseIdleMode(bad).has_value());
    }
    for (IdleMode m : { IdleMode::BUSY_SPIN, IdleMode::BACKOFF, IdleMode::SLEEP })
    {
        CHECK(parseIdleMode(idleModeName(m)) == m); // the name round-trips
    }
}

TEST_CASE("IdleModeTest.settingUnsetIsBackoffAnUnknownValueIsRefusedNamingKeyAndValues")
{
    std::string error;
    CHECK(parseIdleModeSetting("X_IDLE_MODE", false, "", error) == IdleMode::BACKOFF);
    CHECK(error.empty());
    CHECK(parseIdleModeSetting("X_IDLE_MODE", true, "sleep", error) == IdleMode::SLEEP);
    CHECK(parseIdleModeSetting("X_IDLE_MODE", true, "busy_spin", error) == IdleMode::BUSY_SPIN);
    CHECK(error.empty());
    for (const char *bad : { "", "yield", "SLEEP" })
    {
        CAPTURE(bad);
        error.clear();
        CHECK_FALSE(parseIdleModeSetting("X_IDLE_MODE", true, bad, error).has_value()); // present + empty is refused
        CHECK(error.find("X_IDLE_MODE") != std::string::npos);
        CHECK(error.find("busy_spin, backoff or sleep") != std::string::npos);
        CHECK(error.find(std::string("'") + bad + "'") != std::string::npos);
    }
}

TEST_CASE("IdleModeTest.backoffIsTheProcessPolicyUnchanged")
{
    CHECK(idlePolicyFor(IdleMode::BACKOFF, FORWARDER_IDLE) == FORWARDER_IDLE);
    CHECK(idlePolicyFor(IdleMode::BACKOFF, BRIDGE_IDLE) == BRIDGE_IDLE);
    // today's two policies, pinned (the default must change nothing)
    CHECK(FORWARDER_IDLE == IdlePolicy{ 100, 100, 100'000L, false });
    CHECK(BRIDGE_IDLE == IdlePolicy{ 0, 10, 1'000'000L, false });
    // sleep: no spin, no yield, 1 ms park, whatever the process; busy_spin: never yields or parks
    for (const IdlePolicy own : { FORWARDER_IDLE, BRIDGE_IDLE })
    {
        const IdlePolicy sleep = idlePolicyFor(IdleMode::SLEEP, own);
        CHECK(sleep.spins == 0);
        CHECK(sleep.yields == 0);
        CHECK(sleep.parkNs == 1'000'000L);
        CHECK_FALSE(sleep.busySpin);
        CHECK(idlePolicyFor(IdleMode::BUSY_SPIN, own).busySpin);
    }
}

TEST_CASE("IdleModeTest.strategyPhases")
{
    // spin 2, yield 1, then park: the 4th idle call parks (>= the park period), the first three do not
    IdleStrategy s(IdlePolicy{ 2, 1, 20'000'000L, false });
    for (int i = 0; i < 3; i++)
    {
        const std::int64_t t0 = nowNs();
        s.idle(0);
        CHECK(nowNs() - t0 < 15'000'000);
    }
    std::int64_t t0 = nowNs();
    s.idle(0);
    CHECK(nowNs() - t0 >= 20'000'000);
    t0 = nowNs();
    s.idle(0); // still parked after the phases (saturated, never wraps back to spinning)
    CHECK(nowNs() - t0 >= 20'000'000);
    s.idle(1); // work resets the phases
    t0 = nowNs();
    s.idle(0);
    CHECK(nowNs() - t0 < 15'000'000);

    // sleep: the first idle call already parks 1 ms
    IdleStrategy sleep(SLEEP_IDLE);
    t0 = nowNs();
    sleep.idle(0);
    CHECK(nowNs() - t0 >= 1'000'000);

    // busy spin: a million idle calls in far less than one 1 ms park each would take (no sleep, no syscall)
    IdleStrategy spin(BUSY_SPIN_IDLE);
    t0 = nowNs();
    for (int i = 0; i < 1'000'000; i++)
    {
        spin.idle(0);
    }
    CHECK(nowNs() - t0 < 2'000'000'000LL);
}

TEST_CASE("IdleModeTest.driverMappingIsAeronsNames")
{
    // aeron_idle_strategy_table (aeron-client/src/main/c/aeron_agent.c, Aeron 1.53.1)
    CHECK(std::string(driverIdleFor(IdleMode::BUSY_SPIN).strategy) == "spin");
    CHECK(nullptr == driverIdleFor(IdleMode::BUSY_SPIN).initArgs);
    CHECK(std::string(driverIdleFor(IdleMode::BACKOFF).strategy) == "backoff");
    CHECK(nullptr == driverIdleFor(IdleMode::BACKOFF).initArgs);
    CHECK(std::string(driverIdleFor(IdleMode::SLEEP).strategy) == "sleep-ns");
    CHECK(std::string(driverIdleFor(IdleMode::SLEEP).initArgs) == "1ms");
    CHECK(DRIVER_IDLE_ROLES.size() == 5);
}

TEST_CASE("IdleModeTest.shippedIdleOverlaysSetOnlyIdleKeysWithTheEmbeddedMapping")
{
    std::set<std::string> allowed;
    for (const DriverIdleRole &role : DRIVER_IDLE_ROLES)
    {
        allowed.insert(role.strategyKey);
        allowed.insert(role.initArgsKey);
    }
    for (IdleMode mode : { IdleMode::BUSY_SPIN, IdleMode::BACKOFF, IdleMode::SLEEP })
    {
        const std::string path = std::string(CT_MEDIA_DRIVER_DIR) + "/driver-idle-" + idleModeName(mode) + ".properties";
        CAPTURE(path);
        std::vector<std::string> bad;
        const auto props = readProperties(path, bad);
        CHECK(bad.empty());
        for (const auto &[key, value] : props)
        {
            CAPTURE(key);
            CHECK(allowed.count(key) == 1); // only idle keys: never the threading mode or anything else
        }
        const DriverIdleStrategy want = driverIdleFor(mode);
        for (const DriverIdleRole &role : DRIVER_IDLE_ROLES)
        {
            CAPTURE(role.strategyKey);
            REQUIRE(props.count(role.strategyKey) == 1); // every agent role
            CHECK(props.at(role.strategyKey) == want.strategy);
            // init args: the mapping's, or empty (an empty value unsets an earlier file's, aeron_properties_setenv)
            const auto args = props.find(role.initArgsKey);
            REQUIRE(args != props.end());
            CHECK(args->second == (nullptr == want.initArgs ? std::string() : std::string(want.initArgs)));
        }
    }
}

TEST_CASE("IdleModeTest.profileOverlaysStillParseAndKeepTheirThreading")
{
    // the idle overlays never touch threading; the profile overlays still own it
    for (const char *profile : { "dev", "prod" })
    {
        std::vector<std::string> bad;
        const auto props = readProperties(std::string(CT_MEDIA_DRIVER_DIR) + "/driver-" + profile + ".properties", bad);
        CHECK(bad.empty());
        CHECK(props.count("aeron.threading.mode") == 1);
    }
}
