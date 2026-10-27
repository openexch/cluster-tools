#include "cluster_tools/domain/forwarder/EpochBinder.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

namespace cluster_tools { namespace domain { namespace forwarder {

using test_seams::Rule;
using test_seams::disabled;

const char *nameOf(EpochVerdict verdict) noexcept
{
    switch (verdict)
    {
        case EpochVerdict::NONE: return "NONE";
        case EpochVerdict::BOUND: return "BOUND";
        case EpochVerdict::VERIFIED: return "VERIFIED";
        case EpochVerdict::FATAL_MISMATCH: return "FATAL_MISMATCH";
        case EpochVerdict::FATAL_ZERO: return "FATAL_ZERO";
        case EpochVerdict::RECONNECT: return "RECONNECT";
    }
    return "?";
}

void EpochBinder::onSessionStart(std::int64_t nowNs) noexcept
{
    m_active = true;
    if (!disabled(Rule::EPOCH_PER_SESSION))
    {
        m_verified = false;
    }
    m_reconnectReported = false;
    m_sessionStartNs = nowNs;
}

void EpochBinder::onSessionEnd() noexcept
{
    m_active = false;
    if (!disabled(Rule::EPOCH_PER_SESSION))
    {
        m_verified = false;
    }
}

EpochVerdict EpochBinder::onHeartbeat(std::int64_t meEpoch, std::int64_t) noexcept
{
    m_lastSeen = meEpoch;
    if (!m_active)
    {
        return EpochVerdict::NONE;
    }
    if (m_verified)
    {
        if (meEpoch == m_epoch)
        {
            return EpochVerdict::NONE;
        }
        if (0 == meEpoch)
        {
            return disabled(Rule::EPOCH_ZERO_FATAL) ? EpochVerdict::NONE : EpochVerdict::FATAL_ZERO;
        }
        return disabled(Rule::EPOCH_MISMATCH_FATAL) ? EpochVerdict::NONE : EpochVerdict::FATAL_MISMATCH;
    }
    if (0 == meEpoch && !disabled(Rule::EPOCH_ZERO_IGNORED))
    {
        return EpochVerdict::NONE; // the ME is not FRESH yet
    }
    if (0 == m_epoch)
    {
        m_epoch = meEpoch;
        m_verified = true;
        return EpochVerdict::BOUND;
    }
    if (meEpoch == m_epoch)
    {
        m_verified = true;
        return EpochVerdict::VERIFIED;
    }
    return disabled(Rule::EPOCH_MISMATCH_FATAL) ? EpochVerdict::NONE : EpochVerdict::FATAL_MISMATCH;
}

EpochVerdict EpochBinder::tick(std::int64_t nowNs) noexcept
{
    if (!m_active || m_verified || m_reconnectReported || disabled(Rule::EPOCH_WAIT))
    {
        return EpochVerdict::NONE;
    }
    if (nowNs - m_sessionStartNs >= EPOCH_WAIT_NS)
    {
        m_reconnectReported = true;
        return EpochVerdict::RECONNECT;
    }
    return EpochVerdict::NONE;
}

}}}
