// Links archive_aeron of the installed package and tails a core order journal from a real archive: an embedded
// media driver in <parent-dir>, an Aeron client, a SourceSet on the core order journal profile with the expected
// epoch, the timed re-list and archive ids in the keys, reading until <frames> records of that epoch arrived in
// order. Exit 0 and "archive tail consumer OK" = pass.
//
// usage: archive_tail_consumer <parent-dir> <archive host:port> <journal stream> <frames> <epoch>
#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/archive/aeron/SourceSet.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/runtime/EmbeddedDriver.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ar = cluster_tools::archive;
namespace aa = cluster_tools::archive::aeron_archive;
namespace at = cluster_tools::client::aeron_transport;
namespace rt = cluster_tools::runtime;

namespace
{

struct Sink : ar::FrameSink
{
    std::vector<std::int64_t> seqs;
    std::int64_t epoch = 0;
    std::int64_t archiveId = ar::ARCHIVE_ID_UNKNOWN;
    bool wrongEpoch = false;

    ar::SinkAction onFrame(const ar::JournalFrame &f) noexcept override
    {
        seqs.push_back(f.peek.journalSeq);
        wrongEpoch = wrongEpoch || f.peek.epoch() != epoch;
        archiveId = f.key.archiveId;
        std::printf("FRAME seq=%lld epoch=%lld log=%lld recording=%lld archive=%lld\n",
            static_cast<long long>(f.peek.journalSeq), static_cast<long long>(f.peek.epoch()),
            static_cast<long long>(f.peek.position()), static_cast<long long>(f.key.recordingId),
            static_cast<long long>(f.key.archiveId));
        return ar::SinkAction::CONTINUE;
    }

    ar::RefusalAction onRefused(const ar::RefusedFrame &f) noexcept override
    {
        std::fprintf(stderr, "archive tail consumer: refused %s\n", ar::refusalLabel(f.reason));
        return ar::RefusalAction::CLOSE_REPLAY;
    }
};

}

int main(int argc, char **argv)
{
    if (argc != 6)
    {
        std::fprintf(stderr, "usage: archive_tail_consumer <parent-dir> <archive host:port> <journal stream> <frames> <epoch>\n");
        return 64;
    }
    const std::size_t want = static_cast<std::size_t>(std::atoll(argv[4]));
    const std::int64_t epoch = std::atoll(argv[5]);

    rt::EmbeddedDriverOptions options;
    options.processName = "package-archive-consumer";
    options.dirPrefix = "package-archive-consumer-";
    options.instanceKey = "CONSUMER_INSTANCE_ID";
    const std::string dir = rt::EmbeddedDriver::dirFor(argv[1], 1, options);
    auto driver = std::make_unique<rt::EmbeddedDriver>(options,
        [](const std::string &why) { std::fprintf(stderr, "archive tail consumer: driver terminated: %s\n", why.c_str()); });
    std::string error;
    if (!driver->start(dir, false, error))
    {
        std::fprintf(stderr, "archive tail consumer: FAILED the embedded driver did not start: %s\n", error.c_str());
        return 1;
    }
    auto monitor = std::make_shared<at::AeronFatalMonitor>();
    at::AeronClientOptions clientOptions;
    clientOptions.aeronDir = dir;
    clientOptions.clientName = "package-archive-consumer";
    clientOptions.driverTimeoutMs = 10'000;
    std::shared_ptr<aeron::Aeron> aeron = at::connectAeron(clientOptions, monitor);

    const std::map<std::string, std::string> env = {
        { "CONSUMER_JOURNAL_ARCHIVES", argv[2] }, { "CONSUMER_JOURNAL_CONTROL_STREAM", "4010" },
        { "CONSUMER_JOURNAL_STREAM", argv[3] }, { "CONSUMER_JOURNAL_CHANNEL", "aeron:ipc" },
        { "CONSUMER_REPLAY_STREAM", "9201" }, { "CONSUMER_LOCAL_HOST", "127.0.0.1" }, { "CONSUMER_JOURNAL_MTU", "1408" },
    };
    const auto parsed = ar::parseArchiveSettings(
        [&](const std::string &k) -> std::optional<std::string> {
            const auto it = env.find(k);
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        },
        ar::SettingsScope::TOOL, ar::ArchiveSettingsKeys::withPrefix("CONSUMER_", ar::CORE_ORDER_JOURNAL));
    if (!parsed.ok())
    {
        std::fprintf(stderr, "archive tail consumer: FAILED settings: %s\n", parsed.errors.front().c_str());
        return 1;
    }

    ar::FollowSettings follow;
    follow.profile = ar::CORE_ORDER_JOURNAL;
    follow.expectedEpoch = epoch;
    follow.relistInterval = std::chrono::milliseconds(1000);
    follow.keyByArchiveId = true;
    ar::ArchiveMetrics metrics;
    Sink sink;
    sink.epoch = epoch;
    int failures = 0;
    {
        aa::SourceSet set(*parsed.settings, aeron, monitor, [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); },
            metrics, nullptr, {}, follow);
        set.start(std::chrono::steady_clock::now());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (sink.seqs.size() < want && std::chrono::steady_clock::now() < deadline)
        {
            if (0 == set.doWork(sink, 32, std::chrono::steady_clock::now()))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        set.stop(std::chrono::steady_clock::now());
        for (int i = 0; i < 2000 && !set.follower(set.current()).policy().idle(); ++i)
        {
            set.doWork(sink, 0, std::chrono::steady_clock::now());
        }
        for (std::size_t i = 0; i < sink.seqs.size(); ++i)
        {
            if (sink.seqs[i] != static_cast<std::int64_t>(i) + 1)
            {
                std::fprintf(stderr, "archive tail consumer: FAILED frame %zu carries seq %lld\n", i,
                    static_cast<long long>(sink.seqs[i]));
                ++failures;
                break;
            }
        }
        if (sink.seqs.size() != want)
        {
            std::fprintf(stderr, "archive tail consumer: FAILED %zu of %zu frames in 30 s\n", sink.seqs.size(), want);
            ++failures;
        }
        if (sink.wrongEpoch)
        {
            std::fprintf(stderr, "archive tail consumer: FAILED a frame of another epoch was delivered\n");
            ++failures;
        }
        if (ar::ARCHIVE_ID_UNKNOWN == sink.archiveId)
        {
            std::fprintf(stderr, "archive tail consumer: FAILED the recording key carries no archive id\n");
            ++failures;
        }
    }
    monitor->beginDeliberateClose();
    aeron.reset();
    if (!driver->close(5'000))
    {
        std::fprintf(stderr, "archive tail consumer: FAILED the embedded driver did not close\n");
        ++failures;
    }
    if (0 != failures)
    {
        return 1;
    }
    std::printf("archive tail consumer OK frames=%zu archive=%lld foreign=%llu\n", sink.seqs.size(),
        static_cast<long long>(sink.archiveId),
        static_cast<unsigned long long>(ar::ArchiveMetrics::get(metrics.foreignEpochRecordings)));
    return 0;
}
