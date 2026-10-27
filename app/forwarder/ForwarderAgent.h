#ifndef CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_AGENT_H
#define CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_AGENT_H

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "cluster_tools/adapters/sbe/ForwarderCodec.h"
#include "cluster_tools/client/Backoff.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "cluster_tools/domain/forwarder/AdmissionWatcher.h"
#include "cluster_tools/domain/forwarder/Counters.h"
#include "cluster_tools/domain/forwarder/DispatchRules.h"
#include "cluster_tools/domain/forwarder/EpochBinder.h"
#include "cluster_tools/domain/forwarder/ResyncMachine.h"
#include "cluster_tools/domain/forwarder/SendRing.h"
#include "cluster_tools/domain/forwarder/TestSeams.h"

#include "LogRing.h"
#include "Snapshot.h"

// ForwarderAgent - the fwd-duty agent (RC2 S12 contract sections 3, 4 and 8): one duty iteration drives both
// ClusterClients, the CoreSessionRegistrar, the resync machine, the send ring, the epoch binder, the
// admission watcher and the codec. Header-only and templated on the two clients, so the app tests run it over
// FakeTransport and production over AeronTransport. Single-threaded: only the duty thread touches it.
//
// One iteration (section 3): (1) both clients' doWork (connect steps, keepalive every 1 s, deadlines);
// (2) core poll, 256 frames, rule S first (a frame of another leadership term than the client's is dropped);
// (3) the registrar; (4) ME poll, 256 frames (heartbeats to the binder; everything else counted and
// dropped); (5) the resync timers and the RequestPendingDispatch offer; (6) up to 64 ME offers from the ring,
// stopping at the first non-OK result (never retried in place); (7) link supervision (reconnects with an
// exponential 1-30 s backoff, terminal refusals end the agent with a StopReason); main maps the StopReason to
// the process exit code (0, 70, 75, 78).
//
// The agent never blocks, sleeps or allocates in steady state; logs go to the LogRing (first and every
// 1000th occurrence of a repeated event).

namespace cluster_tools { namespace app { namespace forwarder {

inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_SOFTWARE = 70; // EX_SOFTWARE: process health (F1-F4)
inline constexpr int EXIT_TEMPFAIL = 75; // EX_TEMPFAIL: an inferred refusal, restart allowed (F7b)
inline constexpr int EXIT_CONFIG = 78;   // EX_CONFIG: an observed refusal or a wrong binary (F5-F9, F15)
// The Aeron client's driver timeout (F1): a dead media driver is a fatal conductor error after this long; T1-J asserts
// exit 70 within this + 2 s. Fixed, like every other S12 constant (section 9). Review NOTE-10: 5 s, so a starved host
// does not exit on a short driver stall.
inline constexpr std::int64_t DRIVER_TIMEOUT_MS = 5000;
// Review NOTE-3: ready is 0 while the ring's head has waited this long for a positive ME offer.
inline constexpr std::int64_t RING_HEAD_READY_AGE_NS = 30'000'000'000LL;

struct StopReason
{
    int exitCode = -1;
    char code[24] = {};
    char detail[232] = {};

    bool set() const noexcept { return exitCode >= 0; }
};

struct AgentSettings
{
    std::int64_t identity = 0; // 64-bit random non-zero per start (diagnostics only, 7.1)
    client::SessionClass coreCredentialClass = client::SessionClass::FORWARDER;
    const char *version = "dev";
    std::int64_t reconnectBackoffInitialNs = 1'000'000'000LL;
    std::int64_t reconnectBackoffMaxNs = 30'000'000'000LL;
};

template<typename CoreClient, typename MeClient>
class ForwarderAgent
{
public:
    static constexpr int CORE_POLL_LIMIT = 256;
    static constexpr int ME_POLL_LIMIT = 256;
    static constexpr int DRAIN_OFFERS_PER_CYCLE = 64;
    static constexpr std::int64_t ME_EGRESS_READY_AGE_NS = 5'000'000'000LL;
    static constexpr std::uint32_t CHANNELS = client::core_channel::DISPATCH | client::core_channel::CONTROL;

    using Registrar = client::CoreSessionRegistrar<CoreClient>;

    ForwarderAgent(CoreClient &core, MeClient &me, AgentSettings settings, LogRing *log = nullptr) :
        m_core(core),
        m_me(me),
        m_settings(settings),
        m_log(log),
        m_resync(m_ring),
        m_coreBackoff(settings.reconnectBackoffInitialNs, settings.reconnectBackoffMaxNs),
        m_meBackoff(settings.reconnectBackoffInitialNs, settings.reconnectBackoffMaxNs)
    {
        if (domain::forwarder::test_seams::disabled(domain::forwarder::test_seams::Rule::AGENT_EPOCH_BEFORE_REGISTER))
        {
            constructRegistrar(0); // RED shape: registers before the ME epoch is known (sourceEpoch 0)
        }
    }

    ForwarderAgent(const ForwarderAgent &) = delete;
    ForwarderAgent &operator=(const ForwarderAgent &) = delete;

    // ---- control (duty thread)
    void requestStop() noexcept { m_stopRequested = true; }
    void requestResync() noexcept
    {
        m_resync.resyncFromZero(domain::forwarder::Trigger::T4, m_core.nanoTime());
    }
    bool stopped() const noexcept { return m_stopped; }
    const StopReason &stopReason() const noexcept { return m_stopReason; }

