// ct-forwarder - the RC2 S12 production forwarder (contract sections 3, 8, 9): a stateless C++20 process with
// one resumable FORWARDER session on the core and one FORWARDER session on the ME. Every pending core
// MeDispatch becomes the ME's DurableOrderCommand 9 or ReleaseCommandIds 10; releases are relayed; the
// forwarder resyncs from the core after anything that may have lost a frame. Nothing is written to disk (G1).
//
// The media driver is configurable (RC2 S10' section 8, checklist C13, one rule with ct-bridge): FWD_DRIVER_MODE=external
// (prod: the host aeronmd at AERON_DIR that admin / cloud-console manage) or embedded (dev and demo: the C media driver
// in this process at <FWD_DRIVER_DIR>/ct-forwarder-<FWD_INSTANCE_ID>, lock ct-forwarder-<id>.lock). No default.
// FWD_DRIVER_TIMEOUT_MS is the connect wait, the CnC liveness check and the conductor heartbeat threshold.
//
// Exit codes: 0 stopped (SIGTERM/SIGINT); 70 EX_SOFTWARE process health (Aeron fatal from the conductor
// thread incl. a dead or stale driver in either mode, no driver answering at start, the conductor heartbeat check, an
// escaped exception or a broken invariant, the duty watchdog, the 2 s join bound, an INVALID reject of our own 65,
// F_STORAGE_STARVED: an ingress publication, or the embedded driver's start, refused for storage space for 30 s);
// 75 EX_TEMPFAIL an inferred refusal (restart allowed); 78 EX_CONFIG configuration, an embedded driver that cannot
// start (the instance lock, a live driver in the dir, not a directory, host limits; never storage space) or an
// observed refusal (the unit sets RestartPreventExitStatus=78).
//
// Test builds only (never published, publish.yml refuses them): CT_FORWARDER_NC_PRESET names the seams the
// ct_forwarder_nc_* negative-control binaries switch off; CT_FORWARDER_TC_CORRUPT is ct_forwarder_tc_corrupt;
// CT_FORWARDER_TC_CONDUCTORSTALL is ct_forwarder_tc_conductorstall (SIGUSR2 parks the Aeron client conductor).
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <Aeron.h>

#include "cluster_tools/build_info.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/domain/forwarder/TestSeams.h"

#include "common/ConductorWatch.h"
#include "common/EmbeddedDriver.h"

#include "AppSeams.h"
#include "DutyLoop.h"
#include "ForwarderAgent.h"
#include "ForwarderConfig.h"
#include "ForwarderDriver.h"
#include "HttpServer.h"
#include "LogRing.h"
#include "StatusBoard.h"
#include "Supervisor.h"

#ifndef CT_BUILD_SOURCE_SHA
#define CT_BUILD_SOURCE_SHA "unknown" // RC2 S16 W4: -DCT_BUILD_SOURCE_SHA (CMake) names the commit
#endif

#ifndef CT_FORWARDER_VERSION
#define CT_FORWARDER_VERSION "dev"
#endif

namespace fwd = cluster_tools::app::forwarder;
namespace common = cluster_tools::app::common;
namespace cl = cluster_tools::client;
namespace at = cluster_tools::client::aeron_transport;

namespace {

fwd::NonBlockingLogWriter g_stderr(2); // review NOTE-5: main's log drain never blocks on stderr

void writeRecord(const fwd::LogRecord &r)
{
    g_stderr(r);
}

#if defined(CT_FORWARDER_TC_CONDUCTORSTALL)
std::atomic<bool> g_parkConductor{ false };
#endif

}

