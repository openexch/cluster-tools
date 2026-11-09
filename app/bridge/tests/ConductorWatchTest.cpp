// ConductorWatchCore over a fake counters file, as main wires it
// (revalidation on unless the seamed build's CONDUCTOR_REVALIDATE_OFF):
//   reclaimedAndReusedSlotExits70   BRIDGE_DRIVER_TIMEOUT_MS 20000 above the driver's client liveness (10 s): the driver
//                                   reclaims the wedged client and re-allocates its heartbeat slot to another client
//                                   with a fresh stamp; the watch must fail (exit 70) instead of reading the fresh stamp
//                                   (RED CONDUCTOR_REVALIDATE_OFF: it stays healthy)
//   counterNeverFoundExits70        no heartbeat counter within the timeout from the first check: fail, never silently off
//   processPauseIsNamed             a stale stamp after a check gap >= the timeout names a process pause; without the
//                                   gap it names a wedged conductor (both fail: exit 70)
#include "doctest/doctest.h"

#include <map>
#include <string>

#include "AppSeams.h"
#include "common/ConductorWatchCore.h"

namespace {

using cluster_tools::app::common::ConductorWatchCore;
using cluster_tools::app::common::HeartbeatCounters;
namespace app_seams = cluster_tools::app::bridge::app_seams;

struct Record
{
    std::int32_t state = 0;
    std::int32_t type = 0;
    std::int64_t key = 0;
    std::int64_t value = 0;
};

struct FakeCounters final : HeartbeatCounters
{
    std::map<std::int32_t, Record> records;
    std::int32_t maxCounterId() const override { return 64; }
    std::int32_t recordState(std::int32_t id) const override { return at(id).state; }
    std::int32_t typeId(std::int32_t id) const override { return at(id).type; }
    std::int64_t key(std::int32_t id) const override { return at(id).key; }
    std::int64_t value(std::int32_t id) const override { return at(id).value; }
    Record at(std::int32_t id) const
    {
        const auto it = records.find(id);
        return records.end() == it ? Record{} : it->second;
    }
};

constexpr std::int64_t CLIENT = 7;
constexpr std::int32_t SLOT = 48;

bool revalidate()
{
    return !app_seams::disabled(app_seams::Rule::CONDUCTOR_REVALIDATE_OFF); // exactly main.cpp's argument
}

}

TEST_CASE("ConductorWatchTest.reclaimedAndReusedSlotExits70")
{
    FakeCounters counters;
    counters.records[3] = { 1, 11, 99, 0 };               // another client's heartbeat
    counters.records[SLOT] = { 1, 11, CLIENT, 1'000'000 }; // ours
    ConductorWatchCore watch(counters, CLIENT, 20'000, revalidate());
    std::string why;
    std::int64_t now = 1'000'000;
    REQUIRE(watch.check(now, why));
    REQUIRE(watch.counterId() == SLOT);
    // the conductor wedges at now: no more stamps. At +10 s the driver reclaims the client (the record is freed) and
    // re-allocates the slot to the next client that connects, which stamps it fresh
    bool healthyWhileOursWasStillThere = true;
    for (int i = 1; i <= 100; ++i) // 10 s at main's 100 ms cadence
    {
        healthyWhileOursWasStillThere = healthyWhileOursWasStillThere && watch.check(now + 100 * i, why);
    }
    CHECK(healthyWhileOursWasStillThere);
    now += 10'000;
    counters.records[SLOT] = { -1, 11, CLIENT, 1'000'000 }; // RECORD_RECLAIMED
    now += 100;
    const bool freedIsDetected = !watch.check(now, why);
    counters.records[SLOT] = { 1, 11, 4242, now }; // re-used: another client's heartbeat, fresh
    now += 100;
    std::string reusedWhy;
    const bool reclaimDetected = !watch.check(now, reusedWhy);
    MESSAGE("freed: " << why << " | reused: " << reusedWhy);
    CHECK(reclaimDetected); // CONDUCTOR_REVALIDATE_OFF RED: the re-used slot's fresh stamp reads healthy
    CHECK(freedIsDetected);
    CHECK(reusedWhy.find("is no longer this client's") != std::string::npos);
    CHECK(reusedWhy.find("the driver reclaimed this Aeron client") != std::string::npos);
}

TEST_CASE("ConductorWatchTest.counterNeverFoundExits70")
{
    FakeCounters counters;
    counters.records[3] = { 1, 11, 99, 5 }; // someone else's only
    ConductorWatchCore watch(counters, CLIENT, 5'000, revalidate());
    std::string why;
    CHECK(watch.check(100'000, why));
    CHECK(watch.check(104'999, why));
    CHECK(watch.ageMs() == -1);
    const bool neverFoundFails = !watch.check(105'000, why);
    CHECK(neverFoundFails);
    CHECK(why.find("not found within the driver timeout 5000 ms") != std::string::npos);
}

TEST_CASE("ConductorWatchTest.processPauseIsNamed")
{
    for (const bool paused : { true, false })
    {
        CAPTURE(paused);
        FakeCounters counters;
        counters.records[SLOT] = { 1, 11, CLIENT, 200'000 };
        ConductorWatchCore watch(counters, CLIENT, 5'000, revalidate());
        std::string why;
        REQUIRE(watch.check(200'050, why));
        bool failed = false;
        if (paused)
        {
            failed = !watch.check(215'100, why); // SIGSTOP 15 s: the next wake is 15 s later, the stamp 15 s old
        }
        else
        {
            for (std::int64_t t = 200'150; t <= 206'000 && !failed; t += 100) // a wedged conductor, main wakes on time
            {
                failed = !watch.check(t, why);
            }
        }
        CHECK(failed);
        if (paused)
        {
            CHECK(why.find("after a process pause of 15050 ms") != std::string::npos);
            CHECK(why.find("not a conductor or driver wedge") != std::string::npos);
        }
        else
        {
            CHECK(why.find("the Aeron client conductor is wedged") != std::string::npos);
        }
    }
}
