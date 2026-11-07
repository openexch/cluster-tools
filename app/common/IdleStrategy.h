#ifndef CLUSTER_TOOLS_APP_COMMON_IDLE_STRATEGY_H
#define CLUSTER_TOOLS_APP_COMMON_IDLE_STRATEGY_H

#include <sched.h>
#include <time.h>

#include <cstdint>

// The duty threads' idle strategy (RC2 S12 section 3, RC2 S9c W1): after an iteration that did no work, spin
// `spins` times, then sched_yield `yields` times, then nanosleep `parkNs` (the only sleep, between iterations, never
// inside a step). Two named policies, one implementation:
//   FORWARDER_IDLE  spin 100, yield 100, park 100 us   (ct-forwarder, S12)
//   BRIDGE_IDLE     spin 0, yield 10, park 1 ms        (ct-bridge, S9a 3.2 step 6: no busy spin on a shared box)

namespace cluster_tools { namespace app { namespace common {

struct IdlePolicy
{
    int spins = 0;
    int yields = 0;
    long parkNs = 0;
};

inline constexpr IdlePolicy FORWARDER_IDLE{ 100, 100, 100'000L };
inline constexpr IdlePolicy BRIDGE_IDLE{ 0, 10, 1'000'000L };

class IdleStrategy
{
public:
    explicit constexpr IdleStrategy(IdlePolicy policy) noexcept : m_policy(policy) {}

    void idle(int work) noexcept
    {
        if (work > 0)
        {
            m_count = 0;
            return;
        }
        m_count++;
        if (m_count <= m_policy.spins)
        {
            return; // spin
        }
        if (m_count <= m_policy.spins + m_policy.yields)
        {
            sched_yield();
            return;
        }
        timespec ts{ 0, m_policy.parkNs };
        nanosleep(&ts, nullptr);
    }

    const IdlePolicy &policy() const noexcept { return m_policy; }

private:
    IdlePolicy m_policy;
    int m_count = 0;
};

}}}

#endif
