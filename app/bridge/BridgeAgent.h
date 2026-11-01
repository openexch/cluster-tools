#ifndef CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_AGENT_H
#define CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_AGENT_H

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "cluster_tools/adapters/sbe/BridgeCodec.h"
#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/archive/Follow.h"
#include "cluster_tools/archive/ResumeMemo.h"
#include "cluster_tools/client/Backoff.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "cluster_tools/domain/bridge/BridgeDomain.h"
#include "cluster_tools/domain/bridge/BridgeMetrics.h"
#include "cluster_tools/domain/bridge/BridgeSettings.h"

#include "common/LogRing.h"
#include "common/StatusBoardT.h"
#include "common/StorageStarvation.h"

#include "AppSeams.h"
#include "StopBounds.h"
#include "BridgeSnapshot.h"
#include "CorePortAdapter.h"
#include "ObserverTap.h"

// BridgeAgent - the bridge-duty thread's agent (RC2 S9c sections 3-7): one duty iteration drives the S11 core client,
// the CoreSessionRegistrar, the archive SourceSet and the S9b BridgeDomain in the frozen order of 3.2. Header-only and
// templated on the core client and the sources, so the app tests run it over FakeTransport + SimSources and
// production over AeronTransport + SourceSet. Single threaded: only thread B touches it.
//
// G1: the domain decides, the app wires. The agent never offers a feed frame, never reads a record byte, never moves
// `last` or the cursor; the egress it dispatches goes to the registrar or the domain, the frames it sees after an OK
// offer feed two observability histograms only. G5: onSessionUp only at a C3 entry (registrar.isRegistered() for the
// client's current session and term); the NewLeader fence inside the client's callback, before any later step of the
// same iteration can offer. onStateChange only RECORDS (it can fire from doWork() inside the archive invoker, where no
// domain call is allowed); the link machine runs in step 2 only.

namespace cluster_tools { namespace app { namespace bridge {

inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_SOFTWARE = 70;
inline constexpr int EXIT_TEMPFAIL = 75;
inline constexpr int EXIT_CONFIG = 78;
// the core's AdminAck reason for a second live registered BRIDGE session holding an identity (RC2 S10' 4R.1 rule 2)
inline constexpr std::uint8_t REJECT_DUPLICATE_IDENTITY = 44;


struct StopReason
{
    int exitCode = -1;
    char code[32] = {};
    char detail[232] = {};

    bool set() const noexcept { return exitCode >= 0; }
};

// The client observer: a pointer hop so the client type does not depend on the agent type.
class LinkEvents
{
public:
    virtual ~LinkEvents() = default;
    virtual void onClientStateChange(client::ClientState from, client::ClientState to) noexcept = 0;
    virtual void onClientNewLeader(const client::NewLeaderInfo &info) noexcept = 0;
};

struct LinkObserver
{
    LinkEvents *target = nullptr;
    void onStateChange(client::ClientState from, client::ClientState to)
    {
        if (nullptr != target)
        {
            target->onClientStateChange(from, to);
        }
    }
    void onNewLeader(const client::NewLeaderInfo &info)
    {
        if (nullptr != target)
        {
            target->onClientNewLeader(info);
        }
    }
};

struct AgentSettings
{
    std::int64_t identity = 0;                       // BRIDGE_INSTANCE_ID
    std::size_t sourceCount = 1;                     // |BRIDGE_ME_JOURNAL_ARCHIVES|
    std::chrono::milliseconds probeInterval{ 10'000 }; // = the FollowSettings object the SourceSet was given
    client::SessionClass credentialClass = client::SessionClass::BRIDGE;
    const char *version = "dev";
    std::uint8_t driverMode = 0;
    std::int64_t reconnectBackoffInitialNs = 1'000'000'000LL;
    std::int64_t reconnectBackoffMaxNs = 30'000'000'000LL;
    std::int64_t registrarResendNs = 5'000'000'000LL;
    // RC2 S10' (coordinator decisions 2026-11-01): a REJECTED DUPLICATE_IDENTITY (the old session of this identity is still
    // registered at the core until its session timeout, e.g. after a kill -9; the core's 10 s clock restarts after an
    // election) is retried with a bounded backoff for 60 s (2 x the 10 s timeout plus a slow election), then exits 78 as
    // a genuine duplicate BRIDGE_INSTANCE_ID
    std::int64_t duplicateIdentityBudgetNs = 60'000'000'000LL;
    std::int64_t duplicateIdentityRetryInitialNs = 500'000'000LL;
    std::int64_t duplicateIdentityRetryMaxNs = 4'000'000'000LL;
    // Tests only: the domain's timers (S9b 4: not operator knobs) and its correlation base.
    std::function<void(domain::bridge::BridgeSettings &)> tweakDomain;
    // The epoch-ms clock of O3 (default: the system clock) and the 1 ms park of the invoker (default: sleep 1 ms).
    std::function<std::int64_t()> epochMs;
    std::function<void()> park;
};

namespace detail {

template<typename S>
std::size_t currentOf(S &s)
{
    if constexpr (requires { s.current(); })
    {
        return s.current();
    }
    else
    {
        return s.current;
    }
}

template<typename S>
void stopSources(S &s, archive::TimePoint now)
{
    if constexpr (requires { s.stop(now); })
    {
        s.stop(now);
    }
    else
    {
        s.pending.reset();
        s.policies[s.current]->detach(now);
    }
}

template<typename S>
bool currentIdle(S &s)
{
    if constexpr (requires { s.currentIdle(); })
    {
        return s.currentIdle();
    }
    else if constexpr (requires { s.follower(std::size_t{ 0 }); })
    {
        return s.follower(s.current()).policy().idle();
    }
    else
    {
        return s.policies[s.current]->idle();
    }
}

inline std::int64_t systemEpochMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

template<typename CoreClient, typename Sources>
class BridgeAgent final : public LinkEvents, public OfferObserver
{
public:
    static constexpr int EGRESS_POLL_LIMIT = 16;
    static constexpr std::size_t VERDICT_DRAIN = 32;
    static constexpr std::uint32_t CHANNELS = client::core_channel::ACKS | client::core_channel::SETTLEMENTS;
    static constexpr std::int64_t STOP_DRAIN_NS = 1'000'000'000LL; // 3.4: the stop drain is bounded by time
    static constexpr std::int64_t STOP_EXIT_MARGIN_NS = 500'000'000LL; // the close and the exit after the drain

    using Registrar = client::CoreSessionRegistrar<CoreClient>;
    using TimePoint = archive::TimePoint;

    // `memo`: the one ResumeMemo the sources follow with (production: sources.memo()). `tap`: the observer the
    // sources were built with; the agent binds it to its domain. `heartbeat`: the duty heartbeat (the invoker beats it
    // during a blocked archive call).
    BridgeAgent(CoreClient &client, Sources &sources, ObserverTap &tap, archive::ResumeMemo &memo,
        archive::ArchiveMetrics &archiveMetrics, domain::bridge::FrameEncoder &encoder, AgentSettings settings,
        common::LogRing *log = nullptr, common::DutyHeartbeat *heartbeat = nullptr) :
        m_client(client),
        m_sources(sources),
        m_tap(tap),
        m_archive(archiveMetrics),
        m_settings(std::move(settings)),
        m_log(log),
        m_heartbeat(heartbeat),
        m_port(client, m_counters, this),
        m_domainSettings(domainSettings(m_settings)),
        m_domain(m_domainSettings, memo, archiveMetrics, encoder, m_port, m_metrics),
        m_registrar(client, registrarSettings(m_settings)),
        m_backoff(m_settings.reconnectBackoffInitialNs, m_settings.reconnectBackoffMaxNs),
        m_negctlSink(m_domain)
    {
        if (!m_settings.epochMs)
        {
            m_settings.epochMs = &detail::systemEpochMs;
        }
        m_tap.bind(&m_domain);
        if (const std::string error = m_registrar.validateSettings(); !error.empty())
        {
            stop(EXIT_CONFIG, "C1_REGISTRAR", "%s", error.c_str());
        }
    }

