// RC2 S12 contract 11.1 SendRingTest (5.3, 7): capacity, overflow, the drain plan with release grouping.
#include "doctest/doctest.h"

#include "Fixtures.h"

#include "cluster_tools/domain/forwarder/SendRing.h"

using namespace cluster_tools::domain::forwarder;
namespace fx = cluster_tools::domain::forwarder::fixtures;

namespace {

RingSlot slotOf(const Dispatch &d)
{
    RingSlot s;
    REQUIRE(toRingSlot(d, classify(d), s));
    return s;
}

RingSlot releaseSlot(std::int64_t seq)
{
    return slotOf(fx::release(seq, 5000 + seq));
}

RingSlot commandSlot(std::int64_t seq)
{
    return slotOf(fx::create(seq, 5000 + seq));
}

RingSlot isolatedSlot(std::int64_t seq)
{
    Dispatch d = fx::release(seq, 5000 + seq);
    d.releaseScope = 2;
    return slotOf(d);
}

}

TEST_CASE("SendRingTest.capacityIsExactly65536AndAFullPushFails")
{
    SendRing ring;
    CHECK(SendRing::CAPACITY == 65'536);
    for (std::int64_t i = 1; i <= 65'536; i++)
    {
        REQUIRE(ring.push(commandSlot(i)));
    }
    CHECK(ring.full());
    CHECK(ring.depth() == 65'536);
    CHECK_FALSE(ring.push(commandSlot(65'537)));
    CHECK(ring.overflows() == 1);
    ring.pop(1);
    CHECK(ring.push(commandSlot(65'537)));
    CHECK(ring.at(ring.depth() - 1).seq == 65'537);
    CHECK(ring.clear() == 65'536);
    CHECK(ring.empty());
}

TEST_CASE("SendRingTest.releaseRunOf64IsCutAt48")
{
    SendRing ring;
    for (std::int64_t i = 1; i <= 64; i++)
    {
        ring.push(releaseSlot(100 + i));
    }
    FramePlan p = ring.nextFrame();
    CHECK(p.type == FrameType::RELEASES);
    CHECK(p.count == 48);
    CHECK(p.firstSeq == 101);
    // the plan does not consume: only a pop after a positive offer does
    CHECK(ring.nextFrame().count == 48);
    CHECK(ring.depth() == 64);
    ring.pop(p.count);
    p = ring.nextFrame();
    CHECK(p.type == FrameType::RELEASES);
    CHECK(p.count == 16);
    CHECK(p.firstSeq == 149);
    ring.pop(p.count);
    CHECK(ring.nextFrame().type == FrameType::NONE);
}

TEST_CASE("SendRingTest.template9SlotSplitsARunAndRxIsAlone")
{
    SendRing ring;
    ring.push(releaseSlot(1));
    ring.push(releaseSlot(2));
    ring.push(commandSlot(3));
    ring.push(releaseSlot(4));
    ring.push(isolatedSlot(5));
    ring.push(releaseSlot(6));
    ring.push(releaseSlot(7));
    const FramePlan expected[] = {
        { FrameType::RELEASES, 2, 1 },
        { FrameType::COMMAND, 1, 3 },
        { FrameType::RELEASES, 1, 4 },
        { FrameType::RELEASES, 1, 5 }, // RX: a frame of its own
        { FrameType::RELEASES, 2, 6 },
    };
    for (const FramePlan &want : expected)
    {
        const FramePlan got = ring.nextFrame();
        CHECK(got.type == want.type);
        CHECK(got.count == want.count);
        CHECK(got.firstSeq == want.firstSeq);
        ring.pop(got.count);
    }
    CHECK(ring.empty());
}

TEST_CASE("SendRingTest.isolatedSlotIsAloneEvenAtTheHeadOfARun")
{
    SendRing ring;
    ring.push(isolatedSlot(1));
    ring.push(releaseSlot(2));
    const FramePlan first = ring.nextFrame();
    CHECK(first.count == 1);
    CHECK(ring.at(0).form == SlotForm::RELEASE_ISOLATED);
    ring.pop(1);
    CHECK(ring.nextFrame().count == 1);
}
