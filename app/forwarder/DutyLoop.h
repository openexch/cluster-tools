#ifndef CLUSTER_TOOLS_APP_FORWARDER_DUTY_LOOP_H
#define CLUSTER_TOOLS_APP_FORWARDER_DUTY_LOOP_H

#include <sched.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <functional>

#include "MetricsRender.h"
#include "StatusBoard.h"
#include "Supervisor.h"

// The fwd-duty thread body (RC2 S12 contract section 3) and the fwd-http routes.
//
// Duty: one agent iteration, then the idle strategy when the iteration did no work (spin 100, yield 100,
// then nanosleep 100 us: the only sleep, between iterations, never inside a step); the heartbeat counter is
// bumped every iteration (main's watchdog, F3) and the StatusBoard snapshot is published every 100 ms. The
// stop and resync flags are main's (signals) and fwd-http's (POST /resync).

namespace cluster_tools { namespace app { namespace forwarder {

inline constexpr std::int64_t PUBLISH_INTERVAL_NS = 100'000'000LL;
inline constexpr std::int64_t MAX_CYCLE_WINDOW_NS = 10'000'000'000LL;

class IdleStrategy
{
public:
    void idle(int work) noexcept
    {
        if (work > 0)
        {
            m_count = 0;
            return;
        }
        m_count++;
        if (m_count <= 100)
        {
            return; // spin
        }
        if (m_count <= 200)
        {
            sched_yield();
            return;
        }
        timespec ts{ 0, 100'000 };
        nanosleep(&ts, nullptr);
    }

private:
    int m_count = 0;
};

// errorCount: the Aeron monitor's error count (forwarder_aeron_errors_total); may be empty.
template<typename Agent>
int runDuty(Agent &agent, Supervisor &supervisor, StatusBoard &board, const std::function<std::uint64_t()> &errorCount)
{
    ForwarderSnapshot snapshot;
    IdleStrategy idle;
    std::int64_t nextPublish = 0;
    std::int64_t windowStart = Supervisor::monotonicNs();
    std::uint64_t windowMax = 0;
    std::uint64_t lastWindowMax = 0;
    while (!agent.stopped())
    {
        if (supervisor.stopRequested.load(std::memory_order_acquire))
        {
            agent.requestStop();
        }
        if (supervisor.resyncRequested.exchange(false, std::memory_order_acq_rel))
        {
            agent.requestResync(); // T4
        }
        const std::int64_t t0 = Supervisor::monotonicNs();
        const int work = agent.doWork();
        const std::int64_t t1 = Supervisor::monotonicNs();
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

// fwd-http: GET /health /ready /metrics, POST /resync; nothing else (contract section 10).
inline HttpResponse route(const HttpRequest &request, const StatusBoard &board, Supervisor &supervisor)
{
    HttpResponse response;
    if ("GET" == request.method && "/health" == request.path)
    {
        const bool stalled = supervisor.stalled(Supervisor::monotonicNs());
        response.status = stalled ? 503 : 200;
        response.contentType = "application/json";
        response.body = renderHealthJson(board.read(), stalled);
        return response;
    }
    if ("GET" == request.method && "/ready" == request.path)
    {
        const ForwarderSnapshot s = board.read();
        const bool ready = s.ready && 0 != s.sequence && !supervisor.stalled(Supervisor::monotonicNs());
        response.status = ready ? 200 : 503;
        response.body = renderReady(s);
        return response;
    }
    if ("GET" == request.method && "/metrics" == request.path)
    {
        response.status = 200;
        response.contentType = "text/plain; version=0.0.4";
        response.body = renderPrometheus(board.read());
        return response;
    }
    if ("POST" == request.method && "/resync" == request.path)
    {
        supervisor.resyncRequested.store(true, std::memory_order_release); // T4, the only thing http may set
        response.status = 202;
        response.body = "resync requested\n";
        return response;
    }
    if ("/health" == request.path || "/ready" == request.path || "/metrics" == request.path || "/resync" == request.path)
    {
        response.status = 405;
        response.body = "method not allowed\n";
        return response;
    }
    response.status = 404;
    response.body = "not found\n";
    return response;
}

}}}

#endif
