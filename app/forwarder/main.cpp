// ct-forwarder - the RC2 S12 production forwarder (contract sections 3, 8, 9): a stateless C++20 process with
// one resumable FORWARDER session on the core and one FORWARDER session on the ME. Every pending core
// MeDispatch becomes the ME's DurableOrderCommand 9 or ReleaseCommandIds 10; releases are relayed; the
// forwarder resyncs from the core after anything that may have lost a frame. Nothing is written to disk (G1).
//
// Exit codes: 0 stopped (SIGTERM/SIGINT); 70 EX_SOFTWARE process health (Aeron fatal from the conductor
// thread, an escaped exception or a broken invariant, the duty watchdog, the 2 s join bound, an INVALID reject
// of our own 65); 75 EX_TEMPFAIL an inferred refusal (restart allowed); 78 EX_CONFIG configuration or an
// observed refusal (the unit sets RestartPreventExitStatus=78).
//
// Test builds only (never published, publish.yml refuses them): CT_FORWARDER_NC_PRESET names the seams the
// ct_forwarder_nc_* negative-control binaries switch off; CT_FORWARDER_TC_CORRUPT is ct_forwarder_tc_corrupt.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <random>
#include <string>

#include "cluster_tools/build_info.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/domain/forwarder/TestSeams.h"

#include "DutyLoop.h"
#include "ForwarderAgent.h"
#include "ForwarderConfig.h"
#include "HttpServer.h"
#include "LogRing.h"
#include "StatusBoard.h"
#include "Supervisor.h"

#ifndef CT_FORWARDER_VERSION
#define CT_FORWARDER_VERSION "dev"
#endif

namespace fwd = cluster_tools::app::forwarder;
namespace cl = cluster_tools::client;
namespace at = cluster_tools::client::aeron_transport;

namespace {

fwd::NonBlockingLogWriter g_stderr(2); // review NOTE-5: main's log drain never blocks on stderr

void writeRecord(const fwd::LogRecord &r)
{
    g_stderr(r);
}

std::int64_t randomIdentity()
{
    std::random_device device;
    std::uint64_t v = 0;
    while (0 == v)
    {
        v = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }
    return static_cast<std::int64_t>(v & 0x7FFF'FFFF'FFFF'FFFFULL) | 1; // non-zero, positive
}

}

int main()
{
    fwd::Supervisor::blockSignals(); // before any thread (the Aeron conductor included)
    fwd::Supervisor::ignoreSigpipe();
#if defined(CT_FORWARDER_NC_PRESET)
    if (!cluster_tools::domain::forwarder::test_seams::forceDisable(CT_FORWARDER_NC_PRESET))
    {
        std::fprintf(stderr, "ct-forwarder test build: unknown seam in preset '%s'\n", CT_FORWARDER_NC_PRESET);
        return 78;
    }
    std::fprintf(stderr, "ct-forwarder NEGATIVE-CONTROL BUILD: rules off: %s\n", CT_FORWARDER_NC_PRESET);
#endif
#if defined(CT_FORWARDER_TC_CORRUPT)
    std::fprintf(stderr, "ct-forwarder TEST-CORRUPTION BUILD: every 50th CREATE id is corrupted after decode\n");
#endif

    const fwd::ConfigResult config = fwd::parseForwarderConfig([](const char *name) { return std::getenv(name); });
    if (!config.ok())
    {
        for (const std::string &error : config.errors)
        {
            std::fprintf(stderr, "ct-forwarder config: %s\n", error.c_str());
        }
        std::fprintf(stderr, "ct-forwarder: refusing to start (exit 78, F5)\n");
        return 78;
    }
    const fwd::ForwarderConfig &c = config.config;
    const std::int64_t identity = randomIdentity();
    std::fprintf(stderr,
        "ct-forwarder %s identity %" PRId64 " money v%d (%s) order v%d (%s) aeron %s; core %s, ME %s, http %s:%u\n",
        CT_FORWARDER_VERSION, identity, cluster_tools::build_info::MONEY_SCHEMA_VERSION,
        cluster_tools::build_info::MONEY_SCHEMA_SHA256, cluster_tools::build_info::MATCH_SCHEMA_VERSION,
        cluster_tools::build_info::MATCH_SCHEMA_SHA256, cluster_tools::build_info::AERON_VERSION, c.coreIngress.c_str(),
        c.meIngress.c_str(), c.httpHost.c_str(), static_cast<unsigned>(c.httpPort));

    // Everything a configuration error can refuse comes before Aeron is touched (a return after connectAeron would
    // let the client's close handler turn the 78 into a fatal 70).
    fwd::LogRing log;
    fwd::StatusBoard board;
    fwd::Supervisor supervisor(board, log, fwd::SupervisorSettings{}, &fwd::Supervisor::exitProcess, &writeRecord);
    std::function<fwd::HttpResponse(const fwd::HttpRequest &)> handler = [&](const fwd::HttpRequest &request) {
        return fwd::route(request, board, supervisor);
    };
    fwd::HttpServer http(handler);
    std::string error;
    if (!http.bind(c.httpHost, c.httpPort, error))
    {
        std::fprintf(stderr, "ct-forwarder: %s (exit 78)\n", error.c_str());
        return 78;
    }

    auto monitor = std::make_shared<at::AeronFatalMonitor>(); // fatal conductor errors: _Exit(70) (F1)
    std::shared_ptr<aeron::Aeron> aeron;
    try
    {
        at::AeronClientOptions options;
        options.aeronDir = c.aeronDir;
        options.clientName = "ct-forwarder";
        options.driverTimeoutMs = fwd::DRIVER_TIMEOUT_MS; // a dead driver ends the process within ~2 s (T1-J)
        aeron = at::connectAeron(options, monitor);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "ct-forwarder: no media driver at %s: %s (exit 70)\n", c.aeronDir.c_str(), e.what());
        std::fflush(stderr);
        std::_Exit(70);
    }

    at::AeronTransport coreTransport(aeron);
    at::AeronTransport meTransport(aeron);
    cl::ClusterClient<at::AeronTransport> core(coreTransport, fwd::coreClientSettings(c));
    cl::ClusterClient<at::AeronTransport> me(meTransport, fwd::meClientSettings(c));

    fwd::AgentSettings agentSettings;
    agentSettings.identity = identity;
    agentSettings.coreCredentialClass = c.coreCredentials.sessionClass;
    agentSettings.version = CT_FORWARDER_VERSION;
    fwd::ForwarderAgent<cl::ClusterClient<at::AeronTransport>, cl::ClusterClient<at::AeronTransport>> agent(core, me,
        agentSettings, &log);

    const int code = supervisor.run(
        [&] {
            return fwd::runDuty(agent, supervisor, board, [&monitor] { return monitor->errorCount(); });
        },
        &http);
    const fwd::StopReason &reason = agent.stopReason();
    if (reason.set())
    {
        char line[400];
        const int n = std::snprintf(line, sizeof(line), "ct-forwarder stopped: %s (exit %d): %s\n", reason.code,
            reason.exitCode, reason.detail);
        if (n > 0)
        {
            g_stderr.write(line, static_cast<std::size_t>(n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1));
        }
    }
    monitor->beginDeliberateClose();
    std::_Exit(code); // no destructor may hold a half-alive process (G6)
}