    BridgeAgent(const BridgeAgent &) = delete;
    BridgeAgent &operator=(const BridgeAgent &) = delete;

    // ---- 3.3 step 7: the sources start at source 0; the link machine connects on the first iteration
    void start()
    {
        updateNow();
        m_consumedChangedAt = m_now;
        m_sources.start(m_now, 0);
    }

    // ---- control (duty thread)
    // requestedAtNs: steady-clock ns of the signal (main's), so finish() knows how much of the join bound is left; -1 = now
    void requestStop(std::int64_t requestedAtNs = -1) noexcept
    {
        if (!m_stopRequested)
        {
            m_stopRequested = true;
            m_stopRequestedAtNs = requestedAtNs >= 0 ? requestedAtNs : steadyNs();
        }
    }
    bool stopped() const noexcept { return m_stopped; }
    const StopReason &stopReason() const noexcept { return m_stop; }

    // One duty iteration (3.2); the amount of work done (0 = idle).
    int doWork()
    {
        if (m_stopped)
        {
            return 0;
        }
        updateNow();
        const std::int64_t t0 = m_nowNs;
        m_lastInvokeNs = m_nowNs;
        if (m_stopRequested || m_stop.set())
        {
            finish();
            return 1;
        }
        int work = 0;
        // 1. egress: rule S, the registrar first, the codec, the domain; the NewLeader fence inside the callback
        work += m_client.poll([this](const client::EgressMessage &m) { onEgress(m); }, EGRESS_POLL_LIMIT);
        if (m_fencePending) // FENCE_DEFERRED RED shape only
        {
            m_fencePending = false;
            domainCall();
            m_domain.onFence(domain::bridge::FenceTrigger::NEW_LEADER, m_now);
        }
        // 2. client and registrar, then the link machine (the ONLY place it runs)
        work += m_client.doWork();
        work += m_registrar.doWork();
        updateNow();
        linkStep();
        if (m_stop.set())
        {
            finish();
            return work + 1;
        }
        // 3. at most one archive control call, at most fragmentLimit() fragments
        const int limit = m_domain.fragmentLimit();
        const int n = app_seams::disabled(app_seams::Rule::NEGCTL_DROP_RECORDS_2_3) ?
            m_sources.doWork(m_negctlSink, limit, m_now) :
            m_sources.doWork(m_domain, limit, m_now);
        work += n;
        // 4.
        m_domain.onPollDone(n, m_now);
        // 5.
        const domain::bridge::SourceCommand cmd = m_domain.takeSourceCommand();
        if (domain::bridge::SourceCommandKind::SWITCH == cmd.kind)
        {
            m_sources.switchTo(cmd.index, m_now);
            work++;
        }
        else if (domain::bridge::SourceCommandKind::REATTACH == cmd.kind)
        {
            m_sources.reattach(m_now);
            work++;
        }
        // 6.
        m_domain.tick(m_now);
        // 7.
        drainVerdicts();
        trackProgress();
        superviseStorage();
        if (domain::bridge::Fatal::NONE != m_domain.fatal())
        {
            stop(EXIT_SOFTWARE, "F5_DOMAIN_FATAL", "the domain is fatal: %s (the gate was closed by its fallback)",
                domain::bridge::fatalName(m_domain.fatal()));
        }
        if (!m_stop.set() && allRefusedForMtu())
        {
            char sources[160] = {};
            std::size_t at = 0;
            for (std::size_t i = 0; i < m_settings.sourceCount && at + 16 < sizeof(sources); ++i)
            {
                at += static_cast<std::size_t>(std::snprintf(sources + at, sizeof(sources) - at, "%s%zu", i ? "," : "", i));
            }
            stop(EXIT_CONFIG, "C2_MTU", "every configured source (%s) was refused for MTU_MISMATCH against "
                "BRIDGE_JOURNAL_MTU (6.5): fix the recording MTU or BRIDGE_JOURNAL_MTU", sources);
        }
        updateNow();
        archive::ArchiveMetrics::raiseMax(m_archive.dutyIterationMaxUs,
            static_cast<std::uint64_t>((m_nowNs - t0) > 0 ? (m_nowNs - t0) / 1000 : 0));
        return work;
    }

