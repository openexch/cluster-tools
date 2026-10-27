#include "cluster_tools/archive/aeron/SourceSet.h"

#include <stdexcept>

namespace cluster_tools { namespace archive { namespace aeron_archive {

SourceSet::SourceSet(const ArchiveSettings &settings, std::shared_ptr<::aeron::Aeron> aeron,
    std::shared_ptr<client::aeron_transport::AeronFatalMonitor> monitor, Invoker invoker, ArchiveMetrics &metrics,
    FollowObserver *observer, ArchiveSourceSeams seams, FollowSettings followOverrides, ExitAction exit) :
    m_metrics(metrics), m_memo(settings.archives.size(), &metrics)
{
    if (settings.archives.empty() || settings.archives.size() > MAX_SOURCES)
    {
        throw std::invalid_argument("SourceSet: 1.." + std::to_string(MAX_SOURCES) + " archives");
    }
    FollowSettings follow = followOverrides;
    follow.journalMtu = settings.journalMtu;
    follow.journalStreamId = settings.journalStreamId;
    for (std::size_t i = 0; i < settings.archives.size(); ++i)
    {
        ArchiveSourceConfig config;
        config.identity.endpoint = settings.archives[i].text();
        config.identity.controlStreamId = settings.controlStreamId;
        config.identity.journalStreamId = settings.journalStreamId;
        config.identity.channel = settings.journalChannel;
        config.localHost = settings.localHost;
        config.replayStreamId = settings.replayStreamId;
        m_followers.push_back(std::make_unique<ChainFollower>(
            i, config, follow, m_memo, aeron, monitor, invoker, &metrics, observer, seams, exit));
    }
}

void SourceSet::start(TimePoint now, std::size_t first)
{
    m_current = first;
    m_followers.at(m_current)->policy().attach(now);
}

int SourceSet::doWork(FrameSink &sink, int fragmentLimit, TimePoint now)
{
    ChainFollower &current = *m_followers[m_current];
    const int work = current.doWork(sink, fragmentLimit, now);
    if (m_pending && current.policy().idle() && !current.policy().wantsDisconnect())
    {
        m_current = *m_pending;
        m_pending.reset();
        m_followers[m_current]->policy().attach(now);
    }
    return work;
}

void SourceSet::switchTo(std::size_t index, TimePoint now)
{
    if (index >= m_followers.size())
    {
        throw std::out_of_range("SourceSet::switchTo: source " + std::to_string(index));
    }
    if (index == m_current && !m_pending)
    {
        return;
    }
    m_pending = index;
    m_followers[m_current]->policy().detach(now);
}

void SourceSet::reattach(TimePoint now)
{
    m_followers[m_current]->policy().reattach(now);
}

void SourceSet::stop(TimePoint now)
{
    m_pending.reset();
    m_followers[m_current]->policy().detach(now);
}

bool SourceSet::allRefusedForMtu() const
{
    for (std::size_t i = 0; i < m_followers.size(); ++i)
    {
        if (0 == m_metrics.sourceRefusedMtu[i].load(std::memory_order_relaxed))
        {
            return false;
        }
    }
    return true;
}

}}}
