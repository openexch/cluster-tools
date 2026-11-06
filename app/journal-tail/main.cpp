// ct-journal-tail: reads the ME journal (schema 4) from the configured archives through archive/
// end to end, without a core session (RC2 S9a contract section 0 item 4). It is the integration
// test driver of the archive side and the "bridge tooling" the S7 5.6 runbook needs to read journal
// entry s. It reads only: no file is written, no core is contacted.
//
// Every frame admitted by the 4.4 gate and inside [--from-seq, --to-seq] is printed as
//   FRAME seq=<s> template=<t> epoch=<e> egress=<g> recording=<id> end=<position> length=<n> [hex=<bytes>]
// and every refused frame as
//   REFUSED <reason> <position> <length> <hex>
// after which the tail continues with the next frame. A refused frame with a peek (a HEAD refusal)
// is selected by its seq; one without a peek (FRAGMENTED, LENGTH, SCHEMA, ...) by its position: it
// is printed when it sits after the frame of seq from-seq - 1 and before the frame of to-seq + 1.
// Attaches and source faults go to stderr. Exit: 0 after --to-seq (or on SIGTERM/SIGINT), 75 when
// --timeout-ms passes first, 78 on a usage or settings error, 70 on any fatal error (an Aeron
// fatal, an uncaught exception, std::terminate), never 134.
#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/archive/TestSeams.h"
#include "cluster_tools/archive/aeron/SourceSet.h"
#include "cluster_tools/client/aeron/FatalErrors.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace cluster_tools::archive;
using namespace cluster_tools::archive::aeron_archive;
using cluster_tools::client::aeron_transport::AeronClientOptions;
using cluster_tools::client::aeron_transport::AeronFatalMonitor;
using cluster_tools::client::aeron_transport::connectAeron;

constexpr int EXIT_FATAL = 70;
constexpr int EXIT_TIMEOUT = 75;
constexpr int EXIT_CONFIG = 78;

volatile std::sig_atomic_t g_stop = 0;

[[noreturn]] void onTerminate() noexcept
{
    std::fputs("ct-journal-tail: std::terminate (an exception escaped a noexcept boundary), exiting 70\n", stderr);
    std::fflush(stderr);
    std::_Exit(EXIT_FATAL);
}

void onSignal(int)
{
    g_stop = 1;
}

struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

const char *USAGE =
    "usage: ct-journal-tail --aeron-dir <dir> --archive <host:port>[,<host:port>...] --control-stream <id>\n"
    "         --journal-stream <id> --channel <aeron:...> --replay-stream <id> --local-host <host> --journal-mtu <n>\n"
    "         [--from-seq <s>] [--to-seq <s>] [--hex] [--cursor <C> --epoch <E>] [--timeout-ms <n>]\n";

std::int64_t parseCount(const std::string &flag, const std::string &text, std::int64_t min)
{
    if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != std::string::npos)
    {
        throw UsageError(flag + " must be a decimal integer, got '" + text + "'");
    }
    const std::int64_t value = std::stoll(text);
    if (value < min)
    {
        throw UsageError(flag + " must be >= " + std::to_string(min));
    }
    return value;
}

