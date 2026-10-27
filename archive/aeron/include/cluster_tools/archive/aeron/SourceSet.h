#ifndef CLUSTER_TOOLS_ARCHIVE_AERON_SOURCE_SET_H
#define CLUSTER_TOOLS_ARCHIVE_AERON_SOURCE_SET_H

// The configured archive sources in failover order, ONE attached at a time (RC2 S9a contract 2.1,
// 5.1): a ChainFollower per source, one ResumeMemo with a memo per source. The owner (S9b) decides
// failover; SourceSet executes it (switchTo: the current follower finishes its replay, stopReplay
// included, and disconnects before the next one attaches). With no owner verdict a faulted source
// backs off 1 s and re-attaches itself (cpp: P3 parity).

#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/archive/aeron/ChainFollower.h"

#include <memory>
#include <optional>
#include <vector>

namespace cluster_tools { namespace archive { namespace aeron_archive {

class SourceSet
{
public:
    SourceSet(const ArchiveSettings &settings, std::shared_ptr<::aeron::Aeron> aeron,
        std::shared_ptr<client::aeron_transport::AeronFatalMonitor> monitor, Invoker invoker, ArchiveMetrics &metrics,
        FollowObserver *observer, ArchiveSourceSeams seams = {}, FollowSettings followOverrides = {},
        ExitAction exit = &client::aeron_transport::AeronFatalMonitor::exitProcess);

    std::size_t size() const noexcept { return m_followers.size(); }
    std::size_t current() const noexcept { return m_current; }

    void start(TimePoint now, std::size_t first = 0);
    // 3.2 step 3: at most one archive control call, at most `fragmentLimit` fragments.
    int doWork(FrameSink &sink, int fragmentLimit, TimePoint now);

    // 5.4 step 2: every FeedPositionReport.
    void onReport(std::int64_t meEpoch, std::int64_t lastAppliedJournalSeq) { m_memo.onReport(meEpoch, lastAppliedJournalSeq); }
    void switchTo(std::size_t index, TimePoint now);
    void reattach(TimePoint now); // rewind the current source to its memo (fence, hole verification)
    void stop(TimePoint now);     // shutdown: finish the replay and disconnect

    // Section 7 row C2: every configured source refused for MTU_MISMATCH.
    bool allRefusedForMtu() const;

    ResumeMemo &memo() noexcept { return m_memo; }
    ChainFollower &follower(std::size_t index) { return *m_followers.at(index); }

private:
    ArchiveMetrics &m_metrics;
    ResumeMemo m_memo;
    std::vector<std::unique_ptr<ChainFollower>> m_followers;
    std::size_t m_current = 0;
    std::optional<std::size_t> m_pending;
};

}}}

#endif