    // RC2 S10' Q1 (coordinator decision 2026-11-01): the core ingress publication refused for storage space on every
    // connect attempt, or every replay image refused for storage (the S9a precheck or the archive's STORAGE_SPACE) with
    // no source FOLLOWING, held for STORAGE_STARVED_EXIT_NS (30 s, a constant) ends the process 70: restartable and loud,
    // never alive-not-ready forever (the 2026-07-25 /dev/shm shape), never 78. Until then /ready names storage_starved
    // and bridge_storage_starved_seconds_total grows. A replay attempt that fails for another reason, or any source
    // FOLLOWING, ends a replay episode; a core attempt that fails otherwise, or a connected client, ends a core one.
    void superviseStorage()
    {
        if (m_coreStorageFeed.update(m_client, client::ClientState::CONNECTED == m_client.state(), m_nowNs, m_coreStorage))
        {
            log(common::LogLevel::ERROR, "core ingress publication refused: insufficient storage space in the media driver "
                "dir (exit 70 if it lasts 30 s): %s", m_coreStorage.why());
        }
        using archive::OpenFailure;
        const std::uint64_t storage = m_archive.openFailures(OpenFailure::STORAGE) + m_archive.openFailures(OpenFailure::STORAGE_SPACE);
        std::uint64_t other = 0;
        for (std::size_t f = 0; f < static_cast<std::size_t>(OpenFailure::COUNT); ++f)
        {
            other += m_archive.openFailures(static_cast<OpenFailure>(f));
        }
        other -= storage;
        bool anyFollowing = false;
        for (std::size_t i = 0; i < m_settings.sourceCount && i < archive::MAX_SOURCES; ++i)
        {
            anyFollowing = anyFollowing || 0 != archive::ArchiveMetrics::get(m_archive.sourceAttached[i]);
        }
        if (storage != m_seenReplayStorage)
        {
            m_seenReplayStorage = storage;
            if (m_replayStorage.onStarved(m_nowNs, "a replay image needs more free space than the media driver dir has "
                    "(REPLAY_STORAGE / STORAGE_SPACE)"))
            {
                log(common::LogLevel::ERROR, "replay image refused: insufficient storage space in the media driver dir "
                    "(exit 70 if no source can follow for 30 s)");
            }
        }
        else if (other != m_seenReplayOther)
        {
            m_replayStorage.onRelieved(m_nowNs);
        }
        m_seenReplayOther = other;
        if (anyFollowing)
        {
            m_replayStorage.onRelieved(m_nowNs);
        }
        if (m_coreStorage.active() || m_replayStorage.active())
        {
            m_anyStorage.onStarved(m_nowNs, "");
        }
        else
        {
            m_anyStorage.onRelieved(m_nowNs);
        }
        if (app_seams::disabled(app_seams::Rule::STORAGE_STARVED_EXIT_OFF))
        {
            return;
        }
        for (const common::StorageStarvation *w : { &m_coreStorage, &m_replayStorage })
        {
            if (w->expired(m_nowNs))
            {
                stop(EXIT_SOFTWARE, "F_STORAGE_STARVED", "%s for %lld s on every attempt (the media driver dir's tmpfs is "
                    "too small or full): %s", w == &m_coreStorage ? "the core ingress publication was refused for storage space"
                    : "every replay image was refused for storage space", static_cast<long long>(w->heldNs(m_nowNs) / 1'000'000'000LL),
                    w->why());
                return;
            }
        }
    }

    // The invoker given to the SourceSet (S9a 3.3): runs inside a blocked archive call, after every yield of the
    // archive client. client.doWork() (keepalives, deadlines), the heartbeat, the gap gauge, park 1 ms; never
    // poll(), never an offer, never an archive call, never the domain.
    void invoke()
    {
        m_inInvoker = true;
        m_client.doWork();
        m_inInvoker = false;
        if (!app_seams::disabled(app_seams::Rule::INVOKER_NO_BEAT) && nullptr != m_heartbeat)
        {
            m_heartbeat->beat();
        }
        updateNow();
        const std::int64_t gapNs = m_nowNs - m_lastInvokeNs;
        archive::ArchiveMetrics::raiseMax(m_archive.invokerMaxGapMs,
            static_cast<std::uint64_t>(gapNs > 0 ? gapNs / 1'000'000 : 0));
        m_lastInvokeNs = m_nowNs;
        m_invokes++;
        if (m_settings.park)
        {
            m_settings.park();
        }
    }

    // ---- the snapshot for the StatusBoard (every 100 ms) and the tests
    void fillSnapshot(BridgeSnapshot &s) const
    {
        s.nowNs = m_nowNs;
        s.identity = m_settings.identity;
        std::snprintf(s.version, sizeof(s.version), "%s", m_settings.version);
        s.driverMode = m_settings.driverMode;
        s.coreLink = static_cast<std::uint8_t>(m_link);
        s.clientState = static_cast<std::uint8_t>(m_client.state());
        s.closeReason = static_cast<std::uint8_t>(m_client.closeReason());
        s.aeConnected = client::ClientState::CONNECTED == m_client.state();
        s.registered = CoreLink::C3_REGISTERED == m_link;
        s.stopping = m_stopRequested || m_stop.set();
        s.view = m_domain.view();
        s.notReady = notReadyMask();
        s.reportAgeMs = TimePoint{} == s.view.lastReportAt ? -1 : ageMs(s.view.lastReportAt);
        s.consumedPositionAgeMs = ageMs(m_consumedChangedAt);
        s.lastForwardAgeMs = TimePoint{} == s.view.lastForwardAt ? -1 : ageMs(s.view.lastForwardAt);
        s.stormCount = m_domain.stormCount();
        s.confirming = m_domain.confirming();
        s.laneAheadFences = m_metrics.fenceCount(domain::bridge::FenceTrigger::LANE_AHEAD);
        s.syncs = m_domain.syncs();
        const client::ClientCounters &cc = m_client.counters();
        s.client.sessionsOpened = cc.sessionsOpened;
        s.client.newLeaderEvents = cc.newLeaderEvents;
        s.client.keepAlivesSent = cc.keepAlivesSent;
        s.client.keepAliveFailures = cc.keepAliveFailures;
        s.client.offersInEndedTerms = cc.offersInEndedTerms;
        s.client.egressImageLosses = cc.egressImageLosses;
        const client::RegistrarCounters &rc = m_registrar.counters();
        s.registrar.acksAccepted = rc.acksAccepted;
        s.registrar.acksDuplicate = rc.acksDuplicate;
        s.registrar.acksRejected = rc.acksRejected;
        s.registrar.closesWhilePending = rc.closesWhilePending;
        s.registrar.resends = rc.resends;
        s.registrar.sends = rc.sends;
        s.app = m_counters;
        s.app.storageStarvedSeconds = m_anyStorage.starvedSeconds(m_nowNs); // wall seconds with either starved
        s.app.sourceFaults = m_tap.faults();
        s.app.ackMapSkips = m_ackMap.skips();
        s.app.ackUnknown = m_ackMap.unknown();
        s.forwardLatency = m_forward;
        s.ackLatency = m_ackLatency;
        s.logDropped = nullptr == m_log ? 0 : m_log->dropped();
    }

    std::uint32_t notReadyMask() const noexcept
    {
        ReadyInputs in;
        in.link = m_link;
        in.view = m_domain.view();
        in.now = m_now;
        in.progressAt = m_consumedChangedAt > in.view.lastForwardAt ? m_consumedChangedAt : in.view.lastForwardAt;
        in.progressAt = m_syncCursorChangedAt > in.progressAt ? m_syncCursorChangedAt : in.progressAt; // 3.6
        in.forwardAt = TimePoint{} == in.view.lastForwardAt ? m_startedAt : in.view.lastForwardAt;
        in.backlogBytes = m_archive.sourceBacklogBytes.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < m_settings.sourceCount && i < archive::MAX_SOURCES; ++i)
        {
            in.anySourceAttached = in.anySourceAttached ||
                0 != archive::ArchiveMetrics::get(m_archive.sourceAttached[i]);
        }
        in.stopping = m_stopRequested || m_stop.set();
        in.storageStarved = m_coreStorage.active() || m_replayStorage.active();
        in.reportInterval = m_domainSettings.reportInterval;
        in.reportIntervalHalted = m_domainSettings.reportIntervalHalted;
        return notReadyBits(in);
    }
    bool ready() const noexcept { return 0 == notReadyMask(); }

    CoreLink link() const noexcept { return m_link; }
    const domain::bridge::BridgeDomain &domain() const noexcept { return m_domain; }
    domain::bridge::BridgeDomain &domainForTest() noexcept { return m_domain; }
    const domain::bridge::BridgeMetrics &metrics() const noexcept { return m_metrics; }
    const Registrar &registrar() const noexcept { return m_registrar; }
    const AppCounters &counters() const noexcept { return m_counters; }
    const AckMap &ackMap() const noexcept { return m_ackMap; }
    const LatencyHistogram &forwardLatency() const noexcept { return m_forward; }
    const LatencyHistogram &ackLatency() const noexcept { return m_ackLatency; }
    const domain::bridge::BridgeSettings &domainSettings() const noexcept { return m_domainSettings; }
    std::int64_t snapshotReportAgeMs() const noexcept
    {
        const TimePoint at = m_domain.view().lastReportAt;
        return TimePoint{} == at ? -1 : ageMs(at);
    }
    std::uint64_t domainCallsInsideInvoker() const noexcept { return m_domainCallsInInvoker; }
    std::uint64_t invokes() const noexcept { return m_invokes; }
    bool stateChangeRecorded() const noexcept { return m_stateChanged; }
    TimePoint now() const noexcept { return m_now; }

    // ---- LinkEvents (the client's observer, thread B)
    void onClientStateChange(client::ClientState, client::ClientState) noexcept override
    {
        m_stateChanged = true; // records only (FIX-2): the link machine reads the client state in step 2
        if (app_seams::disabled(app_seams::Rule::LINK_MACHINE_IN_CALLBACK))
        {
            linkStep(); // RED shape: the machine runs here, also inside the archive invoker
        }
    }