    // One duty iteration; the amount of work done (0 = idle).
    int doWork()
    {
        if (m_stopped)
        {
            return 0;
        }
        m_now = m_core.nanoTime();
        if (m_stopRequested || m_stopReason.set())
        {
            finish();
            return 1;
        }
        int work = m_core.doWork();
        work += m_me.doWork();
        m_now = m_core.nanoTime();
        // Review FIX-1: the client applies the ME SessionEvent OK inside doWork(); a W9 hello right behind it in the same
        // egress batch reaches the poll below. Start the session (M2) now, so the binder is active for that poll.
        if (!domain::forwarder::test_seams::disabled(domain::forwarder::test_seams::Rule::AGENT_ME_SESSION_BEFORE_POLL))
        {
            enterMeSessionIfConnected();
        }

        work += m_core.poll([this](const client::EgressMessage &m) { onCoreMessage(m); }, CORE_POLL_LIMIT);
        if (m_registrar)
        {
            work += m_registrar->doWork();
        }
        const int mePolled = m_me.poll([this](const client::EgressMessage &m) { onMeMessage(m); }, ME_POLL_LIMIT);
        if (mePolled >= ME_POLL_LIMIT)
        {
            m_counters.mePollSaturated++;
        }
        work += mePolled;

        m_now = m_core.nanoTime();
        superviseCore();
        superviseMe();
        checkVerdicts();
        if (m_stopReason.set())
        {
            finish();
            return work + 1;
        }

        m_resync.tick(m_now);
        work += offerRequest();
        work += drainRing();
        if (m_ring.empty())
        {
            m_ringHeadSinceNs = m_now; // the next push becomes a head that has waited 0
        }
        return work;
    }

    // ---- state for the StatusBoard and the tests
    bool ready() const noexcept { return 0 == notReadyMask(); }
    std::uint32_t notReadyMask() const noexcept
    {
        std::uint32_t mask = 0;
        if (CoreLink::REGISTERED != m_coreLink) mask |= not_ready::CORE_NOT_REGISTERED;
        if (domain::forwarder::Mode::LIVE != m_resync.mode()) mask |= not_ready::NOT_LIVE;
        if (MeLink::BOUND != m_meLink) mask |= not_ready::ME_NOT_BOUND;
        if (!m_haveMeFrame || m_now - m_lastMeFrameNs >= ME_EGRESS_READY_AGE_NS) mask |= not_ready::ME_EGRESS_STALE;
        if (!m_resync.answeredWithin(m_now)) mask |= not_ready::NO_RECENT_ANSWER;
        if (m_ring.depth() > domain::forwarder::ResyncMachine::RESUME_RING_DEPTH) mask |= not_ready::RING_DEEP;
        if (ringHeadAgeNs() >= RING_HEAD_READY_AGE_NS) mask |= not_ready::RING_HEAD_STALE;
        if (CoreLink::REGISTERED == m_coreLink && client::ClientState::AWAIT_NEW_LEADER == m_core.state())
        {
            mask |= not_ready::CORE_NO_LEADER;
        }
        if (m_stopRequested || m_stopReason.set()) mask |= not_ready::STOPPING;
        return mask;
    }

    void fillSnapshot(ForwarderSnapshot &s) const
    {
        s.nowNs = m_now;
        s.identity = m_settings.identity;
        s.meEpoch = m_binder.epoch();
        std::snprintf(s.version, sizeof(s.version), "%s", m_settings.version);
        s.notReady = notReadyMask();
        s.ready = 0 == s.notReady;
        s.stopping = m_stopRequested || m_stopReason.set();
        s.coreLink = static_cast<std::uint8_t>(m_coreLink);
        s.meLink = static_cast<std::uint8_t>(m_meLink);
        s.registered = CoreLink::REGISTERED == m_coreLink;
        fillCluster(s.core, m_core, m_counters.connectClusterErrors[domain::forwarder::CLUSTER_CORE]);
        fillCluster(s.me, m_me, m_counters.connectClusterErrors[domain::forwarder::CLUSTER_ME]);
        if (m_registrar)
        {
            const client::RegistrarCounters &r = m_registrar->counters();
            s.registrationAccepted = r.acksAccepted;
            s.registrationDuplicate = r.acksDuplicate;
            s.registrationRejected = r.acksRejected;
            s.registrationSends = r.sends;
        }
        s.meEpochReconnects = m_counters.meEpochReconnects;
        s.meEgressAgeMs = m_haveMeFrame ? (m_now - m_lastMeFrameNs) / 1'000'000 : -1;
        s.mode = static_cast<std::uint8_t>(m_resync.mode());
        s.cursor = m_resync.cursor();
        s.lastSentSeq = m_lastSentSeq;
        s.coreHead = m_resync.lastHead();
        s.lastResyncEndSeq = m_resync.lastResyncEndSeq();
        s.resyncDurationMs = m_resync.lastResyncDurationNs() / 1'000'000;
        const std::int64_t age = m_resync.lastAnswerAgeNs(m_now);
        s.headProbeAgeMs = age < 0 ? -1 : age / 1'000'000;
        s.ringDepth = m_ring.depth();
        s.ringHeadAgeMs = ringHeadAgeNs() / 1'000'000;
        s.holdSize = m_resync.holdSize();
        s.resync = m_resync.counters();
        s.counters = m_counters;
        s.logDropped = nullptr == m_log ? 0 : m_log->dropped();
    }

