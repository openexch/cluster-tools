#ifndef CLUSTER_TOOLS_CLIENT_BACKOFF_H
#define CLUSTER_TOOLS_CLIENT_BACKOFF_H

#include <algorithm>
#include <cstdint>

// Deterministic exponential backoff for connect attempts: initial, doubling, capped. No
// jitter on purpose (tests are exact and the owners are a handful of processes, not a herd).

namespace cluster_tools { namespace client {

class ExponentialBackoff
{
public:
    ExponentialBackoff(std::int64_t initialNs, std::int64_t maxNs) noexcept :
        m_initialNs(initialNs), m_maxNs(std::max(initialNs, maxNs)), m_nextNs(initialNs)
    {
    }

    // The delay before the next attempt; each call doubles the following one up to the cap.
    std::int64_t next() noexcept
    {
        const std::int64_t delay = m_nextNs;
        m_nextNs = (m_nextNs > m_maxNs / 2) ? m_maxNs : m_nextNs * 2;
        return delay;
    }

    void reset() noexcept { m_nextNs = m_initialNs; }

private:
    std::int64_t m_initialNs;
    std::int64_t m_maxNs;
    std::int64_t m_nextNs;
};

}}

#endif
