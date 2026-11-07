// RC2 S12 contract 11.1 MappingTest: 5.2 and 5.3 field by field with distinct values, so a swapped or
// shifted field cannot pass; dispatchSeq and releaseScope never reach the template 9 command.
#include "doctest/doctest.h"

#include <initializer_list>

#include "Fixtures.h"

#include "cluster_tools/domain/forwarder/DispatchRules.h"

using namespace cluster_tools::domain::forwarder;
namespace fx = cluster_tools::domain::forwarder::fixtures;

TEST_CASE("MappingTest.template9FieldByField")
{
    Dispatch d;
    d.dispatchSeq = 0x1111;
    d.commandIdHigh = 0x2222;
    d.commandIdLow = 0x3332; // kind bits 2
    d.kind = KIND_AMEND;
    d.userId = 0x4444;
    d.omsOrderId = 0x5555;
    d.oldOrderId = 0x6666;
    d.marketId = 0x7777;
    d.orderType = 2;
    d.side = 1;
    d.price = 0x8888;
    d.quantity = 0x9999;
    d.budget = 0xAAAA;
    d.timeInForce = 0; // AMEND writes 0 (S8 3.6)
    d.releaseScope = 1;
    d.expectedOldRemaining = 0xBBBB;
    const DurableCommand c = verbatimCommand(d);
    CHECK(c.commandIdHigh == 0x2222);
    CHECK(c.commandIdLow == 0x3332);
    CHECK(c.commandKind == KIND_AMEND);
    CHECK(c.userId == 0x4444);
    CHECK(c.omsOrderId == 0x5555);
    CHECK(c.oldOrderId == 0x6666);
    CHECK(c.marketId == 0x7777);
    CHECK(c.orderType == 2);
    CHECK(c.orderSide == 1);
    CHECK(c.price == 0x8888);
    CHECK(c.quantity == 0x9999);
    CHECK(c.budget == 0xAAAA);
    CHECK(c.timeInForce == 0);
    CHECK(c.expectedOldRemaining == 0xBBBB);

    Dispatch tif = fx::create();
    tif.timeInForce = 2;
    CHECK(verbatimCommand(tif).timeInForce == 2);
}

TEST_CASE("MappingTest.dispatchSeqAndReleaseScopeNeverReachTheCommand")
{
    for (std::uint8_t kind : { KIND_CREATE, KIND_CANCEL, KIND_AMEND })
    {
        Dispatch a = fx::ofKind(kind, 5);
        Dispatch b = a;
        b.dispatchSeq = 987654;
        b.releaseScope = 1;
        CHECK(verbatimCommand(a) == verbatimCommand(b));
        CHECK(carrierCommand(a) == carrierCommand(b));
        // A resend therefore carries the bytes the core minted (5.2): byte-identical across copies.
        RingSlot sa, sb;
        REQUIRE(toRingSlot(a, classify(a), sa));
        REQUIRE(toRingSlot(b, classify(b), sb));
        CHECK(sa.command == sb.command);
        CHECK(sa.seq == 5);
        CHECK(sb.seq == 987654);
    }
}

TEST_CASE("MappingTest.releaseEntryPer5_3")
{
    Dispatch d = fx::release(77, 4242, (19LL << 2) | 3, SCOPE_ORDER);
    const ReleaseEntry e = releaseEntry(d);
    CHECK(e.commandIdHigh == 4242);
    CHECK(e.idLowFloor == ((19LL << 2) | 3)); // the whole idLow (S8 3.1: ORDER = a << 2 | 3)
    CHECK(e.scope == SCOPE_ORDER);
    RingSlot slot;
    REQUIRE(toRingSlot(d, classify(d), slot));
    CHECK(slot.form == SlotForm::RELEASE);
    CHECK(slot.seq == 77);
    CHECK(slot.release == e);

    // RA: the anomalous payload is not on the wire, the release is exactly the minted one.
    Dispatch ra = d;
    ra.price = 5;
    ra.userId = 9;
    REQUIRE(classify(ra).cls == DispatchClass::RELEASE_ANOMALY);
    RingSlot raSlot;
    REQUIRE(toRingSlot(ra, classify(ra), raSlot));
    CHECK(raSlot.form == SlotForm::RELEASE);
    CHECK(raSlot.release == e);

    // RX travels alone, raw scope kept (the ME decode-rejects only this frame).
    Dispatch rx = d;
    rx.releaseScope = 2;
    RingSlot rxSlot;
    REQUIRE(toRingSlot(rx, classify(rx), rxSlot));
    CHECK(rxSlot.form == SlotForm::RELEASE_ISOLATED);
    CHECK(rxSlot.release.scope == 2);
}
