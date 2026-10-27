// Archive integration scenarios (RC2 S9a contract 9.3): the archive side against a real Java Aeron
// Archive (JournalArchiveFixture.java on the pinned aeron-all 1.53.1) over UDP, the C++ side on the
// in-tree aeronmd in its own directory. One scenario per process run:
//   ct_archive_it <scenario> <work-dir> <aeronmd> <aeron-all.jar> <fixture-classes> <fixture-dir>
//                 <port-base> <ct-journal-tail> <ct-journal-tail-seamed>
// prints SCENARIO PASS <name> or SCENARIO FAIL <name>: <why>. run-it.sh runs them all, then the
// negative controls (the same scenario with a CT_TEST_DISABLE_<rule> seam set must FAIL).
#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/archive/aeron/SourceSet.h"
#include "cluster_tools/client/aeron/FatalErrors.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace cluster_tools::archive;
using namespace cluster_tools::archive::aeron_archive;
using cluster_tools::client::aeron_transport::AeronClientOptions;
using cluster_tools::client::aeron_transport::AeronFatalMonitor;
using cluster_tools::client::aeron_transport::connectAeron;
using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;

namespace {

constexpr std::int64_t EPOCH = 9;

struct Args
{
    std::string scenario, work, aeronmd, jar, classes, fixtures, tail, tailSeamed;
    int portBase = 0;
};
Args g;

struct Failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

void require(bool ok, const std::string &why)
{
    if (!ok)
    {
        throw Failure(why);
    }
}

void note(const std::string &line)
{
    std::printf("  %s\n", line.c_str());
    std::fflush(stdout);
}

double secondsSince(Steady::time_point t)
{
    return std::chrono::duration<double>(Steady::now() - t).count();
}

// ---- child processes

struct Child
{
    pid_t pid = -1;
    int in = -1;  // our write end -> child stdin
    int out = -1; // child stdout -> our read end
    std::string buffer;

    ~Child() { kill(SIGKILL); }

    void kill(int sig)
    {
        if (pid > 0)
        {
            ::kill(pid, sig);
            if (SIGKILL == sig)
            {
                int status = 0;
                ::waitpid(pid, &status, 0);
                pid = -1;
                if (in >= 0) ::close(in);
                if (out >= 0) ::close(out);
                in = out = -1;
            }
        }
    }

    std::string readLine(std::chrono::milliseconds timeout)
    {
        const auto deadline = Steady::now() + timeout;
        for (;;)
        {
            const auto nl = buffer.find('\n');
            if (nl != std::string::npos)
            {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                return line;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Steady::now()).count();
            if (left <= 0)
            {
                throw Failure("child did not answer within " + std::to_string(timeout.count()) + " ms");
            }
            pollfd p{ out, POLLIN, 0 };
            if (::poll(&p, 1, static_cast<int>(left)) > 0)
            {
                char chunk[4096];
                const ssize_t n = ::read(out, chunk, sizeof(chunk));
                if (n <= 0)
                {
                    throw Failure("child closed its stdout");
                }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
        }
    }
};

std::unique_ptr<Child> spawn(const std::vector<std::string> &argv, const std::map<std::string, std::string> &env,
    const std::string &stderrPath, bool pipes, const std::string &stdoutPath = "")
{
    int toChild[2] = { -1, -1 };
    int fromChild[2] = { -1, -1 };
    if (pipes && (::pipe(toChild) != 0 || ::pipe(fromChild) != 0))
    {
        throw Failure("pipe failed");
    }
    const pid_t pid = ::fork();
    if (pid < 0)
    {
        throw Failure("fork failed");
    }
    if (0 == pid)
    {
        if (pipes)
        {
            ::dup2(toChild[0], 0);
            ::dup2(fromChild[1], 1);
            ::close(toChild[1]);
            ::close(fromChild[0]);
        }
        else
        {
            const int devnull = ::open("/dev/null", O_RDONLY);
            ::dup2(devnull, 0);
            if (!stdoutPath.empty())
            {
                const int fd = ::open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                ::dup2(fd, 1);
            }
        }
        const int err = ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        ::dup2(err, 2);
        for (const auto &[k, v] : env)
        {
            ::setenv(k.c_str(), v.c_str(), 1);
        }
        std::vector<char *> raw;
        for (const auto &a : argv)
        {
            raw.push_back(const_cast<char *>(a.c_str()));
        }
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        std::_Exit(127);
    }
    auto child = std::make_unique<Child>();
    child->pid = pid;
    if (pipes)
    {
        ::close(toChild[0]);
        ::close(fromChild[1]);
        child->in = toChild[1];
        child->out = fromChild[0];
    }
    return child;
}

int waitExit(pid_t pid, std::chrono::milliseconds timeout)
{
    const auto deadline = Steady::now() + timeout;
    for (;;)
    {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid)
        {
            return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
        }
        if (Steady::now() > deadline)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(10ms);
    }
}

// ---- the C++ side's media driver

struct Driver
{
    std::string dir;
    std::unique_ptr<Child> process;

