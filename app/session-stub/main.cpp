// ct_session_stub - a TEST-ONLY program on the exported cluster client: one GATEWAY core session, registration with
// the channel mask given on the command line, a script of commands from stdin, and a poll throttle that leaves
// doWork() and the keepalives running (SessionStub.h). It exists so that drills about a slow or silent session can be
// dry-run before the real consumer exists. Never installed, never published (publish.yml refuses it).
//
// Threads: main (signals, the conductor watch, the output drain to stdout), the duty thread (SessionStub), the stdin
// reader (parses lines into commands). The duty thread never writes to a file descriptor.
//
// Exit: 0 quit, SIGTERM or SIGINT; 78 a configuration problem (all listed in one pass) or a REJECTED registration;
// 70 a fatal Aeron error, a dead media driver or a stale client conductor.
//
// usage: ct_session_stub --driver-mode embedded|external ... (StubOptions.h)
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/Credentials.h"
#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/runtime/ConductorWatch.h"
#include "cluster_tools/runtime/EmbeddedDriver.h"
#include "cluster_tools/runtime/IdleStrategy.h"
#include "cluster_tools/runtime/LogRing.h"
#include "session-stub/SessionStub.h"
#include "session-stub/StubCommands.h"
#include "session-stub/StubOptions.h"

namespace cl = cluster_tools::client;
namespace at = cluster_tools::client::aeron_transport;
namespace runtime = cluster_tools::runtime;
namespace st = cluster_tools::app::session_stub;

namespace
{

constexpr int EXIT_CONFIG = 78;
constexpr int EXIT_FATAL = 70;

// stdin reader -> duty thread. The duty thread only try_locks (it never waits on the reader).
struct CommandQueue
{
    std::mutex lock;
    std::deque<st::CommandResult> items;
};

void writeLine(const runtime::LogRecord &r)
{
    std::fputs(r.text, stdout);
    std::fputc('\n', stdout);
}

[[noreturn]] void exitNow(int code, const std::string &why)
{
    std::fprintf(stderr, "ct_session_stub: %s (exit %d)\n", why.c_str(), code);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

}

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    st::OptionsResult parsed = st::parseStubOptions(args);
    cl::CredentialsResult credentials;
    if (!parsed.options.credentialEnv.empty())
    {
        credentials = cl::credentialsFromEnvironment(cl::SessionClass::GATEWAY, parsed.options.credentialEnv.c_str());
        if (!credentials.ok())
        {
            parsed.errors.push_back(credentials.error);
        }
    }
    const st::StubOptions &o = parsed.options;
    cl::ClientSettings settings;
    settings.ingressEndpoints = o.ingressEndpoints;
    settings.ingressChannel = o.ingressChannel;
    settings.responseChannel = o.responseChannel;
    settings.credentials = credentials.ok() ? credentials.credentials.encode() : std::vector<std::uint8_t>{};
    settings.clientName = "ct_session_stub";
    if (parsed.ok())
    {
        if (std::string e = cl::validate(settings); !e.empty())
        {
            parsed.errors.push_back(e);
        }
    }
    if (!parsed.ok())
    {
        for (const std::string &e : parsed.errors)
        {
            std::fprintf(stderr, "ct_session_stub: %s\n", e.c_str());
        }
        std::fprintf(stderr, "ct_session_stub: %zu configuration problem(s) (exit 78)\n", parsed.errors.size());
        return EXIT_CONFIG;
    }

