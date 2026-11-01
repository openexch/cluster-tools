// RC2 S10' Q1 (coordinator decision 2026-11-01): an ingress publication refused for storage space on every attempt for
// 30 s ends ct-forwarder 70; until then /ready names storage_starved and forwarder_storage_starved_seconds grows.
//   StorageStarvationTest.*            the shared common::StorageStarvation rule (pure)
//   ForwarderAgentTest.storageStarvedIngressExits70  the agent over FakeTransport: the ME's ingress publications are
//       refused with the C client's exact text; alive and not ready at 29 s, exit 70 F_STORAGE_STARVED within 30 s + one
//       duty iteration; RED STORAGE_STARVED_EXIT_OFF: alive after 60 s ("alive after 30 s of storage starvation")
//   ForwarderAgentTest.storageStarvationEndsWhenAnAttemptSucceedsOrFailsOtherwise  the episode resets (C4, twice)
#include "doctest/doctest.h"

#include <string>

#include "AgentRig.h"
#include "MetricsRender.h"

#include "common/StorageStarvation.h"

using namespace cluster_tools::app::forwarder;
using namespace cluster_tools::app::forwarder::test;
namespace common = cluster_tools::app::common;

namespace {

// observed on the build box (S10' lane C tmpfs-min, 48 MB): the forwarder's ME connect cycle on a full tmpfs
const char *const OBSERVED_STORAGE =
    "ingress publication: (-12) insufficient storage space [aeron_client_conductor_async_resource_poll, "
    "aeron_client_conductor.c:3717] async_add_publication registration error";

// every publication the clients added so far is refused: the async registration error of a full driver dir
void refuseAll(FakeTransport &t, const std::string &error)
{
    t.error = error;
    for (auto &p : t.publications)
    {
        if (!p.closed && cluster_tools::client::Registration::PENDING == p.status)
        {
            p.status = cluster_tools::client::Registration::FAILED;
        }
    }
}

}

TEST_CASE("StorageStarvationTest.classifiesAeronStorageSpaceOnly")
{
    CHECK(common::isStorageSpaceError(OBSERVED_STORAGE));
    CHECK(common::isStorageSpaceError("insufficient usable storage for new log of length=12582912"));
    CHECK_FALSE(common::isStorageSpaceError("no ingress publication could be added: ingress publication: (-1) generic error"));
    CHECK_FALSE(common::isStorageSpaceError("connect timed out [attempt 3/3]"));
    CHECK_FALSE(common::isStorageSpaceError(""));
}

TEST_CASE("StorageStarvationTest.thirtySecondsContinuousThenExpired")
{
    common::StorageStarvation w;
    const std::int64_t s = 1'000'000'000LL;
    CHECK(w.onStarved(5 * s, "first"));
    CHECK_FALSE(w.onStarved(6 * s, "second")); // the onset is the first failure; its text is kept
    CHECK(std::string(w.why()) == "first");
    CHECK(w.active());
    CHECK_FALSE(w.expired(5 * s + common::STORAGE_STARVED_EXIT_NS - 1));
    CHECK(w.expired(5 * s + common::STORAGE_STARVED_EXIT_NS));
    CHECK(w.starvedSeconds(20 * s) == 15);
    // relieved: not continuous any more; the counter keeps the 15 s; a new episode starts from its own onset
    w.onRelieved(20 * s);
    CHECK_FALSE(w.active());
    CHECK_FALSE(w.expired(100 * s));
    CHECK(w.starvedSeconds(100 * s) == 15);
    CHECK(w.onStarved(100 * s, "again"));
    CHECK_FALSE(w.expired(129 * s));
    CHECK(w.expired(130 * s));
    CHECK(w.starvedSeconds(130 * s) == 45);
    CHECK(w.episodes() == 2);
    CHECK(common::STORAGE_STARVED_EXIT_NS == 30 * s); // a constant in code, not a key
}