    explicit Driver(const std::string &directory) : dir(directory)
    {
        process = spawn({ g.aeronmd },
            { { "AERON_DIR", dir }, { "AERON_DIR_DELETE_ON_START", "1" }, { "AERON_DIR_DELETE_ON_SHUTDOWN", "1" } },
            g.work + "/aeronmd.log", false);
        for (int i = 0; i < 200; ++i)
        {
            struct stat st{};
            if (::stat((dir + "/cnc.dat").c_str(), &st) == 0)
            {
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw Failure("aeronmd did not start in " + dir);
    }
    ~Driver()
    {
        if (process && process->pid > 0)
        {
            ::kill(process->pid, SIGTERM);
            waitExit(process->pid, 3000ms);
            process->pid = -1;
        }
    }
};

// ---- the Java archive fixture

struct Fixture
{
    int port;
    std::unique_ptr<Child> jvm;

    explicit Fixture(int controlPort, int maxReplays = 20, const std::string &name = "a") : port(controlPort)
    {
        jvm = spawn({ "java", "--add-opens", "java.base/jdk.internal.misc=ALL-UNNAMED", "-Xmx512m", "-cp",
                        g.jar + ":" + g.classes, "JournalArchiveFixture", "/dev/shm/ct-archive-it-" + g.scenario + "-" + name,
                        g.work + "/archive-" + name, std::to_string(port), g.fixtures, std::to_string(maxReplays) },
            {}, g.work + "/fixture-" + name + ".log", true);
        const std::string ready = jvm->readLine(60s);
        require(ready.rfind("READY", 0) == 0, "fixture did not start: " + ready);
    }
    ~Fixture()
    {
        if (jvm && jvm->pid > 0)
        {
            try
            {
                ::kill(jvm->pid, SIGCONT);
                cmd("QUIT", 5s);
                waitExit(jvm->pid, 5000ms);
                jvm->pid = -1;
            }
            catch (...)
            {
            }
        }
    }

    std::string cmd(const std::string &line, std::chrono::milliseconds timeout = 120s)
    {
        const std::string l = line + "\n";
        if (::write(jvm->in, l.data(), l.size()) != static_cast<ssize_t>(l.size()))
        {
            throw Failure("fixture stdin closed");
        }
        const std::string answer = jvm->readLine(timeout);
        require(answer.rfind("ERROR", 0) != 0, "fixture " + line + ": " + answer);
        return answer;
    }

    std::string endpoint() const { return "127.0.0.1:" + std::to_string(port); }
};

// ---- the C++ archive side

struct Keepalive
{
    Steady::time_point last = Steady::now();
    std::uint64_t maxGapMs = 0;
    void tick()
    {
        const auto now = Steady::now();
        maxGapMs = std::max<std::uint64_t>(maxGapMs,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count()));
        last = now;
    }
    void reset()
    {
        last = Steady::now();
        maxGapMs = 0;
    }
};

struct Frame
{
    std::size_t source;
    std::int64_t seq;
    std::int64_t end;
    std::int64_t recording;
    std::vector<std::uint8_t> bytes;
};

struct Sink : FrameSink
{
    std::vector<Frame> frames;
    std::vector<Refusal> refused;
    std::uint64_t count = 0;
    bool keepBytes = true;
    SinkAction onFrame(const JournalFrame &f) noexcept override
    {
        ++count;
        if (keepBytes)
        {
            frames.push_back({ f.sourceIndex, f.peek.journalSeq, f.endPosition, f.key.recordingId,
                std::vector<std::uint8_t>(f.bytes, f.bytes + f.length) });
        }
        return SinkAction::CONTINUE;
    }
    RefusalAction onRefused(const RefusedFrame &f) noexcept override
    {
        refused.push_back(f.reason);
        return RefusalAction::CLOSE_REPLAY;
    }
};

struct Observer : FollowObserver
{
    std::vector<SourceFault> faults;
    std::vector<AttachInfo> attaches;
    std::vector<Steady::time_point> faultTimes;
    int ended = 0;
    Steady::time_point endedAt{};
    int refusedSources = 0;
    void onFault(const SourceFault &f) noexcept override
    {
        faults.push_back(f);
        faultTimes.push_back(Steady::now());
        std::printf("  fault %s source=%zu recording=%lld consumed=%lld %s\n", faultName(f.kind), f.source,
            static_cast<long long>(f.key.recordingId), static_cast<long long>(f.consumed), f.detail.c_str());
        std::fflush(stdout);
    }
    void onAttached(const AttachInfo &a) noexcept override
    {
        attaches.push_back(a);
        std::printf("  attach source=%zu recording=%lld from=%lld proof=%d memoFree=%d\n", a.source,
            static_cast<long long>(a.recording.key.recordingId), static_cast<long long>(a.from), a.fromProof, a.memoFree);
        std::fflush(stdout);
    }
    void onRecordingEnded(std::size_t, const Recording &) noexcept override
    {
        ++ended;
        endedAt = Steady::now();
    }
    void onSourceRefused(std::size_t, const Recording &) noexcept override { ++refusedSources; }
    const SourceFault *find(FaultKind kind) const
    {
        for (const auto &f : faults)
        {
            if (f.kind == kind)
            {
                return &f;
            }
        }
        return nullptr;
    }
};

struct Bridge
{
    std::unique_ptr<Driver> driver;
    std::shared_ptr<AeronFatalMonitor> monitor;
    std::shared_ptr<::aeron::Aeron> aeron;
    ArchiveMetrics metrics;
    Observer observer;
    Sink sink;
    Keepalive keepalive;
    std::uint64_t maxIterationUs = 0;
    ArchiveSourceSeams seams;
    std::unique_ptr<SourceSet> set;

    Bridge()
    {
        driver = std::make_unique<Driver>("/dev/shm/ct-archive-it-cpp-" + g.scenario);
        monitor = std::make_shared<AeronFatalMonitor>();
        AeronClientOptions o;
        o.aeronDir = driver->dir;
        o.clientName = "ct-archive-it";
        aeron = connectAeron(o, monitor);
    }
    ~Bridge()
    {
        if (set)
        {
            set->stop(Steady::now());
            for (int i = 0; i < 500 && !set->follower(set->current()).policy().idle(); ++i)
            {
                set->doWork(sink, 0, Steady::now());
                std::this_thread::sleep_for(1ms);
            }
            set->doWork(sink, 0, Steady::now());
        }
        set.reset();
        monitor->beginDeliberateClose();
        aeron.reset();
        driver.reset();
    }

