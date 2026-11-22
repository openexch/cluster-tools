// ct-genesis - the genesis of an empty ME and core pair, one phase per invocation, plus two read verbs.
//
//   ct-genesis <verb> <genesis.json>
//   verbs, in order: me_genesis arm_money_journal core_genesis configure_risk fund verify_funded activate
//   read verbs:      status (the phase table, the last complete phase, the next one) and verify (exit 0 only when the
//                    genesis is complete: every phase, behaviour 4, the request cap, the gate OPEN, the record)
//
// The operator (or the console's genesis operation) starts processes between the phases in the cut's order:
// me_genesis, arm_money_journal, the money projector, the bridges, core_genesis, configure_risk, fund, verify_funded,
// the forwarders, activate, then the gateway set. Settings: GenesisSettings.h. Phases and the prefix rule:
// GenesisState.h.
//
// Exit: 0 done; 1 the phase ran and did not complete, a read failed, or verify found it not done; 3 refused before
// anything was sent (the cluster is not empty, or the state is not the exact prefix of the verb); 64 usage; 78 a
// configuration or input problem (all listed in one pass); 70 a fatal Aeron error or a dead media driver.
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/runtime/ConductorWatch.h"
#include "cluster_tools/runtime/EmbeddedDriver.h"
#include "genesis/ClusterLink.h"
#include "genesis/GenesisPlan.h"
#include "genesis/GenesisRunner.h"
#include "genesis/GenesisSeams.h"
#include "genesis/GenesisSettings.h"

#ifndef CT_BUILD_SOURCE_SHA
#define CT_BUILD_SOURCE_SHA "unknown"
#endif

namespace at = cluster_tools::client::aeron_transport;
namespace runtime = cluster_tools::runtime;
namespace gn = cluster_tools::app::genesis;

namespace {

constexpr int EXIT_USAGE = 64;
constexpr int EXIT_CONFIG = 78;
constexpr int EXIT_FATAL = 70;

[[noreturn]] void exitNow(int code, const std::string &why)
{
    std::fprintf(stderr, "ct-genesis: %s (exit %d)\n", why.c_str(), code);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

int usage()
{
    std::fprintf(stderr,
        "usage: ct-genesis <verb> <genesis.json>\n"
        "  phases: me_genesis arm_money_journal core_genesis configure_risk fund verify_funded activate\n"
        "  reads:  status verify\n");
    return EXIT_USAGE;
}

class AeronLinks : public gn::LinkFactory
{
public:
    AeronLinks(std::shared_ptr<aeron::Aeron> aeron, const gn::GenesisSettings &settings, std::function<void()> stale) :
        m_aeron(std::move(aeron)), m_settings(settings), m_stale(std::move(stale))
    {
    }

    std::unique_ptr<gn::Link> coreAdmin() override
    {
        return make(gn::clientSettings(m_settings, m_settings.coreIngress, m_settings.coreAdmin, "ct-genesis core admin"));
    }
    std::unique_ptr<gn::Link> coreTool() override
    {
        return make(gn::clientSettings(m_settings, m_settings.coreIngress, m_settings.coreTool, "ct-genesis core tool"));
    }
    std::unique_ptr<gn::Link> meAdmin() override
    {
        return make(gn::clientSettings(m_settings, m_settings.meIngress, m_settings.meAdmin, "ct-genesis me admin"));
    }

private:
    std::unique_ptr<gn::Link> make(cluster_tools::client::ClientSettings settings)
    {
        settings.messageTimeoutNs = 5'000'000'000LL;
        return std::make_unique<gn::ClusterLink<at::AeronTransport>>(std::make_unique<at::AeronTransport>(m_aeron),
            std::move(settings), m_stale);
    }

    std::shared_ptr<aeron::Aeron> m_aeron;
    const gn::GenesisSettings &m_settings;
    std::function<void()> m_stale;
};

}

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (2 != args.size())
    {
        return usage();
    }
    const std::string verb = args[0];
    const auto phase = gn::phaseByName(verb);
    if (!phase && "status" != verb && "verify" != verb)
    {
        std::fprintf(stderr, "ct-genesis: unknown verb '%s'\n", verb.c_str());
        return usage();
    }