    // How long the ring's head slot has waited for a positive ME offer (0 when the ring is empty).
    std::int64_t ringHeadAgeNs() const noexcept { return m_ring.empty() ? 0 : m_now - m_ringHeadSinceNs; }

    CoreLink coreLink() const noexcept { return m_coreLink; }
    MeLink meLink() const noexcept { return m_meLink; }
    const domain::forwarder::ResyncMachine &resync() const noexcept { return m_resync; }
    const domain::forwarder::SendRing &ring() const noexcept { return m_ring; }
    const domain::forwarder::EpochBinder &epochBinder() const noexcept { return m_binder; }
    const domain::forwarder::ForwarderCounters &counters() const noexcept { return m_counters; }
    const Registrar *registrar() const noexcept { return m_registrar ? &*m_registrar : nullptr; }
    std::int64_t lastSentSeq() const noexcept { return m_lastSentSeq; }

private:
    using Dispatch = domain::forwarder::Dispatch;

    // ------------------------------------------------------------------ helpers

    template<typename Client>
    static void fillCluster(ClusterStats &out, const Client &client, std::uint64_t clusterErrors)
    {
        const client::ClientCounters &c = client.counters();
        out.clientState = static_cast<std::uint8_t>(client.state());
        out.newLeaderEvents = c.newLeaderEvents;
        out.sessionsOpened = c.sessionsOpened;
        out.sessionsClosed = c.sessionsClosed;
        out.keepAlivesSent = c.keepAlivesSent;
        out.keepAliveFailures = c.keepAliveFailures;
        out.connectClusterErrors = clusterErrors;
        out.clusterSessionId = client.clusterSessionId();
        out.leadershipTermId = client.leadershipTermId();
    }

    void log(LogLevel level, const char *format, ...) __attribute__((format(printf, 3, 4)))
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
        m_log->push(level, m_now, "%s", text);
    }

    void stop(int exitCode, const char *code, const char *format, ...) __attribute__((format(printf, 4, 5)))
    {
        if (m_stopReason.set())
        {
            return; // the first reason wins
        }
        m_stopReason.exitCode = exitCode;
        std::snprintf(m_stopReason.code, sizeof(m_stopReason.code), "%s", code);
        va_list args;
        va_start(args, format);
        std::vsnprintf(m_stopReason.detail, sizeof(m_stopReason.detail), format, args);
        va_end(args);
        log(EXIT_SOFTWARE == exitCode ? LogLevel::CRITICAL : LogLevel::ERROR, "STOP %s exit %d: %s", code, exitCode,
            m_stopReason.detail);
    }

    void finish()
    {
        // F14: polite close on both sessions (one SessionCloseRequest each), then the agent is done.
        m_core.close();
        m_me.close();
        if (!m_stopReason.set())
        {
            m_stopReason.exitCode = EXIT_OK;
            std::snprintf(m_stopReason.code, sizeof(m_stopReason.code), "%s", "STOPPED");
            std::snprintf(m_stopReason.detail, sizeof(m_stopReason.detail), "%s", "stop requested");
        }
        m_stopped = true;
    }

    void constructRegistrar(std::int64_t sourceEpoch)
    {
        client::RegistrarSettings r;
        r.sessionClass = client::SessionClass::FORWARDER;
        r.credentialClass = m_settings.coreCredentialClass;
        r.channels = CHANNELS;
        r.readerKind = 0;
        r.resumable = true;
        r.identity = m_settings.identity;
        r.sourceEpoch = sourceEpoch;
        m_registrar.emplace(m_core, r);
        if (const std::string error = m_registrar->validateSettings(); !error.empty())
        {
            stop(EXIT_CONFIG, "F5_REGISTRAR", "%s", error.c_str());
        }
    }

    static bool namesProtocolVersion(const std::string &detail)
    {
        std::string lower(detail);
        for (char &ch : lower)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return lower.find("version") != std::string::npos;
    }

    // C1/M1 FAILED: retry (F10) or a terminal refusal (F6).
    template<typename Client>
    bool onConnectFailed(Client &client, unsigned cluster, const char *name)
    {
        using client::ConnectError;
        switch (client.connectError())
        {
            case ConnectError::ATTEMPTS_EXHAUSTED:
                log(LogLevel::WARN, "%s connect cycle exhausted: %s", name, client.lastDetail().c_str());
                return true;
            case ConnectError::CLUSTER_ERROR:
                if (namesProtocolVersion(client.lastDetail()) ||
                    domain::forwarder::test_seams::disabled(domain::forwarder::test_seams::Rule::AGENT_CLUSTER_ERROR_RETRY))
                {
                    stop(EXIT_CONFIG, "F6_VERSION", "%s refused the session: %s", name, client.lastDetail().c_str());
                    return false;
                }
                m_counters.connectClusterErrors[cluster]++;
                log(LogLevel::ERROR, "%s CLUSTER_ERROR (retrying, F10): %s", name, client.lastDetail().c_str());
                return true;
            default:
                stop(EXIT_CONFIG, "F6_REFUSED", "%s connect refused (%s): %s", name, client::nameOf(client.connectError()),
                    client.lastDetail().c_str());
                return false;
        }
    }

