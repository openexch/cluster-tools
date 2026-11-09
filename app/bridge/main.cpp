// ct-bridge - the production C++ settlement bridge: a stateless C++20 process with
// one resumable BRIDGE session on the core (the Assets Engine cluster) and one ME journal archive source attached at a
// time. Every journal record is fed to the core exactly once and in order, or the bridge halts loudly; the domain
// decides, this process wires. Nothing is written to disk.
//
// The media driver is configurable: BRIDGE_DRIVER_MODE=external (the prod default: the host aeronmd
// at AERON_DIR that admin / cloud-console manage) or embedded (dev and demo: the C media driver in this process at
// <BRIDGE_DRIVER_DIR>/ct-bridge-<BRIDGE_INSTANCE_ID>). No default.
//
// Exit codes: 0 stopped (SIGTERM/SIGINT); 70 EX_SOFTWARE process health (an Aeron fatal from the conductor
// thread incl. a dead or stale driver in either mode, the duty watchdog, the conductor heartbeat check, an escaped
// exception, the stop join bound, a domain fatal, an INVALID reject of our own 7 / 66); 75 EX_TEMPFAIL an inferred
// registration refusal; 78 EX_CONFIG configuration or an observed refusal (the unit sets RestartPreventExitStatus=78).
//
// Test builds only (never published, publish.yml refuses them): CT_BRIDGE_NC_PRESET names the seams the
// ct_bridge_nc_* / ct_bridge_negctl binaries switch off; CT_BRIDGE_TC_DRIVERSTALL (embedded driver on a manual duty
// cycle that SIGUSR2 stops) and CT_BRIDGE_TC_CONDUCTORSTALL (SIGUSR2 parks the client conductor) are the (b) / (d)
// test-corruption builds.
#include <signal.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <Aeron.h>

#include "cluster_tools/adapters/sbe/BridgeCodec.h"
#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/archive/aeron/SourceSet.h"
#include "cluster_tools/build_info.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/domain/bridge/TestSeams.h"

#include "common/ConductorWatch.h"
#include "common/HttpServer.h"
#include "common/LogRing.h"
#include "common/Supervisor.h"

#include "AppSeams.h"
#include "BridgeAgent.h"
#include "BridgeConfig.h"
#include "BridgeDuty.h"
#include "EmbeddedDriver.h"
#include "ObserverTap.h"

#ifndef CT_BUILD_SOURCE_SHA
#define CT_BUILD_SOURCE_SHA "unknown" // -DCT_BUILD_SOURCE_SHA (CMake) names the commit
#endif

#ifndef CT_BRIDGE_VERSION
#define CT_BRIDGE_VERSION "dev"
#endif

namespace br = cluster_tools::app::bridge;
namespace common = cluster_tools::app::common;
namespace cl = cluster_tools::client;
namespace at = cluster_tools::client::aeron_transport;
namespace arc = cluster_tools::archive;
namespace aa = cluster_tools::archive::aeron_archive;

