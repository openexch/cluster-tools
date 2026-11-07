// RC2 S9c 12.2 zeroAllocDutyIteration (S9b NOTE-10, the forwarder's test): a counted operator new armed around every
// agent iteration, the snapshot fill and the status publish, over 10^4 steady-state iterations with reports, acks,
// cap-64 62 frames (64 histogram observations each, review NOTE-9) and the verdict drain: 0 allocations. The harness's
// own work (journal appends, egress injection, reading the offers) runs with the counter disarmed; the transport copies
// offers into a preallocated ring.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <execinfo.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <new>

#include <fstream>
#include <iterator>

#include "AppRig.h"
#include "BridgeDuty.h"

#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

namespace {
std::atomic<bool> g_count{ false };
std::atomic<std::uint64_t> g_allocations{ 0 };
}

void *operator new(std::size_t n)
{
    if (g_count.load(std::memory_order_relaxed))
    {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        if (nullptr != std::getenv("CT_ALLOC_BACKTRACE"))
        {
            g_count.store(false);
            void *frames[32];
            const int depth = backtrace(frames, 32);
            backtrace_symbols_fd(frames, depth, 2);
            g_count.store(true);
        }
    }
    if (void *p = std::malloc(n == 0 ? 1 : n))
    {
        return p;
    }
    throw std::bad_alloc();
}
void *operator new[](std::size_t n)
{
    return ::operator new(n);
}
void operator delete(void *p) noexcept
{
    std::free(p);
}
void operator delete[](void *p) noexcept
{
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
    std::free(p);
}

using namespace ct_bridge_app_test;

TEST_CASE("ZeroAllocAppTest.zeroAllocDutyIteration")
{
    AppRigT<RingTransport> r(1);
    r.encoder.keepRecords = false;
    SimRecording &rec = r.journal(0, 1, 200);
    r.bringUp();
    REQUIRE(r.runUntil([&] { return r.core.cursor == 200; }));
    // warm-up: a periodic report that proves the ring into the memo (S9a's memo allocates its proof table at the first
    // proof of a recording key: warm-up, not steady state; S9b README deviation 10)
    const std::uint64_t queries = BridgeMetrics::get(r.agent->metrics().reportQueries);
    REQUIRE(r.runUntil([&] { return BridgeMetrics::get(r.agent->metrics().reportQueries) > queries + 1; }, 30 * SECOND));
    r.runFor(500 * MS);
    app::BridgeBoard board;
    app::BridgeSnapshot snapshot;
    std::uint64_t allocations = 0;
    std::int64_t written = 200;
    const std::uint64_t frames62Before = r.agent->metrics().forwardedFrames(FrameKind::K62);
    // a real cap-64 62 for the O3 / O4 path (the agent's onOfferOk after an OK offer): 64 observations each
    std::ifstream in(std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/java-trade.bin", std::ios::binary);
    const Bytes trade((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(trade.size() == TRADE_RECORD_LENGTH);
    std::vector<StagedTrade> staged(64);
    for (auto &t : staged)
    {
        std::memcpy(t.record, trade.data(), TRADE_RECORD_LENGTH);
    }
    cluster_tools::sbe_adapter::bridge_codec::BridgeCodec codec;
    std::vector<std::uint8_t> real62(8192);
    const std::size_t n62 = codec.encodeTradeBatch(staged.data(), staged.size(), real62.data(), real62.size());
    REQUIRE(n62 > 0);
    std::int64_t tradeId = 0;
    std::memcpy(&tradeId, trade.data() + 8 + 24, 8); // Trade.tradeId (schema 4: journalSeq, meEpoch, egressSeq first)
    const Bytes ack = settlementAppliedFrame(tradeId);
    for (int i = 0; i < 10'000; ++i)
    {
        if (0 == i % 50 && written < 200 + 10'000)
        {
            appendJournal(r.archive(0), rec, written + 1, written + 130); // runs of > 64 adjacent trades
            written += 130;
        }
        ++r.stepNo;
        r.t.now += MS;
        r.doAnswerConnects();
        r.pumpEgress();
        g_allocations.store(0);
        g_count.store(true);
        r.agent->doWork();
        if (0 == i % 10)
        {
            r.agent->onOfferOk(real62.data(), n62);
        }
        if (0 == i % 100)
        {
            r.agent->fillSnapshot(snapshot);
            board.tryPublish(snapshot);
        }
        g_count.store(false);
        allocations += g_allocations.load();
        if (5 == i % 10)
        {
            r.inject(ack, r.term); // SettlementApplied for the in-flight trade id (harness work)
        }
        r.collectOffers();
        r.core.commit();
    }
    REQUIRE(r.runUntil([&] { return r.core.cursor == written; }));
    MESSAGE("iterations 10000, records " << written << ", 62 frames " << r.agent->metrics().forwardedFrames(FrameKind::K62) -
        frames62Before << ", reports " << BridgeMetrics::get(r.agent->metrics().reportQueries) << ", allocations "
        << allocations);
    CHECK(r.agent->metrics().forwardedFrames(FrameKind::K62) > frames62Before);
    CHECK(r.agent->forwardLatency().count + r.agent->counters().clockAnomalies >= 64'000); // 1000 real 62s observed
    CHECK(allocations == 0);
}