    ArchiveSettings settings(const std::vector<std::string> &endpoints, const std::string &localHost = "127.0.0.1")
    {
        ArchiveSettings s;
        for (const auto &e : endpoints)
        {
            const auto colon = e.rfind(':');
            s.archives.push_back({ e.substr(0, colon), static_cast<std::uint16_t>(std::stoi(e.substr(colon + 1))) });
        }
        s.controlStreamId = 4010;
        s.journalStreamId = 4001;
        s.journalChannel = "aeron:ipc";
        s.replayStreamId = 9101;
        s.localHost = localHost;
        s.journalMtu = 1408;
        return s;
    }

    void create(const std::vector<std::string> &endpoints, const std::string &localHost = "127.0.0.1")
    {
        Invoker invoker = [this]() {
            keepalive.tick(); // client.doWork() in the bridge: the keepalive stays alive inside a blocked call
            std::this_thread::sleep_for(1ms);
        };
        set = std::make_unique<SourceSet>(settings(endpoints, localHost), aeron, monitor, invoker, metrics, &observer, seams);
    }

    template<typename Pred>
    bool runUntil(Pred pred, std::chrono::milliseconds timeout, int limit = 32)
    {
        const auto deadline = Steady::now() + timeout;
        int idle = 0;
        while (Steady::now() < deadline)
        {
            keepalive.tick(); // first: a gap left by a blocked call in the previous doWork is measured
            if (pred())
            {
                return true;
            }
            const auto t0 = Steady::now();
            const int work = set->doWork(sink, limit, t0);
            maxIterationUs = std::max<std::uint64_t>(maxIterationUs,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Steady::now() - t0).count()));
            if (work == 0)
            {
                if (++idle > 10)
                {
                    std::this_thread::sleep_for(1ms);
                }
                else
                {
                    std::this_thread::yield();
                }
            }
            else
            {
                idle = 0;
            }
        }
        return pred();
    }

    void stopAndIdle()
    {
        set->stop(Steady::now());
        runUntil([&] { return set->follower(set->current()).policy().idle(); }, 10s, 0);
        set->doWork(sink, 0, Steady::now());
    }

    std::vector<std::int64_t> seqs(std::size_t source = 99) const
    {
        std::vector<std::int64_t> out;
        for (const auto &f : sink.frames)
        {
            if (source == 99 || f.source == source)
            {
                out.push_back(f.seq);
            }
        }
        return out;
    }
};

std::vector<std::uint8_t> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const std::vector<std::string> FIXTURE_ORDER = { "java-config.bin", "java-trade.bin", "java-terminal.bin",
    "java-command.first.bin", "java-command.conflict.bin", "java-release.bin" };

std::string appendSix()
{
    std::string line = "APPEND";
    for (const auto &f : FIXTURE_ORDER)
    {
        line += " " + g.fixtures + "/" + f;
    }
    return line;
}

std::string toHex(const std::vector<std::uint8_t> &b)
{
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (auto c : b)
    {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

std::string nonLoopbackIpv4()
{
    ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        return {};
    }
    std::string found;
    for (ifaddrs *a = list; a; a = a->ifa_next)
    {
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET)
        {
            char text[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, text, sizeof(text));
            if (!isLoopbackHost(text))
            {
                found = text;
                break;
            }
        }
    }
    ::freeifaddrs(list);
    return found;
}

// ---- scenarios

void checkSixFixtures(const Bridge &b)
{
    require(b.sink.frames.size() == 6, "expected the six fixture frames, got " + std::to_string(b.sink.frames.size()));
    std::int64_t lastEnd = -1;
    for (std::size_t i = 0; i < 6; ++i)
    {
        const Frame &f = b.sink.frames[i];
        require(f.seq == static_cast<std::int64_t>(i + 1), "seq " + std::to_string(f.seq) + " at index " + std::to_string(i));
        require(f.bytes == readFile(g.fixtures + "/" + FIXTURE_ORDER[i]), FIXTURE_ORDER[i] + " is not byte-identical");
        require(f.end > lastEnd, "end positions must ascend");
        lastEnd = f.end;
    }
}

void emptyGenesisRecordingThenData()
{
    Fixture fx(g.portBase);
    fx.cmd("EMPTY");
    fx.cmd("RECORD");
    fx.cmd(appendSix());
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 6; }, 30s), "the data of recording 1 never arrived");
    checkSixFixtures(b);
    require(ArchiveMetrics::get(b.metrics.emptyRecordingsSkipped) == 1, "the empty genesis recording was not counted skipped");
    for (const auto &a : b.observer.attaches)
    {
        require(a.recording.key.recordingId != 0, "a replay was opened on the empty recording 0");
    }
    require(b.observer.faults.empty(), "a source fault was raised");
}

void fixturesRoundTripThroughARealArchive()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd(appendSix());
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 6; }, 30s), "the six frames never arrived");
    checkSixFixtures(b);
}

