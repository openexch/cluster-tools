// RC2 S9c 12.2 ct_bridge_main_aeron_tests (WITH_AERON, real media drivers; the processes are the real binaries):
//   startSequenceBothModes  external: no driver started, connectAeron(AERON_DIR); no driver there = exit 70 after the
//                           driver timeout. embedded: the per-instance dir is created and deleted, a stale dir of a
//                           killed earlier run is reused and cleaned (bridge_driver_dir_reclaimed_total 1), a second
//                           same-id instance = 78 on the lock, a per-instance path that is not a directory = 78 (RED
//                           EMBEDDED_DIR_PER_PID on the in-process EmbeddedDriver)
//   startSequenceBothModes.embeddedLiveDriverInDir  a LIVE driver in ct-bridge-<id> with no lock held: 78 with "a LIVE
//                           media driver is already in" on 3 attempts, the dir and the driver kept (S10' review C13-1: the
//                           lock refuses every other same-id case before this check runs; its RED EMBEDDED_LIVE_CHECK is
//                           the forwarder's in-process MainAeronTest.liveDriverInDirRefusesTheStart, the code is one copy)
//   sameIdStartsAreSerialised  two same-id embedded processes in the same instant, 5 times (stale dir / no dir): one
//                           live driver, one 78, the live dir kept (the instance lock, Q3)
//   heldInstanceLockRefusesTheStart  in-process: a held <dir>.lock refuses the start and leaves a stale dir untouched
//                           (RED EMBEDDED_INSTANCE_LOCK)
//   conductorStallExits70   ct_bridge_tc_conductorstall on a live aeronmd: SIGUSR2 parks the client conductor; exit 70
//                           within the driver timeout + 2 s with the "conductor heartbeat stale" line, although the duty
//                           heartbeat kept advancing (RED CONDUCTOR_WATCH_OFF: alive after 30 s with /health 200)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/file.h>
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
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "AppSeams.h"
#include "EmbeddedDriver.h"

extern char **environ;

namespace fs = std::filesystem;
using cluster_tools::app::bridge::EmbeddedDriver;

