// RC2 S9c 12.2 ct_bridge_main_tests (pure: the shared app/common Supervisor with the bridge's settings, the bridge-http
// routes, duty stubs):
//   healthStalledAndWatchdog   a duty stub that stops beating: /health 503 STALLED within 6 s and the exit seam 70 at
//                              5 s while an HTTP client trickles 1 byte per 200 ms (RED WATCHDOG_OFF)
//   blockedCallThatKeepsBeating a duty stub inside a 6 s call that beats every 1 ms (the invoker shape): NO exit
//   stopJoinBoundExits70       a duty stub that never returns after SIGTERM: 70 at the 7 s join bound
//   sigtermReturns0            SIGTERM with an idle listener returns 0 within 1 s, the listener shut down
//   sighupIsCountedOnly        COUNT_ONLY counts SIGHUP and never sets the resync flag; the forwarder's RESYNC_FLAG does
//   routesAreGetOnly           405 for another method, 404 elsewhere
// The agent-level cases (the real invoker under a blocked archive call, INVOKER_NO_BEAT, the dead-archive stop) are in
// StopPathTest (ct_bridge_app_tests).
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "BridgeDuty.h"

using namespace cluster_tools::app::bridge;
namespace common = cluster_tools::app::common;
namespace db = cluster_tools::domain::bridge;
namespace arc = cluster_tools::archive;