void chainFollowAcrossRecordingsAndSuccessor()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    fx.cmd("STOP");
    fx.cmd("RECORD");
    fx.cmd("SYNTH 4 3 9");
    fx.cmd("STOP");
    fx.cmd("RECORD");
    fx.cmd("SYNTH 7 3 9");
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 9; }, 30s), "the first three recordings were not followed");
    const auto stoppedAt = Steady::now();
    fx.cmd("STOP");
    fx.cmd("RECORD");
    fx.cmd("SYNTH 10 3 9");
    require(b.runUntil([&] { return b.sink.frames.size() >= 12; }, 30s), "the successor was never followed");
    require(b.observer.faults.empty(), "a source fault was counted (the clean stop must not be one)");
    std::vector<std::int64_t> expect;
    for (int i = 1; i <= 12; ++i) expect.push_back(i);
    require(b.seqs() == expect, "seqs out of order across the chain");
    require(b.observer.ended == 1, "RECORDING_ENDED was not reported once");
    const double took = std::chrono::duration<double>(b.observer.endedAt - stoppedAt).count();
    note("RECORDING_ENDED " + std::to_string(took) + " s after STOP (row 7a; the periodic probe is every 10 s)");
    require(took < 5.0, "RECORDING_ENDED came too late for row 7a");
    require(b.observer.faults.empty(), "a source fault was counted");
    require(b.metrics.totalStalls() == 0, "a stall was counted");
    require(ArchiveMetrics::get(b.metrics.recordingSuccessions) == 1, "successions != 1");
}

void chainFollowWithLateSuccessor()
{
    // Review F-1: the ME writer is down for seconds between the stop and the successor (the normal
    // restart timing). The read continues after the ended recording: each seq exactly once, one
    // replay per recording.
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "the first recording was not followed");
    fx.cmd("STOP");
    require(b.runUntil([&] { return b.observer.ended == 1; }, 10s), "RECORDING_ENDED was not reported");
    b.runUntil([] { return false; }, 3500ms); // the ME is down: chain-exhausted back-offs and re-lists
    fx.cmd("RECORD");
    fx.cmd("SYNTH 4 3 9");
    require(b.runUntil([&] { return b.sink.frames.size() >= 6 && b.sink.frames.back().seq == 6; }, 30s),
        "the late successor was never followed");
    b.runUntil([] { return false; }, 500ms);
    note("delivered " + std::to_string(b.sink.frames.size()) + " frames over " + std::to_string(b.observer.attaches.size()) +
        " replays");
    require(b.observer.attaches.size() == 2, "the chain was re-replayed while the successor was missing (" +
        std::to_string(b.observer.attaches.size()) + " replays)");
    require(b.seqs() == std::vector<std::int64_t>({ 1, 2, 3, 4, 5, 6 }), "a seq was delivered more than once");
    require(b.observer.faults.empty(), "a source fault was counted");
    require(b.observer.attaches.back().memoFree, "the continued read lost its memo-free flag");
}

void replayToEphemeralPortResolvesBeforeStartReplay()
{
    auto once = [](const std::string &localHost) {
        Fixture fx(g.portBase, 20, localHost == "127.0.0.1" ? "a" : "b");
        fx.cmd("RECORD");
        fx.cmd("SYNTH 1 3 9");
        Bridge b;
        b.create({ fx.endpoint() }, localHost);
        b.set->start(Steady::now());
        require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "no frames with local host " + localHost);
        const std::string channel = b.set->follower(0).source().lastReplayChannel();
        note("startReplay channel with local host " + localHost + ": " + channel);
        const auto colon = channel.rfind(':');
        require(channel.find("endpoint=" + localHost + ":") != std::string::npos, "the channel does not carry the local host");
        require(std::stoi(channel.substr(colon + 1)) > 0, "the channel carries port 0");
    };
    once("127.0.0.1");
    const std::string host = nonLoopbackIpv4();
    if (host.empty())
    {
        note("no non-loopback IPv4 address on this host: the second half is skipped");
    }
    else
    {
        once(host);
    }
}

void cHelperSendsPortZeroUnresolved()
{
    // Characterisation of contract 1.4: aeron_archive_replay sends the channel verbatim.
    Fixture fx(g.portBase);
    const std::string rec = fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Bridge b;
    ::aeron::archive::client::Context ctx;
    ctx.aeron(b.aeron)
        .controlRequestChannel("aeron:udp?endpoint=" + fx.endpoint())
        .controlRequestStreamId(4010)
        .controlResponseChannel("aeron:udp?endpoint=127.0.0.1:0")
        .messageTimeoutNs(5'000'000'000ULL);
    auto archive = ::aeron::archive::client::AeronArchive::connect(ctx);
    ::aeron::archive::client::ReplayParams params;
    params.position(0).length(INT64_MAX);
    const std::int64_t recordingId = std::stoll(rec.substr(rec.find(' ') + 1));
    std::string outcome;
    try
    {
        auto sub = archive->replay(recordingId, "aeron:udp?endpoint=127.0.0.1:0", 9102, params);
        int fragments = 0;
        const auto t0 = Steady::now();
        while (secondsSince(t0) < 8.0)
        {
            fragments += sub->poll([](auto &, auto, auto, auto &) {}, 10);
            const std::string error = archive->pollForErrorResponse();
            if (!error.empty())
            {
                outcome = "archive error: " + error;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        if (outcome.empty())
        {
            outcome = fragments == 0 ? "no image, no fragment in 8 s" : "fragments delivered";
        }
        require(fragments == 0, "the C helper replayed to a port-0 endpoint: the upstream fact changed");
    }
    catch (const std::exception &e)
    {
        outcome = std::string("replay threw: ") + e.what();
    }
    note("aeron_archive_replay on endpoint=127.0.0.1:0 -> " + outcome);
}

void neverConnected15s()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Bridge b;
    b.seams.advertisedHostOverride = "192.0.2.1"; // TEST-NET-1: the replay goes nowhere
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return !b.observer.attaches.empty(); }, 15s), "never attached");
    const auto attachedAt = Steady::now();
    require(b.runUntil([&] {
        return b.observer.find(FaultKind::ARCHIVE_ERROR) || b.observer.find(FaultKind::NEVER_CONNECTED);
    }, 17s), "no source fault within 15 s + 1 iteration: a silent idle");
    const auto *f = b.observer.find(FaultKind::ARCHIVE_ERROR) ? b.observer.find(FaultKind::ARCHIVE_ERROR)
                                                              : b.observer.find(FaultKind::NEVER_CONNECTED);
    note(std::string("fault ") + faultName(f->kind) + " " + std::to_string(secondsSince(attachedAt)) + " s after attach");
    require(b.sink.frames.empty(), "frames arrived from an unroutable replay");
}