    void onClientNewLeader(const client::NewLeaderInfo &info) noexcept override
    {
        log(common::LogLevel::INFO, "core NewLeaderEvent: term %lld -> %lld, leader %d (offers in the ended term %llu)",
            static_cast<long long>(info.previousLeadershipTermId), static_cast<long long>(info.leadershipTermId),
            info.leaderMemberId, static_cast<unsigned long long>(info.offersInEndedTerm));
        if (app_seams::disabled(app_seams::Rule::FENCE_WAITS_REPORT_OFF))
        {
            domainCall();
            m_domain.onSessionUp(m_now); // RED shape: a leader change re-ups the session instead of fencing
            return;
        }
        if (app_seams::disabled(app_seams::Rule::FENCE_DEFERRED))
        {
            m_fencePending = true; // RED shape: the fence waits until poll() returned
        }
        else
        {
            domainCall();
            m_domain.onFence(domain::bridge::FenceTrigger::NEW_LEADER, m_now); // G5, 6.1: at the event
        }
        resetUp();
        if (CoreLink::C3_REGISTERED == m_link)
        {
            m_c3BeforeC4 = true;
            m_link = CoreLink::C2_CONNECTED; // the term changed: the registration must be re-observed
        }
    }

    // ---- OfferObserver (O3, O4): after an OK offer of a 61 / 62
    void onOfferOk(const std::uint8_t *frame, std::size_t length) noexcept override
    {
        if (length < 4)
        {
            return;
        }
        const std::uint16_t templateId = static_cast<std::uint16_t>(frame[2] | (frame[3] << 8));
        if (domain::bridge::T_ME_JOURNAL_TRADE != templateId && domain::bridge::T_ME_JOURNAL_TRADE_BATCH != templateId)
        {
            return;
        }
        m_offerEpochMs = m_settings.epochMs();
        sbe_adapter::bridge_codec::BridgeCodec::visitTrades(frame, length, &BridgeAgent::onTradeOffered, this);
    }

private:
    struct UpKey
    {
        bool valid = false;
        std::uint64_t sessionEpoch = 0;
        std::int64_t term = -1;
    };

    // A FrameSink that drops record templates 2 and 3 before the domain (ct_bridge_negctl only, the A02-T2 control).
    class NegctlSink final : public archive::FrameSink
    {
    public:
        explicit NegctlSink(domain::bridge::BridgeDomain &domain) noexcept : m_domain(domain) {}
        archive::SinkAction onFrame(const archive::JournalFrame &frame) noexcept override
        {
            if (2 == frame.peek.templateId || 3 == frame.peek.templateId)
            {
                return archive::SinkAction::CONTINUE;
            }
            return m_domain.onFrame(frame);
        }
        archive::RefusalAction onRefused(const archive::RefusedFrame &frame) noexcept override
        {
            return m_domain.onRefused(frame);
        }

    private:
        domain::bridge::BridgeDomain &m_domain;
    };

    static domain::bridge::BridgeSettings domainSettings(const AgentSettings &a)
    {
        domain::bridge::BridgeSettings s;
        s.identity = a.identity;
        s.sourceCount = a.sourceCount;
        s.probeInterval = a.probeInterval;
        if (a.tweakDomain)
        {
            a.tweakDomain(s);
            s.identity = a.identity;
            s.sourceCount = a.sourceCount;
        }
        return s;
    }

    static client::RegistrarSettings registrarSettings(const AgentSettings &a)
    {
        client::RegistrarSettings r;
        r.sessionClass = client::SessionClass::BRIDGE;
        r.credentialClass = a.credentialClass;
        r.channels = CHANNELS;
        r.readerKind = 0;
        r.resumable = true;
        r.identity = a.identity;
        r.sourceEpoch = 0; // S7 assumption 14: D8a leaves sourceEpoch 0 unchecked for the bridge
        r.resendIntervalNs = a.registrarResendNs;
        r.maxUnansweredSessions = 3;
        return r;
    }

    static void onTradeOffered(void *context, std::int64_t tradeId, std::int64_t meTimestampMs)
    {
        auto *self = static_cast<BridgeAgent *>(context);
        recordForwardLatency(self->m_forward, self->m_counters.clockAnomalies, self->m_offerEpochMs, meTimestampMs);
        self->m_ackMap.onOffered(tradeId, self->m_nowNs);
    }

    void updateNow()
    {
        m_nowNs = m_client.nanoTime();
        m_now = TimePoint(std::chrono::nanoseconds(m_nowNs));
        if (TimePoint{} == m_startedAt)
        {
            m_startedAt = m_now;
        }
    }

    std::int64_t ageMs(TimePoint at) const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(m_now - at).count();
    }

    void domainCall() noexcept
    {
        if (m_inInvoker)
        {
            m_domainCallsInInvoker++;
        }
    }

    void resetUp() noexcept
    {
        if (!app_seams::disabled(app_seams::Rule::UP_FLAG_NEVER_RESET))
        {
            m_upReportedFor = UpKey{};
        }
    }

    void log(common::LogLevel level, const char *format, ...) __attribute__((format(printf, 3, 4)))
    {
        if (nullptr == m_log)
        {
            return;
        }
        char text[240];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        m_log->push(level, m_nowNs, "%s", text);
    }

    void stop(int exitCode, const char *code, const char *format, ...) __attribute__((format(printf, 4, 5)))
    {
        if (m_stop.set())
        {
            return; // the first reason wins
        }
        m_stop.exitCode = app_seams::disabled(app_seams::Rule::EXIT_CODES_ALL_70) ? EXIT_SOFTWARE : exitCode;
        std::snprintf(m_stop.code, sizeof(m_stop.code), "%s", code);
        va_list args;
        va_start(args, format);
        std::vsnprintf(m_stop.detail, sizeof(m_stop.detail), format, args);
        va_end(args);
        log(EXIT_SOFTWARE == m_stop.exitCode ? common::LogLevel::CRITICAL : common::LogLevel::ERROR, "STOP %s exit %d: %s",
            code, m_stop.exitCode, m_stop.detail);
    }

    // 3.4: step 3 with limit 0 only, flush nothing (G1, G2), one SessionCloseRequest, then the sources: a dead
    // source is detached without stopReplay (nothing to stop, FIX-7), a live one finishes its replay within 1 s.
    // Review FIX-3 (2026-11-01): a requested stop must return 0 within the join bound (7 s) even when it landed during a
    // blocked archive call. A stop older than (join bound - archive timeout) at a point where the next step may issue an
    // archive call skips that step: the limit-0 step 3 and stopReplay are not run, the source is detached (the replay
    // ends with this process, the next start re-lists; nothing is acknowledged or flushed either way, G1 / G2), and the
    // drain is bounded by what is left of the join bound.
    bool stopIsLate() const noexcept
    {
        return m_stopRequested && !app_seams::disabled(app_seams::Rule::STOP_DETACH_WHEN_LATE) &&
            steadyNs() - m_stopRequestedAtNs >= STOP_DETACH_AGE_NS;
    }

