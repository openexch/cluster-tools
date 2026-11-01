#ifndef CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_DUTY_H
#define CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_DUTY_H

#include <atomic>
#include <cstdint>
#include <functional>

#include "common/HttpServer.h"
#include "common/IdleStrategy.h"
#include "common/StatusBoardT.h"
#include "common/Supervisor.h"

#include "BridgeMetricsRender.h"
#include "BridgeSnapshot.h"
#include "StopBounds.h"

// The bridge-duty thread body (RC2 S9c 3.2 step 7) and the bridge-http routes (section 9).
//
// Duty: one agent iteration, the heartbeat every iteration (main's watchdog, F3), the StatusBoard snapshot every
// 100 ms, then the idle strategy when the iteration did no work (common::BRIDGE_IDLE: spin 0, yield 10, park 1 ms).
// The stop flag is main's (SIGTERM / SIGINT). SIGHUP is the Supervisor's COUNT_ONLY policy: nothing reaches the agent.

namespace cluster_tools { namespace app { namespace bridge {

using BridgeBoard = common::StatusBoardT<BridgeSnapshot>;

// The one SupervisorSettings shape (G7) with the bridge's values: watchdog and STALLED 5 s (the forwarder's, D-S9c-4),
// join bound = the archive message timeout 5 s + 2 s = 7 s (3.4, FIX-7), SIGHUP counted only (D-S9c-15).
inline common::SupervisorSettings bridgeSupervisorSettings()
{
    common::SupervisorSettings s;
    s.joinBoundNs = STOP_JOIN_BOUND_NS;
    s.sighupPolicy = common::SighupPolicy::COUNT_ONLY;
    s.processName = "ct-bridge";
    s.threadPrefix = "bridge";
    if (app_seams::disabled(app_seams::Rule::WATCHDOG_OFF))
    {
        s.watchdogNs = 3'600'000'000'000LL; // RED shape: the duty watchdog never fires
    }
    return s;
}

inline constexpr std::int64_t PUBLISH_INTERVAL_NS = 100'000'000LL;
inline constexpr std::int64_t MAX_CYCLE_WINDOW_NS = 10'000'000'000LL;

// errorCount: the Aeron monitor's error count (bridge_aeron_errors_total); may be empty.
template<typename Agent>
int runBridgeDuty(Agent &agent, common::Supervisor &supervisor, BridgeBoard &board,
    const std::function<std::uint64_t()> &errorCount)
{
    BridgeSnapshot snapshot;
    common::IdleStrategy idle(common::BRIDGE_IDLE);
    std::int64_t nextPublish = 0;
    std::int64_t windowStart = common::Supervisor::monotonicNs();
    std::uint64_t windowMax = 0;
    std::uint64_t lastWindowMax = 0;
    while (!agent.stopped())
    {
        if (supervisor.stopRequested.load(std::memory_order_acquire))
        {
            agent.requestStop(supervisor.stopRequestedAtNs.load(std::memory_order_acquire));
        }
        const std::int64_t t0 = common::Supervisor::monotonicNs();
        const int work = agent.doWork();
        const std::int64_t t1 = common::Supervisor::monotonicNs();
        const std::uint64_t cycleUs = static_cast<std::uint64_t>((t1 - t0) / 1000);
        if (cycleUs > windowMax)
        {
            windowMax = cycleUs;
        }
        if (t1 - windowStart >= MAX_CYCLE_WINDOW_NS)
        {
            lastWindowMax = windowMax;
            windowMax = 0;
            windowStart = t1;
        }
        board.beat();
        if (t1 - nextPublish >= 0)
        {
            agent.fillSnapshot(snapshot);
            snapshot.dutyHeartbeat = board.heartbeat();
            snapshot.aeronErrors = errorCount ? errorCount() : 0;
            snapshot.dutyMaxCycleUs = windowMax > lastWindowMax ? windowMax : lastWindowMax;
            board.tryPublish(snapshot);
            nextPublish = t1 + PUBLISH_INTERVAL_NS;
        }
        idle.idle(work);
    }
    agent.fillSnapshot(snapshot);
    snapshot.dutyHeartbeat = board.heartbeat();
    board.tryPublish(snapshot);
    return agent.stopReason().exitCode;
}

// What bridge-http reads besides the board: the domain and archive atomics and main's view.
struct HttpContext
{
    const domain::bridge::BridgeMetrics *domain = nullptr;
    archive::ArchiveMetrics *archive = nullptr;
    common::Supervisor *supervisor = nullptr;
    const common::HttpServer *http = nullptr;
    const BridgeBoard *board = nullptr;
    std::function<std::int64_t()> conductorAgeMs; // -1 when not watched
    std::function<bool()> conductorStale;
    std::size_t sourceCount = 1;
    const char *driverMode = "external";
    std::uint64_t driverDirReclaimed = 0;
};

inline HttpExtras extrasOf(const HttpContext &c)
{
    HttpExtras x;
    const bool conductorStale = c.conductorStale ? c.conductorStale() : false;
    x.stalled = (nullptr != c.supervisor && c.supervisor->stalled(common::Supervisor::monotonicNs())) || conductorStale;
    x.conductorHeartbeatAgeMs = c.conductorAgeMs ? c.conductorAgeMs() : -1;
    x.httpTimeouts = nullptr == c.http ? 0 : c.http->timedOut();
    x.publishSkipped = nullptr == c.board ? 0 : c.board->publishSkipped();
    x.sighupIgnored = nullptr == c.supervisor ? 0 : c.supervisor->sighupCount.load(std::memory_order_relaxed);
    x.driverDirReclaimed = c.driverDirReclaimed;
    x.sourceCount = c.sourceCount;
    x.driverMode = c.driverMode;
    return x;
}

// bridge-http: GET /health /ready /metrics; nothing else (section 9).
inline common::HttpResponse routeBridge(const common::HttpRequest &request, const HttpContext &c)
{
    common::HttpResponse response;
    const bool known = "/health" == request.path || "/ready" == request.path || "/metrics" == request.path;
    if (known && "GET" != request.method)
    {
        response.status = 405;
        response.body = "method not allowed\n";
        return response;
    }
    if (!known)
    {
        response.status = 404;
        response.body = "not found\n";
        return response;
    }
    const BridgeSnapshot s = c.board->read();
    const HttpExtras x = extrasOf(c);
    if ("/health" == request.path)
    {
        response.status = healthStatus(s, x);
        response.contentType = "application/json";
        response.body = renderHealthJson(s, x);
        return response;
    }
    if ("/ready" == request.path)
    {
        const std::uint32_t bits = readyBits(s, x);
        response.status = 0 == bits ? 200 : 503;
        response.body = renderReady(bits);
        return response;
    }
    response.status = 200;
    response.contentType = "text/plain; version=0.0.4";
    response.body = renderPrometheus(s, *c.domain, *c.archive, x);
    return response;
}

}}}

#endif