namespace {

struct ExitRecord
{
    std::atomic<int> code{ -1 };
    std::atomic<std::int64_t> atNs{ 0 };
};

double secondsSince(std::int64_t startNs, std::int64_t nowNs)
{
    return static_cast<double>(nowNs - startNs) / 1e9;
}

int connectTo(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (0 != ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string get(std::uint16_t port, const std::string &request)
{
    const int fd = connectTo(port);
    if (fd < 0)
    {
        return {};
    }
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
    {
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

struct Fixture
{
    BridgeBoard board;
    common::LogRing log;
    db::BridgeMetrics domain;
    arc::ArchiveMetrics archive;
    ExitRecord exit;
    common::Supervisor supervisor{ board, log, bridgeSupervisorSettings(),
        [this](int code, const std::string &) {
            exit.atNs.store(common::Supervisor::monotonicNs());
            exit.code.store(code);
        },
        nullptr };
    HttpContext context;
    common::HttpServer http{ [this](const common::HttpRequest &r) { return routeBridge(r, context); }, "BRIDGE_HTTP_BIND" };

    Fixture()
    {
        context.domain = &domain;
        context.archive = &archive;
        context.supervisor = &supervisor;
        context.http = &http;
        context.board = &board;
        std::string error;
        REQUIRE(http.bind("127.0.0.1", 0, error));
        BridgeSnapshot s;
        s.view.reportThisTerm = true;
        board.tryPublish(s);
    }
};

}

TEST_CASE("MainTest.healthStalledAndWatchdog")
{
    Fixture f;
    std::atomic<bool> release{ false };
    std::atomic<std::int64_t> stalledAt{ 0 };
    std::string healthy;
    std::atomic<bool> trickleDone{ false };
    std::thread trickler;
    const int code = f.supervisor.run([&] {
        for (int i = 0; i < 50; i++)
        {
            f.board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        healthy = get(f.http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
        // a client that trickles one byte every 200 ms must not hold bridge-http (the 2 s whole-request deadline)
        trickler = std::thread([&] {
            const int fd = connectTo(f.http.boundPort());
            const std::string request = "GET /health HTTP/1.0\r\n\r\n";
            for (std::size_t i = 0; fd >= 0 && i < request.size() && !trickleDone.load(); ++i)
            {
                ::send(fd, request.data() + i, 1, MSG_NOSIGNAL);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (fd >= 0)
            {
                ::close(fd);
            }
        });
        stalledAt.store(common::Supervisor::monotonicNs());
        // wedged: no heartbeat (bounded at 9 s so the WATCHDOG_OFF RED fails an assertion instead of hanging)
        while (!release.load() && common::Supervisor::monotonicNs() - stalledAt.load() < 9'000'000'000LL)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 0;
    }, &f.http);
    trickleDone.store(true);
    const bool watchdogFired = code == 70 && f.exit.code.load() == 70;
    CHECK(watchdogFired); // WATCHDOG_OFF RED: alive with a wedged duty thread
    const double took = secondsSince(stalledAt.load(), f.exit.atNs.load());
    MESSAGE("watchdog fired " << took << " s after the last heartbeat");
    CHECK(took >= 4.5);
    CHECK(took <= 6.5);
    CHECK(healthy.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(healthy.find("\"status\":\"UP\"") != std::string::npos);
    release.store(true);
    if (trickler.joinable())
    {
        trickler.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.stalledHealthIs503BeforeTheExit")
{
    // the STALLED body with the watchdog later than STALLED (a test-only spread so the 503 is observable)
    BridgeBoard board;
    common::LogRing log;
    db::BridgeMetrics domain;
    arc::ArchiveMetrics archive;
    common::SupervisorSettings settings = bridgeSupervisorSettings();
    settings.stalledNs = 500'000'000LL;
    settings.watchdogNs = 3'000'000'000LL;
    std::atomic<int> exitCode{ -1 };
    common::Supervisor supervisor(board, log, settings, [&](int code, const std::string &) { exitCode.store(code); }, nullptr);
    HttpContext context;
    context.domain = &domain;
    context.archive = &archive;
    context.supervisor = &supervisor;
    context.board = &board;
    common::HttpServer http([&](const common::HttpRequest &r) { return routeBridge(r, context); }, "BRIDGE_HTTP_BIND");
    context.http = &http;
    std::string error;
    REQUIRE(http.bind("127.0.0.1", 0, error));
    BridgeSnapshot s;
    board.tryPublish(s);
    std::string stalled;
    std::string ready;
    std::atomic<bool> release{ false };
    const int code = supervisor.run([&] {
        for (int i = 0; i < 20; i++)
        {
            board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1800)); // past STALLED, before the watchdog
        stalled = get(http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
        ready = get(http.boundPort(), "GET /ready HTTP/1.0\r\n\r\n");
        while (!release.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 0;
    }, &http);
    CHECK(code == 70);
    CHECK(stalled.rfind("HTTP/1.0 503", 0) == 0);
    CHECK(stalled.find("\"halted\":false,") != std::string::npos); // cloud-console's "never restart halted" rule holds
    CHECK(stalled.find("\"status\":\"STALLED\"") != std::string::npos);
    CHECK(ready.rfind("HTTP/1.0 503", 0) == 0);
    CHECK(ready.find("stalled") != std::string::npos);
    release.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.blockedCallThatKeepsBeating")
{
    // the archive invoker beats about every 1 ms during a blocked call: a 6 s call is not a wedged thread (FIX-6)
    Fixture f;
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(6500));
        ::kill(::getpid(), SIGTERM);
    });
    const int code = f.supervisor.run([&] {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < end)
        {
            f.board.beat(); // the invoker's heartbeat
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (!f.supervisor.stopRequested.load())
        {
            f.board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, &f.http);
    killer.join();
    CHECK(code == 0);
    CHECK(f.exit.code.load() == -1);
}

TEST_CASE("MainTest.stopJoinBoundExits70")
{
    Fixture f;
    std::atomic<bool> release{ false };
    std::atomic<std::int64_t> signalledAt{ 0 };
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        signalledAt.store(common::Supervisor::monotonicNs());
        ::kill(::getpid(), SIGTERM);
    });
    const int code = f.supervisor.run([&] {
        while (!release.load())
        {
            f.board.beat(); // alive but never returns
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, &f.http);
    killer.join();
    CHECK(code == 70);
    CHECK(f.exit.code.load() == 70);
    const double took = secondsSince(signalledAt.load(), f.exit.atNs.load());
    MESSAGE("join bound hit " << took << " s after SIGTERM");
    CHECK(took >= 6.9);
    CHECK(took <= 7.6);
    release.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.sigtermReturns0")
{
    Fixture f;
    std::atomic<std::int64_t> signalledAt{ 0 };
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        signalledAt.store(common::Supervisor::monotonicNs());
        ::kill(::getpid(), SIGTERM);
    });
    const int code = f.supervisor.run([&] {
        while (!f.supervisor.stopRequested.load())
        {
            f.board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, &f.http);
    const std::int64_t returnedAt = common::Supervisor::monotonicNs();
    killer.join();
    CHECK(code == 0);
    CHECK(f.exit.code.load() == -1);
    CHECK(f.http.finished());
    CHECK(secondsSince(signalledAt.load(), returnedAt) <= 1.0);
}

TEST_CASE("MainTest.sighupIsCountedOnly")
{
    Fixture f;
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(::getpid(), SIGHUP);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(::getpid(), SIGHUP);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(::getpid(), SIGTERM);
    });
    const int code = f.supervisor.run([&] {
        while (!f.supervisor.stopRequested.load())
        {
            f.board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, &f.http);
    killer.join();
    CHECK(code == 0);
    CHECK(f.supervisor.sighupCount.load() == 2);
    CHECK_FALSE(f.supervisor.resyncRequested.load());
    common::LogRecord r;
    bool logged = false;
    while (f.log.pop(r))
    {
        logged = logged || std::strstr(r.text, "SIGHUP ignored") != nullptr;
    }
    CHECK(logged);

    // the forwarder's policy (the same Supervisor): SIGHUP sets the resync flag (NOTE-8)
    BridgeBoard board;
    common::LogRing log;
    common::Supervisor forwarder(board, log, common::SupervisorSettings{}, [](int, const std::string &) {}, nullptr);
    std::thread hup([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(::getpid(), SIGHUP);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(::getpid(), SIGTERM);
    });
    forwarder.run([&] {
        while (!forwarder.stopRequested.load())
        {
            board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, nullptr);
    hup.join();
    CHECK(forwarder.resyncRequested.load());
    CHECK(forwarder.sighupCount.load() == 1);
}

TEST_CASE("MainTest.routesAreGetOnly")
{
    Fixture f;
    std::string health, post, other, metrics, ready;
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        health = get(f.http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
        post = get(f.http.boundPort(), "POST /ready HTTP/1.0\r\n\r\n");
        other = get(f.http.boundPort(), "GET /resync HTTP/1.0\r\n\r\n");
        metrics = get(f.http.boundPort(), "GET /metrics HTTP/1.0\r\n\r\n");
        ready = get(f.http.boundPort(), "GET /ready HTTP/1.0\r\n\r\n");
        ::kill(::getpid(), SIGTERM);
    });
    const int code = f.supervisor.run([&] {
        while (!f.supervisor.stopRequested.load())
        {
            f.board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, &f.http);
    killer.join();
    CHECK(code == 0);
    CHECK(health.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(health.find("application/json") != std::string::npos);
    CHECK(post.rfind("HTTP/1.0 405", 0) == 0);
    CHECK(other.rfind("HTTP/1.0 404", 0) == 0);
    CHECK(metrics.find("# TYPE bridge_halted gauge") != std::string::npos);
    CHECK(ready.rfind("HTTP/1.0 200", 0) == 0); // the fixture's snapshot has no bit set
    CHECK(ready.find("\r\n\r\nready\n") != std::string::npos);
}

int main(int argc, char **argv)
{
    common::Supervisor::blockSignals(); // before any thread, as main.cpp does
    doctest::Context context(argc, argv);
    return context.run();
}
