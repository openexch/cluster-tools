// RC2 S12 contract 11.3 zeroAllocDutyCycle: after warm-up, 100,000 dispatches through decode, classification,
// the resync machine, the ring, the encoder and the ME offer with a counting operator new armed around every
// agent iteration: 0 allocations. The harness's own frame injection happens with the counter disarmed; the
// fake transport records no offered frame while counting (recordOffers = false), so what is counted is the
// duty cycle: both clients' doWork/poll/tryOffer, the registrar, the domain and the codec.
#include "doctest/doctest.h"

#include "AgentRig.h"
#include "AllocationCounter.h"

using namespace cluster_tools::app::forwarder;
using namespace cluster_tools::app::forwarder::test;
namespace domain = cluster_tools::domain;

TEST_CASE("ZeroAllocTest.zeroAllocDutyCycle")
{
    Rig r;
    r.bringUp();
    // warm-up: one batch end to end
    r.core_(batchFrame({ create(1, 1001) }));
    r.cycles(5);
    REQUIRE(r.agent.lastSentSeq() == 1);
    r.coreT.recordOffers = false;
    r.meT.recordOffers = false;

    const int total = 100'000;
    std::int64_t seq = 2;
    std::uint64_t allocations = 0;
    std::uint64_t cycles = 0;
    std::vector<Dispatch> batch;
    while (seq <= total + 1)
    {
        batch.clear();
        for (int i = 0; i < 64 && seq <= total + 1; i++, seq++)
        {
            Dispatch d = create(seq, 1000 + seq);
            if (0 == seq % 7)
            {
                d.kind = 3; // release entries ride the same path
                d.commandIdLow = (2 << 2) | 1;
                d.userId = d.omsOrderId = 0;
                d.marketId = 0;
                d.price = d.quantity = 0;
            }
            batch.push_back(d);
        }
        r.core_(batchFrame(batch)); // harness work, not counted
        r.advance(MS);
        g_allocations.store(0);
        g_countAllocations.store(true);
        r.agent.doWork();
        r.agent.doWork();
        g_countAllocations.store(false);
        allocations += g_allocations.load();
        cycles += 2;
    }
    for (int i = 0; i < 50; i++)
    {
        r.advance(MS);
        g_allocations.store(0);
        g_countAllocations.store(true);
        r.agent.doWork();
        g_countAllocations.store(false);
        allocations += g_allocations.load();
    }
    MESSAGE("dispatches " << total << " cycles " << cycles << " allocations " << allocations << " ME offers "
        << r.meT.offersNotRecorded);
    CHECK(r.agent.lastSentSeq() == total + 1);
    CHECK(r.agent.resync().mode() == domain::forwarder::Mode::LIVE);
    CHECK(r.agent.ring().depth() == 0);
    CHECK(allocations == 0);
    CHECK(r.agent.resync().counters().pushed >= static_cast<std::uint64_t>(total));
}

TEST_CASE("ZeroAllocTest.resyncHeadProbeTimeoutAndSnapshotPathsAllocateNothing")
{
    // Review NOTE-6: the cold-but-recurring paths, each in a counted window: a T5 gap with its held entry, the page
    // request, finishPage and the pushes; the 10 s head probe and its answer; a T9 timeout and the re-issued request;
    // fillSnapshot. Frames are injected with the counter disarmed (harness work).
    Rig r;
    r.bringUp();
    r.core_(batchFrame({ create(1, 1001) }));
    r.cycles(5);
    r.coreT.recordOffers = false;
    r.meT.recordOffers = false;
    ForwarderSnapshot snapshot;
    std::uint64_t allocations = 0;
    auto counted = [&](int cycles, std::int64_t stepNs) {
        for (int i = 0; i < cycles; i++)
        {
            r.advance(stepNs);
            g_allocations.store(0);
            g_countAllocations.store(true);
            r.agent.doWork();
            r.agent.fillSnapshot(snapshot);
            g_countAllocations.store(false);
            allocations += g_allocations.load();
        }
    };
    // T5: seq 3 arrives while the cursor is 1
    r.core_(batchFrame({ create(3, 1003) }));
    counted(3, MS);
    REQUIRE(r.agent.resync().mode() == domain::forwarder::Mode::RESYNCING);
    const std::int64_t page = r.agent.resync().request().correlationId;
    r.core_(batchFrame({ create(2, 1002), create(3, 1003) }));
    r.core_(pendingEnd(page, 2, 3));
    counted(5, MS);
    REQUIRE(r.agent.resync().mode() == domain::forwarder::Mode::LIVE);
    REQUIRE(r.agent.lastSentSeq() == 3);
    // the head probe 10 s later, and its answer
    counted(1000, 10 * MS);
    REQUIRE(r.agent.resync().requestInFlight());
    REQUIRE(r.agent.resync().request().kind == domain::forwarder::RequestKind::PROBE);
    r.core_(pendingEnd(r.agent.resync().request().correlationId, 0, 3));
    counted(3, MS);
    REQUIRE(r.agent.resync().counters().headProbeCovered >= 1);
    // T9: the next probe goes unanswered for 5 s and is re-issued with a new correlation id
    counted(1000, 10 * MS);
    const std::int64_t first = r.agent.resync().request().correlationId;
    counted(600, 10 * MS);
    REQUIRE(r.agent.resync().counters().timeouts >= 1);
    REQUIRE(r.agent.resync().request().correlationId != first);
    MESSAGE("allocations in the resync / probe / timeout / snapshot windows: " << allocations);
    CHECK(allocations == 0);
}
