// GenesisRunner over the in-process ME and core of FakeClusters.h: the phases in the cut's order, the request cap,
// the activation record, the refusals of a non-empty cluster and of a state that is not the exact prefix, a genesis
// killed after each phase and in the middle of the deposits, the gate OPEN retry bound. The rules a negative control
// switches off come from the seams (CT_TEST_DISABLE_<RULE>=1, negative-controls.txt), read the way ct-genesis reads
// them.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "FakeClusters.h"
#include "genesis/GenesisPlan.h"
#include "genesis/GenesisRunner.h"
#include "genesis/GenesisSeams.h"

namespace gn = cluster_tools::app::genesis;
namespace fk = cluster_tools::app::genesis::fake;

namespace {

gn::GenesisPlan plan()
{
    std::ifstream in(CT_GENESIS_EXAMPLE);
    std::stringstream s;
    s << in.rdbuf();
    gn::PlanResult r = gn::parsePlan(s.str());
    REQUIRE(r.ok());
    return r.plan;
}

struct Harness
{
    fk::Clusters clusters;
    fk::FakeLinks links{ clusters };
    fk::MemoryStore store;
    gn::GenesisPlan p = plan();
    std::ostringstream out;

    gn::RunnerOptions options(std::int64_t retries = 600)
    {
        gn::RunnerOptions o;
        o.timeouts.gateOpenRetries = retries;
        o.sourceSha = "test";
        o.clockMs = [] { return std::int64_t{ 1 }; };
        o.emptinessCheck = !gn::seams::disabled(gn::seams::Rule::EMPTINESS_CHECK);
        o.prefixCheck = !gn::seams::disabled(gn::seams::Rule::PREFIX_CHECK);
        o.limitsStep = !gn::seams::disabled(gn::seams::Rule::LIMITS_STEP);
        return o;
    }

    // One invocation of ct-genesis: a new runner (new sessions), as the binary is one process per verb.
    int run(gn::Phase phase, std::int64_t retries = 600)
    {
        gn::GenesisRunner runner(p, links, store, options(retries), out);
        return runner.runPhase(phase);
    }

    int status()
    {
        gn::GenesisRunner runner(p, links, store, options(), out);
        return runner.status();
    }

    int verify()
    {
        gn::GenesisRunner runner(p, links, store, options(), out);
        return runner.verify();
    }

    std::string statusText()
    {
        out.str("");
        REQUIRE(gn::EXIT_OK == status());
        return out.str();
    }

    void wipe()
    {
        clusters = fk::Clusters{};
        store.text.clear();
    }

    std::size_t sentOf(std::uint16_t templateId) const
    {
        return static_cast<std::size_t>(
            std::count(clusters.coreCommands.begin(), clusters.coreCommands.end(), templateId));
    }
};

constexpr gn::Phase ORDER[] = { gn::Phase::ME_GENESIS, gn::Phase::ARM_MONEY_JOURNAL, gn::Phase::CORE_GENESIS,
    gn::Phase::CONFIGURE_RISK, gn::Phase::FUND, gn::Phase::VERIFY_FUNDED, gn::Phase::ACTIVATE };

bool contains(const std::string &text, const std::string &needle)
{
    return std::string::npos != text.find(needle);
}

// One phase line of status: the name padded to 19, the status to 13.
std::string line(const std::string &phase, const std::string &status)
{
    return "  " + phase + std::string(19 - phase.size(), ' ') + status;
}

void fullGenesis(Harness &h)
{
    for (gn::Phase phase : ORDER)
    {
        CAPTURE(gn::phaseName(phase));
        const int rc = h.run(phase);
        REQUIRE_MESSAGE(gn::EXIT_OK == rc, h.out.str());
    }
}

}

TEST_CASE("full genesis sets the request cap")
{
    gn::seams::resetForTest();
    Harness h;
    for (gn::Phase phase : ORDER)
    {
        h.run(phase);
    }
    // the request cap first: it is the rule this case pins (the fixture never sends CoreLimitsSet)
    if (2048 != h.clusters.cap || 0 == h.sentOf(gn::wire::T_CORE_LIMITS_SET))
    {
        FAIL_CHECK("genesis left the request cap unset (cap " << h.clusters.cap << ", CoreLimitsSet sent "
                                                              << h.sentOf(gn::wire::T_CORE_LIMITS_SET) << " time(s))");
    }
    CHECK(1 == h.clusters.limitsVersion);
    CHECK_MESSAGE(gn::EXIT_OK == h.verify(), h.out.str());
}

