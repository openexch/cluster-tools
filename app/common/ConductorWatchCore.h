#ifndef CLUSTER_TOOLS_APP_COMMON_CONDUCTOR_WATCH_CORE_H
#define CLUSTER_TOOLS_APP_COMMON_CONDUCTOR_WATCH_CORE_H

#include <atomic>
#include <cstdint>
#include <string>

// ConductorWatchCore - the decision half of ConductorWatch, pure: it reads the client heartbeat
// counter through HeartbeatCounters (the Aeron CountersReader in ct-bridge, a fake in the tests) and the caller's
// epoch-ms clock. Every check:
//   - the counter is looked up by (RECORD_ALLOCATED, type 11, key == this client id); not found within the driver
//     timeout from the first check = false (the watch never switches itself off);
//   - once found, the record is re-validated before and after the value is read: a record that is no longer this
//     client's heartbeat (freed or re-allocated to another client after the driver reclaimed this one) = false, also
//     when the slot now carries a fresh stamp of someone else (the zombie shape with a timeout above the driver's
//     client liveness);
//   - a stamp older than the timeout = false, named "process pause" when the gap since the previous check is itself at
//     least the timeout (this process did not run), "conductor wedged" otherwise.
// `revalidate` false is the RED shape only (CONDUCTOR_REVALIDATE_OFF in the seamed builds).

namespace cluster_tools { namespace app { namespace common {

class HeartbeatCounters
{
public:
    virtual ~HeartbeatCounters() = default;
    virtual std::int32_t maxCounterId() const = 0;
    virtual std::int32_t recordState(std::int32_t id) const = 0; // RECORD_ALLOCATED, RECORD_RECLAIMED, ...
    virtual std::int32_t typeId(std::int32_t id) const = 0;
    virtual std::int64_t key(std::int32_t id) const = 0;         // the first 8 key bytes (the client id for type 11)
    virtual std::int64_t value(std::int32_t id) const = 0;
};

inline constexpr std::int32_t COUNTER_RECORD_ALLOCATED = 1;     // aeron CountersReader::RECORD_ALLOCATED
inline constexpr std::int32_t CLIENT_HEARTBEAT_TYPE_ID = 11;    // AERON_COUNTER_CLIENT_HEARTBEAT_TIMESTAMP_TYPE_ID

class ConductorWatchCore
{
public:
    ConductorWatchCore(const HeartbeatCounters &counters, std::int64_t clientId, std::int64_t timeoutMs, bool revalidate) :
        m_counters(counters), m_clientId(clientId), m_timeoutMs(timeoutMs), m_revalidate(revalidate)
    {
    }

    // False (with why) = the process must exit 70.
    bool check(std::int64_t nowMs, std::string &why)
    {
        if (m_firstCheckMs < 0)
        {
            m_firstCheckMs = nowMs;
        }
        const std::int64_t gapMs = m_lastCheckMs < 0 ? 0 : nowMs - m_lastCheckMs;
        m_lastCheckMs = nowMs;
        if (m_counterId < 0 && !find())
        {
            if (nowMs - m_firstCheckMs >= m_timeoutMs)
            {
                why = "conductor heartbeat counter of Aeron client " + std::to_string(m_clientId) +
                    " not found within the driver timeout " + std::to_string(m_timeoutMs) +
                    " ms: the client conductor never stamped it, or the driver reclaimed the client (F3b)";
                return false;
            }
            return true;
        }
        if (m_revalidate && !owned(m_counterId))
        {
            why = reclaimed();
            return false;
        }
        const std::int64_t stamp = m_counters.value(m_counterId);
        if (m_revalidate && !owned(m_counterId))
        {
            why = reclaimed();
            return false;
        }
        const std::int64_t age = nowMs - stamp;
        m_ageMs.store(age < 0 ? 0 : age, std::memory_order_relaxed);
        if (age >= m_timeoutMs)
        {
            if (gapMs >= m_timeoutMs)
            {
                why = "no conductor heartbeat for " + std::to_string(age) + " ms after a process pause of " +
                    std::to_string(gapMs) + " ms (this process did not run: SIGSTOP, a VM or GC freeze), not a conductor "
                    "or driver wedge; the driver timeout is " + std::to_string(m_timeoutMs) + " ms (F3b)";
            }
            else
            {
                why = "conductor heartbeat stale: this client's heartbeat counter " + std::to_string(m_counterId) +
                    " is " + std::to_string(age) + " ms old (> the driver timeout " + std::to_string(m_timeoutMs) +
                    " ms): the Aeron client conductor is wedged (F3b)";
            }
            return false;
        }
        return true;
    }

    std::int64_t ageMs() const noexcept { return m_ageMs.load(std::memory_order_relaxed); }
    std::int32_t counterId() const noexcept { return m_counterId; }

private:
    bool owned(std::int32_t id) const
    {
        return COUNTER_RECORD_ALLOCATED == m_counters.recordState(id) &&
            CLIENT_HEARTBEAT_TYPE_ID == m_counters.typeId(id) && m_clientId == m_counters.key(id);
    }

    bool find()
    {
        const std::int32_t max = m_counters.maxCounterId();
        for (std::int32_t id = 0; id < max; ++id)
        {
            if (owned(id))
            {
                m_counterId = id;
                return true;
            }
        }
        return false;
    }

    std::string reclaimed() const
    {
        return "conductor heartbeat counter " + std::to_string(m_counterId) + " is no longer this client's (state " +
            std::to_string(m_counters.recordState(m_counterId)) + ", type " + std::to_string(m_counters.typeId(m_counterId)) +
            ", key " + std::to_string(m_counters.key(m_counterId)) + "; client " + std::to_string(m_clientId) +
            "): the driver reclaimed this Aeron client (F3b)";
    }

    const HeartbeatCounters &m_counters;
    std::int64_t m_clientId;
    std::int64_t m_timeoutMs;
    bool m_revalidate;
    std::int32_t m_counterId = -1;
    std::int64_t m_firstCheckMs = -1;
    std::int64_t m_lastCheckMs = -1;
    std::atomic<std::int64_t> m_ageMs{ -1 };
};

}}}

#endif
