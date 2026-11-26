#ifndef CLUSTER_TOOLS_RUNTIME_IDLE_STRATEGY_H
#define CLUSTER_TOOLS_RUNTIME_IDLE_STRATEGY_H

#include <sched.h>
#include <time.h>

#include <cstdint>

// The duty threads' idle strategy: after an iteration that did no work, spin
// `spins` times, then sched_yield `yields` times, then nanosleep `parkNs` (the only sleep, between iterations, never
// inside a step). `busySpin` instead never yields or sleeps (a CPU pause hint per idle iteration, no syscall). The
// named policies, one implementation:
//   FORWARDER_IDLE  spin 100, yield 100, park 100 us   (ct-forwarder, the backoff idle mode)
//   BRIDGE_IDLE     spin 0, yield 10, park 1 ms        (ct-bridge, the backoff idle mode: no busy spin on a shared box)
//   SLEEP_IDLE      spin 0, yield 0, park 1 ms         (the sleep idle mode of every process: Agrona's
//                                                       SleepingMillisIdleStrategy(1), the Java engines' sleep)
//   BUSY_SPIN_IDLE  never yields or sleeps             (the busy_spin idle mode of every process)
// The idle mode (IdleMode.h) picks one per process.

namespace cluster_tools { namespace runtime {

struct IdlePolicy
{
    int spins = 0;
    int yields = 0;
    long parkNs = 0;
    bool busySpin = false;
};

inline constexpr IdlePolicy FORWARDER_IDLE{ 100, 100, 100'000L, false };
inline constexpr IdlePolicy BRIDGE_IDLE{ 0, 10, 1'000'000L, false };
inline constexpr IdlePolicy SLEEP_IDLE{ 0, 0, 1'000'000L, false };
inline constexpr IdlePolicy BUSY_SPIN_IDLE{ 0, 0, 0L, true };

inline constexpr bool operator==(const IdlePolicy &a, const IdlePolicy &b) noexcept
{
    return a.spins == b.spins && a.yields == b.yields && a.parkNs == b.parkNs && a.busySpin == b.busySpin;
}

// A spin-wait hint to the core (x86 PAUSE, arm64 YIELD): not a scheduler call.
inline void cpuRelax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

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
        if (m_policy.busySpin)
        {
            cpuRelax(); // busy spin: never a yield, never a sleep (and no counter to overflow)
            return;
        }
        if (m_count <= m_policy.spins + m_policy.yields)
        {
            m_count++; // saturates past the spin + yield phases: a long idle stretch parks every time, never wraps
        }
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

}}

#endif