    // ------------------------------------------------------------------ 4.1 core link

    void superviseCore()
    {
        using client::ClientState;
        const ClientState st = m_core.state();
        switch (m_coreLink)
        {
            case CoreLink::DOWN:
                if (m_now - m_coreNextConnectNs >= 0)
                {
                    if (!m_core.connect())
                    {
                        if (ClientState::FAILED == m_core.state())
                        {
                            onConnectFailed(m_core, domain::forwarder::CLUSTER_CORE, "core");
                        }
                        return;
                    }
                    m_coreLink = CoreLink::CONNECTING;
                }
                return;
            case CoreLink::CONNECTING:
                if (ClientState::CONNECTED == st)
                {
                    m_coreLink = CoreLink::UNREGISTERED;
                    m_watcher.reset();
                    m_expectOrigin = false;
                    m_registrationLost = false;
                    log(LogLevel::INFO, "core session %lld open, term %lld", static_cast<long long>(m_core.clusterSessionId()),
                        static_cast<long long>(m_core.leadershipTermId()));
                }
                else if (ClientState::FAILED == st)
                {
                    if (onConnectFailed(m_core, domain::forwarder::CLUSTER_CORE, "core"))
                    {
                        coreDown();
                    }
                }
                return;
            case CoreLink::UNREGISTERED:
            case CoreLink::REGISTERED:
                break;
        }
        if (ClientState::CLOSED == st || ClientState::FAILED == st || ClientState::IDLE == st)
        {
            log(LogLevel::WARN, "core session closed (%s): %s", client::nameOf(m_core.closeReason()), m_core.lastDetail().c_str());
            leaveC3();
            coreDown();
            checkRegistrarRefusal();
            return;
        }
        checkRegistrarRefusal();
        if (m_stopReason.set())
        {
            return;
        }
        if (CoreLink::REGISTERED == m_coreLink)
        {
            if (m_core.leadershipTermId() != m_registeredTerm || m_core.sessionEpoch() != m_registeredSessionEpoch)
            {
                log(LogLevel::INFO, "core new leader: term %lld -> %lld, re-registering",
                    static_cast<long long>(m_registeredTerm), static_cast<long long>(m_core.leadershipTermId()));
                leaveC3(); // C3 -> C2 (same session, new term): the registrar re-registers by itself
            }
            else if (m_registrationLost)
            {
                m_counters.registrationLost++;
                log(LogLevel::WARN, "core says this session is unregistered (OrderReject naming 65): re-registering");
                leaveC3();
                if (m_registrar)
                {
                    m_registrar->requestReRegistration();
                }
            }
        }
        m_registrationLost = false;
        if (CoreLink::UNREGISTERED == m_coreLink && m_registrar && m_binder.bound() && m_registrar->isRegistered())
        {
            m_coreLink = CoreLink::REGISTERED;
            m_registeredTerm = m_core.leadershipTermId();
            m_registeredSessionEpoch = m_core.sessionEpoch();
            m_coreBackoff.reset();
            log(LogLevel::INFO, "registered (term %lld, sourceEpoch %lld): T1 head probe, then RESYNC(0)",
                static_cast<long long>(m_registeredTerm), static_cast<long long>(m_binder.epoch()));
            m_resync.onRegistered(m_now); // T1
        }
    }

    void checkRegistrarRefusal()
    {
        if (!m_registrar || !m_registrar->isRejected() || m_stopReason.set())
        {
            return;
        }
        if (m_registrar->counters().acksRejected > 0)
        {
            stop(EXIT_CONFIG, "F7_REJECTED", "%s", m_registrar->detail().c_str());
        }
        else
        {
            stop(EXIT_TEMPFAIL, "F7B_INFERRED", "%s (closes while pending %llu)", m_registrar->detail().c_str(),
                static_cast<unsigned long long>(m_registrar->counters().closesWhilePending));
        }
    }

    void leaveC3()
    {
        if (CoreLink::REGISTERED == m_coreLink)
        {
            m_resync.onUnregistered(); // every exit from C3 is D0
        }
        if (CoreLink::REGISTERED == m_coreLink || CoreLink::UNREGISTERED == m_coreLink)
        {
            m_coreLink = CoreLink::UNREGISTERED;
        }
        m_expectOrigin = false;
    }

    void coreDown()
    {
        m_coreLink = CoreLink::DOWN;
        m_coreNextConnectNs = m_now + m_coreBackoff.next();
        m_watcher.reset();
    }

    // ------------------------------------------------------------------ 4.2 ME link

