// RC2 S12 contract 11.1 AdmissionWatcherTest (T3, W16, NOTE-4, D-S12-4).
#include "doctest/doctest.h"

#include "cluster_tools/domain/forwarder/AdmissionWatcher.h"

using namespace cluster_tools::domain::forwarder;

namespace {
constexpr std::uint8_t OPEN = 0;
constexpr std::uint8_t SUSPENDED = 1;
constexpr std::uint32_t BRIDGE_HALT = 1u << 2;
constexpr std::uint32_t ME_CAPACITY = 1u << 4;
}

TEST_CASE("AdmissionWatcherTest.originResetsTheBaselineAndNeverTriggers")
{
    AdmissionWatcher w;
    CHECK_FALSE(w.onOrigin(0, SUSPENDED, ME_CAPACITY));
    CHECK(w.hasBaseline());
    CHECK_FALSE(w.onOrigin(0, OPEN, 0)); // a changed ORIGIN (re-registration) still never fires
    CHECK_FALSE(w.onBroadcast(0, OPEN, 0));
}

TEST_CASE("AdmissionWatcherTest.broadcastChangeInStateOrAnyReasonBitTriggers")
{
    AdmissionWatcher w;
    w.onOrigin(0, SUSPENDED, BRIDGE_HALT | ME_CAPACITY);
    CHECK(w.onBroadcast(0, SUSPENDED, BRIDGE_HALT)); // bit 4 cleared: re-offer the CAPACITY'd dispatches
    CHECK_FALSE(w.onBroadcast(0, SUSPENDED, BRIDGE_HALT)); // equal value: nothing
    CHECK(w.onBroadcast(0, OPEN, 0));
    CHECK(w.onBroadcast(0, OPEN, 1u << 8)); // a reason bit alone (state unchanged)
}

TEST_CASE("AdmissionWatcherTest.perMarketIgnored")
{
    AdmissionWatcher w;
    w.onOrigin(0, OPEN, 0);
    CHECK_FALSE(w.onBroadcast(3, SUSPENDED, ME_CAPACITY));
    CHECK_FALSE(w.onOrigin(4, SUSPENDED, ME_CAPACITY));
    CHECK(w.state() == OPEN);
    CHECK_FALSE(w.onBroadcast(0, OPEN, 0));
}

TEST_CASE("AdmissionWatcherTest.newCoreSessionResetsTheBaseline")
{
    AdmissionWatcher w;
    w.onOrigin(0, OPEN, 0);
    w.reset();
    CHECK_FALSE(w.hasBaseline());
    CHECK_FALSE(w.onOrigin(0, SUSPENDED, ME_CAPACITY));
    CHECK_FALSE(w.onBroadcast(0, SUSPENDED, ME_CAPACITY));
    // a BROADCAST without any baseline sets it and fires (the conservative side)
    w.reset();
    CHECK(w.onBroadcast(0, OPEN, 0));
}
