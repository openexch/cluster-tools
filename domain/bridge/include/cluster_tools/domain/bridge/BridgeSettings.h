#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_BRIDGE_SETTINGS_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_BRIDGE_SETTINGS_H

// BridgeSettings: the defaults are the values of the contract. The app reads none of them from
// the environment (they are not operator knobs); it supplies only `identity` (= BRIDGE_INSTANCE_ID), the source
// count and the archive's probe interval. Tests shorten the timers.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

struct BridgeSettings
{
    std::int64_t identity = 0;            // FeedHalt bridgeIdentity (BRIDGE_INSTANCE_ID)
    std::size_t sourceCount = 1;          // 1..MAX_SOURCES, configured order = index order
    int fragmentLimit = 32;
    std::size_t ringCapacity = std::size_t(1) << 20;
    std::size_t keyTableCapacity = 4096;  // per source
    std::chrono::milliseconds reportInterval{ 10'000 };
    std::chrono::milliseconds reportIntervalHalted{ 5'000 };
    std::chrono::milliseconds queryResend{ 500 };
    std::chrono::milliseconds reportTimeout{ 5'000 };
    std::chrono::milliseconds invalidReportResend{ 5'000 };
    std::chrono::milliseconds feedHaltResend{ 5'000 };
    std::chrono::milliseconds haltRescan{ 60'000 };
    std::chrono::milliseconds behindDwell{ 10'000 };
    std::chrono::milliseconds behindRescan{ 60'000 };
    std::chrono::milliseconds holeGrace{ 60'000 };
    std::chrono::milliseconds unreachableRetry{ 10'000 };
    std::chrono::milliseconds probeInterval{ 10'000 }; // the archive's (FollowSettings::probeInterval, 8.4)
    int transientStrikes = 3;
    int stormFences = 2;
    // The correlation counter's base (queries and FeedHalts share it). 0 = a random base per process (9.2).
    std::int64_t correlationBase = 0;
};

}}}

#endif