    void superviseMe()
    {
        using client::ClientState;
        const ClientState st = m_me.state();
        switch (m_meLink)
        {
            case MeLink::DOWN:
                if (m_now - m_meNextConnectNs >= 0)
                {
                    if (!m_me.connect())
                    {
                        if (ClientState::FAILED == m_me.state())
                        {
                            onConnectFailed(m_me, domain::forwarder::CLUSTER_ME, "ME");
                        }
                        return;
                    }
                    m_meLink = MeLink::CONNECTING;
                }
                return;
            case MeLink::CONNECTING:
                if (ClientState::CONNECTED == st)
                {
                    enterMeSessionIfConnected();
                }
                else if (ClientState::FAILED == st)
                {
                    if (onConnectFailed(m_me, domain::forwarder::CLUSTER_ME, "ME"))
                    {
                        meDown(false);
                    }
                }
                return;
            case MeLink::UNVERIFIED:
            case MeLink::BOUND:
                break;
        }
        if (ClientState::CLOSED == st || ClientState::FAILED == st || ClientState::IDLE == st)
        {
            log(LogLevel::WARN, "ME session closed (%s): %s", client::nameOf(m_me.closeReason()), m_me.lastDetail().c_str());
            meDown(false);
            return;
        }
        if (MeLink::BOUND == m_meLink && m_me.leadershipTermId() != m_meTerm)
        {
            log(LogLevel::INFO, "ME new leader: term %lld -> %lld: T2 RESYNC(0)", static_cast<long long>(m_meTerm),
                static_cast<long long>(m_me.leadershipTermId()));
            m_meTerm = m_me.leadershipTermId();
            m_resync.resyncFromZero(domain::forwarder::Trigger::T2, m_now);
        }
        if (MeLink::UNVERIFIED == m_meLink && domain::forwarder::EpochVerdict::RECONNECT == m_binder.tick(m_now))
        {
            m_counters.meEpochReconnects++;
            if (shouldLog(m_counters.meEpochReconnects))
            {
                log(LogLevel::WARN, "no ClusterHeartbeat with the ME epoch within 5 s: reconnecting (%llu)",
                    static_cast<unsigned long long>(m_counters.meEpochReconnects));
            }
            m_me.close();
            meDown(true);
        }
    }

    void enterMeSessionIfConnected()
    {
        if (MeLink::CONNECTING != m_meLink || client::ClientState::CONNECTED != m_me.state())
        {
            return;
        }
        m_meLink = MeLink::UNVERIFIED;
        m_meTerm = m_me.leadershipTermId();
        m_binder.onSessionStart(m_now);
        log(LogLevel::INFO, "ME session %lld open, term %lld: waiting for the epoch",
            static_cast<long long>(m_me.clusterSessionId()), static_cast<long long>(m_meTerm));
    }

    void meDown(bool immediate)
    {
        m_binder.onSessionEnd();
        m_meLink = MeLink::DOWN;
        m_meNextConnectNs = immediate ? m_now : m_now + m_meBackoff.next();
    }

    void onEpochVerdict(domain::forwarder::EpochVerdict verdict, std::int64_t meEpoch)
    {
        using domain::forwarder::EpochVerdict;
        switch (verdict)
        {
            case EpochVerdict::BOUND:
                log(LogLevel::INFO, "ME epoch bound: %lld", static_cast<long long>(meEpoch));
                if (!m_registrar)
                {
                    constructRegistrar(meEpoch); // lazily, so a RegisterSession with sourceEpoch 0 is never sent
                }
                [[fallthrough]];
            case EpochVerdict::VERIFIED:
                m_meLink = MeLink::BOUND;
                m_meBackoff.reset();
                m_meTerm = m_me.leadershipTermId();
                m_resync.resyncFromZero(domain::forwarder::Trigger::T2, m_now); // a new ME session reached M3
                return;
            case EpochVerdict::FATAL_MISMATCH:
                stop(EXIT_CONFIG, "F8_EPOCH", "ME heartbeat meEpoch %lld differs from the bound epoch %lld (ME re-genesis)",
                    static_cast<long long>(meEpoch), static_cast<long long>(m_binder.epoch()));
                return;
            case EpochVerdict::FATAL_ZERO:
                stop(EXIT_CONFIG, "F8_EPOCH_ZERO", "ME heartbeat meEpoch 0 after the bind to %lld (the ME lost its identity)",
                    static_cast<long long>(m_binder.epoch()));
                return;
            default:
                return;
        }
    }

    // ------------------------------------------------------------------ core egress