namespace {

common::NonBlockingLogWriter g_stderr(2); // main's log drain never blocks on stderr

void writeRecord(const common::LogRecord &r)
{
    g_stderr(r);
}

[[noreturn]] void onTerminate() noexcept
{
    static const char line[] = "ct-bridge: std::terminate (an exception escaped a noexcept boundary), exit 70\n";
    g_stderr.write(line, sizeof(line) - 1);
    std::_Exit(70);
}

void line(const char *format, const std::string &a)
{
    char text[600];
    const int n = std::snprintf(text, sizeof(text), format, a.c_str());
    if (n > 0)
    {
        g_stderr.write(text, static_cast<std::size_t>(n < static_cast<int>(sizeof(text)) ? n : static_cast<int>(sizeof(text)) - 1));
    }
}

#if defined(CT_BRIDGE_TC_DRIVERSTALL) || defined(CT_BRIDGE_TC_CONDUCTORSTALL)
std::atomic<bool> g_parkConductor{ false };
#endif

int run()
{
    common::Supervisor::blockSignals(); // before any thread (the Aeron conductor and the embedded driver included)
    common::Supervisor::ignoreSigpipe();
    common::Supervisor::setProcessName("ct-bridge");
#if defined(CT_BRIDGE_TC_DRIVERSTALL) || defined(CT_BRIDGE_TC_CONDUCTORSTALL)
    {
        sigset_t usr2;
        sigemptyset(&usr2);
        sigaddset(&usr2, SIGUSR2);
        pthread_sigmask(SIG_BLOCK, &usr2, nullptr);
    }
#endif
#if defined(CT_BRIDGE_NC_PRESET)
    {
        const bool app = br::app_seams::forceDisable(CT_BRIDGE_NC_PRESET, true);
        const bool dom = cluster_tools::domain::bridge::test_seams::forceDisable(CT_BRIDGE_NC_PRESET);
        (void)app;
        (void)dom;
        std::fprintf(stderr, "ct-bridge NEGATIVE-CONTROL BUILD: rules off: %s\n", CT_BRIDGE_NC_PRESET);
    }
#endif
#if defined(CT_BRIDGE_TC_DRIVERSTALL)
    std::fprintf(stderr, "ct-bridge TEST-CORRUPTION BUILD: the embedded driver runs on a manual duty cycle; SIGUSR2 stops it\n");
#endif
#if defined(CT_BRIDGE_TC_CONDUCTORSTALL)
    std::fprintf(stderr, "ct-bridge TEST-CORRUPTION BUILD: SIGUSR2 parks the Aeron client conductor\n");
#endif

    // 3.3 step 2: the strict configuration, before any Aeron object (a 78 after connectAeron would become a 70)
    const br::ConfigResult config = br::parseBridgeConfig([](const char *name) { return std::getenv(name); });
    if (!config.ok())
    {
        for (const std::string &error : config.errors)
        {
            std::fprintf(stderr, "ct-bridge config: %s\n", error.c_str());
        }
        std::fprintf(stderr, "ct-bridge: refusing to start (exit 78, C1)\n");
        return 78;
    }
    const br::BridgeConfig &c = config.config;
    std::fprintf(stderr,
        "ct-bridge %s identity %" PRId64 " money v%d (%s) journal v%d (%s) aeron %s; driver %s (%s, timeout %" PRId64
        " ms); core %s egress %s; %zu archive source(s); http %s:%u\n",
        CT_BRIDGE_VERSION, c.instanceId, cluster_tools::build_info::MONEY_SCHEMA_VERSION,
        cluster_tools::build_info::MONEY_SCHEMA_SHA256, cluster_tools::build_info::ME_JOURNAL_SCHEMA_VERSION,
        cluster_tools::build_info::ME_JOURNAL_SCHEMA_SHA256, cluster_tools::build_info::AERON_VERSION,
        br::driverModeName(c.driverMode),
        br::DriverMode::EXTERNAL == c.driverMode ? c.aeronDir.c_str() : c.driverDir.c_str(), c.driverTimeoutMs,
        c.ingressEndpoints.c_str(), c.egressEndpoint.c_str(), c.archive.archives.size(), c.httpHost.c_str(),
        static_cast<unsigned>(c.httpPort));

    common::LogRing log;
    br::BridgeBoard board;
    common::SupervisorSettings supervisorSettings = br::bridgeSupervisorSettings();
    static_assert(br::STOP_ARCHIVE_TIMEOUT_NS == static_cast<std::int64_t>(aa::ARCHIVE_MESSAGE_TIMEOUT_NS));
    static_assert(br::STOP_JOIN_BOUND_NS == 7'000'000'000LL);
    common::Supervisor supervisor(board, log, supervisorSettings, &common::Supervisor::exitProcess, &writeRecord);

    // 3.3 step 3: the HTTP bind (78)
    br::HttpContext context;
    common::HttpServer http([&](const common::HttpRequest &request) { return br::routeBridge(request, context); },
        "BRIDGE_HTTP_BIND");
    std::string error;
    if (!http.bind(c.httpHost, c.httpPort, error))
    {
        std::fprintf(stderr, "ct-bridge: %s (exit 78, C1)\n", error.c_str());
        return 78;
    }
    std::fprintf(stderr, "ct-bridge: http listening on %s:%u\n", c.httpHost.c_str(), static_cast<unsigned>(http.boundPort()));

    // 3.3 step 4: the driver per mode
    std::unique_ptr<br::EmbeddedDriver> driver;
    std::string aeronDir = c.aeronDir;
    if (br::DriverMode::EMBEDDED == c.driverMode)
    {
        driver = std::make_unique<br::EmbeddedDriver>([](const std::string &why) { common::Supervisor::exitProcess(70, why); });
        const std::string dir = br::EmbeddedDriver::dirFor(c.driverDir, c.instanceId);
#if defined(CT_BRIDGE_TC_DRIVERSTALL)
        const bool manual = true;
#else
        const bool manual = false;
#endif
        if (!driver->start(dir, manual, error))
        {
            std::fprintf(stderr, "ct-bridge: embedded media driver: %s (exit 78, C3)\n", error.c_str());
            return 78;
        }
        aeronDir = dir;
        std::fprintf(stderr, "ct-bridge: embedded media driver at %s%s\n", dir.c_str(),
            driver->reclaimed() ? " (a stale dir of this instance was reused and cleaned)" : "");
    }

    // 3.3 step 5: connectAeron with the driver timeout (70: no driver answered, both modes)
    auto monitor = std::make_shared<at::AeronFatalMonitor>(); // fatal conductor errors: _Exit(70)
    std::shared_ptr<aeron::Aeron> aeron;
    try
    {
        at::AeronClientOptions options;
        options.aeronDir = aeronDir;
        options.clientName = "ct-bridge";
        options.driverTimeoutMs = c.driverTimeoutMs;
        aeron = at::connectAeron(options, monitor);
    }
    catch (const std::exception &e)
    {
        line("ct-bridge: no media driver answered at %s (exit 70, F1)\n", aeronDir + ": " + e.what());
        std::_Exit(70);
    }

#if defined(CT_BRIDGE_TC_CONDUCTORSTALL)
    // (d): a handler on the conductor thread that parks it once SIGUSR2 arrived and a counter became available
    aeron->addAvailableCounterHandler([](aeron::concurrent::CountersReader &, std::int64_t, std::int32_t) {
        while (g_parkConductor.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
#endif

    // 3.3 step 6: the objects
    br::ObserverTap tap;
    arc::ArchiveMetrics archiveMetrics;
    const arc::FollowSettings follow; // the one object: probeInterval for both the sources and the domain
    std::function<void()> invokeAgent;
    aa::Invoker invoker = [&invokeAgent]() {
        if (invokeAgent)
        {
            invokeAgent();
        }
    };
    aa::SourceSet sources(c.archive, aeron, monitor, invoker, archiveMetrics, &tap, aa::ArchiveSourceSeams{}, follow);
    at::AeronTransport transport(aeron);
    br::LinkObserver observer;
    using Client = cl::ClusterClient<at::AeronTransport, br::LinkObserver>;
    Client client(transport, br::coreClientSettings(c), &observer);
    cluster_tools::sbe_adapter::bridge_codec::BridgeCodec codec;

    br::AgentSettings agentSettings;
    agentSettings.identity = c.instanceId;
    agentSettings.sourceCount = c.archive.archives.size();
    agentSettings.probeInterval = follow.probeInterval;
    agentSettings.credentialClass = c.credentials.sessionClass;
    agentSettings.version = CT_BRIDGE_VERSION;
    agentSettings.build = CT_BUILD_SOURCE_SHA;
    agentSettings.driverMode = static_cast<std::uint8_t>(c.driverMode);
    agentSettings.park = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
    br::BridgeAgent<Client, aa::SourceSet> agent(client, sources, tap, sources.memo(), archiveMetrics, codec, agentSettings,
        &log, &board);
    observer.target = &agent;
    invokeAgent = [&agent] { agent.invoke(); };

    common::ConductorWatch watch(aeron, c.driverTimeoutMs, {},
        !br::app_seams::disabled(br::app_seams::Rule::CONDUCTOR_REVALIDATE_OFF)); // re-validated every check
    if (!br::app_seams::disabled(br::app_seams::Rule::CONDUCTOR_WATCH_OFF))
    {
        supervisor.setLivenessCheck([&watch](std::int64_t, std::string &why) { return watch.check(why); });
    }
    context.domain = &agent.metrics();
    context.archive = &archiveMetrics;
    context.supervisor = &supervisor;
    context.http = &http;
    context.board = &board;
    context.conductorAgeMs = [&watch] { return watch.ageMs(); };
    context.conductorStale = [&watch] { return watch.stale(); };
    context.sourceCount = c.archive.archives.size();
    context.driverMode = br::driverModeName(c.driverMode);
    context.driverDirReclaimed = nullptr != driver && driver->reclaimed() ? 1 : 0;

#if defined(CT_BRIDGE_TC_DRIVERSTALL) || defined(CT_BRIDGE_TC_CONDUCTORSTALL)
    std::thread tc([&] {
        sigset_t usr2;
        sigemptyset(&usr2);
        sigaddset(&usr2, SIGUSR2);
        int sig = 0;
        if (0 != sigwait(&usr2, &sig))
        {
            return;
        }
#if defined(CT_BRIDGE_TC_DRIVERSTALL)
        std::fprintf(stderr, "ct-bridge TEST-CORRUPTION: SIGUSR2: the embedded driver stops (the client stays alive)\n");
        if (driver)
        {
            driver->stopDriving();
        }
#else
        std::fprintf(stderr, "ct-bridge TEST-CORRUPTION: SIGUSR2: parking the client conductor\n");
        g_parkConductor.store(true, std::memory_order_release);
        const std::uint8_t key[8] = {};
        aeron->addCounter(1101, key, sizeof(key), "ct-bridge conductor-stall trigger");
#endif
    });
    tc.detach();
#endif

    // 3.3 steps 7-8
    agent.start();
    const int code = supervisor.run(
        [&] { return br::runBridgeDuty(agent, supervisor, board, [&monitor] { return monitor->errorCount(); }); }, &http);
    const br::StopReason &reason = agent.stopReason();
    if (reason.set())
    {
        char text[400];
        const int n = std::snprintf(text, sizeof(text), "ct-bridge stopped: %s (exit %d): %s\n", reason.code,
            reason.exitCode, reason.detail);
        if (n > 0)
        {
            g_stderr.write(text, static_cast<std::size_t>(n < static_cast<int>(sizeof(text)) ? n : static_cast<int>(sizeof(text)) - 1));
        }
    }
    monitor->beginDeliberateClose();
    int exitCode = code;
    if (driver && !driver->close(2'000))
    {
        static const char slow[] = "ct-bridge: the embedded driver did not close within 2 s (exit 70)\n";
        g_stderr.write(slow, sizeof(slow) - 1);
        exitCode = 70;
    }
    std::_Exit(exitCode); // no destructor may hold a half-alive process
}

}

int main()
{
    std::set_terminate(&onTerminate); // first: nothing ends the process with SIGABRT (3.1)
    try
    {
        return run();
    }
    catch (const std::exception &e)
    {
        line("ct-bridge: fatal: %s, exit 70\n", e.what());
    }
    catch (...)
    {
        static const char unknown[] = "ct-bridge: fatal: unknown exception, exit 70\n";
        g_stderr.write(unknown, sizeof(unknown) - 1);
    }
    std::_Exit(70);
}
