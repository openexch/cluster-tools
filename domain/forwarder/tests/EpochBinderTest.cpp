// RC2 S12 contract 11.1 EpochBinderTest (4.2, F8, NOTE-6, D-S12-8).
#include "doctest/doctest.h"

#include "cluster_tools/domain/forwarder/EpochBinder.h"

using namespace cluster_tools::domain::forwarder;

namespace {
constexpr std::int64_t S = 1'000'000'000LL;
constexpr std::int64_t E = 0x0A1B2C3D4E5F6071LL;
}

TEST_CASE("EpochBinderTest.zeroIgnoredThenFirstNonZeroBinds")
{
    EpochBinder b;
    b.onSessionStart(0);
    CHECK(b.onHeartbeat(0, 1) == EpochVerdict::NONE);
    CHECK_FALSE(b.bound());
    CHECK_FALSE(b.verified());
    CHECK(b.onHeartbeat(E, 2) == EpochVerdict::BOUND);
    CHECK(b.bound());
    CHECK(b.epoch() == E);
    CHECK(b.verified());
    CHECK(b.onHeartbeat(E, 3) == EpochVerdict::NONE);
}

TEST_CASE("EpochBinderTest.differentEpochIsFatalInM2AndM3")
{
    EpochBinder b;
    b.onSessionStart(0);
    REQUIRE(b.onHeartbeat(E, 1) == EpochVerdict::BOUND);
    CHECK(b.onHeartbeat(E + 1, 2) == EpochVerdict::FATAL_MISMATCH); // M3
    EpochBinder c;
    c.onSessionStart(0);
    REQUIRE(c.onHeartbeat(E, 1) == EpochVerdict::BOUND);
    c.onSessionEnd();
    c.onSessionStart(5);
    CHECK(c.onHeartbeat(E + 1, 6) == EpochVerdict::FATAL_MISMATCH); // M2 of a later session: re-genesis
}

TEST_CASE("EpochBinderTest.zeroInM3IsFatal")
{
    EpochBinder b;
    b.onSessionStart(0);
    REQUIRE(b.onHeartbeat(E, 1) == EpochVerdict::BOUND);
    CHECK(b.onHeartbeat(0, 2) == EpochVerdict::FATAL_ZERO); // the ME lost its identity
}

TEST_CASE("EpochBinderTest.newSessionNeedsANewVerification")
{
    EpochBinder b;
    b.onSessionStart(0);
    REQUIRE(b.onHeartbeat(E, 1) == EpochVerdict::BOUND);
    b.onSessionEnd();
    CHECK_FALSE(b.verified());
    b.onSessionStart(10);
    CHECK_FALSE(b.verified());        // M2: nothing may be offered
    CHECK(b.onHeartbeat(0, 11) == EpochVerdict::NONE);
    CHECK_FALSE(b.verified());
    CHECK(b.onHeartbeat(E, 12) == EpochVerdict::VERIFIED);
    CHECK(b.verified());
    CHECK(b.epoch() == E);            // E never changes
}

TEST_CASE("EpochBinderTest.fiveSecondsUnverifiedReconnects")
{
    EpochBinder b;
    b.onSessionStart(100);
    CHECK(b.tick(100 + 5 * S - 1) == EpochVerdict::NONE);
    CHECK(b.tick(100 + 5 * S) == EpochVerdict::RECONNECT);
    CHECK(b.tick(100 + 6 * S) == EpochVerdict::NONE); // once per session
    b.onSessionEnd();
    b.onSessionStart(200 * S);
    REQUIRE(b.onHeartbeat(E, 200 * S + 1) == EpochVerdict::BOUND);
    CHECK(b.tick(300 * S) == EpochVerdict::NONE); // verified sessions never time out
    CHECK(b.tick(1000 * S) == EpochVerdict::NONE);
}
