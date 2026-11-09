// agentOverLaggedCoreRecoversHalts: the property shape driven THROUGH the app wiring
// (the real agent, the real ClusterClient, the registrar, rule S, the link machine): per seed, two sources, egress
// 1..8 iterations late, lossy elections (only while a frame is uncommitted), egress image losses before an election,
// session losses (also while halted: a kept halt), a hole on one source (failover) or on every source (GAP halt,
// then an operator fill of one node: recovery). Asserts per seed: the core applied exactly 1..N once each and in order,
// never saw an entry above cursor + 1 (no FEED_GAP), no duplicate that differs; the run completes. Non-vacuity over the
// run: lossy elections > 0, session losses > 0, kept halts across a loss > 0, GAP recoveries > 0.
// CT_BRIDGE_AGENT_PROPERTY_RUNS seeds (ctest 2000; the CI size 10^4 once on the build box).
#include "doctest/doctest.h"

#include <cstdlib>
#include <random>

#include "AppRig.h"

using namespace ct_bridge_app_test;

namespace {

struct Totals
{
    std::uint64_t seeds = 0;
    std::uint64_t completed = 0;
    std::uint64_t lossyElections = 0;
    std::uint64_t lostFrames = 0;
    std::uint64_t elections = 0;
    std::uint64_t imageLosses = 0;
    std::uint64_t sessionLosses = 0;
    std::uint64_t keptHaltsAcrossLoss = 0;
    std::uint64_t gapHalts = 0;
    std::uint64_t gapRecoveries = 0;
    std::uint64_t failovers = 0;
    std::uint64_t fences = 0;
};

int runs()
{
    const char *env = std::getenv("CT_BRIDGE_AGENT_PROPERTY_RUNS");
    const int n = nullptr == env ? 200 : std::atoi(env);
    return n > 0 ? n : 200;
}

// One seed; false (with the reason) on a violation.
bool oneSeed(std::uint64_t seed, Totals &totals, std::string &why)
{
    std::mt19937_64 rng(seed);
    auto chance = [&](std::uint64_t oneIn) { return 0 == rng() % oneIn; };
    AppRig r(2, {}, 8);
    r.lagMin = 1;
    r.rng.seed(seed * 7 + 1);
    r.core.commitImmediately = false;
    r.core.lossHook = [&](std::size_t) { totals.lostFrames++; };
    const std::int64_t n = 100 + static_cast<std::int64_t>(rng() % 300);
    const int shape = static_cast<int>(rng() % 4); // 0 clean, 1 hole on source 0, 2 hole everywhere (GAP + fill), 3 clean
    const std::int64_t hole = 20 + static_cast<std::int64_t>(rng() % (n - 40));
    SimRecording *recs[2] = { &r.archive(0).record(), &r.archive(1).record() };
    std::int64_t written = 0;
    auto writeTo = [&](std::int64_t upto) {
        for (std::int64_t s = written + 1; s <= upto; ++s)
        {
            for (std::size_t src = 0; src < 2; ++src)
            {
                const bool absent = (1 == shape && 0 == src && s == hole) || (2 == shape && s == hole);
                if (!absent)
                {
                    appendJournal(r.archive(src), *recs[src], s, s);
                }
            }
        }
        written = upto;
    };
    writeTo(std::min<std::int64_t>(n, 30));
    r.agent->start();
    bool filled = false;
    bool halted = false;
    const std::int64_t deadline = r.t.now + 600 * SECOND;
    while (r.t.now < deadline)
    {
        r.step();
        // commit, or elect while something is uncommitted
        if (!r.core.pending.empty() && chance(150))
        {
            const std::size_t lost = r.core.pending.size();
            totals.elections++;
            totals.lossyElections += lost > 0 ? 1 : 0;
            if (chance(3))
            {
                r.loseImage();
                totals.imageLosses++;
                r.step();
            }
            r.elect(true);
        }
        else
        {
            r.core.commit();
        }
        if (app::CoreLink::C3_REGISTERED == r.agent->link() && chance(4000))
        {
            if (BridgeHalt::NONE != r.agent->domain().view().halt)
            {
                totals.keptHaltsAcrossLoss++;
            }
            totals.sessionLosses++;
            r.closeSession();
        }
        if (written < n && chance(20))
        {
            writeTo(std::min<std::int64_t>(n, written + 1 + static_cast<std::int64_t>(rng() % 20)));
        }
        const BridgeHalt h = r.agent->domain().view().halt;
        if (BridgeHalt::GAP == h && !halted)
        {
            halted = true;
            totals.gapHalts++;
            if (chance(2) && app::CoreLink::C3_REGISTERED == r.agent->link())
            {
                totals.keptHaltsAcrossLoss++; // a session loss while halted: the halt must be kept
                totals.sessionLosses++;
                r.closeSession();
            }
        }
        if (2 == shape && halted && !filled && written == n && r.core.cursor == hole - 1 && chance(50))
        {
            // the operator fills one node: a new recording on source 0 carries hole..n
            // (one complete active recording: the node's journal re-written, the property's fill)
            r.archive(0).recordings.clear();
            SimRecording &fill = r.archive(0).record();
            appendJournal(r.archive(0), fill, 1, n);
            recs[0] = &fill;
            filled = true;
        }
        if (halted && BridgeHalt::NONE == h && filled)
        {
            halted = false;
            totals.gapRecoveries++;
        }
        if (r.core.gapRejects > 0 || r.core.aboveCursorPlusOne > 0)
        {
            why = "the core saw an entry above cursor + 1 (FEED_GAP " + std::to_string(r.core.gapRejects) + ")";
            return false;
        }
        if (written == n && r.core.cursor == n && 0 == r.core.latch)
        {
            break;
        }
        if (r.agent->stopped())
        {
            why = std::string("the agent stopped: ") + r.agent->stopReason().code + " " + r.agent->stopReason().detail;
            return false;
        }
        if (r.core.latch != 0 && 2 == r.core.latch)
        {
            why = "the core latched EPOCH_MISMATCH";
            return false;
        }
    }
    for (std::size_t i = 0; i < r.core.applied.size(); ++i)
    {
        if (r.core.applied[i] != static_cast<std::int64_t>(i) + 1)
        {
            why = "applied out of order at index " + std::to_string(i) + ": " + std::to_string(r.core.applied[i]);
            return false;
        }
    }
    if (r.core.cursor != n)
    {
        why = "did not complete: cursor " + std::to_string(r.core.cursor) + " of " + std::to_string(n) + " (shape " +
            std::to_string(shape) + ", hole " + std::to_string(hole) + ", latch " + std::to_string(r.core.latch) +
            ", state " + stateName(r.agent->domain().view().state) + ", steps " + std::to_string(r.stepNo) + ", filled " +
            std::to_string(filled) + ", written " + std::to_string(written) + ")";
        return false;
    }
    totals.completed++;
    totals.failovers += BridgeMetrics::get(r.agent->metrics().sourceFailovers);
    totals.fences += r.agent->metrics().totalFences();
    return true;
}

}

