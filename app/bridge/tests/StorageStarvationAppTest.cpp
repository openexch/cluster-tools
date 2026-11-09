// Storage starvation for ct-bridge: storage starvation held for 30 s ends the process 70;
// until then /ready names storage_starved and bridge_storage_starved_seconds_total grows.
//   coreIngressStarvedOfStorageExits70  the core ingress publication refused on every attempt with the C client's text
//   replayStarvedOfStorageExits70       every replay image refused for storage (the precheck counter) and no source
//                                       FOLLOWING: the 240 MB tmpfs shape (61 REPLAY_STORAGE faults, alive)
//   replayStarvationEndsWhenASourceFollowsOrFailsOtherwise  the episode resets (C4, twice)
// RED STORAGE_STARVED_EXIT_OFF: alive after 60 s ("alive after 30 s of storage starvation").
#include "doctest/doctest.h"

#include <string>

#include "AppRig.h"

using namespace ct_bridge_app_test;
namespace bridge_ready = cluster_tools::app::bridge::not_ready;

namespace {

const char *const OBSERVED_STORAGE =
    "ingress publication: (-12) insufficient storage space [aeron_client_conductor_async_resource_poll, "
    "aeron_client_conductor.c:3717] async_add_publication registration error";

void refuseAll(FakeTransport &t)
{
    t.error = OBSERVED_STORAGE;
    for (auto &p : t.publications)
    {
        if (!p.closed && cluster_tools::client::Registration::PENDING == p.status)
        {
            p.status = cluster_tools::client::Registration::FAILED;
        }
    }
}

struct Outcome
{
    std::int64_t onsetNs = -1;
    bool aliveNotReadyAt29 = false;
    std::uint64_t secondsAt29 = 0;
};

// steps for up to 60 s; `each` runs before every step
template<typename Each>
Outcome starve(AppRig &r, Each &&each)
{
    Outcome o;
    const std::int64_t step = 10 * MS;
    const std::int64_t end = r.t.now + 60 * SECOND;
    while (r.t.now < end && !r.agent->stopped())
    {
        each();
        r.step(step);
        const std::uint32_t mask = r.agent->notReadyMask();
        if (o.onsetNs < 0 && 0 != (mask & bridge_ready::STORAGE_STARVED))
        {
            o.onsetNs = r.t.now;
        }
        if (o.onsetNs >= 0 && r.t.now - o.onsetNs >= 29 * SECOND && r.t.now - o.onsetNs < 29 * SECOND + step)
        {
            app::BridgeSnapshot s;
            r.agent->fillSnapshot(s);
            o.aliveNotReadyAt29 = !r.agent->stopped() && 0 != (mask & bridge_ready::STORAGE_STARVED);
            o.secondsAt29 = s.app.storageStarvedSeconds;
        }
    }
    return o;
}

void checkExit70(AppRig &r, const Outcome &o, const char *what)
{
    REQUIRE(o.onsetNs >= 0);
    CHECK(o.aliveNotReadyAt29);
    CHECK(o.secondsAt29 >= 28);
    const bool exited70 = r.agent->stopped() && 70 == r.agent->stopReason().exitCode;
    CHECK_MESSAGE(exited70, "alive after 30 s of storage starvation"); // STORAGE_STARVED_EXIT_OFF RED
    if (exited70)
    {
        CHECK(std::string(r.agent->stopReason().code) == "F_STORAGE_STARVED");
        CHECK(std::string(r.agent->stopReason().detail).find(what) != std::string::npos);
        const std::int64_t took = r.t.now - o.onsetNs;
        CHECK(took >= common::STORAGE_STARVED_EXIT_NS);
        CHECK(took <= common::STORAGE_STARVED_EXIT_NS + 3 * 10 * MS); // stop in one iteration, finish in the next
    }
}

}

TEST_CASE("BridgeAgentTest.coreIngressStarvedOfStorageExits70")
{
    AppRig r(1, {}, 3);
    r.journal(0, 1, 50);
    r.t.autoReady = false;
    r.agent->start();
    const Outcome o = starve(r, [&] { refuseAll(r.t); });
    checkExit70(r, o, "core ingress publication");
}

TEST_CASE("BridgeAgentTest.replayStarvedOfStorageExits70")
{
    // no recording on the source: it never reaches FOLLOWING; the precheck refuses one replay per second
    AppRig r(1, {}, 3);
    r.agent->start();
    long n = 0;
    const Outcome o = starve(r, [&] {
        if (0 == n++ % 100)
        {
            r.archiveMetrics.openFailure(OpenFailure::STORAGE);
        }
    });
    checkExit70(r, o, "every replay image");
}

TEST_CASE("BridgeAgentTest.replayStarvationEndsWhenASourceFollowsOrFailsOtherwise")
{
    AppRig r(1, {}, 3);
    r.agent->start();
    auto refusing = [&](std::int64_t ns, OpenFailure kind) {
        const std::int64_t end = r.t.now + ns;
        long n = 0;
        while (r.t.now < end && !r.agent->stopped())
        {
            if (0 == n++ % 100)
            {
                r.archiveMetrics.openFailure(kind);
            }
            r.step(10 * MS);
        }
    };
    refusing(20 * SECOND, OpenFailure::STORAGE);
    CHECK(0 != (r.agent->notReadyMask() & bridge_ready::STORAGE_STARVED));
    refusing(2 * SECOND, OpenFailure::ENDPOINT); // the next attempt failed for another reason: not continuous
    CHECK(0 == (r.agent->notReadyMask() & bridge_ready::STORAGE_STARVED));
    refusing(20 * SECOND, OpenFailure::STORAGE_SPACE);
    CHECK(0 != (r.agent->notReadyMask() & bridge_ready::STORAGE_STARVED));
    CHECK_FALSE(r.agent->stopped());
    // the tmpfs is freed: a replay opens and the source follows (the policy's FOLLOWING gauge): the episode ends again,
    // and refusals counted while a source follows never make an episode
    r.journal(0, 1, 50);
    REQUIRE(r.runUntil([&] { return 0 != ArchiveMetrics::get(r.archiveMetrics.sourceAttached[0]); }, 30 * SECOND));
    r.step(10 * MS);
    CHECK(0 == (r.agent->notReadyMask() & bridge_ready::STORAGE_STARVED));
    refusing(35 * SECOND, OpenFailure::STORAGE);
    CHECK_FALSE(r.agent->stopped());
    CHECK(0 == (r.agent->notReadyMask() & bridge_ready::STORAGE_STARVED));
    app::BridgeSnapshot s;
    r.agent->fillSnapshot(s);
    CHECK(s.app.storageStarvedSeconds >= 39); // both closed episodes stay counted
}