int main()
{
    fwd::Supervisor::blockSignals(); // before any thread (the Aeron conductor and the embedded driver included)
    fwd::Supervisor::ignoreSigpipe();
#if defined(CT_FORWARDER_TC_CONDUCTORSTALL)
    {
        sigset_t usr2;
        sigemptyset(&usr2);
        sigaddset(&usr2, SIGUSR2);
        pthread_sigmask(SIG_BLOCK, &usr2, nullptr);
    }
    std::fprintf(stderr, "ct-forwarder TEST-CORRUPTION BUILD: SIGUSR2 parks the Aeron client conductor\n");
#endif
#if defined(CT_FORWARDER_NC_PRESET)
    if (!cluster_tools::domain::forwarder::test_seams::forceDisable(CT_FORWARDER_NC_PRESET) &&
        !fwd::app_seams::forceDisable(CT_FORWARDER_NC_PRESET))
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
    const std::int64_t identity = fwd::forwarderIdentity(c); // FWD_INSTANCE_ID on both cluster sessions (S10' 8.2 rule 3)
    std::fprintf(stderr,
        "ct-forwarder %s identity %" PRId64 " money v%d (%s) order v%d (%s) aeron %s; driver %s (%s, timeout %" PRId64
        " ms); core %s, ME %s, http %s:%u\n",
        CT_FORWARDER_VERSION, identity, cluster_tools::build_info::MONEY_SCHEMA_VERSION,
        cluster_tools::build_info::MONEY_SCHEMA_SHA256, cluster_tools::build_info::MATCH_SCHEMA_VERSION,
        cluster_tools::build_info::MATCH_SCHEMA_SHA256, cluster_tools::build_info::AERON_VERSION,
        fwd::driverModeName(c.driverMode), fwd::DriverMode::EXTERNAL == c.driverMode ? c.aeronDir.c_str() : c.driverDir.c_str(),
        c.driverTimeoutMs, c.coreIngress.c_str(), c.meIngress.c_str(), c.httpHost.c_str(), static_cast<unsigned>(c.httpPort));

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
    std::fprintf(stderr, "ct-forwarder: http listening on %s:%u\n", c.httpHost.c_str(), static_cast<unsigned>(http.boundPort()));

    // S10' 8.2 rule 5: the driver per mode (embedded: its own C media driver; a start failure is host configuration, 78,
    // except a start starved of storage space: retried for 30 s, then 70 F_STORAGE_STARVED; review C13-6)
    std::unique_ptr<common::EmbeddedDriver> driver;
    std::string aeronDir = c.aeronDir;
    if (fwd::DriverMode::EMBEDDED == c.driverMode)
    {
        const std::string dir = common::EmbeddedDriver::dirFor(c.driverDir, c.instanceId, fwd::forwarderDriverOptions());
        common::StorageStarvation startStorage;
        for (;;)
        {
            driver = std::make_unique<common::EmbeddedDriver>(fwd::forwarderDriverOptions(),
                [](const std::string &why) { fwd::Supervisor::exitProcess(70, why); });
            if (driver->start(dir, false, error))
            {
                break;
            }
            const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const bool onset = !startStorage.active();
            const fwd::DriverStartVerdict verdict =
                fwd::driverStartVerdict(driver->startFailedForStorage(), error, nowNs, startStorage);
            if (fwd::DriverStartVerdict::REFUSE_78 == verdict)
            {
                std::fprintf(stderr, "ct-forwarder: embedded media driver: %s (exit 78)\n", error.c_str());
                return 78;
            }
            if (fwd::DriverStartVerdict::EXIT_70_STORAGE == verdict)
            {
                std::fprintf(stderr, "ct-forwarder stopped: F_STORAGE_STARVED (exit 70): the embedded media driver could not "
                    "allocate its files for insufficient storage space on every attempt for %lld s (the media driver dir's "
                    "tmpfs is too small or full): %s\n", static_cast<long long>(startStorage.heldNs(nowNs) / 1'000'000'000LL),
                    error.c_str());
                return 70;
            }
            if (onset)
            {
                std::fprintf(stderr, "ct-forwarder: embedded media driver starved of storage space at start (retried 1 s "
                    "apart, exit 70 if it lasts 30 s): %s\n", error.c_str());
            }
            driver.reset(); // releases the instance lock: the next attempt is a new object (and may lose it, then 78)
            sigset_t stopSignals;
            sigemptyset(&stopSignals);
            sigaddset(&stopSignals, SIGTERM);
            sigaddset(&stopSignals, SIGINT);
            const timespec second{ 1, 0 };
            if (const int sig = ::sigtimedwait(&stopSignals, nullptr, &second); SIGTERM == sig || SIGINT == sig)
            {
                std::fprintf(stderr, "ct-forwarder: stopped by signal %d while the embedded driver was starved (exit 0)\n", sig);
                return 0;
            }
        }
        aeronDir = dir;
        std::fprintf(stderr, "ct-forwarder: embedded media driver at %s%s\n", dir.c_str(),
            driver->reclaimed() ? " (a stale dir of this instance was reused and cleaned)" : "");
    }

    // connectAeron with the driver timeout (70: no driver answered, both modes); from then on a dead or stale driver is
    // a fatal conductor error after the same timeout (F1, both modes)
    auto monitor = std::make_shared<at::AeronFatalMonitor>(); // fatal conductor errors: _Exit(70) (F1)
    std::shared_ptr<aeron::Aeron> aeron;
    try
    {
        at::AeronClientOptions options;
        options.aeronDir = aeronDir;
        options.clientName = "ct-forwarder";
        options.driverTimeoutMs = c.driverTimeoutMs; // FWD_DRIVER_TIMEOUT_MS (S10' 8.2 rule 2)
        aeron = at::connectAeron(options, monitor);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "ct-forwarder: no media driver answered at %s: %s (exit 70)\n", aeronDir.c_str(), e.what());
        std::fflush(stderr);
        std::_Exit(70);
    }

#if defined(CT_FORWARDER_TC_CONDUCTORSTALL)
    // the bridge's R-K13 (d) shape: a handler on the conductor thread that parks it once SIGUSR2 arrived and a counter
    // became available
    aeron->addAvailableCounterHandler([](aeron::concurrent::CountersReader &, std::int64_t, std::int32_t) {
        while (g_parkConductor.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
#endif

    at::AeronTransport coreTransport(aeron);
    at::AeronTransport meTransport(aeron);
    cl::ClusterClient<at::AeronTransport> core(coreTransport, fwd::coreClientSettings(c));
    cl::ClusterClient<at::AeronTransport> me(meTransport, fwd::meClientSettings(c));

    fwd::AgentSettings agentSettings;
    agentSettings.identity = identity;
    agentSettings.coreCredentialClass = c.coreCredentials.sessionClass;
    agentSettings.version = CT_FORWARDER_VERSION;
    agentSettings.build = CT_BUILD_SOURCE_SHA;
    agentSettings.driverMode = static_cast<std::uint8_t>(c.driverMode);
    agentSettings.driverDirReclaimed = nullptr != driver && driver->reclaimed() ? 1 : 0;
    fwd::ForwarderAgent<cl::ClusterClient<at::AeronTransport>, cl::ClusterClient<at::AeronTransport>> agent(core, me,
        agentSettings, &log);

    // S10' 8.2 rule 2 / S9c 19.5 FIX-1: the client-conductor heartbeat check, re-validated on every check (a reclaimed or
    // re-used counter slot is a 70, never a healthy read), threshold FWD_DRIVER_TIMEOUT_MS; main runs it at every wake
    common::ConductorWatch watch(aeron, c.driverTimeoutMs);
    if (!fwd::app_seams::disabled(fwd::app_seams::Rule::CONDUCTOR_WATCH_OFF))
    {
        supervisor.setLivenessCheck([&watch](std::int64_t, std::string &why) { return watch.check(why); });
    }

#if defined(CT_FORWARDER_TC_CONDUCTORSTALL)
    std::thread tc([&] {
        sigset_t usr2;
        sigemptyset(&usr2);
        sigaddset(&usr2, SIGUSR2);
        int sig = 0;
        if (0 != sigwait(&usr2, &sig))
        {
            return;
        }
        std::fprintf(stderr, "ct-forwarder TEST-CORRUPTION: SIGUSR2: parking the client conductor\n");
        g_parkConductor.store(true, std::memory_order_release);
        const std::uint8_t key[8] = {};
        aeron->addCounter(1101, key, sizeof(key), "ct-forwarder conductor-stall trigger");
    });
    tc.detach();
#endif

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
    int exitCode = code;
    if (driver && !driver->close(2'000))
    {
        static const char slow[] = "ct-forwarder: the embedded driver did not close within 2 s (exit 70)\n";
        g_stderr.write(slow, sizeof(slow) - 1);
        exitCode = 70;
    }
    std::_Exit(exitCode); // no destructor may hold a half-alive process (G6)
}
