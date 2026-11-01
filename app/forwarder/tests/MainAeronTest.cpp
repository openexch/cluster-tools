// RC2 S10' 8.3 ct_forwarder_main_aeron_tests (WITH_AERON, real media drivers; the processes are the real binaries; the
// bridge's S9c 12.2 MainAeronTest naming, review N-7). No cluster runs: the forwarder retries its connects with backoff
// and stays UP, which is all the start sequence needs.
//   startSequenceBothModes  external: no driver started, connectAeron(AERON_DIR); no driver there = exit 70 after the
//                           driver timeout; at the 3000 floor a live host driver is never read as dead. embedded: the
//                           per-instance dir ct-forwarder-<id> is created and deleted, a stale dir of a killed earlier run
//                           is reused and cleaned (forwarder_driver_dir_reclaimed_total 1), a second same-id instance =
//                           78 on the lock (3 attempts) while the live dir is kept, a per-instance path that is not a
//                           directory = 78
//   sameIdStartsAreSerialised  two same-id embedded processes in the same instant, 3 times (stale dir / no dir): one
//                           live driver, one 78, the live dir kept (the instance lock)
//   heldInstanceLockRefusesTheStart  in-process: a held <dir>.lock refuses the start and leaves a stale dir untouched
//                           (RED EMBEDDED_INSTANCE_LOCK)
//   embeddedDirIsPerInstance  in-process: the stale dir of this instance is reclaimed (RED EMBEDDED_DIR_PER_PID)
//   conductorStallExits70   ct_forwarder_tc_conductorstall on a live aeronmd: SIGUSR2 parks the client conductor; exit 70
//                           within the driver timeout + 2.5 s with the "conductor heartbeat stale" line, although the duty
//                           heartbeat kept advancing (RED CONDUCTOR_WATCH_OFF: alive after 30 s)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "AppSeams.h"
#include "ForwarderDriver.h"

namespace fs = std::filesystem;
using cluster_tools::app::common::EmbeddedDriver;
using cluster_tools::app::forwarder::forwarderDriverOptions;

