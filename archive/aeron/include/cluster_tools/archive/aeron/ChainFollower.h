#ifndef CLUSTER_TOOLS_ARCHIVE_AERON_CHAIN_FOLLOWER_H
#define CLUSTER_TOOLS_ARCHIVE_AERON_CHAIN_FOLLOWER_H

// One source follower on Aeron (RC2 S9a contract 2.1): the pure FollowPolicy wired to one
// ArchiveSource by the same FollowerLoop the pure tests drive. doWork(sink, limit) is bounded: at
// most `limit` fragments (0 = paused, none) and at most one archive control call, so a paused or
// fenced follower still probes (3.2 step 3).

#include "cluster_tools/archive/FollowerLoop.h"
#include "cluster_tools/archive/aeron/ArchiveSource.h"

#include <memory>

namespace cluster_tools { namespace archive { namespace aeron_archive {

class ChainFollower
{
public:
    ChainFollower(std::size_t sourceIndex, ArchiveSourceConfig config, FollowSettings settings, ResumeMemo &memo,
        std::shared_ptr<::aeron::Aeron> aeron, std::shared_ptr<client::aeron_transport::AeronFatalMonitor> monitor,
        Invoker invoker, ArchiveMetrics *metrics, FollowObserver *observer, ArchiveSourceSeams seams = {},
        ExitAction exit = &client::aeron_transport::AeronFatalMonitor::exitProcess) :
        m_source(std::move(config), std::move(aeron), std::move(monitor), std::move(invoker), metrics, std::move(seams),
            std::move(exit)),
        m_policy(sourceIndex, settings, memo, metrics, observer),
        m_loop(m_source, m_policy, metrics)
    {
    }

    int doWork(FrameSink &sink, int fragmentLimit, TimePoint now) { return m_loop.doWork(sink, fragmentLimit, now); }

    ArchiveSource &source() noexcept { return m_source; }
    FollowPolicy &policy() noexcept { return m_policy; }

private:
    ArchiveSource m_source;
    FollowPolicy m_policy;
    FollowerLoop<ArchiveSource> m_loop;
};

}}}

#endif
