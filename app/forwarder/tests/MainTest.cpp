// RC2 S12 contract 11.3 ct_forwarder_main_tests (FIX-8): main's responsibilities against duty stubs.
//   watchdogExitsWhenDutyStalls  a duty stub that stops bumping its heartbeat -> exit seam 70 within 6 s, while an
//                                HTTP client holds a connection open without sending
//   stopJoinBoundExits70         a duty stub that never returns -> 70 after 2 s
//   stopShutsDownAccept          SIGTERM with an idle listener returns (0) within 1 s
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <new>
#include <cstring>
#include <fcntl.h>
#include <chrono>
#include <string>
#include <thread>

#include "DutyLoop.h"
#include "HttpServer.h"
#include "StatusBoard.h"
#include "Supervisor.h"

using namespace cluster_tools::app::forwarder;

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

int connectIdleClient(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(0 == ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    return fd; // sends nothing
}

std::string get(std::uint16_t port, const std::string &request)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (0 != ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        ::close(fd);
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

}

TEST_CASE("MainTest.watchdogExitsWhenDutyStalls")
{
    StatusBoard board;
    LogRing log;
    ExitRecord exit;
    Supervisor supervisor(board, log, SupervisorSettings{}, [&](int code, const std::string &) {
        exit.atNs.store(Supervisor::monotonicNs());
        exit.code.store(code);
    }, nullptr);
    HttpServer http([&](const HttpRequest &r) { return route(r, board, supervisor); });
    std::string error;
    REQUIRE(http.bind("127.0.0.1", 0, error));
    std::atomic<bool> release{ false };
    std::atomic<std::int64_t> stalledAt{ 0 };
    int idleClient = -1;
    const int code = supervisor.run([&] {
        for (int i = 0; i < 20; i++)
        {
            board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        idleClient = connectIdleClient(http.boundPort()); // holds fwd-http in recv (2 s timeout)
        stalledAt.store(Supervisor::monotonicNs());
        while (!release.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // wedged: no heartbeat
        }
        return 0;
    }, &http);
    CHECK(code == 70);
    CHECK(exit.code.load() == 70);
    const double took = secondsSince(stalledAt.load(), exit.atNs.load());
    MESSAGE("watchdog fired " << took << " s after the last heartbeat");
    CHECK(took >= 4.0);
    CHECK(took <= 6.0);
    release.store(true);
    if (idleClient >= 0)
    {
        ::close(idleClient);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.healthTurnsStalledWithTheWatchdog")
{
    StatusBoard board;
    LogRing log;
    std::atomic<int> exitCode{ -1 };
    SupervisorSettings settings;
    settings.stalledNs = 2'000'000'000LL; // observable before the (later) watchdog shuts the listener down
    settings.watchdogNs = 8'000'000'000LL;
    Supervisor supervisor(board, log, settings, [&](int code, const std::string &) { exitCode.store(code); }, nullptr);
    HttpServer http([&](const HttpRequest &r) { return route(r, board, supervisor); });
    std::string error;
    REQUIRE(http.bind("127.0.0.1", 0, error));
    std::atomic<bool> release{ false };
    std::string healthy;
    std::string stalled;
    std::string notFound;
    std::string resync;
    supervisor.run([&] {
        board.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        healthy = get(http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
        notFound = get(http.boundPort(), "GET /admin HTTP/1.0\r\n\r\n");
        resync = get(http.boundPort(), "POST /resync HTTP/1.0\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(3500)); // > 2 s + main's 1 s sampling
        stalled = get(http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
        while (!release.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 0;
    }, &http);
    CHECK(healthy.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(healthy.find("\"status\":\"UP\"") != std::string::npos);
    CHECK(notFound.rfind("HTTP/1.0 404", 0) == 0);
    CHECK(resync.rfind("HTTP/1.0 202", 0) == 0);
    CHECK(supervisor.resyncRequested.load());
    CHECK(stalled.rfind("HTTP/1.0 503", 0) == 0);
    CHECK(stalled.find("\"status\":\"STALLED\"") != std::string::npos);
    CHECK(exitCode.load() == 70); // then the watchdog
    release.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.stopJoinBoundExits70")
{
    StatusBoard board;
    LogRing log;
    ExitRecord exit;
    Supervisor supervisor(board, log, SupervisorSettings{}, [&](int code, const std::string &) {
        exit.atNs.store(Supervisor::monotonicNs());
        exit.code.store(code);
    }, nullptr);
    std::atomic<bool> release{ false };
    std::atomic<std::int64_t> signalledAt{ 0 };
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        signalledAt.store(Supervisor::monotonicNs());
        ::kill(::getpid(), SIGTERM);
    });
    const int code = supervisor.run([&] {
        while (!release.load())
        {
            board.beat(); // alive but never returns
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }, nullptr);
    killer.join();
    CHECK(code == 70);
    CHECK(exit.code.load() == 70);
    CHECK(supervisor.stopRequested.load());
    const double took = secondsSince(signalledAt.load(), exit.atNs.load());
    MESSAGE("join bound hit " << took << " s after SIGTERM");
    CHECK(took >= 1.9);
    CHECK(took <= 2.6);
    release.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.stopShutsDownAccept")
{
    StatusBoard board;
    LogRing log;
    std::atomic<int> exitCode{ -1 };
    Supervisor supervisor(board, log, SupervisorSettings{}, [&](int code, const std::string &) { exitCode.store(code); },
        nullptr);
    HttpServer http([&](const HttpRequest &r) { return route(r, board, supervisor); });
    std::string error;
    REQUIRE(http.bind("127.0.0.1", 0, error));
    std::atomic<std::int64_t> signalledAt{ 0 };
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        signalledAt.store(Supervisor::monotonicNs());
        ::kill(::getpid(), SIGTERM);
    });
    const int code = supervisor.run([&] {
        while (!supervisor.stopRequested.load())
        {
            board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }, &http);
    const std::int64_t returnedAt = Supervisor::monotonicNs();
    killer.join();
    CHECK(code == 0);
    CHECK(exitCode.load() == -1); // the exit seam was never needed
    CHECK(http.finished());       // accept() returned: the listener was shut down
    const double took = secondsSince(signalledAt.load(), returnedAt);
    MESSAGE("stopped " << took << " s after SIGTERM");
    CHECK(took <= 1.0);
}

TEST_CASE("MainTest.tricklingClientCannotHoldTheHttpThread")
{
    // Review FIX-2: one byte every 1.5 s from one client; a second client's GET /health is answered within 2.5 s.
    std::function<HttpResponse(const HttpRequest &)> handler = [](const HttpRequest &) {
        HttpResponse resp;
        resp.status = 200;
        resp.body = "ok\n";
        return resp;
    };
    HttpServer http(handler);
    std::string error;
    REQUIRE(http.bind("127.0.0.1", 0, error));
    std::thread server([&] { http.run(); });
    std::atomic<bool> stop{ false };
    std::thread trickle([&] {
        const int fd = connectIdleClient(http.boundPort());
        const char *bytes = "GET /health HTTP/1.0\r\nX: ";
        for (int i = 0; i < 6 && !stop.load(); i++)
        {
            ::send(fd, bytes + i, 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        ::close(fd);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::int64_t t0 = Supervisor::monotonicNs();
    const std::string reply = get(http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
    const double waited = secondsSince(t0, Supervisor::monotonicNs());
    MESSAGE("a second client's GET /health was answered after " << waited << " s");
    CHECK(reply.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(waited <= 2.5);
    CHECK(http.timedOut() >= 1);
    stop.store(true);
    trickle.join();
    http.shutdown();
    server.join();
}

TEST_CASE("MainTest.anExceptionOnTheHttpThreadExits70")
{
    // Review NOTE-5: a handler that throws must end the process with 70 (not std::terminate, 134).
    StatusBoard board;
    LogRing log;
    std::atomic<int> exitCode{ -1 };
    Supervisor supervisor(board, log, SupervisorSettings{}, [&](int code, const std::string &) { exitCode.store(code); },
        nullptr);
    HttpServer http([](const HttpRequest &) -> HttpResponse { throw std::bad_alloc(); });
    std::string error;
    REQUIRE(http.bind("127.0.0.1", 0, error));
    std::atomic<bool> release{ false };
    const std::int64_t t0 = Supervisor::monotonicNs();
    const int code = supervisor.run([&] {
        get(http.boundPort(), "GET /health HTTP/1.0\r\n\r\n");
        while (!release.load())
        {
            board.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }, &http);
    CHECK(code == 70);
    CHECK(exitCode.load() == 70);
    CHECK(supervisor.httpFatal.load() == 70);
    CHECK(secondsSince(t0, Supervisor::monotonicNs()) <= 2.0);
    release.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE("MainTest.aFullStderrPipeDropsLogRecordsInsteadOfBlocking")
{
    // Review NOTE-5: main's log writer never blocks (a stuck stderr must not stall the watchdog).
    int fds[2];
    REQUIRE(0 == ::pipe(fds));
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    char junk[4096];
    std::memset(junk, 'x', sizeof(junk));
    while (::write(fds[1], junk, sizeof(junk)) > 0)
    {
    }
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) & ~O_NONBLOCK); // a blocking fd, as stderr is
    NonBlockingLogWriter writer(fds[1]);
    LogRecord r;
    std::snprintf(r.text, sizeof(r.text), "%s", "a record nobody reads");
    const std::int64_t t0 = Supervisor::monotonicNs();
    for (int i = 0; i < 100; i++)
    {
        writer(r);
    }
    CHECK(secondsSince(t0, Supervisor::monotonicNs()) < 0.5);
    CHECK(writer.dropped() == 100);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("MainTest.sigpipeIsIgnored")
{
    Supervisor::ignoreSigpipe();
    int fds[2];
    REQUIRE(0 == ::pipe(fds));
    ::close(fds[0]);
    errno = 0;
    CHECK(::write(fds[1], "x", 1) == -1); // EPIPE instead of a fatal SIGPIPE (exit 141)
    CHECK(errno == EPIPE);
    ::close(fds[1]);
}

int main(int argc, char **argv)
{
    Supervisor::blockSignals(); // before any thread, as main.cpp does
    doctest::Context context(argc, argv);
    return context.run();
}