namespace {

const std::string SECRET(64, '6');

// Kills and reaps a child that a failed REQUIRE left running (the bridge test's guard).
struct ChildGuard
{
    pid_t pid = -1;
    bool reaped = false;
    ~ChildGuard()
    {
        if (pid > 0 && !reaped)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

struct Process
{
    pid_t pid = -1;
    std::shared_ptr<ChildGuard> guard;
    std::string log;
    std::chrono::steady_clock::time_point started;
};

Process spawn(const std::string &binary, const std::map<std::string, std::string> &env, const std::string &log)
{
    std::vector<std::string> envStrings;
    for (const auto &[k, v] : env)
    {
        envStrings.push_back(k + "=" + v);
    }
    std::vector<char *> envp;
    for (auto &s : envStrings)
    {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);
    std::string arg0 = binary;
    char *argv[] = { arg0.data(), nullptr };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    Process p;
    p.log = log;
    p.started = std::chrono::steady_clock::now();
    const int rc = posix_spawn(&p.pid, binary.c_str(), &actions, &attr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    REQUIRE(0 == rc);
    p.guard = std::make_shared<ChildGuard>();
    p.guard->pid = p.pid;
    return p;
}

// The exit code (or -signal), or nullopt when still alive after `timeoutMs`.
std::optional<int> waitExit(Process &p, int timeoutMs)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        if (p.pid <= 0)
        {
            return std::nullopt;
        }
        int status = 0;
        const pid_t r = ::waitpid(p.pid, &status, WNOHANG);
        if (r == p.pid)
        {
            if (p.guard)
            {
                p.guard->reaped = true;
            }
            p.pid = -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        }
        if (std::chrono::steady_clock::now() >= end)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool alive(const Process &p)
{
    return p.pid > 0 && ::kill(p.pid, 0) == 0;
}

void kill9(Process &p)
{
    if (p.pid > 0)
    {
        ::kill(p.pid, SIGKILL);
        waitExit(p, 5000);
    }
}

std::string slurp(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

double secondsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// A port the kernel just handed out (FWD_HTTP_BIND refuses port 0).
int freePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(0 == ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    socklen_t len = sizeof(addr);
    REQUIRE(0 == ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len));
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

std::string httpGet(int port, const std::string &path)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (0 != ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        ::close(fd);
        return {};
    }
    const std::string request = "GET " + path + " HTTP/1.0\r\n\r\n";
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
    {
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// /health answers 200 within 10 s (the HTTP server binds before the driver; a 200 means the duty runs)
bool waitHealthy(int port, const Process &p)
{
    for (int i = 0; i < 500 && alive(p); ++i)
    {
        if (httpGet(port, "/health").rfind("HTTP/1.0 200", 0) == 0)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string workDir(const std::string &name)
{
    const std::string base = fs::exists("/dev/shm") ? "/dev/shm" : fs::temp_directory_path().string();
    const std::string dir = base + "/ct-forwarder-main-aeron-" + std::to_string(::getpid()) + "-" + name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// No cluster listens on these ports: the forwarder retries its connects with backoff and stays UP.
std::map<std::string, std::string> forwarderEnv(const std::string &mode, const std::string &dir, int httpPort,
    std::int64_t timeoutMs = 3000, std::int64_t instanceId = 1)
{
    std::map<std::string, std::string> env{
        { "FWD_CORE_INGRESS", "0=127.0.0.1:47302" },
        { "FWD_CORE_RESPONSE_CHANNEL", "aeron:udp?endpoint=127.0.0.1:0" },
        { "AE_AUTH_CREDENTIAL", "6:FORWARDER:" + SECRET },
        { "FWD_ME_INGRESS", "0=127.0.0.1:47402" },
        { "FWD_ME_RESPONSE_CHANNEL", "aeron:udp?endpoint=127.0.0.1:0" },
        { "ME_AUTH_CREDENTIAL", "7:FORWARDER:" + SECRET },
        { "FWD_INGRESS_CHANNEL", "aeron:udp?term-length=4m|mtu=8192" },
        { "FWD_DRIVER_MODE", mode },
        { "FWD_DRIVER_TIMEOUT_MS", std::to_string(timeoutMs) },
        { "FWD_INSTANCE_ID", std::to_string(instanceId) },
        { "FWD_HTTP_BIND", "127.0.0.1:" + std::to_string(httpPort) },
    };
    env["external" == mode ? "AERON_DIR" : "FWD_DRIVER_DIR"] = dir;
    if (const char *path = std::getenv("PATH"))
    {
        env["PATH"] = path;
    }
    return env;
}

Process startAeronmd(const std::string &dir, const std::string &log)
{
    Process p = spawn(CT_AERONMD_BINARY, { { "AERON_DIR", dir }, { "AERON_DIR_DELETE_ON_START", "1" } }, log);
    for (int i = 0; i < 250 && !fs::exists(dir + "/cnc.dat"); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(fs::exists(dir + "/cnc.dat"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return p;
}

std::size_t forwarderDirs(const std::string &dir)
{
    std::size_t n = 0;
    for (const auto &e : fs::directory_iterator(dir))
    {
        n += e.is_directory() && e.path().filename().string().rfind("ct-forwarder-", 0) == 0 ? 1 : 0; // the .lock is a file
    }
    return n;
}

}

TEST_CASE("MainAeronTest.startSequenceBothModes.externalWithoutADriverExits70")
{
    const std::string dir = workDir("ext-none");
    Process fwd = spawn(CT_FORWARDER_BINARY, forwarderEnv("external", dir, freePort(), 3000), dir + "/../ct-forwarder-ext-none.log");
    const auto code = waitExit(fwd, 15000);
    const double took = secondsSince(fwd.started);
    MESSAGE("external, no driver: exit " << (code ? *code : -999) << " after " << took << " s");
    REQUIRE(code.has_value());
    CHECK(*code == 70);
    CHECK(took >= 2.9);
    CHECK(took <= 8.0);
    CHECK(slurp(fwd.log).find("no media driver answered") != std::string::npos);
    CHECK(forwarderDirs(dir) == 0); // nothing embedded was started
    fs::remove_all(dir);
    fs::remove(dir + "/../ct-forwarder-ext-none.log");
}

TEST_CASE("MainAeronTest.startSequenceBothModes.externalOnAHostDriver")
{
    const std::string dir = workDir("ext-host");
    Process aeronmd = startAeronmd(dir + "/aeron", dir + "/aeronmd.log");
    const int port = freePort();
    // at the floor of FWD_DRIVER_TIMEOUT_MS (3000): a live host driver is never read as dead; alive with /health 200 for 30 s
    Process fwd = spawn(CT_FORWARDER_BINARY, forwarderEnv("external", dir + "/aeron", port, 3000), dir + "/fwd.log");
    REQUIRE(waitHealthy(port, fwd));
    bool aliveAtTheFloor = true;
    for (int i = 0; i < 30 && aliveAtTheFloor; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        aliveAtTheFloor = !waitExit(fwd, 0).has_value() && httpGet(port, "/health").rfind("HTTP/1.0 200", 0) == 0;
    }
    CHECK(aliveAtTheFloor);
    const std::string health = httpGet(port, "/health");
    const std::string metrics = httpGet(port, "/metrics");
    CHECK(health.find("\"driverMode\":\"external\"") != std::string::npos);
    CHECK(health.find("\"identity\":1,") != std::string::npos);
    CHECK(metrics.find("forwarder_driver_mode{mode=\"external\"} 1") != std::string::npos);
    CHECK(metrics.find("forwarder_driver_dir_reclaimed_total 0") != std::string::npos);
    CHECK(httpGet(port, "/ready").rfind("HTTP/1.0 503", 0) == 0);
    CHECK(forwarderDirs(dir) == 0);
    ::kill(fwd.pid, SIGTERM);
    const auto code = waitExit(fwd, 10000);
    REQUIRE(code.has_value());
    CHECK(*code == 0);
    // the external driver is untouched by the forwarder's stop
    CHECK(!waitExit(aeronmd, 200).has_value());
    kill9(aeronmd);
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.startSequenceBothModes.embedded")
{
    const std::string dir = workDir("emb");
    const std::string instanceDir = dir + "/ct-forwarder-1";
    // a clean start: the per-instance dir is created, reclaimed 0; SIGTERM: 0 and the dir is deleted
    {
        const int port = freePort();
        Process fwd = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, port, 5000), dir + "/../fwd-emb-1.log");
        REQUIRE(waitHealthy(port, fwd));
        CHECK(fs::exists(instanceDir + "/cnc.dat"));
        CHECK(fs::exists(instanceDir + ".lock"));
        const std::string metrics = httpGet(port, "/metrics");
        CHECK(metrics.find("forwarder_driver_mode{mode=\"embedded\"} 1") != std::string::npos);
        CHECK(metrics.find("forwarder_driver_dir_reclaimed_total 0") != std::string::npos);
        CHECK(httpGet(port, "/health").find("\"driverMode\":\"embedded\"") != std::string::npos);
        ::kill(fwd.pid, SIGTERM);
        const auto code = waitExit(fwd, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 0);
        CHECK(!fs::exists(instanceDir));
    }
    // kill -9: the dir stays behind; the restart reuses and cleans it (no dead sibling on the tmpfs)
    {
        const int port = freePort();
        Process fwd = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, port, 5000), dir + "/../fwd-emb-2.log");
        REQUIRE(waitHealthy(port, fwd));
        kill9(fwd);
        REQUIRE(fs::exists(instanceDir + "/cnc.dat"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const int port2 = freePort();
        Process again = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, port2, 5000), dir + "/../fwd-emb-3.log");
        REQUIRE(waitHealthy(port2, again));
        CHECK(httpGet(port2, "/metrics").find("forwarder_driver_dir_reclaimed_total 1") != std::string::npos);
        CHECK(slurp(again.log).find("a stale dir of this instance was reused and cleaned") != std::string::npos);
        CHECK(forwarderDirs(dir) == 1);
        // a second instance with the same id on this host: 78 on the lock, every time; the running one keeps its driver
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            Process second = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, freePort(), 5000),
                dir + "/../fwd-emb-4-" + std::to_string(attempt) + ".log");
            const auto secondCode = waitExit(second, 10000);
            REQUIRE(secondCode.has_value());
            CHECK(*secondCode == 78);
            CHECK(slurp(second.log).find("another ct-forwarder holds the instance lock") != std::string::npos);
            CHECK(slurp(second.log).find("FWD_INSTANCE_ID") != std::string::npos);
            CHECK(fs::exists(instanceDir + "/cnc.dat"));
            CHECK(alive(again));
        }
        // another id on the same parent dir is a separate instance: it starts beside the first
        {
            const int port3 = freePort();
            Process other = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, port3, 5000, 2), dir + "/../fwd-emb-6.log");
            REQUIRE(waitHealthy(port3, other));
            CHECK(fs::exists(dir + "/ct-forwarder-2/cnc.dat"));
            CHECK(forwarderDirs(dir) == 2);
            ::kill(other.pid, SIGTERM);
            const auto code = waitExit(other, 10000);
            REQUIRE(code.has_value());
            CHECK(*code == 0);
        }
        CHECK(httpGet(port2, "/metrics").find("forwarder_aeron_errors_total 0") != std::string::npos);
        ::kill(again.pid, SIGTERM);
        const auto code = waitExit(again, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 0);
    }
    // a per-instance path that cannot be the driver dir: 78 (host configuration)
    {
        std::ofstream(instanceDir) << "not a directory";
        Process fwd = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, freePort(), 5000), dir + "/../fwd-emb-5.log");
        const auto code = waitExit(fwd, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 78);
        fs::remove(instanceDir);
    }
    // a parent dir that does not exist: 78 before any Aeron object (the lock cannot be created)
    {
        Process fwd = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir + "/missing", freePort(), 5000),
            dir + "/../fwd-emb-7.log");
        const auto code = waitExit(fwd, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 78);
    }
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.sameIdStartsAreSerialised")
{
    // two embedded ct-forwarder processes with the same FWD_INSTANCE_ID started in the same instant: exactly one live
    // driver and one 78, the running driver dir never deleted. Even attempts over a stale dir, odd ones over no dir.
    const std::string dir = workDir("same-id");
    const std::string instanceDir = dir + "/ct-forwarder-1";
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (0 == attempt % 2)
        {
            Process stale = startAeronmd(instanceDir, dir + "/stale-" + std::to_string(attempt) + ".log");
            kill9(stale);
            REQUIRE(fs::exists(instanceDir + "/cnc.dat"));
        }
        const std::string tag = std::to_string(attempt);
        const int portA = freePort();
        const int portB = freePort();
        Process a = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, portA, 5000), dir + "/../same-id-a-" + tag + ".log");
        Process b = spawn(CT_FORWARDER_BINARY, forwarderEnv("embedded", dir, portB, 5000), dir + "/../same-id-b-" + tag + ".log");
        std::optional<int> codeA;
        std::optional<int> codeB;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < until && !(codeA && codeB))
        {
            if (!codeA)
            {
                codeA = waitExit(a, 0);
            }
            if (!codeB)
            {
                codeB = waitExit(b, 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const int exited78 = (codeA && 78 == *codeA ? 1 : 0) + (codeB && 78 == *codeB ? 1 : 0);
        const int stillAlive = (codeA ? 0 : 1) + (codeB ? 0 : 1);
        const bool exactlyOne78 = 1 == exited78 && 1 == stillAlive;
        MESSAGE("attempt " << attempt << std::string(0 == attempt % 2 ? " (stale dir)" : " (no dir)") << ": a "
                           << (codeA ? std::to_string(*codeA) : std::string("alive")) << ", b "
                           << (codeB ? std::to_string(*codeB) : std::string("alive")));
        CHECK(exactlyOne78);
        if (exactlyOne78)
        {
            Process &survivor = codeA ? b : a;
            const Process &loser = codeA ? a : b;
            const int port = codeA ? portB : portA;
            CHECK(slurp(loser.log).find("another ct-forwarder holds the instance lock") != std::string::npos);
            CHECK(fs::exists(instanceDir + "/cnc.dat"));
            REQUIRE(waitHealthy(port, survivor));
            CHECK(httpGet(port, "/metrics").find("forwarder_driver_mode{mode=\"embedded\"} 1") != std::string::npos);
            ::kill(survivor.pid, SIGTERM);
            const auto code = waitExit(survivor, 10000);
            REQUIRE(code.has_value());
            CHECK(*code == 0);
        }
        kill9(a);
        kill9(b);
        fs::remove_all(instanceDir);
    }
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.heldInstanceLockRefusesTheStart")
{
    // the in-process EmbeddedDriver with the forwarder's options (the seamed build): while another holder has <dir>.lock,
    // start() refuses (78 in ct-forwarder) and leaves the dir as it is, even when it is stale; RED EMBEDDED_INSTANCE_LOCK
    const std::string dir = workDir("held-lock");
    const std::string instanceDir = EmbeddedDriver::dirFor(dir, 9, forwarderDriverOptions());
    Process stale = startAeronmd(instanceDir, dir + "/stale-aeronmd.log");
    kill9(stale);
    REQUIRE(fs::exists(instanceDir + "/cnc.dat"));
    const int held = ::open((instanceDir + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(held >= 0);
    REQUIRE(0 == ::flock(held, LOCK_EX | LOCK_NB)); // the other instance
    EmbeddedDriver driver(forwarderDriverOptions(), [](const std::string &) {});
    std::string error;
    const bool started = driver.start(instanceDir, false, error);
    const bool refused = !started;
    CHECK(refused); // EMBEDDED_INSTANCE_LOCK RED: no lock, the stale dir is reclaimed under the holder
    CHECK(error.find("another ct-forwarder holds the instance lock") != std::string::npos);
    CHECK(fs::exists(instanceDir + "/cnc.dat"));
    if (started)
    {
        CHECK(driver.close(2000));
    }
    ::close(held);
    EmbeddedDriver again(forwarderDriverOptions(), [](const std::string &) {});
    REQUIRE(again.start(instanceDir, false, error));
    CHECK(again.reclaimed() == !started);
    CHECK(again.close(2000));
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.embeddedDirIsPerInstance")
{
    // the in-process EmbeddedDriver with the forwarder's options: a stale dir left by a killed driver of this instance
    // is reclaimed; per pid it would be left behind as a dead sibling (RED EMBEDDED_DIR_PER_PID)
    const std::string dir = workDir("per-instance");
    Process stale = startAeronmd(dir + "/ct-forwarder-7", dir + "/stale-aeronmd.log");
    kill9(stale);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EmbeddedDriver driver(forwarderDriverOptions(), [](const std::string &) {});
    std::string error;
    REQUIRE(driver.start(EmbeddedDriver::dirFor(dir, 7, forwarderDriverOptions()), false, error));
    const bool reclaimed = driver.reclaimed();
    CHECK(reclaimed); // EMBEDDED_DIR_PER_PID RED: a stale sibling is left behind
    CHECK(forwarderDirs(dir) == 1);
    CHECK(driver.close(2000));
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.conductorStallExits70")
{
#ifndef CT_FORWARDER_TC_CONDUCTORSTALL_BINARY
    FAIL("ct_forwarder_tc_conductorstall was not built (configure with CT_FORWARDER_TEST_BUILDS=ON)");
#else
    const std::string dir = workDir("conductor");
    Process aeronmd = startAeronmd(dir + "/aeron", dir + "/aeronmd.log");
    const int port = freePort();
    auto env = forwarderEnv("external", dir + "/aeron", port, 5000);
    if (cluster_tools::app::forwarder::app_seams::disabled(cluster_tools::app::forwarder::app_seams::Rule::CONDUCTOR_WATCH_OFF))
    {
        env["CT_TEST_DISABLE_CONDUCTOR_WATCH_OFF"] = "1"; // the RED: the tc binary without the watch
    }
    Process fwd = spawn(CT_FORWARDER_TC_CONDUCTORSTALL_BINARY, env, dir + "/fwd.log");
    REQUIRE(waitHealthy(port, fwd));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto parkedAt = std::chrono::steady_clock::now();
    ::kill(fwd.pid, SIGUSR2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const std::string during = httpGet(port, "/metrics"); // the duty keeps running while the conductor is parked
    const auto code = waitExit(fwd, 30000);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - parkedAt).count();
    const bool exited70 = code.has_value() && 70 == *code;
    CHECK(exited70); // CONDUCTOR_WATCH_OFF RED: alive after 30 s
    MESSAGE("conductor parked: exit " << (code ? *code : -999) << " after " << took << " s");
    if (exited70)
    {
        CHECK(took <= 7.5);
        CHECK(slurp(fwd.log).find("conductor heartbeat stale") != std::string::npos);
        CHECK(during.find("forwarder_duty_max_cycle_us") != std::string::npos);
    }
    kill9(fwd);
    kill9(aeronmd);
    fs::remove_all(dir);
#endif
}
