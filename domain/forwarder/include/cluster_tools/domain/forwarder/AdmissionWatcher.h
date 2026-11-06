#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_ADMISSION_WATCHER_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_ADMISSION_WATCHER_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/TestSeams.h"

// AdmissionWatcher - trigger T3 (RC2 S12 contract 6.2, W16, S8 3.5, D-S12-4): every BROADCAST GLOBAL
// AdmissionState (marketId 0) whose (gateState, gateReasons) differs from the baseline fires a RESYNC(0),
// so the core's CAPACITY'd dispatches are re-offered. An ORIGIN AdmissionState (the RegisterSession reply)
// only resets the baseline and never fires. Per-market states are not triggers (Q9). A new core session
// starts with no baseline; a BROADCAST seen without a baseline sets it and fires (the conservative side:
// a RESYNC(0) is always safe, a missed one may strand a CAPACITY'd dispatch).

namespace cluster_tools { namespace domain { namespace forwarder {

class AdmissionWatcher
{
public:
    void reset() noexcept { m_hasBaseline = false; }

    // Always false (an ORIGIN never fires T3); true only in a seamed build with ADMISSION_ORIGIN_BASELINE off.
    bool onOrigin(std::int32_t marketId, std::uint8_t gateState, std::uint32_t gateReasons) noexcept
    {
        if (0 != marketId && !test_seams::disabled(test_seams::Rule::ADMISSION_GLOBAL_ONLY))
        {
            return false;
        }
        if (test_seams::disabled(test_seams::Rule::ADMISSION_ORIGIN_BASELINE))
        {
            return onBroadcast(marketId, gateState, gateReasons);
        }
        set(gateState, gateReasons);
        return false;
    }

    // True = T3.
    bool onBroadcast(std::int32_t marketId, std::uint8_t gateState, std::uint32_t gateReasons) noexcept
    {
        if (0 != marketId && !test_seams::disabled(test_seams::Rule::ADMISSION_GLOBAL_ONLY))
        {
            return false;
        }
        const bool reasonBits = !test_seams::disabled(test_seams::Rule::ADMISSION_REASON_BITS);
        const bool changed = !m_hasBaseline || gateState != m_state || (reasonBits && gateReasons != m_reasons);
        set(gateState, gateReasons);
        return changed;
    }

    bool hasBaseline() const noexcept { return m_hasBaseline; }
    std::uint8_t state() const noexcept { return m_state; }
    std::uint32_t reasons() const noexcept { return m_reasons; }

private:
    void set(std::uint8_t gateState, std::uint32_t gateReasons) noexcept
    {
        m_hasBaseline = true;
        m_state = gateState;
        m_reasons = gateReasons;
    }

    bool m_hasBaseline = false;
    std::uint8_t m_state = 0;
    std::uint32_t m_reasons = 0;
};

}}}

#endif
