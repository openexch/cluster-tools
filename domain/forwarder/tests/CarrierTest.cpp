// RC2 S12 contract 11.1 CarrierTest (5.5, BLOCKER-1 Fix A): for every R-rule failure, V12 and V1 the
// carrier is reader-valid (passes the ME's isValid and decode ranges) and fails behaviourTwoCause, so the
// ME journals INVALID 8 for the core's own command id; it keeps both ids, is a pure function and carries
// the signature orderType 2 (LIMIT_MAKER), timeInForce 1 (IOC).
#include "doctest/doctest.h"

#include <functional>
#include <string>
#include <vector>

#include "Fixtures.h"
#include "MeRulesPort.h"

#include "cluster_tools/domain/forwarder/DispatchRules.h"

using namespace cluster_tools::domain::forwarder;
namespace fx = cluster_tools::domain::forwarder::fixtures;

namespace {

std::vector<std::pair<std::string, Dispatch>> carrierCases()
{
    std::vector<std::pair<std::string, Dispatch>> cases;
    const std::function<Dispatch()> bases[3] = { [] { return fx::create(); }, [] { return fx::cancel(); },
        [] { return fx::amend(); } };
    const char *names[3] = { "CREATE", "CANCEL", "AMEND" };
    const std::vector<std::pair<std::string, std::function<void(Dispatch &)>>> mutations = {
        { "R2 userId 0", [](Dispatch &d) { d.userId = 0; } },
        { "R2 userId -7", [](Dispatch &d) { d.userId = -7; } },
        { "R2 omsOrderId 0", [](Dispatch &d) { d.omsOrderId = 0; } },
        { "R2 omsOrderId 0 idHigh 0", [](Dispatch &d) {
            d.omsOrderId = 0;
            d.commandIdHigh = 0;
        } },
        { "R2 omsOrderId -1 idHigh -5", [](Dispatch &d) {
            d.omsOrderId = -1;
            d.commandIdHigh = -5;
        } },
        { "R2 marketId 0", [](Dispatch &d) { d.marketId = 0; } },
        { "R2 orderType 9", [](Dispatch &d) { d.orderType = 9; } },
        { "R2 side 7", [](Dispatch &d) { d.side = 7; } },
        { "R3 old leg", [](Dispatch &d) { d.oldOrderId = d.kind == KIND_CREATE ? 99 : 0; } },
        { "R3 old leg negative", [](Dispatch &d) { d.oldOrderId = d.kind == KIND_CREATE ? -99 : -1; } },
        { "R4 tif 3", [](Dispatch &d) { d.timeInForce = 3; } },
        { "R5 eor", [](Dispatch &d) { d.expectedOldRemaining = d.kind == KIND_AMEND ? -4 : 6; } },
        { "V1 seq 0", [](Dispatch &d) { d.dispatchSeq = 0; } },
        { "V1 seq -9", [](Dispatch &d) { d.dispatchSeq = -9; } },
        { "R2 and B3 together", [](Dispatch &d) {
            d.userId = 0;
            d.omsOrderId = d.commandIdHigh + 5;
        } },
        { "everything wrong", [](Dispatch &d) {
            d.userId = 0;
            d.omsOrderId = 0;
            d.marketId = -3;
            d.orderType = 200;
            d.side = 200;
            d.oldOrderId = -1;
            d.timeInForce = 200;
            d.expectedOldRemaining = -1;
        } },
    };
    for (int k = 0; k < 3; k++)
    {
        for (const auto &[name, mutate] : mutations)
        {
            Dispatch d = bases[k]();
            mutate(d);
            cases.emplace_back(std::string(names[k]) + " " + name, d);
        }
    }
    for (int kind : { 4, 7, 255 })
    {
        for (std::int64_t low : { 0LL, 1LL, 2LL, 3LL, 4LL + 3, -1LL })
        {
            Dispatch d = fx::create();
            d.kind = static_cast<std::uint8_t>(kind);
            d.commandIdLow = low;
            cases.emplace_back("V12 kind " + std::to_string(kind) + " idLow " + std::to_string(low), d);
        }
    }
    return cases;
}

}

TEST_CASE("CarrierTest.everyCarrierIsReaderValidAndJournaledInvalid")
{
    int n = 0;
    for (const auto &[name, d] : carrierCases())
    {
        INFO(name);
        const Classification c = classify(d);
        REQUIRE(DispatchClass::CARRIER == c.cls);
        RingSlot slot;
        REQUIRE(toRingSlot(d, c, slot));
        REQUIRE(SlotForm::COMMAND == slot.form);
        const DurableCommand &carrier = slot.command;
        CHECK(me_port::react(carrier) == me_port::MeReaction::INVALID);
        CHECK(me_port::isValid(carrier.commandIdHigh, carrier.commandIdLow, carrier.userId, carrier.omsOrderId,
            carrier.oldOrderId, carrier.marketId, carrier.commandKind, carrier.orderType, carrier.orderSide,
            carrier.timeInForce, carrier.expectedOldRemaining));
        CHECK(me_port::B2_OK != me_port::behaviourTwoCause(carrier.commandIdHigh, carrier.commandIdLow,
            carrier.omsOrderId, carrier.commandKind, carrier.orderType, carrier.timeInForce,
            carrier.expectedOldRemaining));
        CHECK(carrier.commandIdHigh == d.commandIdHigh);
        CHECK(carrier.commandIdLow == d.commandIdLow);
        CHECK(carrier.orderType == 2);
        CHECK(carrier.timeInForce == 1);
        CHECK(carrier.price == d.price);
        CHECK(carrier.quantity == d.quantity);
        CHECK(carrier.budget == d.budget);
        CHECK(carrierCommand(d) == carrierCommand(d)); // pure
        n++;
    }
    CHECK(n == 3 * 16 + 18);
}

