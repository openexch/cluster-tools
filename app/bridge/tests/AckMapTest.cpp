// AckMapTest: the O4 map is bounded at 2^20 (an overwrite is counted), cleared at a session loss, and an
// ack for an unknown trade records nothing; the O3 anomaly bounds (negative, > 1 h, non-positive timestamps).
#include "doctest/doctest.h"

#include "BridgeSnapshot.h"

using namespace cluster_tools::app::bridge;

TEST_CASE("AckMapTest.boundedOverwriteCountedClearedOnSessionDown")
{
    AckMap map;
    const std::int64_t n = static_cast<std::int64_t>(AckMap::CAPACITY) + 5;
    for (std::int64_t t = 1; t <= n; ++t)
    {
        map.onOffered(t, t * 1'000'000);
    }
    const bool bounded = map.size() == AckMap::CAPACITY;
    CHECK(bounded); // ACKMAP_UNBOUNDED RED: the map keeps growing
    CHECK(map.skips() == 5);
    std::int64_t latency = -1;
    CHECK(map.onApplied(n, n * 1'000'000 + 7'000'000, latency));
    CHECK(latency == 7);
    CHECK_FALSE(map.onApplied(3, 0, latency)); // overwritten by 3 + 2^20
    CHECK(map.unknown() == 1);
    CHECK_FALSE(map.onApplied(999'999'999'999, 0, latency)); // never offered
    CHECK(map.unknown() == 2);
    map.clear();
    CHECK(map.size() == 0);
    CHECK_FALSE(map.onApplied(n - 1, 0, latency));
}

TEST_CASE("AckMapTest.forwardLatencyAnomalyBounds")
{
    LatencyHistogram h;
    std::uint64_t anomalies = 0;
    CHECK(recordForwardLatency(h, anomalies, 10'000, 9'990));
    CHECK(h.count == 1);
    CHECK(h.sumMs == 10);
    const bool futureRefused = !recordForwardLatency(h, anomalies, 10'000, 10'001);
    CHECK(futureRefused); // LATENCY_RECORDS_GARBAGE RED
    CHECK_FALSE(recordForwardLatency(h, anomalies, 10'000'000'000, 10'000'000'000 - 3'600'001)); // > 1 h
    CHECK_FALSE(recordForwardLatency(h, anomalies, 10'000, 0));
    CHECK(anomalies == 3);
    CHECK(h.count == 1);
    CHECK(recordForwardLatency(h, anomalies, 10'000'000'000, 10'000'000'000 - 3'600'000)); // exactly 1 h is kept
    CHECK(h.buckets[LatencyHistogram::BOUNDS] == 1); // +Inf
}