void imageLostMidFollowEndsSync()
{
    auto fx = std::make_unique<Fixture>(g.portBase);
    fx->cmd("RECORD");
    fx->cmd("SYNTH 1 3 9");
    Bridge b;
    b.create({ fx->endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "no frames");
    const auto killedAt = Steady::now();
    fx->jvm->kill(SIGKILL);
    require(b.runUntil([&] {
        return b.observer.find(FaultKind::IMAGE_LOST) || b.observer.find(FaultKind::ARCHIVE_GONE);
    }, 25s), "the dead source was followed silently");
    const auto *f = b.observer.find(FaultKind::IMAGE_LOST) ? b.observer.find(FaultKind::IMAGE_LOST)
                                                           : b.observer.find(FaultKind::ARCHIVE_GONE);
    note(std::string("fault ") + faultName(f->kind) + " " + std::to_string(secondsSince(killedAt)) + " s after SIGKILL");
    fx.reset();
}

void invokerRunsWhileArchiveCallBlocks()
{
    auto fx = std::make_unique<Fixture>(g.portBase);
    fx->cmd("RECORD");
    fx->cmd("SYNTH 1 3 9");
    Bridge b;
    b.create({ fx->endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "no frames");
    b.runUntil([] { return false; }, 200ms);
    b.keepalive.reset();
    ::kill(fx->jvm->pid, SIGSTOP);
    const auto stoppedAt = Steady::now();
    b.set->follower(0).policy().forceProbe(); // before the image liveness timeout (review N8)
    require(b.runUntil([&] { return b.observer.find(FaultKind::ARCHIVE_GONE) != nullptr; }, 8s), "no ARCHIVE_GONE");
    const double took = secondsSince(stoppedAt);
    note("ARCHIVE_GONE " + std::to_string(took) + " s after SIGSTOP; keepalive max gap " +
        std::to_string(b.keepalive.maxGapMs) + " ms; archive call max " +
        std::to_string(ArchiveMetrics::get(b.metrics.archiveCallMaxMs)) + " ms");
    ::kill(fx->jvm->pid, SIGCONT);
    require(took <= 6.0, "the probe took longer than the message timeout + 1 s");
    require(b.keepalive.maxGapMs < 50, "the keepalive gap reached " + std::to_string(b.keepalive.maxGapMs) + " ms");
    fx.reset();
}

void detachStopsReplayFreeingTheSlot()
{
    Fixture fx(g.portBase, 1); // maxConcurrentReplays 1
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "no frames");
    for (int round = 0; round < 3; ++round)
    {
        const std::size_t attaches = b.observer.attaches.size();
        b.set->reattach(Steady::now()); // a rewind: the replay is detached and a new one opened
        require(b.runUntil([&] { return b.observer.attaches.size() > attaches; }, 10s),
            "re-attach " + std::to_string(round) + " did not succeed");
    }
    require(b.metrics.openFailures(OpenFailure::MAX_REPLAYS) == 0, "REPLAY_OPEN{max_replays}: a replay leaked");
}

void rejectedRememberedPositionClearsProofs()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 2 9");
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->onReport(EPOCH, 2);
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 2; }, 30s), "no frames");
    const RecordingKey key = b.set->memo().chain(0).front().key;
    // 96 is 32-aligned but inside the first 160 B trade frame (BridgeEndToEndTest.java:741 shape).
    b.set->memo().injectProofForTest(0, key, Proof{ 1, EPOCH, 96 });
    const std::size_t frames = b.sink.frames.size();
    b.set->reattach(Steady::now());
    require(b.runUntil([&] { return b.observer.find(FaultKind::POSITION_REJECTED) != nullptr; }, 15s),
        "the rejected position was not reported POSITION_REJECTED");
    require(b.metrics.openFailures(OpenFailure::INVALID_POSITION) >= 1, "no INVALID_POSITION answer counted");
    require(b.runUntil([&] { return b.sink.frames.size() >= frames + 2; }, 15s), "data lost after the rejection");
    require(b.observer.attaches.back().from == 0 && b.observer.attaches.back().memoFree, "the next resume is not at the chain start");
}

void capacityRejectionKeepsProofs()
{
    Fixture fx(g.portBase, 1);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->onReport(EPOCH, 1);
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "no frames");
    const RecordingKey key = b.set->memo().chain(0).front().key;
    require(b.set->memo().proof(0, key) && b.set->memo().proof(0, key)->endPosition == 160, "no proof at 160");
    b.stopAndIdle();
    fx.cmd("HOLD_REPLAY"); // the only replay slot
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.metrics.openFailures(OpenFailure::MAX_REPLAYS) >= 1; }, 15s), "no REPLAY_OPEN{max_replays}");
    require(b.set->memo().proof(0, key) && b.set->memo().proof(0, key)->endPosition == 160, "the capacity rejection cleared the proof");
    const std::size_t attaches = b.observer.attaches.size();
    fx.cmd("RELEASE_REPLAY");
    require(b.runUntil([&] { return b.observer.attaches.size() > attaches; }, 15s), "no attach after the slot freed");
    require(b.observer.attaches.back().fromProof && b.observer.attaches.back().from == 160, "the resume is not at the proof");
}