TEST_CASE("CarrierTest.carrierFieldsPerTable")
{
    // The 5.5 table, row by row, on the E2E CREATE of the Java goldens with userId 0 (the carrier golden,
    // JAVA_DURABLE_ORDER_COMMAND_V12_CARRIER, checked in Java with isValid true and behaviourTwoCause 4).
    Dispatch d;
    d.dispatchSeq = 501;
    d.commandIdHigh = 1001;
    d.commandIdLow = (3 << 2) | 0;
    d.kind = KIND_CREATE;
    d.userId = 0;
    d.omsOrderId = 1001;
    d.marketId = 7;
    d.orderType = 0;
    d.side = 1;
    d.price = 700'000'001;
    d.quantity = 800'000'001;
    d.budget = 900'000'001;
    d.timeInForce = 1;
    const DurableCommand c = carrierCommand(d);
    CHECK(c.commandIdHigh == 1001);
    CHECK(c.commandIdLow == 12);
    CHECK(c.userId == 1);
    CHECK(c.omsOrderId == 1001);
    CHECK(c.oldOrderId == 0);
    CHECK(c.price == 700'000'001);
    CHECK(c.quantity == 800'000'001);
    CHECK(c.budget == 900'000'001);
    CHECK(c.marketId == 7);
    CHECK(c.commandKind == 0);
    CHECK(c.orderType == 2);
    CHECK(c.orderSide == 1);
    CHECK(c.timeInForce == 1);
    CHECK(c.expectedOldRemaining == 0);
    CHECK(me_port::behaviourTwoCause(c.commandIdHigh, c.commandIdLow, c.omsOrderId, c.commandKind, c.orderType,
              c.timeInForce, c.expectedOldRemaining) == me_port::B2_INVALID_TIF);

    // kind mapping for V12: the low bits if 0..2, else 0; oldOrderId follows the carrier kind.
    Dispatch v = fx::cancel();
    v.kind = 9;
    v.commandIdLow = (5 << 2) | 1;
    CHECK(carrierCommand(v).commandKind == 1);
    CHECK(carrierCommand(v).oldOrderId == 55);
    v.commandIdLow = (5 << 2) | 3;
    CHECK(carrierCommand(v).commandKind == 0);
    CHECK(carrierCommand(v).oldOrderId == 0);
    // omsOrderId falls back to idHigh, then 1; userId/marketId to 1; side to 0; eor kept only on AMEND.
    Dispatch a = fx::amend();
    a.omsOrderId = 0;
    a.userId = -1;
    a.marketId = 0;
    a.side = 4;
    a.oldOrderId = 0;
    a.expectedOldRemaining = 7;
    const DurableCommand ac = carrierCommand(a);
    CHECK(ac.omsOrderId == a.commandIdHigh);
    CHECK(ac.userId == 1);
    CHECK(ac.marketId == 1);
    CHECK(ac.orderSide == 0);
    CHECK(ac.oldOrderId == 1);
    CHECK(ac.expectedOldRemaining == 7);
    a.expectedOldRemaining = -7;
    CHECK(carrierCommand(a).expectedOldRemaining == 0);
    a.commandIdHigh = -2;
    CHECK(carrierCommand(a).omsOrderId == 1);
}

TEST_CASE("CarrierTest.unroutableHasNoCarrier")
{
    Dispatch d = fx::create();
    d.commandIdHigh = 0;
    d.commandIdLow = 0;
    const Classification c = classify(d);
    CHECK(DispatchClass::UNROUTABLE == c.cls);
    RingSlot slot;
    CHECK_FALSE(toRingSlot(d, c, slot)); // the only dispatch never put on the wire
}

TEST_CASE("CarrierTest.classIGoesVerbatim")
{
    Dispatch d = fx::amend();
    d.expectedOldRemaining = 0; // B5
    const Classification c = classify(d);
    REQUIRE(DispatchClass::ME_INVALID == c.cls);
    RingSlot slot;
    REQUIRE(toRingSlot(d, c, slot));
    CHECK(slot.command == verbatimCommand(d));
    CHECK(me_port::react(slot.command) == me_port::MeReaction::INVALID);
}
