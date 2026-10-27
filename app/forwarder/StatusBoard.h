#ifndef CLUSTER_TOOLS_APP_FORWARDER_STATUS_BOARD_H
#define CLUSTER_TOOLS_APP_FORWARDER_STATUS_BOARD_H

#include <atomic>
#include <cstdint>
#include <mutex>

#include "Snapshot.h"

// StatusBoard - the duty thread's published snapshot (RC2 S12 contract section 3). The writer never blocks:
// it publishes with try_lock and simply publishes on its next 100 ms tick if a reader holds the lock; readers
// (fwd-http, main) copy under the lock. The duty heartbeat is a separate atomic, bumped every iteration, so
// the watchdog never depends on a publish.

namespace cluster_tools { namespace app { namespace forwarder {

class StatusBoard
{
public:
    // Duty thread. False if a reader held the lock (try again on the next tick).
    bool tryPublish(const ForwarderSnapshot &snapshot) noexcept
    {
        std::unique_lock<std::mutex> lock(m_lock, std::try_to_lock);
        if (!lock.owns_lock())
        {
            m_publishSkipped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_snapshot = snapshot;
        m_snapshot.sequence = ++m_sequence;
        return true;
    }

    ForwarderSnapshot read() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_snapshot;
    }

    void beat() noexcept { m_heartbeat.fetch_add(1, std::memory_order_release); }
    std::uint64_t heartbeat() const noexcept { return m_heartbeat.load(std::memory_order_acquire); }
    std::uint64_t publishSkipped() const noexcept { return m_publishSkipped.load(std::memory_order_relaxed); }

private:
    mutable std::mutex m_lock;
    ForwarderSnapshot m_snapshot;
    std::uint64_t m_sequence = 0;
    std::atomic<std::uint64_t> m_heartbeat{ 0 };
    std::atomic<std::uint64_t> m_publishSkipped{ 0 };
};

}}}

#endif