void perSourceMemoSurvivesFailBack()
{
    Fixture a(g.portBase, 20, "a");
    Fixture bfx(g.portBase + 1, 20, "b");
    a.cmd("RECORD");
    a.cmd("SYNTH 1 2 9");
    bfx.cmd("RECORD");
    bfx.cmd("PAD 64"); // a different byte history: every frame of B ends 96 bytes later
    bfx.cmd("SYNTH 1 2 9");
    Bridge b;
    b.create({ a.endpoint(), bfx.endpoint() });
    b.set->onReport(EPOCH, 2);
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.seqs(0).size() >= 2; }, 30s), "no frames from A");
    b.set->switchTo(1, Steady::now());
    require(b.runUntil([&] { return b.seqs(1).size() >= 2; }, 30s), "no frames from B");
    b.set->switchTo(0, Steady::now());
    require(b.runUntil([&] { return b.observer.attaches.size() >= 3; }, 30s), "no fail-back attach on A");
    std::int64_t endA = 0, endB = 0;
    for (const auto &f : b.sink.frames)
    {
        if (f.seq == 2)
        {
            (f.source == 0 ? endA : endB) = f.end;
        }
    }
    note("seq 2 ends at " + std::to_string(endA) + " on A and " + std::to_string(endB) + " on B");
    require(endA != endB && endA > 0 && endB > 0, "the end positions of seq 2 must differ between the sources");
    const AttachInfo &onB = b.observer.attaches[1];
    const AttachInfo &back = b.observer.attaches[2];
    require(onB.source == 1 && onB.from == 0 && onB.memoFree, "B did not start from its own (empty) memo");
    require(back.source == 0 && back.fromProof && back.from == endA, "fail-back did not resume A at A's proof");
    for (const auto &att : b.observer.attaches)
    {
        require(!(att.source == 1 && att.from == endA), "a byte position of A was used on B");
    }
    require(b.observer.faults.empty(), "a source fault was raised");
}

void activePurgeKeepsTheMemo()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD"); // term-length 64k, 1 MiB segments
    fx.cmd("SYNTH 1 20000 9"); // ~3.2 MB: several segments
    Bridge b;
    b.sink.keepBytes = false;
    b.create({ fx.endpoint() });
    b.set->onReport(EPOCH, 8000);
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.count >= 20000; }, 60s), "the recording was not read");
    const RecordingKey key = b.set->memo().chain(0).front().key;
    const auto proof = b.set->memo().proof(0, key);
    require(proof && proof->seq == 8000, "no proof at seq 8000");
    note("proof at seq 8000 ends at " + std::to_string(proof->endPosition));
    b.stopAndIdle();
    // a purge below the proof: the memo survives (the Java key with startPosition lost it)
    fx.cmd("PURGE 1048576");
    std::size_t attaches = b.observer.attaches.size();
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.observer.attaches.size() > attaches; }, 15s), "no attach after the first purge");
    require(b.observer.attaches.back().fromProof && b.observer.attaches.back().from == proof->endPosition,
        "the resume after a purge below the proof is not at the proof");
    require(b.metrics.openFailures(OpenFailure::INVALID_POSITION) == 0, "INVALID_POSITION after the purge below the proof");
    b.stopAndIdle();
    // a purge above the proof: the proof is trimmed, the resume is at the new start
    fx.cmd("PURGE 2097152");
    attaches = b.observer.attaches.size();
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.observer.attaches.size() > attaches; }, 15s), "no attach after the second purge");
    require(b.observer.attaches.back().from == 2097152, "the resume after a purge above the proof is not at the new start");
    require(ArchiveMetrics::get(b.metrics.memoTrims) >= 1, "the proof was not trimmed");
    require(b.observer.find(FaultKind::POSITION_REJECTED) == nullptr, "a purge became POSITION_REJECTED");
}

void storagePrecheckRefusesWithoutRescan()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Bridge b;
    bool lowSpace = false;
    b.seams.freeBytes = [&](const std::string &dir) -> std::optional<std::uint64_t> {
        if (lowSpace)
        {
            return 3ULL * 65536; // below 3 terms + 16 MiB
        }
        struct statvfs fs{};
        ::statvfs(dir.c_str(), &fs);
        return static_cast<std::uint64_t>(fs.f_bavail) * fs.f_frsize;
    };
    b.create({ fx.endpoint() });
    b.set->onReport(EPOCH, 3);
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.sink.frames.size() >= 3; }, 30s), "no frames");
    const RecordingKey key = b.set->memo().chain(0).front().key;
    b.stopAndIdle();
    lowSpace = true;
    const std::size_t attaches = b.observer.attaches.size();
    const std::string channelBefore = b.set->follower(0).source().lastReplayChannel();
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.observer.find(FaultKind::REPLAY_STORAGE) != nullptr; }, 15s), "no REPLAY_STORAGE");
    b.runUntil([] { return false; }, 2500ms);
    require(b.observer.attaches.size() == attaches, "a replay was requested although the storage precheck failed");
    require(b.metrics.openFailures(OpenFailure::STORAGE) >= 1, "replay_open_failures{storage} not counted");
    require(b.set->memo().proof(0, key).has_value(), "the proofs were cleared");
    lowSpace = false;
    require(b.runUntil([&] { return b.observer.attaches.size() > attaches; }, 10s), "no attach once space returned");
    require(b.observer.attaches.back().fromProof, "the retry rescanned from the chain start");
    (void)channelBefore;
}