std::string hex(const std::uint8_t *bytes, std::size_t length)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i)
    {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

struct Options
{
    std::map<std::string, std::string> settings; // the BRIDGE_* keys
    std::string aeronDir;
    std::int64_t fromSeq = 1;
    std::optional<std::int64_t> toSeq;
    bool hex = false;
    std::optional<std::int64_t> cursor;
    std::optional<std::int64_t> epoch;
    std::optional<std::int64_t> timeoutMs;
};

Options parseArgs(int argc, char **argv)
{
    static const std::map<std::string, std::string> toKey = {
        { "--archive", "BRIDGE_ME_JOURNAL_ARCHIVES" },
        { "--control-stream", "BRIDGE_JOURNAL_CONTROL_STREAM" },
        { "--journal-stream", "BRIDGE_JOURNAL_STREAM" },
        { "--channel", "BRIDGE_JOURNAL_CHANNEL" },
        { "--replay-stream", "BRIDGE_REPLAY_STREAM" },
        { "--local-host", "BRIDGE_LOCAL_HOST" },
        { "--journal-mtu", "BRIDGE_JOURNAL_MTU" },
    };
    Options o;
    std::map<std::string, bool> seen;
    for (int i = 1; i < argc; ++i)
    {
        const std::string flag = argv[i];
        if (seen[flag])
        {
            throw UsageError(flag + " given twice");
        }
        seen[flag] = true;
        if (flag == "--hex")
        {
            o.hex = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            throw UsageError(flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (auto key = toKey.find(flag); key != toKey.end())
        {
            o.settings[key->second] = value;
        }
        else if (flag == "--aeron-dir")
        {
            o.aeronDir = value;
        }
        else if (flag == "--from-seq")
        {
            o.fromSeq = parseCount(flag, value, 1);
        }
        else if (flag == "--to-seq")
        {
            o.toSeq = parseCount(flag, value, 1);
        }
        else if (flag == "--cursor")
        {
            o.cursor = parseCount(flag, value, 0);
        }
        else if (flag == "--epoch")
        {
            o.epoch = parseCount(flag, value, 1);
        }
        else if (flag == "--timeout-ms")
        {
            o.timeoutMs = parseCount(flag, value, 1);
        }
        else
        {
            throw UsageError("unknown flag " + flag);
        }
    }
    if (o.aeronDir.empty())
    {
        throw UsageError("--aeron-dir is required");
    }
    if (o.cursor.has_value() != o.epoch.has_value())
    {
        throw UsageError("--cursor and --epoch go together (a FeedPositionReport)");
    }
    if (o.toSeq && *o.toSeq < o.fromSeq)
    {
        throw UsageError("--to-seq is below --from-seq");
    }
    return o;
}

class TailSink : public FrameSink
{
public:
    explicit TailSink(const Options &options) : m_options(options) {}

    SinkAction onFrame(const JournalFrame &frame) noexcept override
    {
#ifdef CT_TEST_SEAMS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wterminate"
        if (std::getenv("CT_TEST_THROW_IN_SINK"))
        {
            throw std::runtime_error("injected sink exception"); // escapes a noexcept function: std::terminate
        }
#pragma GCC diagnostic pop
#endif
        m_lastSeq = frame.peek.journalSeq;
        if (frame.peek.journalSeq < m_options.fromSeq || (m_options.toSeq && frame.peek.journalSeq > *m_options.toSeq))
        {
            return SinkAction::CONTINUE;
        }
        std::printf("FRAME seq=%lld template=%u epoch=%lld egress=%lld recording=%lld end=%lld length=%zu",
            static_cast<long long>(frame.peek.journalSeq), static_cast<unsigned>(frame.peek.templateId),
            static_cast<long long>(frame.peek.meEpoch), static_cast<long long>(frame.peek.egressSeq),
            static_cast<long long>(frame.key.recordingId), static_cast<long long>(frame.endPosition), frame.length);
        if (m_options.hex)
        {
            std::printf(" hex=%s", hex(frame.bytes, frame.length).c_str());
        }
        std::printf("\n");
        std::fflush(stdout);
        if (m_options.toSeq && frame.peek.journalSeq == *m_options.toSeq)
        {
            m_done = true;
            return SinkAction::BREAK;
        }
        return SinkAction::CONTINUE;
    }

    RefusalAction onRefused(const RefusedFrame &frame) noexcept override
    {
        bool selected;
        if (frame.hasPeek)
        {
            selected = frame.peek.journalSeq >= m_options.fromSeq &&
                (!m_options.toSeq || frame.peek.journalSeq <= *m_options.toSeq);
        }
        else
        {
            // no seq: selected by position, between the frame of from-seq - 1 and the frame of to-seq + 1
            selected = m_lastSeq >= m_options.fromSeq - 1 && (!m_options.toSeq || m_lastSeq < *m_options.toSeq);
        }
        if (selected)
        {
            std::printf("REFUSED %s %lld %zu %s\n", refusalLabel(frame.reason), static_cast<long long>(frame.endPosition),
                frame.length, hex(frame.bytes, frame.length).c_str());
            std::fflush(stdout);
        }
        return RefusalAction::SKIP_AND_CONTINUE; // the tail prints and goes on (section 0 item 4)
    }

    bool done() const noexcept { return m_done; }

private:
    const Options &m_options;
    std::int64_t m_lastSeq = 0;
    bool m_done = false;
};

class TailObserver : public FollowObserver
{
public:
    void onFault(const SourceFault &f) noexcept override
    {
        std::fprintf(stderr, "FAULT %s source=%zu recording=%lld consumed=%lld %s\n", faultName(f.kind), f.source,
            static_cast<long long>(f.key.recordingId), static_cast<long long>(f.consumed), f.detail.c_str());
    }
    void onAttached(const AttachInfo &a) noexcept override
    {
        std::fprintf(stderr, "ATTACH source=%zu recording=%lld from=%lld proof=%d memoFree=%d\n", a.source,
            static_cast<long long>(a.recording.key.recordingId), static_cast<long long>(a.from), a.fromProof ? 1 : 0,
            a.memoFree ? 1 : 0);
    }
    void onRecordingEnded(std::size_t source, const Recording &r) noexcept override
    {
        std::fprintf(stderr, "ENDED source=%zu recording=%lld stop=%lld\n", source, static_cast<long long>(r.key.recordingId),
            static_cast<long long>(r.stopPosition));
    }
    void onSourceRefused(std::size_t source, const Recording &r) noexcept override
    {
        std::fprintf(stderr, "REFUSED_SOURCE source=%zu recording=%lld mtu=%d\n", source,
            static_cast<long long>(r.key.recordingId), r.key.mtuLength);
    }
};

int run(int argc, char **argv)
{
    Options options;
    std::optional<ArchiveSettings> settings;
    try
    {
        options = parseArgs(argc, argv);
        const auto parsed = parseArchiveSettings(
            [&](const std::string &key) -> std::optional<std::string> {
                auto it = options.settings.find(key);
                if (it == options.settings.end())
                {
                    return std::nullopt;
                }
                return it->second;
            },
            SettingsScope::TOOL);
        if (!parsed.ok())
        {
            for (const auto &e : parsed.errors)
            {
                std::fprintf(stderr, "ct-journal-tail: %s\n", e.c_str());
            }
            std::fputs(USAGE, stderr);
            return EXIT_CONFIG;
        }
        settings = *parsed.settings;
    }
    catch (const UsageError &e)
    {
        std::fprintf(stderr, "ct-journal-tail: %s\n%s", e.what(), USAGE);
        return EXIT_CONFIG;
    }

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    auto monitor = std::make_shared<AeronFatalMonitor>(); // exit 70 from the conductor on a fatal error
    AeronClientOptions clientOptions;
    clientOptions.aeronDir = options.aeronDir;
    clientOptions.clientName = "ct-journal-tail";
    auto aeron = connectAeron(clientOptions, monitor);

    ArchiveMetrics metrics;
    TailObserver observer;
    TailSink sink(options);
    // The invoker: the only park on the archive wait path (3.3). The tail has no core session, so
    // there is no client.doWork() to run; it parks 1 ms and records the gap.
    auto lastInvoke = std::chrono::steady_clock::now();
    Invoker invoker = [&]() {
        const auto now = std::chrono::steady_clock::now();
        ArchiveMetrics::raiseMax(metrics.invokerMaxGapMs,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastInvoke).count()));
        lastInvoke = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    SourceSet sources(*settings, aeron, monitor, invoker, metrics, &observer);
    if (options.cursor)
    {
        sources.onReport(*options.epoch, *options.cursor); // proofs from skips at or below C (5.2 rule 1)
    }
    const auto started = std::chrono::steady_clock::now();
    sources.start(started);
    int idle = 0;
    while (!g_stop && !sink.done())
    {
        const auto now = std::chrono::steady_clock::now();
        lastInvoke = now;
        const int work = sources.doWork(sink, 32, now);
        if (options.timeoutMs && now - started > std::chrono::milliseconds(*options.timeoutMs))
        {
            std::fprintf(stderr, "ct-journal-tail: --to-seq not reached within %lld ms\n",
                static_cast<long long>(*options.timeoutMs));
            monitor->beginDeliberateClose();
            return EXIT_TIMEOUT;
        }
        if (work > 0)
        {
            idle = 0;
            continue;
        }
        // idle backoff: spin 0, yield 10, then park up to 1 ms (no busy spin on a shared box)
        if (++idle <= 10)
        {
            std::this_thread::yield();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto stopAt = std::chrono::steady_clock::now();
    sources.stop(stopAt);
    for (int i = 0; i < 2000 && !sources.follower(sources.current()).policy().idle(); ++i)
    {
        sources.doWork(sink, 0, std::chrono::steady_clock::now());
    }
    sources.doWork(sink, 0, std::chrono::steady_clock::now());
    monitor->beginDeliberateClose();
    std::fprintf(stderr, "ct-journal-tail: done (archive call max %llu ms, invoker max gap %llu ms)\n",
        static_cast<unsigned long long>(ArchiveMetrics::takeMax(metrics.archiveCallMaxMs)),
        static_cast<unsigned long long>(ArchiveMetrics::takeMax(metrics.invokerMaxGapMs)));
    return 0;
}

}

int main(int argc, char **argv)
{
    if (!cluster_tools::archive::test_seams::disabled(cluster_tools::archive::test_seams::Rule::TAIL_TERMINATE_HANDLER))
    {
        std::set_terminate(&onTerminate); // first: nothing ends the process with SIGABRT (contract 3.1)
    }
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "ct-journal-tail: fatal: %s, exiting 70\n", e.what());
    }
    catch (...)
    {
        std::fputs("ct-journal-tail: fatal: unknown exception, exiting 70\n", stderr);
    }
    std::fflush(stderr);
    std::_Exit(EXIT_FATAL);
}