TEST_CASE("ForwarderAgentTest.storageStarvedIngressExits70")
{
    Rig r;
    // both clusters' ingress publications wait for their registration, which the full tmpfs refuses (the box's 48 MB case)
    r.meT.autoReady = false;
    r.coreT.autoReady = false;
    const std::int64_t step = 10 * MS;
    std::int64_t onsetNs = -1;
    bool aliveNotReadyAt29 = false;
    std::uint64_t secondsAt29 = 0;
    for (int i = 0; i < 6000 && !r.agent.stopped(); i++) // 60 s
    {
        refuseAll(r.meT, OBSERVED_STORAGE);
        refuseAll(r.coreT, OBSERVED_STORAGE);
        r.cycle(step);
        ForwarderSnapshot s;
        r.agent.fillSnapshot(s);
        if (onsetNs < 0 && 0 != (s.notReady & not_ready::STORAGE_STARVED))
        {
            onsetNs = r.coreT.now;
        }
        if (onsetNs >= 0 && r.coreT.now - onsetNs >= 29 * SECOND && r.coreT.now - onsetNs < 29 * SECOND + step)
        {
            aliveNotReadyAt29 = !r.agent.stopped() && !s.ready && 0 != (s.notReady & not_ready::STORAGE_STARVED);
            secondsAt29 = s.storageStarvedSeconds;
            CHECK(renderReady(s).find("storage_starved") != std::string::npos);
        }
    }
    REQUIRE(onsetNs >= 0);
    CHECK(aliveNotReadyAt29);
    CHECK(secondsAt29 >= 28);
    CHECK(secondsAt29 <= 30); // wall seconds with either starved, not a sum over the two publications
    const bool exited70 = r.agent.stopped() && 70 == r.agent.stopReason().exitCode;
    CHECK_MESSAGE(exited70, "alive after 30 s of storage starvation"); // STORAGE_STARVED_EXIT_OFF RED
    if (exited70)
    {
        CHECK(std::string(r.agent.stopReason().code) == "F_STORAGE_STARVED");
        CHECK(std::string(r.agent.stopReason().detail).find("insufficient storage space") != std::string::npos);
        // at most one duty iteration after 30 s from the first refused attempt (the onset, seen within the same step)
        const std::int64_t took = r.coreT.now - onsetNs;
        CHECK(took >= common::STORAGE_STARVED_EXIT_NS);
        CHECK(took <= common::STORAGE_STARVED_EXIT_NS + 2 * step);
    }
}

TEST_CASE("ForwarderAgentTest.storageStarvationEndsWhenAnAttemptSucceedsOrFailsOtherwise")
{
    // the phases advance by observed connect attempts (the agent's reconnect backoff spaces the cycles 1, 2, 4, 8 s ...;
    // between two attempts the last evidence stands): an attempt that fails otherwise ends the episode, a storage
    // refusal starts a new one, a connected client ends it again; no phase reaches 30 s, so the process never exits
    Rig r;
    r.meT.autoReady = false;
    const std::int64_t step = 10 * MS;
    auto meFailed = [&] { return r.me.counters().connectAttemptsFailed; };
    auto starved = [&] { return 0 != (r.agent.notReadyMask() & not_ready::STORAGE_STARVED); };
    // run with `error` on every new publication until `done`, at most `limitNs`
    auto runRefusing = [&](const std::string &error, auto done, std::int64_t limitNs) {
        const std::int64_t end = r.coreT.now + limitNs;
        while (r.coreT.now < end && !r.agent.stopped() && !done())
        {
            refuseAll(r.meT, error);
            r.cycle(step);
        }
        return done();
    };
    // A: starved, held 5 s more
    REQUIRE(runRefusing(OBSERVED_STORAGE, starved, 10 * SECOND));
    const std::int64_t onsetA = r.coreT.now;
    runRefusing(OBSERVED_STORAGE, [&] { return r.coreT.now - onsetA >= 5 * SECOND; }, 6 * SECOND);
    CHECK(starved());
    // B: the next attempt fails for another reason: the episode ends
    const std::uint64_t beforeB = meFailed();
    REQUIRE(runRefusing("ingress publication: (-1) generic error", [&] { return meFailed() > beforeB; }, 20 * SECOND));
    CHECK_FALSE(starved());
    CHECK(r.coreT.now - onsetA < 30 * SECOND);
    // C: a storage refusal again: a NEW episode from its own onset
    REQUIRE(runRefusing(OBSERVED_STORAGE, starved, 30 * SECOND));
    const std::int64_t onsetC = r.coreT.now;
    CHECK_FALSE(r.agent.stopped());
    // D: the tmpfs is freed: the next cycle connects, the episode ends again, the process never exited
    r.meT.autoReady = true;
    for (auto &p : r.meT.publications)
    {
        if (!p.closed && cluster_tools::client::Registration::PENDING == p.status)
        {
            p.status = cluster_tools::client::Registration::READY;
        }
    }
    REQUIRE(r.agent.stopped() == false);
    const std::int64_t end = r.coreT.now + 40 * SECOND;
    while (r.coreT.now < end && !r.agent.stopped() && !r.me.isConnected())
    {
        r.cycle(step);
    }
    CHECK(r.me.isConnected());
    r.cycles(5);
    CHECK_FALSE(starved());
    CHECK_FALSE(r.agent.stopped());
    CHECK(r.coreT.now - onsetC < 30 * SECOND);
    ForwarderSnapshot s;
    r.agent.fillSnapshot(s);
    CHECK(s.storageStarvedSeconds >= 5); // both episodes counted (A >= 5 s, C until the connect)
}