TEST_CASE("AgentPropertyTest.agentOverLaggedCoreRecoversHalts")
{
    Totals totals;
    const int n = runs();
    int failures = 0;
    for (int i = 0; i < n; ++i)
    {
        const std::uint64_t seed = 31102026ULL + static_cast<std::uint64_t>(i);
        std::string why;
        totals.seeds++;
        if (!oneSeed(seed, totals, why))
        {
            failures++;
            if (failures <= 5)
            {
                MESSAGE("seed " << seed << ": " << why);
            }
        }
    }
    MESSAGE("seeds " << totals.seeds << " completed " << totals.completed << " elections " << totals.elections
                     << " (lossy " << totals.lossyElections << ", frames lost " << totals.lostFrames << ") image losses "
                     << totals.imageLosses << " session losses " << totals.sessionLosses << " kept halts across a loss "
                     << totals.keptHaltsAcrossLoss << " GAP halts " << totals.gapHalts << " GAP recoveries "
                     << totals.gapRecoveries << " failovers " << totals.failovers << " fences " << totals.fences);
    const bool noViolation = 0 == failures;
    CHECK(noViolation); // FENCE_OFF / REGISTRATION_GATE_OFF REDs: an entry above cursor + 1 reaches the core
    CHECK(totals.lossyElections > 0);
    CHECK(totals.lostFrames > 0);
    CHECK(totals.sessionLosses > 0);
    CHECK(totals.keptHaltsAcrossLoss > 0);
    CHECK(totals.gapRecoveries > 0);
    CHECK(totals.failovers > 0);
}