    void finish()
    {
        const bool lateAtStart = stopIsLate();
        if (!lateAtStart)
        {
            m_sources.doWork(m_domain, 0, m_now); // may issue one archive control call
        }
        m_client.close();
        const std::size_t current = detail::currentOf(m_sources);
        if (m_tap.sourceIsDead(current))
        {
            log(common::LogLevel::INFO, "stop: source %zu is dead (%s): detached without stopReplay", current,
                archive::faultName(m_tap.lastFault(current)));
        }
        else if (lateAtStart || stopIsLate())
        {
            log(common::LogLevel::WARN, "stop: the stop request is %.1f s old (>= the join bound %.0f s - the archive "
                "timeout %.0f s): source %zu detached without stopReplay (the replay ends with this process; the next "
                "start re-lists)", static_cast<double>(steadyNs() - m_stopRequestedAtNs) / 1e9,
                static_cast<double>(STOP_JOIN_BOUND_NS) / 1e9, static_cast<double>(STOP_ARCHIVE_TIMEOUT_NS) / 1e9, current);
        }
        else
        {
            detail::stopSources(m_sources, m_now);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(STOP_DRAIN_NS);
            if (m_stopRequested)
            {
                const auto bound = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(
                    m_stopRequestedAtNs + STOP_JOIN_BOUND_NS - STOP_EXIT_MARGIN_NS));
                deadline = std::min(deadline, bound);
            }
            while (!detail::currentIdle(m_sources) && std::chrono::steady_clock::now() < deadline)
            {
                updateNow();
                m_sources.doWork(m_domain, 0, m_now);
            }
        }
        if (!m_stop.set())
        {
            m_stop.exitCode = EXIT_OK;
            std::snprintf(m_stop.code, sizeof(m_stop.code), "%s", "STOPPED");
            std::snprintf(m_stop.detail, sizeof(m_stop.detail), "%s", "stop requested");
        }
        m_stopped = true;
    }

