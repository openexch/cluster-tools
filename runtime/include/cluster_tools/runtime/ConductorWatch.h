#ifndef CLUSTER_TOOLS_RUNTIME_CONDUCTOR_WATCH_H
#define CLUSTER_TOOLS_RUNTIME_CONDUCTOR_WATCH_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cluster_tools/runtime/ConductorWatchCore.h"

namespace aeron { class Aeron; }

// ConductorWatch - the client-conductor heartbeat check (WITH_AERON only).
//
// A WEDGED Aeron client conductor fires no callback: the fatal monitor never runs, the duty thread keeps beating and
// keepalives go out on the mapped buffers until the driver reclaims the client, after which every reconnect ends
// ATTEMPTS_EXHAUSTED forever with /health 200 (a half-alive process). The conductor stamps this client's heartbeat
// counter (AERON_COUNTER_CLIENT_HEARTBEAT_TIMESTAMP_TYPE_ID 11, key = the client id, value = epoch ms) at every
// keepalive; main reads it through the CountersReader (shared memory, no conductor involved) at every wake. The
// decisions are ConductorWatchCore's: the record is re-validated on every check (a reclaimed or re-used slot = 70),
// a counter never found within the timeout = 70, a stale stamp = 70 (named a process pause when this
// process itself did not run for the timeout).
//
// The heartbeat counter exists only while the client holds an Aeron resource. A client that has added no publication,
// subscription or counter has no heartbeat counter for the driver to stamp, so this check reads it as reclaimed (70).
// A process that may sit with no resource (for example before its first session) registers a counter of its own, such
// as a process counter keyed by its instance id, so the driver keeps a heartbeat record for it.

namespace cluster_tools { namespace runtime {

class ConductorWatch
{
public:
    using EpochMsClock = std::function<std::int64_t()>;

    // timeoutMs: the process's driver timeout (BRIDGE_DRIVER_TIMEOUT_MS). clock: tests only (default system clock).
    // revalidate: false only in the RED shape (the seamed builds' CONDUCTOR_REVALIDATE_OFF).
    ConductorWatch(std::shared_ptr<::aeron::Aeron> aeron, std::int64_t timeoutMs, EpochMsClock clock = {},
        bool revalidate = true);
    ~ConductorWatch();

    // Main, every wake. False (with why) = exit 70.
    bool check(std::string &why);

    // -1 until the counter was found; otherwise the age at the last check (bridge_conductor_heartbeat_age_ms).
    std::int64_t ageMs() const noexcept { return m_core->ageMs(); }
    bool stale() const noexcept { return m_core->ageMs() >= m_timeoutMs; }
    std::int32_t counterId() const noexcept { return m_core->counterId(); }
    std::int64_t timeoutMs() const noexcept { return m_timeoutMs; }

private:
    std::shared_ptr<::aeron::Aeron> m_aeron;
    std::int64_t m_timeoutMs;
    EpochMsClock m_clock;
    std::unique_ptr<HeartbeatCounters> m_counters;
    std::unique_ptr<ConductorWatchCore> m_core;
};

}}

#endif