    std::vector<std::string> problems;
    gn::PlanResult plan;
    {
        std::ifstream in(args[1], std::ios::binary);
        if (!in)
        {
            problems.push_back("the genesis input " + args[1] + " cannot be read");
        }
        else
        {
            std::stringstream text;
            text << in.rdbuf();
            plan = gn::parsePlan(text.str());
            for (const std::string &e : plan.errors)
            {
                problems.push_back(e);
            }
        }
    }
    const gn::SettingsResult settings = gn::parseGenesisSettings(
        [](const char *key) { return std::getenv(key); }, phase && gn::Phase::FUND == *phase);
    problems.insert(problems.end(), settings.errors.begin(), settings.errors.end());
    if (!problems.empty())
    {
        for (const std::string &p : problems)
        {
            std::fprintf(stderr, "ct-genesis: %s\n", p.c_str());
        }
        std::fprintf(stderr, "ct-genesis: %zu configuration problem(s) (exit 78)\n", problems.size());
        return EXIT_CONFIG;
    }
    const gn::GenesisSettings &s = settings.settings;
    signal(SIGPIPE, SIG_IGN);

    std::unique_ptr<runtime::EmbeddedDriver> driver;
    std::string aeronDir = s.aeronDir;
    if (gn::DriverMode::EMBEDDED == s.driverMode)
    {
        runtime::EmbeddedDriverOptions options;
        options.processName = "ct-genesis";
        options.dirPrefix = "ct-genesis-";
        options.instanceKey = "GENESIS_INSTANCE_ID";
        const std::string dir = runtime::EmbeddedDriver::dirFor(s.driverDir, s.instanceId, options);
        driver = std::make_unique<runtime::EmbeddedDriver>(options,
            [](const std::string &why) { exitNow(EXIT_FATAL, "embedded media driver terminated: " + why); });
        std::string error;
        if (!driver->start(dir, false, error))
        {
            const bool storage = driver->startFailedForStorage();
            std::fprintf(stderr, "ct-genesis: embedded media driver: %s (exit %d)\n", error.c_str(),
                storage ? EXIT_FATAL : EXIT_CONFIG);
            return storage ? EXIT_FATAL : EXIT_CONFIG;
        }
        aeronDir = dir;
    }
    std::printf("ct-genesis %s: genesis %lld, driver %s at %s, source %s\n", verb.c_str(),
        static_cast<long long>(plan.plan.genesisId), gn::driverModeName(s.driverMode), aeronDir.c_str(),
        CT_BUILD_SOURCE_SHA);
    std::fflush(stdout);

    auto monitor = std::make_shared<at::AeronFatalMonitor>(); // fatal conductor errors: _Exit(70)
    std::shared_ptr<aeron::Aeron> aeron;
    try
    {
        at::AeronClientOptions clientOptions;
        clientOptions.aeronDir = aeronDir;
        clientOptions.clientName = "ct-genesis";
        clientOptions.driverTimeoutMs = s.driverTimeoutMs;
        aeron = at::connectAeron(clientOptions, monitor);
    }
    catch (const std::exception &e)
    {
        exitNow(EXIT_FATAL, "no media driver answered at " + aeronDir + ": " + e.what());
    }

    int rc = gn::EXIT_FAILED;
    {
        runtime::ConductorWatch watch(aeron, s.driverTimeoutMs);
        auto stale = [&watch] {
            std::string why;
            if (!watch.check(why))
            {
                exitNow(EXIT_FATAL, "the client conductor is stale: " + why);
            }
        };
        AeronLinks links(aeron, s, stale);
        gn::FileActivationStore store(s.activationRecord);
        gn::RunnerOptions options;
        options.timeouts = s.timeouts;
        options.sourceSha = CT_BUILD_SOURCE_SHA;
        options.emptinessCheck = !gn::seams::disabled(gn::seams::Rule::EMPTINESS_CHECK);
        options.prefixCheck = !gn::seams::disabled(gn::seams::Rule::PREFIX_CHECK);
        options.limitsStep = !gn::seams::disabled(gn::seams::Rule::LIMITS_STEP);
        if (const std::int64_t n = gn::seams::exitAfterSends(); n > 0)
        {
            auto sent = std::make_shared<std::int64_t>(0);
            options.afterSend = [sent, n] {
                if (++*sent >= n)
                {
                    std::printf("ct-genesis: test build: exit after %lld command(s)\n", static_cast<long long>(n));
                    std::fflush(stdout);
                    std::_Exit(137);
                }
            };
        }
        gn::GenesisRunner runner(plan.plan, links, store, std::move(options), std::cout);
        if (phase)
        {
            rc = runner.runPhase(*phase);
        }
        else if ("status" == verb)
        {
            rc = runner.status();
        }
        else
        {
            rc = runner.verify();
        }
        std::cout.flush();
    } // every session and transport is closed before the client and the driver
    monitor->beginDeliberateClose();
    aeron.reset();
    if (driver && !driver->close(5'000))
    {
        exitNow(EXIT_FATAL, "the embedded media driver did not close within 5 s");
    }
    return rc;
}