namespace {

const std::string SECRET(64, '3');

// Kills and reaps a child that a failed REQUIRE left running (a test case aborts on the first failed REQUIRE, and the
// bridge it spawned would outlive the test binary: two orphans found on the build box, 2026-10-31).
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

Process spawn(const std::string &binary, const std::vector<std::string> &args, const std::map<std::string, std::string> &env,
    const std::string &log)
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
    std::vector<std::string> argStrings{ binary };
    argStrings.insert(argStrings.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &s : argStrings)
    {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
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
    const int rc = posix_spawn(&p.pid, binary.c_str(), &actions, &attr, argv.data(), envp.data());
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

// The HTTP port the bridge logged ("http listening on 127.0.0.1:<port>"), waiting up to 10 s.
int httpPort(const Process &p)
{
    const std::regex re("http listening on 127\\.0\\.0\\.1:([0-9]+)");
    for (int i = 0; i < 500; ++i)
    {
        std::smatch m;
        const std::string text = slurp(p.log);
        if (std::regex_search(text, m, re))
        {
            return std::stoi(m[1]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

std::string workDir(const std::string &name)
{
    const std::string base = fs::exists("/dev/shm") ? "/dev/shm" : fs::temp_directory_path().string();
    const std::string dir = base + "/ct-bridge-main-aeron-" + std::to_string(::getpid()) + "-" + name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::map<std::string, std::string> bridgeEnv(const std::string &mode, const std::string &dir, std::int64_t timeoutMs = 3000)
{
    std::map<std::string, std::string> env{
        { "BRIDGE_ME_JOURNAL_ARCHIVES", "127.0.0.1:47010" },
        { "BRIDGE_JOURNAL_CONTROL_STREAM", "4010" },
        { "BRIDGE_JOURNAL_STREAM", "4001" },
        { "BRIDGE_JOURNAL_CHANNEL", "aeron:ipc" },
        { "BRIDGE_REPLAY_STREAM", "9101" },
        { "BRIDGE_JOURNAL_MTU", "1408" },
        { "BRIDGE_LOCAL_HOST", "127.0.0.1" },
        { "BRIDGE_DRIVER_MODE", mode },
        { "BRIDGE_DRIVER_TIMEOUT_MS", std::to_string(timeoutMs) },
        { "BRIDGE_AE_CLUSTER_ADDRESSES", "127.0.0.1" },
        { "BRIDGE_AE_PORT_BASE", "47300" },
        { "CLUSTER_PORT_STRIDE", "100" },
        { "BRIDGE_AE_EGRESS_ENDPOINT", "127.0.0.1:0" },
        { "AE_AUTH_CREDENTIAL", "3:BRIDGE:" + SECRET },
        { "BRIDGE_INSTANCE_ID", "1" },
        { "BRIDGE_HTTP_BIND", "127.0.0.1:0" },
    };
    env["external" == mode ? "AERON_DIR" : "BRIDGE_DRIVER_DIR"] = dir;
    if (const char *path = std::getenv("PATH"))
    {
        env["PATH"] = path;
    }
    return env;
}

Process startAeronmd(const std::string &dir, const std::string &log)
{
    Process p = spawn(CT_AERONMD_BINARY, {}, { { "AERON_DIR", dir }, { "AERON_DIR_DELETE_ON_START", "1" } }, log);
    for (int i = 0; i < 250 && !fs::exists(dir + "/cnc.dat"); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(fs::exists(dir + "/cnc.dat"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return p;
}

}

TEST_CASE("MainAeronTest.startSequenceBothModes.externalWithoutADriverExits70")
{
    const std::string dir = workDir("ext-none");
    Process bridge = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("external", dir, 3000), dir + "/../ct-bridge-ext-none.log");
    const auto code = waitExit(bridge, 15000);
    const double took = secondsSince(bridge.started);
    MESSAGE("external, no driver: exit " << (code ? *code : -999) << " after " << took << " s");
    REQUIRE(code.has_value());
    CHECK(*code == 70);
    CHECK(took >= 2.9);
    CHECK(took <= 8.0);
    CHECK(slurp(bridge.log).find("no media driver answered") != std::string::npos);
    CHECK(!fs::exists(dir + "/ct-bridge-1")); // nothing embedded was started
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.startSequenceBothModes.externalOnAHostDriver")
{
    const std::string dir = workDir("ext-host");
    Process aeronmd = startAeronmd(dir + "/aeron", dir + "/aeronmd.log");
    // at the floor of BRIDGE_DRIVER_TIMEOUT_MS (3000, review FIX-2): a live host driver is never read as dead; alive with
    // /health 200 for 60 s (at 1000 the CnC heartbeat cadence of 1 s ends it with a spurious 70)
    Process bridge = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("external", dir + "/aeron", 3000), dir + "/bridge.log");
    const int port = httpPort(bridge);
    REQUIRE(port > 0);
    bool aliveAtTheFloor = true;
    for (int i = 0; i < 60 && aliveAtTheFloor; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        aliveAtTheFloor = !waitExit(bridge, 0).has_value() && httpGet(port, "/health").rfind("HTTP/1.0 200", 0) == 0;
    }
    CHECK(aliveAtTheFloor);
    const std::string health = httpGet(port, "/health");
    const std::string metrics = httpGet(port, "/metrics");
    CHECK(health.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(health.find("\"aeConnected\":false") != std::string::npos);
    CHECK(metrics.find("bridge_driver_mode{mode=\"external\"} 1") != std::string::npos);
    CHECK(httpGet(port, "/ready").rfind("HTTP/1.0 503", 0) == 0);
    ::kill(bridge.pid, SIGTERM);
    const auto code = waitExit(bridge, 10000);
    REQUIRE(code.has_value());
    CHECK(*code == 0);
    // the external driver is untouched by the bridge's stop
    CHECK(!waitExit(aeronmd, 200).has_value());
    kill9(aeronmd);
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.startSequenceBothModes.embedded")
{
    const std::string dir = workDir("emb");
    const std::string instanceDir = dir + "/ct-bridge-1";
    // a clean start: the per-instance dir is created, reclaimed 0; SIGTERM: 0 and the dir is deleted
    {
        Process bridge = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000), dir + "/../bridge-emb-1.log");
        const int port = httpPort(bridge);
        REQUIRE(port > 0);
        CHECK(fs::exists(instanceDir + "/cnc.dat"));
        const std::string metrics = httpGet(port, "/metrics");
        CHECK(metrics.find("bridge_driver_mode{mode=\"embedded\"} 1") != std::string::npos);
        CHECK(metrics.find("bridge_driver_dir_reclaimed_total 0") != std::string::npos);
        ::kill(bridge.pid, SIGTERM);
        const auto code = waitExit(bridge, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 0);
        CHECK(!fs::exists(instanceDir));
    }
    // kill -9: the dir stays behind; the restart reuses and cleans it (no dead sibling on the tmpfs, FIX-1)
    {
        Process bridge = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000), dir + "/../bridge-emb-2.log");
        REQUIRE(httpPort(bridge) > 0);
        kill9(bridge);
        REQUIRE(fs::exists(instanceDir + "/cnc.dat"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // the check itself waits out a fresh beat
        Process again = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000), dir + "/../bridge-emb-3.log");
        const int port = httpPort(again);
        REQUIRE(port > 0);
        CHECK(httpGet(port, "/metrics").find("bridge_driver_dir_reclaimed_total 1") != std::string::npos);
        std::size_t siblings = 0;
        for (const auto &e : fs::directory_iterator(dir))
        {
            siblings += e.is_directory() && e.path().filename().string().rfind("ct-bridge-", 0) == 0 ? 1 : 0; // dirs: the .lock sits next to it
        }
        CHECK(siblings == 1);
        // a second instance with the same id on this host: 78 on the instance lock, every time, and the running instance
        // keeps its driver (the live-driver check behind the lock: embeddedLiveDriverInDir below)
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            Process second = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000),
                dir + "/../bridge-emb-4-" + std::to_string(attempt) + ".log");
            const auto secondCode = waitExit(second, 10000);
            REQUIRE(secondCode.has_value());
            CHECK(*secondCode == 78);
            CHECK(slurp(second.log).find("another ct-bridge holds the instance lock") != std::string::npos);
            CHECK(fs::exists(instanceDir + "/cnc.dat"));
            CHECK(::kill(again.pid, 0) == 0);
        }
        CHECK(httpGet(port, "/metrics").find("bridge_aeron_errors_total 0") != std::string::npos);
        ::kill(again.pid, SIGTERM);
        const auto code = waitExit(again, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 0);
    }
    // a per-instance path that cannot be the driver dir: 78 (host configuration, FIX-12)
    {
        std::ofstream(instanceDir) << "not a directory";
        Process bridge = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000), dir + "/../bridge-emb-5.log");
        const auto code = waitExit(bridge, 10000);
        REQUIRE(code.has_value());
        CHECK(*code == 78);
        fs::remove(instanceDir);
    }
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.startSequenceBothModes.embeddedLiveDriverInDir")
{
    // a live aeronmd in <BRIDGE_DRIVER_DIR>/ct-bridge-1 and NO lock file (an aeronmd pointed at that path, or a cleanup
    // that removed the .lock under a running instance): the lock is taken on a fresh inode, so only the live-driver check
    // (the driver beats once per 1 s timer interval, the check samples longer than that) keeps dir_delete_on_start off a
    // live driver dir
    const std::string dir = workDir("emb-live");
    const std::string instanceDir = dir + "/ct-bridge-1";
    Process aeronmd = startAeronmd(instanceDir, dir + "/../bridge-emb-live-aeronmd.log");
    REQUIRE(!fs::exists(instanceDir + ".lock"));
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        Process bridge = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000),
            dir + "/../bridge-emb-live-" + std::to_string(attempt) + ".log");
        const auto code = waitExit(bridge, 15000);
        const double took = secondsSince(bridge.started);
        const std::string log = slurp(bridge.log);
        MESSAGE("attempt " << attempt << ": exit " << (code ? *code : -999) << " after " << took << " s");
        REQUIRE(code.has_value());
        CHECK(*code == 78);
        CHECK(log.find("a LIVE media driver is already in " + instanceDir) != std::string::npos);
        CHECK(log.find("BRIDGE_INSTANCE_ID") != std::string::npos);
        CHECK(log.find("holds the instance lock") == std::string::npos); // refused by the live check, not the lock
        CHECK(took >= 1.5);
        CHECK(fs::exists(instanceDir + "/cnc.dat"));
        CHECK(::kill(aeronmd.pid, 0) == 0);
    }
    CHECK(!waitExit(aeronmd, 200).has_value());
    kill9(aeronmd);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        fs::remove(dir + "/../bridge-emb-live-" + std::to_string(attempt) + ".log");
    }
    fs::remove(dir + "/../bridge-emb-live-aeronmd.log");
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.sameIdStartsAreSerialised")
{
    // Q3 (coordinator decision 2026-11-01): two embedded ct-bridge processes with the same BRIDGE_INSTANCE_ID started in
    // the same instant give exactly one live driver and one exit 78, and the running driver dir is never deleted.
    // Five attempts: even ones over a stale dir (a killed driver's: without the lock both would wait out the live check,
    // both read it stale and both reclaim it, the second deleting the first's live dir), odd ones over no dir.
    const std::string dir = workDir("same-id");
    const std::string instanceDir = dir + "/ct-bridge-1";
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (0 == attempt % 2)
        {
            Process stale = startAeronmd(instanceDir, dir + "/stale-" + std::to_string(attempt) + ".log");
            kill9(stale);
            REQUIRE(fs::exists(instanceDir + "/cnc.dat"));
        }
        const std::string tag = std::to_string(attempt);
        Process a = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000), dir + "/../same-id-a-" + tag + ".log");
        Process b = spawn(CT_BRIDGE_BINARY, {}, bridgeEnv("embedded", dir, 5000), dir + "/../same-id-b-" + tag + ".log");
        // the loser exits at once (the lock); give a reclaiming pair time for its whole live check (about 3 s) and more
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
        const int alive = (codeA ? 0 : 1) + (codeB ? 0 : 1);
        const bool exactlyOne78 = 1 == exited78 && 1 == alive;
        MESSAGE("attempt " << attempt << std::string(0 == attempt % 2 ? " (stale dir)" : " (no dir)") << ": a "
                           << (codeA ? std::to_string(*codeA) : std::string("alive")) << ", b "
                           << (codeB ? std::to_string(*codeB) : std::string("alive")));
        CHECK(exactlyOne78); // EMBEDDED_INSTANCE_LOCK is proved RED in-process (heldInstanceLockRefusesTheStart)
        Process &survivor = codeA ? b : a;
        const Process &loser = codeA ? a : b;
        if (exactlyOne78)
        {
            CHECK(slurp(loser.log).find("another ct-bridge holds the instance lock") != std::string::npos);
            const bool liveDirKept = fs::exists(instanceDir + "/cnc.dat") && ::kill(survivor.pid, 0) == 0;
            CHECK(liveDirKept);
            const int port = httpPort(survivor);
            REQUIRE(port > 0);
            CHECK(httpGet(port, "/metrics").find("bridge_driver_mode{mode=\"embedded\"} 1") != std::string::npos);
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
    // the in-process EmbeddedDriver (the seamed build): while another holder has <dir>.lock, start() refuses (78 in
    // ct-bridge) and leaves the dir as it is, even when that dir is stale; RED EMBEDDED_INSTANCE_LOCK: the start reclaims it
    const std::string dir = workDir("held-lock");
    const std::string instanceDir = EmbeddedDriver::dirFor(dir, 9);
    Process stale = startAeronmd(instanceDir, dir + "/stale-aeronmd.log");
    kill9(stale);
    REQUIRE(fs::exists(instanceDir + "/cnc.dat"));
    const int held = ::open((instanceDir + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(held >= 0);
    REQUIRE(0 == ::flock(held, LOCK_EX | LOCK_NB)); // the other instance
    EmbeddedDriver driver([](const std::string &) {});
    std::string error;
    const bool started = driver.start(instanceDir, false, error);
    const bool refused = !started;
    CHECK(refused); // EMBEDDED_INSTANCE_LOCK RED: no lock, the stale dir is reclaimed under the holder
    CHECK(error.find("another ct-bridge holds the instance lock") != std::string::npos);
    CHECK(fs::exists(instanceDir + "/cnc.dat")); // untouched
    if (started)
    {
        CHECK(driver.close(2000));
    }
    ::close(held);
    // once the holder is gone the same start succeeds and reclaims the stale dir
    EmbeddedDriver again([](const std::string &) {});
    REQUIRE(again.start(instanceDir, false, error));
    CHECK(again.reclaimed() == !started);
    CHECK(again.close(2000));
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.embeddedDirIsPerInstance")
{
    // the in-process EmbeddedDriver (the seamed build): a stale dir left by a killed driver of this instance is
    // reclaimed; per pid, it would be left behind as a dead sibling (RED EMBEDDED_DIR_PER_PID)
    const std::string dir = workDir("per-instance");
    Process stale = startAeronmd(dir + "/ct-bridge-7", dir + "/stale-aeronmd.log");
    kill9(stale);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EmbeddedDriver driver([](const std::string &) {});
    std::string error;
    REQUIRE(driver.start(EmbeddedDriver::dirFor(dir, 7), false, error));
    const bool reclaimed = driver.reclaimed();
    CHECK(reclaimed); // EMBEDDED_DIR_PER_PID RED: a stale sibling is left behind
    std::size_t siblings = 0;
    for (const auto &e : fs::directory_iterator(dir))
    {
        siblings += e.is_directory() && e.path().filename().string().rfind("ct-bridge-", 0) == 0 ? 1 : 0; // dirs only
    }
    CHECK(siblings == 1);
    CHECK(driver.close(2000));
    fs::remove_all(dir);
}

TEST_CASE("MainAeronTest.conductorStallExits70")
{
#ifndef CT_BRIDGE_TC_CONDUCTORSTALL_BINARY
    FAIL("ct_bridge_tc_conductorstall was not built (configure with CT_BRIDGE_TEST_BUILDS=ON)");
#else
    const std::string dir = workDir("conductor");
    Process aeronmd = startAeronmd(dir + "/aeron", dir + "/aeronmd.log");
    auto env = bridgeEnv("external", dir + "/aeron", 5000);
    if (cluster_tools::app::bridge::app_seams::disabled(cluster_tools::app::bridge::app_seams::Rule::CONDUCTOR_WATCH_OFF))
    {
        env["CT_TEST_DISABLE_CONDUCTOR_WATCH_OFF"] = "1"; // the RED: the tc binary without the watch
    }
    Process bridge = spawn(CT_BRIDGE_TC_CONDUCTORSTALL_BINARY, {}, env, dir + "/bridge.log");
    const int port = httpPort(bridge);
    REQUIRE(port > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(httpGet(port, "/health").rfind("HTTP/1.0 200", 0) == 0);
    const auto parkedAt = std::chrono::steady_clock::now();
    ::kill(bridge.pid, SIGUSR2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const std::string during = httpGet(port, "/metrics"); // the duty heartbeat keeps advancing while parked
    const auto code = waitExit(bridge, 30000);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - parkedAt).count();
    const bool exited70 = code.has_value() && 70 == *code;
    CHECK(exited70); // CONDUCTOR_WATCH_OFF RED: alive after 30 s
    MESSAGE("conductor parked: exit " << (code ? *code : -999) << " after " << took << " s");
    if (exited70)
    {
        CHECK(took <= 7.5);
        CHECK(slurp(bridge.log).find("conductor heartbeat stale") != std::string::npos);
        CHECK(during.find("bridge_duty_max_cycle_us") != std::string::npos);
    }
    kill9(bridge);
    kill9(aeronmd);
    fs::remove_all(dir);
#endif
}