    void onCoreMessage(const client::EgressMessage &m)
    {
        namespace codec = sbe_adapter::forwarder_codec;
        if (m_stopReason.set())
        {
            return;
        }
        // 6.2 rule S: frames of another term than the client's are dropped before the domain sees them (FIX-1).
        if (!domain::forwarder::test_seams::disabled(domain::forwarder::test_seams::Rule::AGENT_STALE_TERM_FILTER))
        {
            const std::int64_t term = m_core.leadershipTermId();
            if (m.leadershipTermId < term)
            {
                if (shouldLog(++m_counters.coreStaleTerm))
                {
                    log(LogLevel::WARN, "core frame of ended term %lld dropped (client term %lld)",
                        static_cast<long long>(m.leadershipTermId), static_cast<long long>(term));
                }
                return;
            }
            if (m.leadershipTermId > term)
            {
                m_counters.coreFutureTerm++;
                return;
            }
        }
        codec::Header h;
        if (!codec::readHeader(m.payload, m.length, h))
        {
            m_counters.coreMalformed++;
            return;
        }
        if (codec::MONEY_SCHEMA_ID != h.schemaId)
        {
            m_counters.unknown(domain::forwarder::CLUSTER_CORE, h.templateId);
            return;
        }
        if (codec::MONEY_VERSION != h.version)
        {
            stop(EXIT_CONFIG, "F9_MONEY_VERSION", "core frame template %u has money-schema version %u, this binary speaks 7",
                static_cast<unsigned>(h.templateId), static_cast<unsigned>(h.version));
            return;
        }
        m_resync.onCoreFrame(m_now); // T9: every core frame of this term restarts the 108 timer
        if (m_registrar && m_registrar->onEgress(m))
        {
            // the AdmissionState right after a registration ack is its ORIGIN reply (resets the T3 baseline)
            m_expectOrigin = client::RegistrationState::REGISTERED == m_registrar->state();
            return;
        }
        switch (h.templateId)
        {
            case codec::T_ME_DISPATCH:
            {
                Dispatch d;
                bool belowOne = false;
                if (!codec::decodeDispatch(m.payload, m.length, h, d, &belowOne))
                {
                    malformedDispatch(belowOne);
                    return;
                }
                handleDispatch(d);
                return;
            }
            case codec::T_ME_DISPATCH_BATCH:
            {
                bool belowOne = false;
                if (!codec::decodeDispatchBatch(m.payload, m.length, h, &ForwarderAgent::onBatchEntry, this, &belowOne))
                {
                    malformedDispatch(belowOne);
                }
            }
                return;
            case codec::T_PENDING_DISPATCH_END:
            {
                codec::PendingEnd end;
                if (!codec::decodePendingEnd(m.payload, m.length, h, end))
                {
                    m_counters.coreMalformed++;
                    return;
                }
                m_counters.pendingEnds++;
                m_resync.onPendingEnd(end.correlationId, end.count, end.headDispatchSeq, m_now);
                return;
            }
            case codec::T_ADMISSION_STATE:
            {
                codec::Admission a;
                if (!codec::decodeAdmission(m.payload, m.length, h, a))
                {
                    m_counters.coreMalformed++;
                    return;
                }
                onAdmission(a);
                return;
            }
            case codec::T_ORDER_REJECT:
            {
                codec::OrderRejectView r;
                if (!codec::decodeOrderReject(m.payload, m.length, h, r))
                {
                    m_counters.coreMalformed++;
                    return;
                }
                onOrderReject(r);
                return;
            }
            case codec::T_ORDER_FAULT:
            case codec::T_CONFIG_APPLIED:
                m_counters.coreControlFrames++; // CH_CONTROL, not a trigger (Q9)
                return;
            default:
                m_counters.unknown(domain::forwarder::CLUSTER_CORE, h.templateId);
                return;
        }
    }

    static void onBatchEntry(void *context, const Dispatch &d)
    {
        static_cast<ForwarderAgent *>(context)->handleDispatch(d);
    }

    void malformedDispatch(bool seqBelowOne)
    {
        if (seqBelowOne && shouldLog(++m_counters.dispatchSeqBelowOne))
        {
            log(LogLevel::ERROR, "core MeDispatch frame with a dispatchSeq below 1 refused whole (a core defect; %llu)",
                static_cast<unsigned long long>(m_counters.dispatchSeqBelowOne));
        }
        // includes a 104/105 carrying a dispatchSeq below 1 (review NOTE-2: only a corrupt core produces one; nothing of
        // the frame is sent, no carrier, the cursor stays)
        if (shouldLog(++m_counters.coreMalformed))
        {
            log(LogLevel::ERROR, "malformed MeDispatch frame: T6 RESYNC(0) (it may have carried a dispatch)");
        }
        m_resync.onMalformedDispatch(m_now);
    }

    void handleDispatch(Dispatch d)
    {
#ifdef CT_FORWARDER_TC_CORRUPT
        // ct_forwarder_tc_corrupt (T1-G, never published): simulate a core defect after decode and before
        // classification. Deterministic per command id (every copy of one command is corrupted alike, so
        // resends stay byte-identical): CREATEs whose orderId is a multiple of 50, alternating class C
        // (userId := 0) and class I (LIMIT_MAKER with timeInForce 2).
        if (domain::forwarder::KIND_CREATE == d.kind && d.commandIdHigh > 0 && 0 == d.commandIdHigh % 50)
        {
            if (0 == (d.commandIdHigh / 50) % 2)
            {
                d.userId = 0;
            }
            else
            {
                d.orderType = 2;
                d.timeInForce = 2;
            }
        }
#endif
        const domain::forwarder::Classification c = domain::forwarder::classify(d);
        if (domain::forwarder::DispatchClass::PASS != c.cls)
        {
            m_counters.invalid.add(d.kind, c);
            const std::uint64_t perRule = ++m_invalidPerRule[static_cast<unsigned>(c.rule)];
            if (shouldLog(perRule))
            {
                log(LogLevel::ERROR,
                    "anomalous dispatch seq %lld id %lld/%lld kind %u: rule %s class %s action %s (a core defect; occurrence %llu)",
                    static_cast<long long>(d.dispatchSeq), static_cast<long long>(d.commandIdHigh),
                    static_cast<long long>(d.commandIdLow), static_cast<unsigned>(d.kind), domain::forwarder::nameOf(c.rule),
                    domain::forwarder::nameOf(c.cls), domain::forwarder::actionOf(c.cls),
                    static_cast<unsigned long long>(perRule));
            }
        }
        m_resync.onDispatch(d, c, m_now);
    }