    bool allRefusedForMtu() const noexcept
    {
        if constexpr (requires { m_sources.allRefusedForMtu(); })
        {
            return m_sources.allRefusedForMtu();
        }
        for (std::size_t i = 0; i < m_settings.sourceCount && i < archive::MAX_SOURCES; ++i)
        {
            if (0 == archive::ArchiveMetrics::get(m_archive.sourceRefusedMtu[i]))
            {
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------ 5.3 egress dispatch

    static std::uint16_t le16(const std::uint8_t *p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

    static bool namesBridgeTemplate(std::uint16_t t) noexcept
    {
        using namespace domain::bridge;
        return T_QUERY_FEED_POSITION == t || T_ME_JOURNAL_TRADE == t || T_ME_JOURNAL_TRADE_BATCH == t ||
            T_ME_JOURNAL_TERMINAL == t || T_ME_COMMAND_OUTCOME == t || T_FEED_HALT == t || T_ME_COMMAND_RELEASE == t ||
            T_ME_ENGINE_CONFIG == t;
    }

    void ignored(std::uint16_t templateId) noexcept
    {
        m_counters.egressIgnored[templateId < EGRESS_TEMPLATE_LABELS - 1 ? templateId : EGRESS_TEMPLATE_LABELS - 1]++;
    }

    void malformed(std::uint16_t templateId)
    {
        if (app_seams::disabled(app_seams::Rule::MALFORMED_IS_CONFIG_ERROR))
        {
            stop(EXIT_CONFIG, "MALFORMED", "a malformed core frame (template %u) treated as a configuration error",
                static_cast<unsigned>(templateId)); // RED shape (C8)
            return;
        }
        domainCall();
        m_domain.onMalformedEgress(templateId, m_now);
    }

    void onEgress(const client::EgressMessage &m)
    {
        namespace codec = sbe_adapter::bridge_codec;
        using namespace domain::bridge;
        if (m_stop.set())
        {
            return;
        }
        // row 0: rule S (a defence; the fresh correlation ids per fence are the mechanism, 6.4)
        if (!app_seams::disabled(app_seams::Rule::TERM_FILTER_OFF))
        {
            const std::int64_t term = m_client.leadershipTermId();
            if (m.leadershipTermId < term)
            {
                if (common::shouldLog(++m_counters.coreStaleTerm))
                {
                    log(common::LogLevel::WARN, "core frame of ended term %lld dropped (client term %lld)",
                        static_cast<long long>(m.leadershipTermId), static_cast<long long>(term));
                }
                return;
            }
            if (m.leadershipTermId > term)
            {
                if (common::shouldLog(++m_counters.coreFutureTerm))
                {
                    log(common::LogLevel::WARN, "core frame of a later term %lld dropped before its NewLeaderEvent (client "
                        "term %lld)", static_cast<long long>(m.leadershipTermId), static_cast<long long>(term));
                }
                return;
            }
        }
        if (nullptr == m.payload || m.length < codec::MESSAGE_HEADER_LENGTH)
        {
            m_counters.coreMalformed++;
            return;
        }
        const std::uint16_t templateId = le16(m.payload + 2);
        const std::uint16_t schemaId = le16(m.payload + 4);
        const std::uint16_t version = le16(m.payload + 6);
        if (codec::MONEY_SCHEMA_ID != schemaId)
        {
            ignored(templateId);
            return;
        }
        // row 1: this binary speaks money v7 exactly (D2), in every link state
        if (codec::MONEY_VERSION != version)
        {
            stop(EXIT_CONFIG, "F10_WRONG_CORE_VERSION", "core frame template %u has money-schema version %u; this binary "
                "speaks 7 (a wrong core for this binary)", static_cast<unsigned>(templateId), static_cast<unsigned>(version));
            return;
        }
        switch (templateId)
        {
            case T_ADMIN_ACK:
            {
                codec::EgressEvent event;
                if (codec::EgressKind::ADMIN_ACK != codec::BridgeCodec::decodeEgress(m.payload, m.length, event))
                {
                    malformed(templateId);
                    return;
                }
                if (Registrar::REGISTER_SESSION_TEMPLATE_ID == event.ack.refTemplateId)
                {
                    m_registrar.onEgress(m); // row 2: the link machine reads isRegistered() in step 2
                    return;
                }
                if (T_FEED_HALT == event.ack.refTemplateId)
                {
                    domainCall();
                    m_domain.onAdminAck(event.ack, m_now); // row 3
                    return;
                }
                ignored(templateId);
                return;
            }
            case T_FEED_POSITION_REPORT:
            case T_SETTLE_REJECTED:
            {
                codec::EgressEvent event;
                const codec::EgressKind kind = codec::BridgeCodec::decodeEgress(m.payload, m.length, event);
                if (codec::EgressKind::REPORT == kind)
                {
                    domainCall();
                    m_domain.onReport(event.report, m_now); // row 4
                }
                else if (codec::EgressKind::SETTLE_REJECTED == kind)
                {
                    domainCall();
                    m_domain.onSettleRejected(event.reject, m_now); // row 5
                }
                else
                {
                    malformed(templateId);
                }
                return;
            }
            case codec::T_ORDER_REJECT:
            {
                codec::OrderRejectView r;
                if (!codec::BridgeCodec::decodeOrderReject(m.payload, m.length, r))
                {
                    m_counters.coreMalformed++;
                    return;
                }
                if (!namesBridgeTemplate(r.refTemplateId))
                {
                    ignored(templateId);
                    return;
                }
                onOrderReject(r); // row 6
                return;
            }
            case codec::T_SETTLEMENT_APPLIED:
            {
                std::int64_t tradeId = 0;
                if (!codec::BridgeCodec::decodeSettlementApplied(m.payload, m.length, tradeId))
                {
                    m_counters.coreMalformed++;
                    return;
                }
                std::int64_t latencyMs = 0;
                if (m_ackMap.onApplied(tradeId, m_nowNs, latencyMs)) // row 7 (O4, observability only)
                {
                    m_ackLatency.record(latencyMs);
                }
                return;
            }
            default:
                ignored(templateId); // row 8
                return;
        }
    }

    void onOrderReject(const sbe_adapter::bridge_codec::OrderRejectView &r)
    {
        namespace codec = sbe_adapter::bridge_codec;
        using namespace domain::bridge;
        if (codec::CORE_REJECT_UNREGISTERED_SESSION == r.reasonRaw)
        {
            if (app_seams::disabled(app_seams::Rule::UNREGISTERED_IGNORED) || CoreLink::C3_REGISTERED != m_link)
            {
                m_counters.orderReject[r.reasonRaw]++; // C2 already re-registers; the RED shape only counts
                return;
            }
            // 6.2: the core forgot this session's registration inside a live session
            m_counters.registrationLost++;
            log(common::LogLevel::WARN, "core says this session is unregistered (OrderReject naming %u): re-registering "
                "and fencing (SESSION_REESTABLISHED)", static_cast<unsigned>(r.refTemplateId));
            m_registrar.requestReRegistration();
            domainCall();
            m_domain.onFence(FenceTrigger::SESSION_REESTABLISHED, m_now);
            resetUp();
            m_link = CoreLink::C2_CONNECTED;
            return;
        }
        if (codec::CORE_REJECT_UNAUTHORIZED_CLASS == r.reasonRaw)
        {
            stop(EXIT_CONFIG, "F10_CORE_REFUSES_CLASS", "the core refuses the BRIDGE class for template %u (the credential "
                "class was asserted at start: a wrong core for this binary)", static_cast<unsigned>(r.refTemplateId));
            return;
        }
        if (codec::CORE_REJECT_INVALID == r.reasonRaw && (T_QUERY_FEED_POSITION == r.refTemplateId || T_FEED_HALT == r.refTemplateId))
        {
            stop(EXIT_SOFTWARE, "F6_ENCODER_BUG", "the core rejected our template %u INVALID (an encoder bug)",
                static_cast<unsigned>(r.refTemplateId));
            return;
        }
        m_counters.orderReject[r.reasonRaw]++;
        if (common::shouldLog(m_counters.orderReject[r.reasonRaw]))
        {
            log(common::LogLevel::WARN, "core OrderReject naming %u reason %u (counted)", static_cast<unsigned>(r.refTemplateId),
                static_cast<unsigned>(r.reasonRaw));
        }
    }

    // ------------------------------------------------------------------ 4.1 the core link machine (step 2 only)

    static bool namesProtocolVersion(const std::string &detail)
    {
        std::string lower(detail);
        for (char &ch : lower)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return lower.find("version") != std::string::npos;
    }

    // C1 FAILED / connect() refused: true = retry after the backoff (F12), false = a terminal refusal (F9).
    bool onConnectFailed()
    {
        using client::ConnectError;
        switch (m_client.connectError())
        {
            case ConnectError::ATTEMPTS_EXHAUSTED:
                m_counters.connectCyclesFailed++;
                log(common::LogLevel::WARN, "core connect cycle exhausted: %s", m_client.lastDetail().c_str());
                return true;
            case ConnectError::CLUSTER_ERROR:
                if (namesProtocolVersion(m_client.lastDetail()))
                {
                    stop(EXIT_CONFIG, "F9_VERSION", "the core refused the session: %s", m_client.lastDetail().c_str());
                    return false;
                }
                m_counters.connectClusterErrors++;
                m_counters.connectCyclesFailed++;
                log(common::LogLevel::ERROR, "core CLUSTER_ERROR (retrying, F12): %s", m_client.lastDetail().c_str());
                return true;
            default:
                stop(EXIT_CONFIG, "F9_REFUSED", "core connect refused (%s): %s", client::nameOf(m_client.connectError()),
                    m_client.lastDetail().c_str());
                return false;
        }
    }

    void checkRegistrarRefusal()
    {
        if (!m_registrar.isRejected() || m_stop.set())
        {
            return;
        }
        const bool byAck = app_seams::disabled(app_seams::Rule::INFERRED_READS_LAST_ACK) ?
            m_registrar.counters().acksRejected > 0 : m_registrar.rejectedByAck();
        if (byAck) // S10' F2: the CURRENT refusal came as an AdminAck, not by inference
        {
            if (REJECT_DUPLICATE_IDENTITY == m_registrar.lastReason() &&
                !app_seams::disabled(app_seams::Rule::DUPLICATE_IDENTITY_EXITS))
            {
                duplicateIdentity();
                return;
            }
            stop(EXIT_CONFIG, "F8_REJECTED", "%s", m_registrar.detail().c_str());
        }
        else
        {
            stop(EXIT_TEMPFAIL, "F8B_INFERRED", "%s (closes while pending %llu)", m_registrar.detail().c_str(),
                static_cast<unsigned long long>(m_registrar.counters().closesWhilePending));
        }
    }

    // RC2 S10': REJECTED DUPLICATE_IDENTITY: another live registered session holds this identity. After a kill -9 the
    // dead session stays registered at the core until its session timeout, so a prompt restart meets it: retry with a
    // bounded backoff for 60 s, then exit 78 (a second process with the same BRIDGE_INSTANCE_ID).
    // The bound counts CONNECTED time only (S10' F1, dupClock): while the bridge has no core session nothing can be
    // learnt about the other holder, and a core restart or election restarts the core's session-timeout clock anyway. A
    // new session or term inside an episode retries at once (the registrar keeps its REJECTED across the session change),
    // so every 78 follows a REJECTED ack in the current session and term.
    void duplicateIdentity()
    {
        const bool wallClock = app_seams::disabled(app_seams::Rule::DUPLICATE_BUDGET_WALL_CLOCK);
        if (!m_client.isConnected())
        {
            return; // sessionLost's check: judged on the next CONNECTED step, in that session
        }
        const std::int64_t budgetNs = m_settings.duplicateIdentityBudgetNs;
        const std::int64_t epoch = static_cast<std::int64_t>(m_client.sessionEpoch());
        const std::int64_t term = m_client.leadershipTermId();
        if (!m_dupActive)
        {
            m_dupActive = true;
            m_dupHeldNs = 0;
            m_dupLastNs = m_nowNs;
            m_dupEpoch = epoch;
            m_dupTerm = term;
            m_dupBackoffNs = m_settings.duplicateIdentityRetryInitialNs;
        }
        else if ((epoch != m_dupEpoch || term != m_dupTerm) && !wallClock)
        {
            // the REJECTED is the previous session's (or term's): retry in this one before the bound is judged
            m_dupEpoch = epoch;
            m_dupTerm = term;
            m_dupNextNs = -1;
            m_dupBackoffNs = m_settings.duplicateIdentityRetryInitialNs;
            if (m_registrar.retryAfterRejection())
            {
                m_counters.duplicateIdentityRetries++;
                log(common::LogLevel::WARN, "RegisterSession REJECTED DUPLICATE_IDENTITY (reason 44) in an earlier session or "
                    "term: retry %llu now in session epoch %lld term %lld, %.1f s of the %.1f s connected budget used",
                    static_cast<unsigned long long>(m_counters.duplicateIdentityRetries), static_cast<long long>(epoch),
                    static_cast<long long>(term), static_cast<double>(m_dupHeldNs) / 1e9, static_cast<double>(budgetNs) / 1e9);
            }
            return;
        }
        const std::int64_t heldNs = m_dupHeldNs;
        if (heldNs >= budgetNs)
        {
            stop(EXIT_CONFIG, "F8_DUPLICATE_IDENTITY",
                "RegisterSession REJECTED DUPLICATE_IDENTITY (reason 44) for %.1f s connected (>= the %.0f s bound: 2 x the "
                "core session timeout plus an election): another live process holds identity %lld (a duplicate "
                "BRIDGE_INSTANCE_ID)",
                static_cast<double>(heldNs) / 1e9, static_cast<double>(budgetNs) / 1e9,
                static_cast<long long>(m_settings.identity));
            return;
        }
        if (m_dupNextNs < 0)
        {
            m_dupNextNs = m_nowNs + m_dupBackoffNs;
            log(common::LogLevel::WARN, "RegisterSession REJECTED DUPLICATE_IDENTITY (reason 44): identity %lld is held by "
                "another live session (the previous process before its core session timeout?); retry %llu in %lld ms, "
                "%.1f s of the %.1f s connected budget left", static_cast<long long>(m_settings.identity),
                static_cast<unsigned long long>(m_counters.duplicateIdentityRetries + 1),
                static_cast<long long>(m_dupBackoffNs / 1'000'000), static_cast<double>(budgetNs - heldNs) / 1e9,
                static_cast<double>(budgetNs) / 1e9);
            return;
        }
        if (m_nowNs - m_dupNextNs >= 0)
        {
            m_dupNextNs = -1;
            m_dupBackoffNs = std::min(2 * m_dupBackoffNs, m_settings.duplicateIdentityRetryMaxNs);
            if (m_registrar.retryAfterRejection())
            {
                m_counters.duplicateIdentityRetries++;
            }
        }
    }

    // S10' F1: the DUPLICATE_IDENTITY episode's clock: adds the time between two CONNECTED link steps, nothing while the
    // client is connecting, closed or awaiting a new leader. Runs at the top of every link step of an open episode.
    void dupClock(bool connected)
    {
        if (!m_dupActive)
        {
            return;
        }
        if (app_seams::disabled(app_seams::Rule::DUPLICATE_BUDGET_WALL_CLOCK))
        {
            connected = true; // RED shape: the pre-review wall clock (an outage counts against the budget)
        }
        if (connected && m_dupLastNs >= 0)
        {
            m_dupHeldNs += m_nowNs - m_dupLastNs;
        }
        m_dupLastNs = connected ? m_nowNs : -1;
    }

    void down()
    {
        m_link = CoreLink::C0_DOWN;
        m_nextConnectNs = m_nowNs + m_backoff.next();
    }

    void sessionLost()
    {
        const std::size_t reason = static_cast<std::size_t>(m_client.closeReason());
        if (reason < m_counters.sessionLosses.size())
        {
            m_counters.sessionLosses[reason]++;
        }
        log(common::LogLevel::WARN, "core session lost (%s): %s", client::nameOf(m_client.closeReason()),
            m_client.lastDetail().c_str());
        if (!app_seams::disabled(app_seams::Rule::RECONNECT_WITHOUT_SESSION_DOWN))
        {
            domainCall();
            m_domain.onSessionDown(m_now); // exactly once per session loss
            m_counters.sessionDowns++;
        }
        m_ackMap.clear();
        resetUp();
        m_c3BeforeC4 = false;
        down();
        checkRegistrarRefusal();
    }

    void enterC3()
    {
        const UpKey key{ true, m_client.sessionEpoch(), m_client.leadershipTermId() };
        if (!m_upReportedFor.valid || m_upReportedFor.sessionEpoch != key.sessionEpoch || m_upReportedFor.term != key.term)
        {
            domainCall();
            m_domain.onSessionUp(m_now);
            m_counters.sessionUps++;
            m_upReportedFor = key;
            log(common::LogLevel::INFO, "registered: session %lld term %lld (epoch %llu): sync from the core's cursor",
                static_cast<long long>(m_client.clusterSessionId()), static_cast<long long>(key.term),
                static_cast<unsigned long long>(key.sessionEpoch));
        }
        m_link = CoreLink::C3_REGISTERED;
        m_c3BeforeC4 = false;
        m_backoff.reset();
        if (!app_seams::disabled(app_seams::Rule::DUP_EPISODE_NEVER_RESET))
        {
            m_dupActive = false; // a registration ends a DUPLICATE_IDENTITY episode (C4: the next one starts at 0)
            m_dupHeldNs = 0;
            m_dupLastNs = -1;
            m_dupNextNs = -1;
        }
    }

    void linkStep()
    {
        using client::ClientState;
        const ClientState st = m_client.state();
        m_stateChanged = false;
        dupClock(ClientState::CONNECTED == st);
        switch (m_link)
        {
            case CoreLink::C0_DOWN:
                if (m_nowNs - m_nextConnectNs >= 0)
                {
                    m_counters.connectCycles++;
                    if (!m_client.connect())
                    {
                        if (ClientState::FAILED == m_client.state() && onConnectFailed())
                        {
                            down();
                        }
                        return;
                    }
                    m_link = CoreLink::C1_CONNECTING;
                }
                return;
            case CoreLink::C1_CONNECTING:
                if (ClientState::CONNECTED == st)
                {
                    m_link = CoreLink::C2_CONNECTED;
                    m_pendingSinceNs = -1;
                    log(common::LogLevel::INFO, "core session %lld open, term %lld",
                        static_cast<long long>(m_client.clusterSessionId()), static_cast<long long>(m_client.leadershipTermId()));
                    break;
                }
                if (ClientState::FAILED == st)
                {
                    if (onConnectFailed())
                    {
                        down();
                    }
                    return;
                }
                if (ClientState::CLOSED == st || ClientState::IDLE == st)
                {
                    down();
                }
                return;
            case CoreLink::C2_CONNECTED:
            case CoreLink::C3_REGISTERED:
            case CoreLink::C4_AWAITING:
                break;
        }
        if (ClientState::CLOSED == st || ClientState::FAILED == st || ClientState::IDLE == st)
        {
            sessionLost();
            return;
        }
        if (ClientState::AWAIT_NEW_LEADER == st || ClientState::AWAIT_NEW_LEADER_CONNECTION == st)
        {
            if (CoreLink::C4_AWAITING != m_link)
            {
                m_c3BeforeC4 = m_c3BeforeC4 || CoreLink::C3_REGISTERED == m_link;
                m_link = CoreLink::C4_AWAITING;
            }
            return;
        }
        // CONNECTED
        if (CoreLink::C4_AWAITING == m_link)
        {
            if (m_c3BeforeC4 && app_seams::disabled(app_seams::Rule::C4_RESTORES_PREVIOUS))
            {
                enterC3(); // RED shape: back to C3 without the new term's ack
                return;
            }
            m_link = CoreLink::C2_CONNECTED; // NEVER back to C3: the registration must be re-observed
        }
        checkRegistrarRefusal();
        if (m_stop.set())
        {
            return;
        }
        if (CoreLink::C3_REGISTERED == m_link &&
            (m_client.sessionEpoch() != m_upReportedFor.sessionEpoch || m_client.leadershipTermId() != m_upReportedFor.term))
        {
            m_link = CoreLink::C2_CONNECTED; // a term change the callback did not announce: re-observe
        }
        if (CoreLink::C2_CONNECTED == m_link)
        {
            if (app_seams::disabled(app_seams::Rule::RESEND_NEW_CORRELATION))
            {
                if (client::RegistrationState::PENDING == m_registrar.state())
                {
                    if (m_pendingSinceNs < 0)
                    {
                        m_pendingSinceNs = m_nowNs;
                    }
                    else if (m_nowNs - m_pendingSinceNs >= m_settings.registrarResendNs)
                    {
                        m_registrar.requestReRegistration(); // RED shape: a late ack's round is abandoned
                        m_pendingSinceNs = -1;
                    }
                }
                else
                {
                    m_pendingSinceNs = -1;
                }
            }
            if (m_registrar.isRegistered() || app_seams::disabled(app_seams::Rule::REGISTRATION_GATE_OFF))
            {
                enterC3();
            }
        }
    }

    // ------------------------------------------------------------------ step 7

    void drainVerdicts()
    {
        const std::size_t n = m_domain.drainVerdictEvents(m_verdictBuffer.data(), m_verdictBuffer.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            const domain::bridge::VerdictEvent &v = m_verdictBuffer[i];
            m_counters.verdictEvents++;
            log(common::LogLevel::INFO, "verdict source %zu e %lld %s seq %lld epoch %lld bad %s fault %s", v.source,
                static_cast<long long>(v.e), domain::bridge::verdictName(v.kind), static_cast<long long>(v.seq),
                static_cast<long long>(v.epoch),
                domain::bridge::BadReason::COUNT == v.bad ? "-" : domain::bridge::badReasonLabel(v.bad),
                archive::faultName(v.fault));
        }
        // RC2 S10' (W0 finding: the T1 rows time a FeedHalt's AdminAck against the gate stream, and the log had no halt
        // line): one line when a halt starts, when its FeedHalt is acknowledged (ACCEPTED or DUPLICATE: the core alone
        // knows which) and when the halt ends; the log time is the steady clock, as `t1-core gate --watch`'s t
        const domain::bridge::HaltRecord &h = m_domain.halt();
        if (h.active != m_loggedHaltActive || h.correlationId != m_loggedHaltCorrelation || h.acked != m_loggedHaltAcked)
        {
            if (h.active && (!m_loggedHaltActive || h.correlationId != m_loggedHaltCorrelation))
            {
                log(common::LogLevel::WARN, "halt %s e %lld seen %lld epoch %lld: FeedHalt %s",
                    domain::bridge::bridgeHaltName(h.halt), static_cast<long long>(h.e), static_cast<long long>(h.seen),
                    static_cast<long long>(h.syncEpoch), h.sendsFeedHalt() ? "pending" : "none (CORE_REFUSED)");
            }
            if (h.active && h.acked && !(m_loggedHaltAcked && h.correlationId == m_loggedHaltCorrelation) &&
                h.sendsFeedHalt())
            {
                log(common::LogLevel::INFO, "FeedHalt AdminAck %s e %lld seen %lld (ACCEPTED or DUPLICATE)",
                    domain::bridge::bridgeHaltName(h.halt), static_cast<long long>(h.e), static_cast<long long>(h.seen));
            }
            if (!h.active && m_loggedHaltActive)
            {
                log(common::LogLevel::INFO, "halt left: cursor %lld last %lld", static_cast<long long>(m_domain.view().cursor),
                    static_cast<long long>(m_domain.view().last));
            }
            m_loggedHaltActive = h.active;
            m_loggedHaltCorrelation = h.correlationId;
            m_loggedHaltAcked = h.acked;
        }
    }

    void trackProgress()
    {
        const std::int64_t consumed = m_domain.view().consumedPosition;
        if (consumed != m_lastConsumed)
        {
            m_lastConsumed = consumed;
            m_consumedChangedAt = m_now;
        }
        // RC2 S10' 3.6: a confirm / lane_ahead fence moves the sync cursor (another lane delivered): progress
        const std::int64_t syncCursor = m_domain.view().syncCursor;
        if (syncCursor != m_lastSyncCursor && !app_seams::disabled(app_seams::Rule::READY_IGNORES_SYNC_CURSOR))
        {
            m_lastSyncCursor = syncCursor;
            m_syncCursorChangedAt = m_now;
        }
    }

    CoreClient &m_client;
    Sources &m_sources;
    ObserverTap &m_tap;
    archive::ArchiveMetrics &m_archive;
    AgentSettings m_settings;
    common::LogRing *m_log;
    common::DutyHeartbeat *m_heartbeat;
    AppCounters m_counters;
    common::StorageStarvation m_coreStorage;   // S10' Q1
    common::StorageStarvation m_replayStorage;
    common::StorageStarvation m_anyStorage;    // either of them: bridge_storage_starved_seconds_total
    common::ClientStorageFeed<CoreClient> m_coreStorageFeed;
    std::uint64_t m_seenReplayStorage = 0;
    std::uint64_t m_seenReplayOther = 0;
    domain::bridge::BridgeMetrics m_metrics;
    CorePortAdapter<CoreClient> m_port;
    domain::bridge::BridgeSettings m_domainSettings;
    domain::bridge::BridgeDomain m_domain;
    Registrar m_registrar;
    client::ExponentialBackoff m_backoff;
    NegctlSink m_negctlSink;
    AckMap m_ackMap;
    LatencyHistogram m_forward;
    LatencyHistogram m_ackLatency;
    std::array<domain::bridge::VerdictEvent, VERDICT_DRAIN> m_verdictBuffer{};

    CoreLink m_link = CoreLink::C0_DOWN;
    std::int64_t m_nextConnectNs = 0;
    UpKey m_upReportedFor;
    bool m_c3BeforeC4 = false;
    bool m_fencePending = false;
    bool m_stateChanged = false;
    bool m_inInvoker = false;
    std::uint64_t m_domainCallsInInvoker = 0;
    std::uint64_t m_invokes = 0;
    std::int64_t m_pendingSinceNs = -1;
    bool m_loggedHaltActive = false;      // S10': the halt lines of drainVerdicts
    std::int64_t m_loggedHaltCorrelation = 0;
    bool m_loggedHaltAcked = false;
    bool m_dupActive = false;         // S10': a REJECTED DUPLICATE_IDENTITY episode is open (until enterC3)
    std::int64_t m_dupHeldNs = 0;     // its CONNECTED time so far (F1: the 60 s bound counts only this)
    std::int64_t m_dupLastNs = -1;    // the last CONNECTED link step dupClock saw, -1 while not connected
    std::int64_t m_dupEpoch = -1;     // the session epoch and term of the episode's last REJECTED judgement
    std::int64_t m_dupTerm = -1;
    std::int64_t m_dupNextNs = -1;    // the next RegisterSession retry
    std::int64_t m_dupBackoffNs = 0;
    bool m_stopRequested = false;
    std::int64_t m_stopRequestedAtNs = -1;
    bool m_stopped = false;
    StopReason m_stop;
    std::int64_t m_nowNs = 0;
    TimePoint m_now{};
    TimePoint m_startedAt{};
    std::int64_t m_lastInvokeNs = 0;
    std::int64_t m_offerEpochMs = 0;
    std::int64_t m_lastConsumed = -1;
    std::int64_t m_lastSyncCursor = 0; // RC2 S10' 3.6
    TimePoint m_syncCursorChangedAt{};
    TimePoint m_consumedChangedAt{};
};

}}}

#endif