void journalMtuMismatchRefusesSource()
{
    Fixture a(g.portBase, 20, "a");
    Fixture bfx(g.portBase + 1, 20, "b");
    a.cmd("RECORD aeron:ipc?term-length=64k|mtu=4096");
    a.cmd("SYNTH 1 2 9");
    bfx.cmd("RECORD aeron:ipc?term-length=64k|mtu=4096");
    bfx.cmd("SYNTH 1 2 9");
    Bridge b;
    b.create({ a.endpoint(), bfx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.observer.refusedSources >= 1; }, 15s), "source A was not refused for its MTU");
    require(!b.set->allRefusedForMtu(), "C2 before every source was seen");
    b.set->switchTo(1, Steady::now());
    require(b.runUntil([&] { return b.set->allRefusedForMtu(); }, 15s), "the C2 verdict (every source refused) was not reached");
    require(b.sink.frames.empty(), "a refused source was followed");
    note("both sources refused with mtu 4096 != BRIDGE_JOURNAL_MTU 1408: section 7 row C2 verdict");
}

void seqRegressionWithinRecordingFaults()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 6 9");
    fx.cmd("SYNTH 5 1 9");
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.observer.find(FaultKind::SEQ_REGRESSION_IN_RECORDING) != nullptr; }, 15s),
        "the regression was delivered silently");
    require(b.seqs() == std::vector<std::int64_t>({ 1, 2, 3, 4, 5, 6 }), "the regressed frame reached the sink");
}

void zeroSeqRecordIsRefusedLoudly()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 0 3 0"); // a behaviour-1 shape: 0/0 head
    Bridge b;
    b.create({ fx.endpoint() });
    b.set->start(Steady::now());
    require(b.runUntil([&] { return b.observer.find(FaultKind::SCHEMA_REFUSED) != nullptr; }, 15s), "no SCHEMA_REFUSED");
    require(b.observer.find(FaultKind::SCHEMA_REFUSED)->refusal == Refusal::HEAD, "not refused for the head");
    require(b.sink.frames.empty(), "a 0/0 frame reached the sink");
    require(b.metrics.refusals(Refusal::HEAD) >= 1, "bridge_frames_refused_total{head} not counted");
}

void appliedHeadKeepsDutyBounded()
{
    const char *env = std::getenv("CT_IT_APPLIED_HEAD");
    const std::int64_t n = env ? std::atoll(env) : 100000;
    Fixture fx(g.portBase);
    fx.cmd("RECORD aeron:ipc?term-length=1m");
    for (std::int64_t from = 1; from <= n; from += 50000)
    {
        fx.cmd("SYNTH " + std::to_string(from) + " " + std::to_string(std::min<std::int64_t>(50000, n - from + 1)) + " 9", 600s);
    }
    Bridge b;
    b.sink.keepBytes = false;
    b.create({ fx.endpoint() });
    b.set->onReport(EPOCH, n); // every record already applied: the cold walk skips them all
    b.set->start(Steady::now());
    require(b.runUntil([&] { return !b.observer.attaches.empty(); }, 15s), "never attached");
    b.keepalive.reset();
    b.maxIterationUs = 0;
    const auto t0 = Steady::now();
    require(b.runUntil([&] { return static_cast<std::int64_t>(b.sink.count) >= n; }, 600s), "the applied head was not walked");
    note("cold walk of " + std::to_string(n) + " applied records in " + std::to_string(secondsSince(t0)) +
        " s; max iteration " + std::to_string(b.maxIterationUs) + " us; keepalive max gap " +
        std::to_string(b.keepalive.maxGapMs) + " ms");
    require(b.maxIterationUs < 50000, "an iteration took 50 ms or more");
    require(b.keepalive.maxGapMs < 100, "the keepalive gap reached 100 ms");
    const RecordingKey key = b.set->memo().chain(0).front().key;
    const auto proof = b.set->memo().proof(0, key);
    require(proof && proof->seq == n, "no proof at the head's end");
    const std::size_t attaches = b.observer.attaches.size();
    b.set->reattach(Steady::now());
    require(b.runUntil([&] { return b.observer.attaches.size() > attaches; }, 15s), "no second attach");
    require(b.observer.attaches.back().fromProof && b.observer.attaches.back().from == proof->endPosition,
        "the second resume rescans instead of starting at the proof");
}