    void onAdmission(const sbe_adapter::forwarder_codec::Admission &a)
    {
        bool trigger = false;
        if (0 == a.marketId && m_expectOrigin)
        {
            m_expectOrigin = false;
            m_counters.admissionOrigin++;
            trigger = m_watcher.onOrigin(a.marketId, a.gateState, a.gateReasons);
        }
        else
        {
            m_counters.admissionBroadcast++;
            trigger = m_watcher.onBroadcast(a.marketId, a.gateState, a.gateReasons);
        }
        if (trigger)
        {
            log(LogLevel::INFO, "GLOBAL admission changed (state %u, reasons 0x%x): T3 RESYNC(0)",
                static_cast<unsigned>(a.gateState), static_cast<unsigned>(a.gateReasons));
            m_resync.resyncFromZero(domain::forwarder::Trigger::T3, m_now);
        }
    }

    void onOrderReject(const sbe_adapter::forwarder_codec::OrderRejectView &r)
    {
        namespace codec = sbe_adapter::forwarder_codec;
        if (codec::T_REQUEST_PENDING_DISPATCH != r.refTemplateId)
        {
            m_counters.unknown(domain::forwarder::CLUSTER_CORE, codec::T_ORDER_REJECT);
            return;
        }
        if (codec::REJECT_INVALID == r.reason)
        {
            stop(EXIT_SOFTWARE, "F4_INVALID_65", "the core refused our RequestPendingDispatch (corr %lld) as INVALID: encoder bug",
                static_cast<long long>(r.correlationId));
            return;
        }
        if (codec::REJECT_UNREGISTERED_SESSION == r.reason || codec::REJECT_UNAUTHORIZED_CLASS == r.reason)
        {
            m_registrationLost = true; // C3 -> C2 at supervision (F7 if the re-registration is refused)
            return;
        }
        m_counters.unknown(domain::forwarder::CLUSTER_CORE, codec::T_ORDER_REJECT);
    }

    // ------------------------------------------------------------------ ME egress

    void onMeMessage(const client::EgressMessage &m)
    {
        namespace codec = sbe_adapter::forwarder_codec;
        if (m_stopReason.set())
        {
            return;
        }
        m_lastMeFrameNs = m_now;
        m_haveMeFrame = true;
        codec::Header h;
        if (!codec::readHeader(m.payload, m.length, h))
        {
            m_counters.unknown(domain::forwarder::CLUSTER_ME, 0);
            return;
        }
        if (codec::MATCH_SCHEMA_ID != h.schemaId)
        {
            m_counters.unknown(domain::forwarder::CLUSTER_ME, h.templateId);
            return;
        }
        if (codec::MATCH_VERSION != h.version)
        {
            stop(EXIT_CONFIG, "F9_ME_VERSION", "ME frame template %u has order-schema version %u, this binary speaks 12",
                static_cast<unsigned>(h.templateId), static_cast<unsigned>(h.version));
            return;
        }
        if (codec::T_CLUSTER_HEARTBEAT == h.templateId)
        {
            std::int64_t meEpoch = 0;
            if (codec::decodeHeartbeatEpoch(m.payload, m.length, h, meEpoch))
            {
                onEpochVerdict(m_binder.onHeartbeat(meEpoch, m_now), meEpoch);
            }
            return;
        }
        m_counters.unknown(domain::forwarder::CLUSTER_ME, h.templateId); // book/trade frames: S13
    }

    // ------------------------------------------------------------------ verdicts, request, drain

    void checkVerdicts()
    {
        using domain::forwarder::ResyncVerdict;
        switch (m_resync.verdict())
        {
            case ResyncVerdict::PRE_S8_CORE:
                stop(EXIT_CONFIG, "F15_PRE_S8_CORE",
                    "core older than RC2 S8: the head probe (maxEntries 0) was answered with %lld entries",
                    static_cast<long long>(m_resync.verdictDetail()));
                return;
            case ResyncVerdict::INVARIANT:
                stop(EXIT_SOFTWARE, "F2_INVARIANT", "resync invariant broken: push of seq %lld not ascending (cursor %lld)",
                    static_cast<long long>(m_resync.verdictDetail()), static_cast<long long>(m_resync.cursor()));
                return;
            default:
                return;
        }
    }

    int offerRequest()
    {
        namespace codec = sbe_adapter::forwarder_codec;
        const domain::forwarder::PendingRequest *request = m_resync.requestDue();
        if (nullptr == request || CoreLink::REGISTERED != m_coreLink)
        {
            return 0;
        }
        const std::size_t length = codec::encodeRequestPendingDispatch(m_requestFrame, sizeof(m_requestFrame), *request);
        if (0 == length)
        {
            stop(EXIT_SOFTWARE, "F2_ENCODE_65", "RequestPendingDispatch did not encode");
            return 0;
        }
        if (m_core.tryOffer(m_requestFrame, length).ok())
        {
            m_resync.onRequestSent(m_now);
        }
        else
        {
            m_counters.requestOfferBackPressured++; // retried on the next cycle, same correlation id
        }
        return 1;
    }