TEST_CASE("full genesis: every phase in the cut's order, the gate OPEN inside core_genesis, behaviour 4, the record")
{
    gn::seams::resetForTest();
    Harness h;
    h.out.str("");
    REQUIRE(gn::EXIT_OK == h.status());
    CHECK(contains(h.out.str(), "state: EMPTY (next: me_genesis)"));
    fullGenesis(h);

    // the ME first, one EngineConfig
    CHECK(1 == h.clusters.engineConfigs);
    CHECK(h.p.genesisId == h.clusters.meEpoch);
    // core order: SetMoneyJournal, CoreConfig 1 2 3, the OPEN tries, the risk rows, CoreLimitsSet, deposits, CoreConfig 4
    std::vector<std::uint16_t> commands;
    for (std::uint16_t t : h.clusters.coreCommands)
    {
        if (gn::wire::T_SET_MONEY_JOURNAL == t || gn::wire::T_CORE_CONFIG == t || gn::wire::T_ORDER_ENTRY_GATE == t ||
            gn::wire::T_RISK_CONFIG_SET == t || gn::wire::T_CORE_LIMITS_SET == t || gn::wire::T_DEPOSIT == t)
        {
            if (commands.empty() || commands.back() != t || gn::wire::T_CORE_CONFIG == t)
            {
                commands.push_back(t);
            }
        }
    }
    const std::vector<std::uint16_t> want = { 32, 70, 70, 70, 73, 71, 77, 1, 70 };
    CHECK(want == commands);
    CHECK(4 == h.clusters.openTries); // refused ME_LANE_INACTIVE until the bridge bound the epoch, then OPEN
    CHECK(0 == h.sentOf(gn::wire::T_INIT_TRADE_HIGH_WATER));
    CHECK(4 == h.clusters.configVersion);
    CHECK(4 == h.clusters.behaviorVersion);
    CHECK(0 == h.clusters.gate);
    CHECK(2048 == h.clusters.cap);
    CHECK(1 == h.store.writes);
    CHECK(contains(h.store.text, "\"genesisId\": 7301000000000001"));
    CHECK(contains(h.store.text, "\"requestPerUserCap\": 2048"));
    const std::string s = h.statusText();
    CHECK(contains(s, "state: COMPLETE"));
    CHECK(contains(s, "last complete phase: activate"));
    h.out.str("");
    CHECK(gn::EXIT_OK == h.verify());
    CHECK(contains(h.out.str(), "verify: genesis 7301000000000001 DONE"));
}

TEST_CASE("second genesis on a live cluster is refused")
{
    gn::seams::resetForTest();
    Harness h;
    fullGenesis(h);
    const int engineConfigs = h.clusters.engineConfigs;
    const std::size_t coreCommands = h.clusters.coreCommands.size();
    h.out.str("");
    const int rc = h.run(gn::Phase::ME_GENESIS);
    // the emptiness rule: nothing is sent to a cluster with history
    if (h.clusters.engineConfigs != engineConfigs || gn::EXIT_REFUSED != rc)
    {
        FAIL_CHECK("genesis ran on a cluster with history (exit " << rc << ", EngineConfig sent "
                                                                  << h.clusters.engineConfigs - engineConfigs << " more)");
    }
    CHECK(contains(h.out.str(), "cluster is not empty: genesis refused"));
    // the reads are queries; nothing was commanded
    for (std::size_t i = coreCommands; i < h.clusters.coreCommands.size(); ++i)
    {
        const std::uint16_t t = h.clusters.coreCommands[i];
        CHECK((7 == t || 56 == t || 8 == t || 9 == t || 53 == t));
    }
    // and on a cluster that holds only a balance (no genesis at all)
    h.wipe();
    h.clusters.balances[{ 9, 0 }] = 1;
    h.out.str("");
    CHECK(gn::EXIT_REFUSED == h.run(gn::Phase::ME_GENESIS));
    CHECK(contains(h.out.str(), "cluster is not empty: genesis refused"));
    CHECK(0 == h.clusters.engineConfigs);
}

