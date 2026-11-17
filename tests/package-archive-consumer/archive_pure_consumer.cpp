// Links archive_core of the installed package: the core order journal profile admits its own frame and refuses an
// ME journal frame, the prefixed settings parse, and a seeded proof waits for a listing. Exit 0 and
// "archive pure consumer OK" = pass.
//
// usage: archive_pure_consumer                         the checks
//        archive_pure_consumer --core-frame-hex <seq> <epoch>   prints one core order journal frame as hex
#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/archive/FramePeek.h"
#include "cluster_tools/archive/ResumeMemo.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ar = cluster_tools::archive;

namespace
{

int failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "archive pure consumer: FAILED %s\n", what);
        failures++;
    }
}

void put(std::vector<std::uint8_t> &b, std::size_t at, std::uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::vector<std::uint8_t> coreFrame(std::int64_t seq, std::int64_t epoch)
{
    std::vector<std::uint8_t> b(ar::CORE_ORDER_FRAME_LENGTH, 0);
    put(b, 0, ar::CORE_ORDER_BLOCK_EVENT, 2);
    put(b, 2, ar::CORE_ORDER_TEMPLATE_EVENT, 2);
    put(b, 4, ar::CORE_ORDER_JOURNAL_SCHEMA_ID, 2);
    put(b, 6, ar::CORE_ORDER_JOURNAL_SCHEMA_VERSION, 2);
    put(b, ar::FRAME_OFFSET_JOURNAL_SEQ, static_cast<std::uint64_t>(seq), 8);
    put(b, ar::FRAME_OFFSET_ME_EPOCH, static_cast<std::uint64_t>(epoch), 8);
    put(b, ar::FRAME_OFFSET_EGRESS_SEQ, static_cast<std::uint64_t>(seq * 64), 8);
    return b;
}

}

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--core-frame-hex")
    {
        for (const std::uint8_t byte : coreFrame(std::atoll(argv[2]), std::atoll(argv[3])))
        {
            std::printf("%02x", byte);
        }
        std::printf("\n");
        return 0;
    }
    const auto frame = coreFrame(7, 3);
    const ar::Admission ok = ar::admitFrame(ar::CORE_ORDER_JOURNAL, ar::FRAME_FLAGS_UNFRAGMENTED, frame.data(), frame.size());
    expect(ok.accepted() && ok.peek.journalSeq == 7 && ok.peek.epoch() == 3, "the core profile admits a core frame");
    expect(ar::admitFrame(ar::FRAME_FLAGS_UNFRAGMENTED, frame.data(), frame.size()).refusal == ar::Refusal::SCHEMA,
        "the ME gate refuses a core frame");

    const std::map<std::string, std::string> env = {
        { "CONSUMER_JOURNAL_ARCHIVES", "10.0.0.1:8010" }, { "CONSUMER_JOURNAL_CONTROL_STREAM", "10" },
        { "CONSUMER_JOURNAL_STREAM", "4102" }, { "CONSUMER_JOURNAL_CHANNEL", "aeron:ipc" },
        { "CONSUMER_REPLAY_STREAM", "4103" }, { "CONSUMER_LOCAL_HOST", "10.0.0.9" }, { "CONSUMER_JOURNAL_MTU", "256" },
    };
    const auto parsed = ar::parseArchiveSettings(
        [&](const std::string &k) -> std::optional<std::string> {
            const auto it = env.find(k);
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        },
        ar::SettingsScope::TOOL, ar::ArchiveSettingsKeys::withPrefix("CONSUMER_", ar::CORE_ORDER_JOURNAL));
    expect(parsed.ok() && parsed.settings->journalStreamId == 4102, "prefixed settings parse");

    ar::ResumeMemo memo(1);
    memo.onReport(3, 10);
    ar::RecordingKey key;
    key.recordingId = 1;
    expect(memo.seed(0, key, 5, 3, 512) && memo.pendingSeeds(0) == 1, "a seed waits for the listing");

    if (0 != failures)
    {
        return 1;
    }
    std::printf("archive pure consumer OK\n");
    return 0;
}