std::string runTail(const std::string &binary, const std::string &driverDir, const std::string &endpoint,
    const std::map<std::string, std::string> &env, int &status)
{
    const std::string out = g.work + "/tail.out";
    auto child = spawn({ binary, "--aeron-dir", driverDir, "--archive", endpoint, "--control-stream", "4010",
                           "--journal-stream", "4001", "--channel", "aeron:ipc", "--replay-stream", "9101",
                           "--local-host", "127.0.0.1", "--journal-mtu", "1408", "--from-seq", "2", "--to-seq", "4",
                           "--hex", "--timeout-ms", "30000" },
        env, g.work + "/tail.err", false, out);
    status = waitExit(child->pid, 40000ms);
    child->pid = -1;
    std::ifstream in(out);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void journalTailPrintsSeqRangeAndRefusedFrames()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    std::vector<std::uint8_t> schema3 = readFile(g.fixtures + "/java-trade.bin");
    schema3[4] = 3; // schemaId 3: refused (SCHEMA), no peek
    fx.cmd("HEX " + toHex(schema3));
    fx.cmd("SYNTH 4 3 9");
    Driver driver("/dev/shm/ct-archive-it-tail-" + g.scenario);
    int status = 0;
    const std::string out = runTail(g.tail, driver.dir, fx.endpoint(), {}, status);
    std::printf("%s", out.c_str());
    require(status == 0, "ct-journal-tail exited " + std::to_string(status));
    std::vector<std::string> lines;
    std::istringstream s(out);
    for (std::string line; std::getline(s, line);)
    {
        lines.push_back(line);
    }
    require(lines.size() == 4, "expected 4 lines (seq 2, 3, the refused frame, seq 4), got " + std::to_string(lines.size()));
    require(lines[0].rfind("FRAME seq=2 ", 0) == 0, "line 1: " + lines[0]);
    require(lines[1].rfind("FRAME seq=3 ", 0) == 0, "line 2: " + lines[1]);
    require(lines[2].rfind("REFUSED schema ", 0) == 0, "line 3: " + lines[2]);
    require(lines[2].find(toHex(schema3)) != std::string::npos, "the refused frame's bytes are not printed");
    require(lines[3].rfind("FRAME seq=4 ", 0) == 0, "line 4 (the tail continued after the refusal): " + lines[3]);
    require(lines[0].find(" hex=") != std::string::npos, "--hex was not honoured");
}

void journalTailExitsSeventyOnUncaughtException()
{
    Fixture fx(g.portBase);
    fx.cmd("RECORD");
    fx.cmd("SYNTH 1 3 9");
    Driver driver("/dev/shm/ct-archive-it-tail-" + g.scenario);
    int status = 0;
    runTail(g.tailSeamed, driver.dir, fx.endpoint(), { { "CT_TEST_THROW_IN_SINK", "1" } }, status);
    note("ct-journal-tail with an exception thrown in the sink exited " + std::to_string(status));
    require(status == 70, "exit status " + std::to_string(status) + ", expected 70 (134 = SIGABRT)");
}

const std::map<std::string, std::function<void()>> SCENARIOS = {
    { "emptyGenesisRecordingThenData", emptyGenesisRecordingThenData },
    { "fixturesRoundTripThroughARealArchive", fixturesRoundTripThroughARealArchive },
    { "chainFollowAcrossRecordingsAndSuccessor", chainFollowAcrossRecordingsAndSuccessor },
    { "chainFollowWithLateSuccessor", chainFollowWithLateSuccessor },
    { "replayToEphemeralPortResolvesBeforeStartReplay", replayToEphemeralPortResolvesBeforeStartReplay },
    { "cHelperSendsPortZeroUnresolved", cHelperSendsPortZeroUnresolved },
    { "neverConnected15s", neverConnected15s },
    { "imageLostMidFollowEndsSync", imageLostMidFollowEndsSync },
    { "invokerRunsWhileArchiveCallBlocks", invokerRunsWhileArchiveCallBlocks },
    { "detachStopsReplayFreeingTheSlot", detachStopsReplayFreeingTheSlot },
    { "rejectedRememberedPositionClearsProofs", rejectedRememberedPositionClearsProofs },
    { "capacityRejectionKeepsProofs", capacityRejectionKeepsProofs },
    { "perSourceMemoSurvivesFailBack", perSourceMemoSurvivesFailBack },
    { "activePurgeKeepsTheMemo", activePurgeKeepsTheMemo },
    { "storagePrecheckRefusesWithoutRescan", storagePrecheckRefusesWithoutRescan },
    { "journalMtuMismatchRefusesSource", journalMtuMismatchRefusesSource },
    { "seqRegressionWithinRecordingFaults", seqRegressionWithinRecordingFaults },
    { "zeroSeqRecordIsRefusedLoudly", zeroSeqRecordIsRefusedLoudly },
    { "appliedHeadKeepsDutyBounded", appliedHeadKeepsDutyBounded },
    { "journalTailPrintsSeqRangeAndRefusedFrames", journalTailPrintsSeqRangeAndRefusedFrames },
    { "journalTailExitsSeventyOnUncaughtException", journalTailExitsSeventyOnUncaughtException },
};

}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--list")
    {
        for (const auto &[name, fn] : SCENARIOS)
        {
            std::printf("%s\n", name.c_str());
        }
        return 0;
    }
    if (argc != 10)
    {
        std::fprintf(stderr, "usage: %s <scenario> <work-dir> <aeronmd> <aeron-all.jar> <fixture-classes> <fixture-dir> "
                             "<port-base> <ct-journal-tail> <ct-journal-tail-seamed> | --list\n", argv[0]);
        return 64;
    }
    g.scenario = argv[1];
    g.work = argv[2];
    g.aeronmd = argv[3];
    g.jar = argv[4];
    g.classes = argv[5];
    g.fixtures = argv[6];
    g.portBase = std::atoi(argv[7]);
    g.tail = argv[8];
    g.tailSeamed = argv[9];
    ::signal(SIGPIPE, SIG_IGN);
    const auto it = SCENARIOS.find(g.scenario);
    if (it == SCENARIOS.end())
    {
        std::fprintf(stderr, "unknown scenario %s\n", g.scenario.c_str());
        return 64;
    }
    std::printf("=== %s\n", g.scenario.c_str());
    std::fflush(stdout);
    try
    {
        it->second();
        std::printf("SCENARIO PASS %s\n", g.scenario.c_str());
        return 0;
    }
    catch (const std::exception &e)
    {
        std::printf("SCENARIO FAIL %s: %s\n", g.scenario.c_str(), e.what());
        return 1;
    }
}