TEST_CASE("genesis killed after phase k: status names k, the next verb continues, the rest refuse")
{
    gn::seams::resetForTest();
    for (std::size_t k = 0; k + 1 < std::size(ORDER); ++k)
    {
        CAPTURE(gn::phaseName(ORDER[k]));
        Harness h;
        for (std::size_t i = 0; i <= k; ++i)
        {
            REQUIRE_MESSAGE(gn::EXIT_OK == h.run(ORDER[i]), h.out.str());
        }
        const std::string s = h.statusText();
        // verify_funded is a check: after it, status names fund and offers verify_funded again (it is idempotent)
        const gn::Phase lastState = gn::Phase::VERIFY_FUNDED == ORDER[k] ? gn::Phase::FUND : ORDER[k];
        CHECK(contains(s, std::string("last complete phase: ") + gn::phaseName(lastState)));
        CHECK(contains(s, "started, not done"));
        h.out.str("");
        CHECK(gn::EXIT_FAILED == h.verify());
        // an earlier verb refuses
        if (k > 0)
        {
            h.out.str("");
            CHECK(gn::EXIT_REFUSED == h.run(ORDER[k - 1]));
            CHECK(contains(h.out.str(), gn::Phase::ME_GENESIS == ORDER[k - 1] ? "cluster is not empty: genesis refused" :
                                                                                 "genesis state is not a prefix of the plan: "));
        }
        // the next verb continues from the exact prefix to the end
        for (std::size_t i = k + 1; i < std::size(ORDER); ++i)
        {
            REQUIRE_MESSAGE(gn::EXIT_OK == h.run(ORDER[i]), h.out.str());
        }
        CHECK(gn::EXIT_OK == h.verify());
    }
}

TEST_CASE("a partial fund refuses the rerun")
{
    gn::seams::resetForTest();
    Harness h;
    for (std::size_t i = 0; i < 4; ++i)
    {
        REQUIRE(gn::EXIT_OK == h.run(ORDER[i]));
    }
    // killed after two of the six deposits (the admission reads are five queries)
    h.clusters.sendBudget = 5 + 2;
    CHECK(gn::EXIT_FAILED == h.run(gn::Phase::FUND));
    REQUIRE(2 == h.sentOf(gn::wire::T_DEPOSIT));
    h.clusters.sendBudget = -1;
    const std::string s = h.statusText();
    CHECK(contains(s, line("fund", "PARTIAL")));
    CHECK(contains(s, "state: NOT_A_PREFIX"));
    CHECK(contains(s, "last complete phase: none"));
    h.out.str("");
    const int rc = h.run(gn::Phase::FUND);
    const std::size_t deposits = h.sentOf(gn::wire::T_DEPOSIT);
    if (gn::EXIT_REFUSED != rc || 2 != deposits)
    {
        FAIL_CHECK("genesis continued from an unknown state (exit " << rc << ", " << deposits << " deposits sent)");
    }
    CHECK(contains(h.out.str(), "genesis state is not a prefix of the plan: fund is PARTIAL"));
    CHECK(contains(h.out.str(), "wipe both clusters and redo"));
    // every later verb refuses as well, and a new genesis on top is refused
    for (gn::Phase later : { gn::Phase::VERIFY_FUNDED, gn::Phase::ACTIVATE })
    {
        h.out.str("");
        CHECK(gn::EXIT_REFUSED == h.run(later));
    }
    h.out.str("");
    CHECK(gn::EXIT_REFUSED == h.run(gn::Phase::ME_GENESIS));
    CHECK(contains(h.out.str(), "cluster is not empty: genesis refused"));
    // wipe and redo: a full genesis holds
    h.wipe();
    fullGenesis(h);
    CHECK(gn::EXIT_OK == h.verify());
}

