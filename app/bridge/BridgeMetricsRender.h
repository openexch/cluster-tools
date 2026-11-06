#ifndef CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_METRICS_RENDER_H
#define CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_METRICS_RENDER_H

#include <cstdint>
#include <string>

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/domain/bridge/BridgeMetrics.h"

#include "BridgeSnapshot.h"

// The bridge-http bodies (RC2 S9c 9.1-9.3), pure: the 100 ms BridgeSnapshot plus the relaxed atomics of the domain
// (BridgeMetrics) and of the archive (ArchiveMetrics, the three max gauges read with takeMax), plus what main and the
// HTTP thread know (HttpExtras).

namespace cluster_tools { namespace app { namespace bridge {

struct HttpExtras
{
    bool stalled = false;                 // duty heartbeat age >= 5 s, or the conductor heartbeat stale (main's view)
    std::int64_t conductorHeartbeatAgeMs = -1;
    std::uint64_t httpTimeouts = 0;
    std::uint64_t publishSkipped = 0;
    std::uint64_t sighupIgnored = 0;
    std::uint64_t driverDirReclaimed = 0;
    std::size_t sourceCount = 1;
    const char *driverMode = "external";
};

// 9.1: the JSON body (keys in the frozen order) and the HTTP status (503 iff halted or STALLED).
std::string renderHealthJson(const BridgeSnapshot &s, const HttpExtras &x);
int healthStatus(const BridgeSnapshot &s, const HttpExtras &x);
bool healthHalted(const BridgeSnapshot &s) noexcept; // halted from the halt record (HEALTH_HALTED_FROM_STATE: the state)

// 9.2: "ready\n" or "not ready: <bit names>\n"; the bits are the snapshot's plus bit 14 from main.
std::uint32_t readyBits(const BridgeSnapshot &s, const HttpExtras &x) noexcept;
std::string renderReady(std::uint32_t bits);

// 9.3: Prometheus text 0.0.4.
std::string renderPrometheus(const BridgeSnapshot &s, const domain::bridge::BridgeMetrics &domain,
    archive::ArchiveMetrics &archive, const HttpExtras &x);

}}}

#endif
