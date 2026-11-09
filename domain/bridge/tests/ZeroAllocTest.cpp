// ZeroAllocTest ("allocates nothing after construction"; the forwarder's pattern): a
// counted-allocation window over the domain's steady-state duty calls, driven directly (no follower): staged trades,
// 61/62 flushes, singles, the empty-poll flush, periodic queries and reports that pop and prove the ring, back-pressure
// and its retry, ticks. The global operator new counts while the window is open; the first-touch work (the seams' env
// read, the memo's proof slots for a new recording key) happens in a warm-up before it opens.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

#include <atomic>
#include <cstdlib>
#include <new>

// the replaced global new/delete pair malloc with free; GCC's -Wmismatched-new-delete cannot see that across inlining
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {
std::atomic<bool> g_counting{ false };
std::atomic<long> g_allocations{ 0 };
}

void *operator new(std::size_t n)
{
    if (g_counting.load(std::memory_order_relaxed))
    {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void *p = std::malloc(n == 0 ? 1 : n))
    {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace ct_bridge_test;

namespace {

// a fixed set of frame bytes made before the window (the follower hands the domain borrowed bytes)
struct Frames
{
    std::vector<Bytes> bytes;
    std::vector<JournalFrame> frames;
};

Frames prepare(std::int64_t from, std::int64_t to)
{
    Frames f;
    f.bytes.reserve(static_cast<std::size_t>(to - from + 1));
    for (std::int64_t s = from; s <= to; ++s)
    {
        f.bytes.push_back(makeFrame(s % 3 == 0 ? TEMPLATE_TERMINAL : TEMPLATE_TRADE, s, EPOCH, 64 * s));
    }
    for (const Bytes &b : f.bytes)
    {
        const Admission a = admitFrame(FRAME_FLAGS_UNFRAGMENTED, b.data(), b.size());
        JournalFrame jf;
        jf.sourceIndex = 0;
        jf.key = key(0);
        jf.endPosition = 64 * a.peek.journalSeq;
        jf.bytes = b.data();
        jf.length = b.size();
        jf.peek = a.peek;
        f.frames.push_back(jf);
    }
    return f;
}

}

// a core port that allocates nothing: it applies every feed entry at once (a dense cursor) and keeps the last query
struct QuietCore : CorePort
{
    std::int64_t cursor = 10;
    std::int64_t pendingQuery = 0;
    int retryEvery = 0;
    long calls = 0;
    long queries = 0;
    OfferOutcome tryOffer(const std::uint8_t *f, std::size_t n) noexcept override
    {
        ++calls;
        if (retryEvery > 0 && calls % retryEvery == 0)
        {
            return OfferOutcome::RETRY;
        }
        const std::uint16_t t = static_cast<std::uint16_t>(f[0] | (f[1] << 8));
        if (T_QUERY_FEED_POSITION == t)
        {
            ++queries;
            pendingQuery = rd64(f + 8);
            return OfferOutcome::OK;
        }
        const std::size_t count = static_cast<std::size_t>(f[2] | (f[3] << 8));
        for (std::size_t i = 0; i < count && 8 + 32 * (i + 1) <= n; ++i)
        {
            cursor = std::max(cursor, rd64(f + 8 + 32 * i));
        }
        return OfferOutcome::OK;
    }
};

TEST_CASE("ZeroAllocTest.steadyStateDutyCallsAllocateNothing")
{
    BridgeSettings settings = testSettings(1);
    ArchiveMetrics archiveMetrics;
    ResumeMemo memo(1, &archiveMetrics);
    memo.onListing(0, { active(0, 0) });
    FakeEncoder encoder;
    encoder.keepRecords = false;
    QuietCore core;
    BridgeMetrics metrics;
    BridgeDomain domain(settings, memo, archiveMetrics, encoder, core, metrics);
    TimePoint now{};
    auto answer = [&]() {
        if (0 == core.pendingQuery)
        {
            return;
        }
        FeedPositionReport r;
        r.correlationId = core.pendingQuery;
        r.meEpoch = EPOCH;
        r.lastAppliedJournalSeq = core.cursor;
        core.pendingQuery = 0;
        domain.onReport(r, now);
    };
    domain.onSessionUp(now);
    answer();
    AttachInfo info;
    info.source = 0;
    info.recording = active(0, 0);
    domain.onAttached(info);
    REQUIRE(domain.view().state == State::STREAMING);
    Frames warm = prepare(11, 400);
    Frames hot = prepare(401, 3'000);
    auto drive = [&](Frames &frames) {
        for (std::size_t i = 0; i < frames.frames.size(); ++i)
        {
            while (domain.onFrame(frames.frames[i]) != SinkAction::CONTINUE)
            {
                now += 1ms;
                domain.tick(now); // the back-pressured flush is retried by tick
                answer();
            }
            if (i % 9 == 0)
            {
                (void)domain.fragmentLimit();
                domain.onPollDone(0, now); // the empty-poll flush
            }
            now += (i % 40 == 0 ? 10'001ms : 1ms); // a periodic query every 40 frames
            domain.tick(now);
            answer(); // the report pops and proves the ring
        }
    };
    core.retryEvery = 13; // back-pressure now and then
    drive(warm);
    g_allocations = 0;
    g_counting = true;
    drive(hot);
    g_counting = false;
    const long allocations = g_allocations.load();
    MESSAGE("steady state: " << hot.frames.size() << " frames, " << core.calls << " offers, " << core.queries
            << " queries, allocations " << allocations);
    CHECK(domain.view().last == 3'000);
    CHECK(core.cursor == 3'000);
    CHECK(allocations == 0);
}