TEST_CASE("a partial core_genesis refuses every verb; killed inside the ME phase it is a prefix of nothing")
{
    gn::seams::resetForTest();
    Harness h;
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ME_GENESIS));
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ARM_MONEY_JOURNAL));
    const std::size_t before = h.clusters.sends;
    // the reads of the admission (5 queries) and CoreConfig 1: killed right after it
    h.clusters.sendBudget = 6;
    CHECK(gn::EXIT_FAILED == h.run(gn::Phase::CORE_GENESIS));
    CHECK(before + 6 == h.clusters.sends);
    REQUIRE(1 == h.clusters.configVersion);
    h.clusters.sendBudget = -1;
    const std::string s = h.statusText();
    CHECK(contains(s, line("core_genesis", "PARTIAL")));
    CHECK(contains(s, "CoreConfig 1 of 3 recorded"));
    h.out.str("");
    CHECK(gn::EXIT_REFUSED == h.run(gn::Phase::CORE_GENESIS));
    CHECK(contains(h.out.str(), "genesis state is not a prefix of the plan: core_genesis is PARTIAL: CoreConfig 1 of 3"));
}

TEST_CASE("the gate OPEN is retried up to the setting and names the last refusal")
{
    gn::seams::resetForTest();
    Harness h;
    h.clusters.bindAfterOpenTries = 10;
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ME_GENESIS));
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ARM_MONEY_JOURNAL));
    h.out.str("");
    CHECK(gn::EXIT_FAILED == h.run(gn::Phase::CORE_GENESIS, 5));
    CHECK(5 == h.clusters.openTries);
    CHECK(contains(h.out.str(), "never accepted in 5 tries (GENESIS_GATE_OPEN_RETRIES): last REJECTED reason 41 "
                                "ME_LANE_INACTIVE"));
    // the state is CoreConfig 3 with the gate SUSPENDED: not a prefix (the recovery is wipe and redo)
    CHECK(contains(h.statusText(), "the global gate is SUSPENDED"));
    h.wipe();
    h.clusters.bindAfterOpenTries = 10;
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ME_GENESIS));
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ARM_MONEY_JOURNAL));
    CHECK(gn::EXIT_OK == h.run(gn::Phase::CORE_GENESIS, 600));
    CHECK(11 == h.clusters.openTries);
}

TEST_CASE("without a bridge the core never binds the ME epoch and core_genesis fails at the bound")
{
    gn::seams::resetForTest();
    Harness h;
    h.clusters.bridgeRunning = false;
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ME_GENESIS));
    REQUIRE(gn::EXIT_OK == h.run(gn::Phase::ARM_MONEY_JOURNAL));
    h.out.str("");
    CHECK(gn::EXIT_FAILED == h.run(gn::Phase::CORE_GENESIS, 20));
    CHECK(contains(h.out.str(), "is a bridge running and bound to the ME?"));
}

TEST_CASE("a foreign ME genesis is not a prefix; the ME genesis of another id refuses the core phases")
{
    gn::seams::resetForTest();
    Harness h;
    h.clusters.meEpoch = 12345;
    h.out.str("");
    CHECK(gn::EXIT_REFUSED == h.run(gn::Phase::ME_GENESIS));
    CHECK(contains(h.out.str(), "cluster is not empty: genesis refused"));
    h.out.str("");
    CHECK(gn::EXIT_REFUSED == h.run(gn::Phase::ARM_MONEY_JOURNAL));
    CHECK(contains(h.out.str(), "genesis state is not a prefix of the plan: me_genesis is FOREIGN"));
}

TEST_CASE("a second genesis after a wipe starts clean with a new genesis id")
{
    gn::seams::resetForTest();
    Harness h;
    fullGenesis(h);
    h.wipe();
    h.store.text = "{\"genesisId\": 7301000000000001}"; // the old record survives the wipe of the clusters
    h.p.genesisId = 7301000000000003;
    h.p.me.genesisId = h.p.genesisId;
    CHECK(contains(h.statusText(), "state: EMPTY"));
    fullGenesis(h);
    CHECK(7301000000000003 == h.clusters.meEpoch);
    CHECK(contains(h.store.text, "\"genesisId\": 7301000000000003"));
    CHECK(gn::EXIT_OK == h.verify());
}