    int drainRing()
    {
        namespace codec = sbe_adapter::forwarder_codec;
        using domain::forwarder::FramePlan;
        using domain::forwarder::FrameType;
        if (MeLink::BOUND != m_meLink)
        {
            return 0; // 4.2: nothing is offered to an ME session whose epoch is not verified (M3)
        }
        int offers = 0;
        while (offers < DRAIN_OFFERS_PER_CYCLE)
        {
            const FramePlan plan = m_ring.nextFrame();
            if (FrameType::NONE == plan.type)
            {
                break;
            }
            std::size_t length = 0;
            if (FrameType::COMMAND == plan.type)
            {
                length = codec::encodeDurableCommand(m_meFrame, sizeof(m_meFrame), m_ring.at(0).command);
            }
            else
            {
                const std::uint32_t n = plan.count < codec::RELEASE_FRAME_MAX_ENTRIES ? plan.count : codec::RELEASE_FRAME_MAX_ENTRIES;
                for (std::uint32_t i = 0; i < n; i++)
                {
                    m_releaseScratch[i] = m_ring.at(i).release;
                }
                length = codec::encodeReleaseCommandIds(m_meFrame, sizeof(m_meFrame), plan.firstSeq, m_releaseScratch, n);
            }
            if (0 == length)
            {
                stop(EXIT_SOFTWARE, "F2_ENCODE_ME", "ME frame did not encode (plan type %u count %u)",
                    static_cast<unsigned>(plan.type), static_cast<unsigned>(plan.count));
                return offers;
            }
            client::OfferResult result = m_me.tryOffer(m_meFrame, length);
            offers++;
            if (!result.ok() &&
                domain::forwarder::test_seams::disabled(domain::forwarder::test_seams::Rule::AGENT_ONE_ATTEMPT_PER_CYCLE))
            {
                for (int retry = 0; retry < 3 && !result.ok(); retry++)
                {
                    result = m_me.tryOffer(m_meFrame, length); // RED shape: retried in place
                }
            }
            if (!result.ok())
            {
                // F11 / F12: stop draining this cycle, nothing popped; MAX_POSITION / TRANSPORT_ERROR close the
                // session inside the client (T2 at the next M3).
                if (client::OfferStatus::BACK_PRESSURED == result.status || client::OfferStatus::NOT_CONNECTED == result.status ||
                    client::OfferStatus::NOT_READY == result.status)
                {
                    m_counters.meBackPressure++;
                }
                else
                {
                    m_counters.meOfferFailures++;
                }
                break;
            }
            const std::uint32_t popped = FrameType::COMMAND == plan.type ? 1u :
                (plan.count < codec::RELEASE_FRAME_MAX_ENTRIES ? plan.count : codec::RELEASE_FRAME_MAX_ENTRIES);
            for (std::uint32_t i = 0; i < popped; i++)
            {
                const std::int64_t seq = m_ring.at(i).seq;
                if (seq > m_lastSentSeq)
                {
                    m_lastSentSeq = seq;
                }
            }
            if (FrameType::COMMAND == plan.type)
            {
                m_counters.sentTemplate9++;
            }
            else
            {
                m_counters.sentTemplate10++;
                m_counters.releaseEntriesSent += popped;
            }
            m_ring.pop(popped);
            m_ringHeadSinceNs = m_now;
        }
        return offers;
    }

    CoreClient &m_core;
    MeClient &m_me;
    AgentSettings m_settings;
    LogRing *m_log;

    domain::forwarder::SendRing m_ring;
    domain::forwarder::ResyncMachine m_resync;
    domain::forwarder::EpochBinder m_binder;
    domain::forwarder::AdmissionWatcher m_watcher;
    domain::forwarder::ForwarderCounters m_counters;
    std::uint64_t m_invalidPerRule[domain::forwarder::RULE_COUNT] = {};
    std::optional<Registrar> m_registrar;

    CoreLink m_coreLink = CoreLink::DOWN;
    MeLink m_meLink = MeLink::DOWN;
    client::ExponentialBackoff m_coreBackoff;
    client::ExponentialBackoff m_meBackoff;
    std::int64_t m_coreNextConnectNs = 0;
    std::int64_t m_meNextConnectNs = 0;
    std::int64_t m_registeredTerm = -1;
    std::uint64_t m_registeredSessionEpoch = 0;
    std::int64_t m_meTerm = -1;
    bool m_expectOrigin = false;
    bool m_registrationLost = false;

    std::int64_t m_now = 0;
    std::int64_t m_lastMeFrameNs = 0;
    bool m_haveMeFrame = false;
    std::int64_t m_lastSentSeq = 0;
    std::int64_t m_ringHeadSinceNs = 0;

    bool m_stopRequested = false;
    bool m_stopped = false;
    StopReason m_stopReason;

    std::uint8_t m_requestFrame[64] = {};
    std::uint8_t m_meFrame[sbe_adapter::forwarder_codec::RELEASE_FRAME_MAX_LENGTH + 64] = {};
    domain::forwarder::ReleaseEntry m_releaseScratch[sbe_adapter::forwarder_codec::RELEASE_FRAME_MAX_ENTRIES] = {};
};

}}}

#endif