    // Signals are taken by main only (sigtimedwait); every thread started below inherits the mask.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGTERM);
    sigaddset(&stopSignals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    signal(SIGPIPE, SIG_IGN);

    std::unique_ptr<runtime::EmbeddedDriver> driver;
    std::string aeronDir = o.aeronDir;
    if (st::DriverMode::EMBEDDED == o.driverMode)
    {
        runtime::EmbeddedDriverOptions driverOptions;
        driverOptions.processName = "ct_session_stub";
        driverOptions.dirPrefix = "ct-session-stub-";
        driverOptions.instanceKey = "--instance-id";
        const std::string dir = runtime::EmbeddedDriver::dirFor(o.driverDir, o.instanceId, driverOptions);
        driver = std::make_unique<runtime::EmbeddedDriver>(driverOptions,
            [](const std::string &why) { exitNow(EXIT_FATAL, "embedded media driver terminated: " + why); });
        std::string error;
        if (!driver->start(dir, false, error))
        {
            const bool storage = driver->startFailedForStorage();
            std::fprintf(stderr, "ct_session_stub: embedded media driver: %s (exit %d)\n", error.c_str(),
                storage ? EXIT_FATAL : EXIT_CONFIG);
            return storage ? EXIT_FATAL : EXIT_CONFIG;
        }
        aeronDir = dir;
        std::fprintf(stderr, "ct_session_stub: embedded media driver at %s\n", dir.c_str());
    }

    auto monitor = std::make_shared<at::AeronFatalMonitor>(); // fatal conductor errors: _Exit(70)
    std::shared_ptr<aeron::Aeron> aeron;
    try
    {
        at::AeronClientOptions clientOptions;
        clientOptions.aeronDir = aeronDir;
        clientOptions.clientName = "ct_session_stub";
        clientOptions.driverTimeoutMs = o.driverTimeoutMs;
        aeron = at::connectAeron(clientOptions, monitor);
    }
    catch (const std::exception &e)
    {
        exitNow(EXIT_FATAL, "no media driver answered at " + aeronDir + ": " + e.what());
    }

    runtime::LogRing out;
    CommandQueue queue;
    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> dutyDone{ false };
    std::atomic<int> dutyExit{ 0 };
    int exitCode = 0;
    { // everything that holds the Aeron client goes before the driver closes
        at::AeronTransport transport(aeron);
        cl::ClusterClient<at::AeronTransport> client(transport, settings);
        st::SessionStub<cl::ClusterClient<at::AeronTransport>> stub(client, o, out);
        if (std::string e = stub.validate(); !e.empty())
        {
            std::fprintf(stderr, "ct_session_stub: %s (exit 78)\n", e.c_str());
            return EXIT_CONFIG;
        }

        std::thread duty([&] {
            pthread_setname_np(pthread_self(), "stub-duty");
            runtime::IdleStrategy idle(o.busySpin ? runtime::IdlePolicy{ 1, 0, 0 } : runtime::BRIDGE_IDLE);
            if (!stub.start())
            {
                out.push(runtime::LogLevel::ERROR, 0, "event=start_refused detail=\"%s\"", client.lastDetail().c_str());
            }
            while (!stopRequested.load(std::memory_order_acquire) && !stub.finished())
            {
                {
                    std::unique_lock<std::mutex> guard(queue.lock, std::try_to_lock);
                    if (guard.owns_lock())
                    {
                        while (!queue.items.empty())
                        {
                            st::CommandResult r = std::move(queue.items.front());
                            queue.items.pop_front();
                            if (!r.ok())
                            {
                                r.command.kind = st::CommandKind::INVALID; // reported in its place in the script
                                r.command.error = r.error;
                            }
                            stub.submit(std::move(r.command));
                        }
                    }
                }
                idle.idle(stub.dutyCycle());
            }
            if (!stub.finished())
            {
                stub.stop();
            }
            dutyExit.store(stub.exitCode(), std::memory_order_release);
            dutyDone.store(true, std::memory_order_release);
        });

        std::thread reader([&] {
            pthread_setname_np(pthread_self(), "stub-stdin");
            std::string pending;
            char buffer[4096];
            while (!dutyDone.load(std::memory_order_acquire))
            {
                pollfd fd{ STDIN_FILENO, POLLIN, 0 };
                const int ready = ::poll(&fd, 1, 100);
                if (ready <= 0)
                {
                    continue;
                }
                const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
                if (n <= 0)
                {
                    return; // EOF: the stub keeps running until quit or a signal
                }
                pending.append(buffer, static_cast<std::size_t>(n));
                std::size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos)
                {
                    st::CommandResult r = st::parseCommand(pending.substr(0, newline));
                    pending.erase(0, newline + 1);
                    if (st::CommandKind::NONE == r.command.kind && r.ok())
                    {
                        continue;
                    }
                    std::lock_guard<std::mutex> guard(queue.lock);
                    queue.items.push_back(std::move(r));
                }
            }
        });

        runtime::ConductorWatch watch(aeron, o.driverTimeoutMs);
        for (;;)
        {
            const timespec wait{ 0, 50'000'000L };
            const int sig = ::sigtimedwait(&stopSignals, nullptr, &wait);
            if (SIGTERM == sig || SIGINT == sig)
            {
                stopRequested.store(true, std::memory_order_release);
            }
            out.drain(writeLine);
            std::fflush(stdout);
            std::string why;
            if (!watch.check(why))
            {
                exitNow(EXIT_FATAL, "the client conductor is stale: " + why);
            }
            if (dutyDone.load(std::memory_order_acquire))
            {
                exitCode = dutyExit.load(std::memory_order_acquire);
                break;
            }
        }
        duty.join();
        reader.join();
    } // the session, transport and watch are gone; main's reference is the last one
    out.drain(writeLine);
    if (out.dropped() > 0)
    {
        std::printf("event=output_dropped count=%llu\n", static_cast<unsigned long long>(out.dropped()));
    }
    std::fflush(stdout);

    monitor->beginDeliberateClose();
    aeron.reset();
    if (driver && !driver->close(5'000))
    {
        exitNow(EXIT_FATAL, "the embedded media driver did not close within 5 s");
    }
    return exitCode;
}
