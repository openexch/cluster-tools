#ifndef CLUSTER_TOOLS_APP_BRIDGE_OBSERVER_TAP_H
#define CLUSTER_TOOLS_APP_BRIDGE_OBSERVER_TAP_H

#include <array>
#include <cstdint>

#include "cluster_tools/archive/Follow.h"

#include "AppSeams.h"
#include "BridgeSnapshot.h"

// ObserverTap - the FollowObserver given to the SourceSet: it counts every onFault
// by FaultKind (bridge_source_faults_total{kind}) and forwards EVERY call to the domain unchanged and in order. It
// also remembers each source's last fault (the stop path detaches a dead source without stopReplay). It is
// built before the SourceSet (which needs the observer at construction) and bound to the domain by the agent; no call
// reaches it before the first sources.start().

namespace cluster_tools { namespace app { namespace bridge {

static_assert(static_cast<std::size_t>(archive::FaultKind::ARCHIVE_ERROR) + 1 == FAULT_KINDS);

class ObserverTap final : public archive::FollowObserver
{
public:
    void bind(archive::FollowObserver *target) noexcept { m_target = target; }

    void onFault(const archive::SourceFault &fault) noexcept override
    {
        const std::size_t kind = static_cast<std::size_t>(fault.kind);
        if (kind < FAULT_KINDS)
        {
            m_faults[kind]++;
        }
        if (fault.source < m_lastFault.size())
        {
            m_lastFault[fault.source] = fault.kind;
        }
        if (app_seams::disabled(app_seams::Rule::TAP_SWALLOWS))
        {
            return; // RED shape: counted but not forwarded
        }
        if (nullptr != m_target)
        {
            m_target->onFault(fault);
        }
    }
    void onAttached(const archive::AttachInfo &info) noexcept override
    {
        if (info.source < m_lastFault.size())
        {
            m_lastFault[info.source] = archive::FaultKind::NONE;
        }
        if (nullptr != m_target)
        {
            m_target->onAttached(info);
        }
    }
    void onRecordingEnded(std::size_t source, const archive::Recording &recording) noexcept override
    {
        if (nullptr != m_target)
        {
            m_target->onRecordingEnded(source, recording);
        }
    }
    void onChainEmpty(std::size_t source) noexcept override
    {
        if (nullptr != m_target)
        {
            m_target->onChainEmpty(source);
        }
    }
    void onChainExhausted(std::size_t source) noexcept override
    {
        if (nullptr != m_target)
        {
            m_target->onChainExhausted(source);
        }
    }
    void onSourceRefused(std::size_t source, const archive::Recording &recording) noexcept override
    {
        if (nullptr != m_target)
        {
            m_target->onSourceRefused(source, recording);
        }
    }

    const std::array<std::uint64_t, FAULT_KINDS> &faults() const noexcept { return m_faults; }
    archive::FaultKind lastFault(std::size_t source) const noexcept
    {
        return source < m_lastFault.size() ? m_lastFault[source] : archive::FaultKind::NONE;
    }
    // Nothing to stop on a source whose archive is gone or never answered.
    bool sourceIsDead(std::size_t source) const noexcept
    {
        const archive::FaultKind k = lastFault(source);
        return archive::FaultKind::ARCHIVE_GONE == k || archive::FaultKind::CONNECT_FAILED == k ||
            archive::FaultKind::NEVER_CONNECTED == k;
    }

private:
    archive::FollowObserver *m_target = nullptr;
    std::array<std::uint64_t, FAULT_KINDS> m_faults{};
    std::array<archive::FaultKind, archive::MAX_SOURCES> m_lastFault{};
};

}}}

#endif
