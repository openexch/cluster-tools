#ifndef CLUSTER_TOOLS_APP_SESSION_STUB_SESSION_STUB_H
#define CLUSTER_TOOLS_APP_SESSION_STUB_SESSION_STUB_H

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <string>

#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "cluster_tools/runtime/LogRing.h"
#include "session-stub/StubCommands.h"
#include "session-stub/StubOptions.h"

// SessionStub - the duty cycle of ct_session_stub, over any ClusterTransport (Aeron in the binary, the FakeTransport
// in the tests): one GATEWAY core session with its registrar, and a poll throttle.
//
// Each dutyCycle():
//   1. client.doWork() and registrar.doWork() EVERY cycle: connect steps, keepalives (ClientSettings
//      keepAliveIntervalNs, sent from doWork independent of poll) and the registration (re-)send keep running whatever
//      the throttle says;
//   2. client.poll() only when the throttle allows it: every cycle (off), at most once per interval with a fragment
//      limit (throttle <ms> [n]), or never (pause). Egress the stub does not poll stays in the core's per-session egress
//      queue: that is the slow consumer the drills need, with a session the core still sees as alive;
//   3. a closed or failed session is replaced after the reconnect delay (when reconnect is on), and registers again;
//   4. the stdin script advances (sleep and await hold the script, never the cycle).
// Every event is one line in the LogRing (never a write on the duty thread); main drains it to stdout.
// A REJECTED registration ends the stub with 78 (a configuration error, as every owner of the registrar).

namespace cluster_tools { namespace app { namespace session_stub {

// Negative-control seams, compiled only into the seamed test build (CT_SESSION_STUB_SEAMED, never the stub binary):
// a rule is off when CT_TEST_DISABLE_<RULE>=1 (read once per rule). tests/negative-controls.txt names each one.
//   DOWORK_EVERY_CYCLE  off: doWork() runs only when the throttle allows a poll (keepalives stop with the poll)
//   POLL_THROTTLE       off: poll() runs every cycle whatever the throttle says
#if defined(CT_SESSION_STUB_SEAMED)
inline bool stubRuleOff(const char *variable)
{
    const char *value = std::getenv(variable);
    return nullptr != value && std::string(value) == "1";
}
#define CT_STUB_RULE_OFF(rule) ([] { static const bool off = stubRuleOff("CT_TEST_DISABLE_" #rule); return off; }())
#else
#define CT_STUB_RULE_OFF(rule) false
#endif

struct StubCounters
{
    std::uint64_t dutyCycles = 0;
    std::uint64_t polls = 0;
    std::uint64_t fragments = 0;
    std::uint64_t egressMessages = 0;
    std::uint64_t egressBytes = 0;
    std::uint64_t offers = 0;
    std::uint64_t offersOk = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t commandErrors = 0;
    std::array<std::uint64_t, 256> moneyTemplates{}; // egress count per money-schema template id (< 256)
};

template<typename Client>
class SessionStub
{
public:
    using Registrar = client::CoreSessionRegistrar<Client>;
    static constexpr std::int64_t MS = 1'000'000LL;
    static constexpr std::uint16_t MONEY_SCHEMA_ID = 2;

    SessionStub(Client &client, const StubOptions &options, runtime::LogRing &out) :
        m_client(client),
        m_registrar(client, registrarSettings(options)),
        m_out(out),
        m_channels(options.channels),
        m_defaultFragments(options.pollFragments),
        m_pollIntervalNs(options.pollIntervalMs * MS),
        m_pollFragments(options.pollFragments),
        m_statusIntervalNs(options.statusIntervalMs * MS),
        m_reconnect(options.reconnect),
        m_reconnectDelayNs(options.reconnectDelayMs * MS)
    {
    }

    static client::RegistrarSettings registrarSettings(const StubOptions &options)
    {
        client::RegistrarSettings r;
        r.sessionClass = client::SessionClass::GATEWAY;
        r.credentialClass = client::SessionClass::GATEWAY; // main refuses a key of another class before this
        r.channels = options.channels;
        r.identity = options.instanceId;
        return r;
    }

    std::string validate() const { return m_registrar.validateSettings(); }

    // Starts the first session. False = the client refused (its settings), with the client's detail.
    bool start()
    {
        const std::int64_t now = m_client.nanoTime();
        m_nextStatusNs = now + m_statusIntervalNs;
        emit(now, "event=start channels=%s mask=0x%02x poll_interval_ms=%" PRId64 " poll_fragments=%d",
            channelMaskNames(m_channels).c_str(), static_cast<unsigned>(m_channels), m_pollIntervalNs / MS, m_pollFragments);
        return m_client.connect();
    }

    // Duty thread only. Commands run in submission order.
    void submit(Command command) { m_script.push_back(std::move(command)); }
    void reportCommandError(const std::string &line, const std::string &error)
    {
        m_counters.commandErrors++;
        emit(m_client.nanoTime(), "event=command_error line=\"%s\" error=\"%s\"", line.c_str(), error.c_str());
    }

    int dutyCycle()
    {
        m_counters.dutyCycles++;
        const bool pollNow = pollAllowed(m_client.nanoTime());
        int work = 0;
        if (pollNow || !CT_STUB_RULE_OFF(DOWORK_EVERY_CYCLE))
        {
            work += m_client.doWork();
            work += m_registrar.doWork();
        }
        const std::int64_t now = m_client.nanoTime();

        if (pollNow || CT_STUB_RULE_OFF(POLL_THROTTLE))
        {
            m_lastPollNs = now;
            m_counters.polls++;
            const int n = m_client.poll([this](const client::EgressMessage &m) { onEgress(m); }, m_pollFragments);
            m_counters.fragments += static_cast<std::uint64_t>(n > 0 ? n : 0);
            work += n;
        }

        observe(now);
        if (m_registrar.isRejected() && !m_finished)
        {
            emit(now, "event=registration_rejected detail=\"%s\" exit=78", m_registrar.detail().c_str());
            finish(78);
            return work;
        }
        maybeReconnect(now);
        work += runScript(now);
        if (m_statusIntervalNs > 0 && now - m_nextStatusNs >= 0)
        {
            status(now);
            m_nextStatusNs = now + m_statusIntervalNs;
        }
        return work;
    }

    bool finished() const { return m_finished; }
    int exitCode() const { return m_exitCode; }
    const StubCounters &counters() const { return m_counters; }
    const Registrar &registrar() const { return m_registrar; }
    std::int64_t pollIntervalNs() const { return m_pollIntervalNs; }
    int pollFragments() const { return m_pollFragments; }
    bool paused() const { return m_paused; }

    // Stop path (signal): close the session; the caller exits 0.
    void stop()
    {
        m_client.close();
        finish(0);
    }

private:
    bool pollAllowed(std::int64_t now) const
    {
        if (m_paused)
        {
            return false;
        }
        return 0 == m_pollIntervalNs || m_lastPollNs < 0 || now - m_lastPollNs >= m_pollIntervalNs;
    }

    void onEgress(const client::EgressMessage &m)
    {
        if (m_registrar.onEgress(m))
        {
            return; // the registrar's AdminAck
        }
        m_counters.egressMessages++;
        m_counters.egressBytes += m.length;
        if (m.length >= 8)
        {
            const std::uint16_t templateId = static_cast<std::uint16_t>(m.payload[2] | (m.payload[3] << 8));
            const std::uint16_t schemaId = static_cast<std::uint16_t>(m.payload[4] | (m.payload[5] << 8));
            if (MONEY_SCHEMA_ID == schemaId && templateId < m_counters.moneyTemplates.size())
            {
                m_counters.moneyTemplates[templateId]++;
            }
        }
    }

    void observe(std::int64_t now)
    {
        const client::ClientState state = m_client.state();
        if (state != m_lastState)
        {
            if (client::ClientState::CLOSED == state)
            {
                emit(now, "event=state from=%s to=%s close_reason=%s epoch=%" PRIu64 " detail=\"%s\"", client::nameOf(m_lastState),
                    client::nameOf(state), client::nameOf(m_client.closeReason()), m_client.sessionEpoch(),
                    m_client.lastDetail().c_str());
            }
            else if (client::ClientState::FAILED == state)
            {
                emit(now, "event=state from=%s to=%s connect_error=%s detail=\"%s\"", client::nameOf(m_lastState),
                    client::nameOf(state), client::nameOf(m_client.connectError()), m_client.lastDetail().c_str());
            }
            else
            {
                emit(now, "event=state from=%s to=%s session=%" PRId64 " term=%" PRId64 " leader=%d epoch=%" PRIu64,
                    client::nameOf(m_lastState), client::nameOf(state), m_client.clusterSessionId(),
                    m_client.leadershipTermId(), m_client.leaderMemberId(), m_client.sessionEpoch());
            }
            if (client::ClientState::CLOSED == state || client::ClientState::FAILED == state)
            {
                m_inactiveSinceNs = now;
            }
            m_lastState = state;
        }
        const client::RegistrationState registration = m_registrar.state();
        if (registration != m_lastRegistration)
        {
            emit(now, "event=registration state=%s epoch=%" PRIu64 " term=%" PRId64, client::nameOf(registration),
                m_client.sessionEpoch(), m_client.leadershipTermId());
            m_lastRegistration = registration;
        }
        if (m_client.counters().newLeaderEvents != m_lastNewLeaderEvents)
        {
            m_lastNewLeaderEvents = m_client.counters().newLeaderEvents;
            emit(now, "event=new_leader term=%" PRId64 " leader=%d", m_client.leadershipTermId(), m_client.leaderMemberId());
        }
    }

    void maybeReconnect(std::int64_t now)
    {
        const client::ClientState state = m_client.state();
        if (!m_reconnect || m_finished ||
            (client::ClientState::CLOSED != state && client::ClientState::FAILED != state) ||
            now - m_inactiveSinceNs < m_reconnectDelayNs)
        {
            return;
        }
        m_counters.reconnects++;
        const bool started = m_client.connect();
        emit(now, "event=reconnect started=%s detail=\"%s\"", started ? "true" : "false",
            started ? "" : m_client.lastDetail().c_str());
        m_inactiveSinceNs = now; // a refused connect is retried after the delay, never in a tight loop
    }

    int runScript(std::int64_t now)
    {
        int work = 0;
        while (!m_script.empty() && !m_finished)
        {
            Command &c = m_script.front();
            if (CommandKind::SLEEP == c.kind)
            {
                if (m_holdUntilNs < 0)
                {
                    m_holdUntilNs = now + c.valueMs * MS;
                }
                if (now - m_holdUntilNs < 0)
                {
                    return work;
                }
                m_holdUntilNs = -1;
                m_script.pop_front();
                continue;
            }
            if (CommandKind::AWAIT_REGISTERED == c.kind || CommandKind::AWAIT_CLOSED == c.kind)
            {
                if (m_holdUntilNs < 0)
                {
                    m_holdUntilNs = now + c.valueMs * MS;
                    m_awaitClosedBase = m_client.counters().sessionsClosed;
                }
                const bool met = CommandKind::AWAIT_REGISTERED == c.kind ? m_registrar.isRegistered() :
                    m_client.counters().sessionsClosed > m_awaitClosedBase;
                const bool expired = now - m_holdUntilNs >= 0;
                if (!met && !expired)
                {
                    return work;
                }
                emit(now, "event=await what=%s result=%s", CommandKind::AWAIT_REGISTERED == c.kind ? "registered" : "closed",
                    met ? "ok" : "timeout");
                m_holdUntilNs = -1;
                m_script.pop_front();
                continue;
            }
            execute(c, now);
            m_script.pop_front();
            work++;
        }
        return work;
    }

    void execute(const Command &c, std::int64_t now)
    {
        switch (c.kind)
        {
            case CommandKind::NONE:
                return;
            case CommandKind::INVALID:
                reportCommandError(c.text, c.error);
                return;
            case CommandKind::THROTTLE_OFF:
                m_paused = false;
                m_pollIntervalNs = 0;
                m_pollFragments = m_defaultFragments;
                emit(now, "event=throttle mode=off");
                return;
            case CommandKind::THROTTLE:
                m_paused = false;
                m_pollIntervalNs = c.valueMs * MS;
                m_pollFragments = c.fragments > 0 ? c.fragments : m_defaultFragments;
                emit(now, "event=throttle mode=interval interval_ms=%" PRId64 " fragments=%d", c.valueMs, m_pollFragments);
                return;
            case CommandKind::THROTTLE_PAUSE:
                m_paused = true;
                emit(now, "event=throttle mode=pause");
                return;
            case CommandKind::OFFER:
            {
                m_counters.offers++;
                const client::OfferResult result = m_client.tryOffer(c.bytes.data(), c.bytes.size());
                m_counters.offersOk += result.ok() ? 1 : 0;
                emit(now, "event=offer result=%s bytes=%zu position=%" PRId64, client::nameOf(result.status), c.bytes.size(),
                    result.position);
                return;
            }
            case CommandKind::STATUS:
                status(now);
                return;
            case CommandKind::RECONNECT:
                m_reconnect = c.flag;
                emit(now, "event=reconnect_mode %s", c.flag ? "on" : "off");
                return;
            case CommandKind::CLOSE:
                m_client.close();
                emit(now, "event=close");
                return;
            case CommandKind::QUIT:
                m_client.close();
                emit(now, "event=quit");
                finish(0);
                return;
            case CommandKind::SLEEP:
            case CommandKind::AWAIT_REGISTERED:
            case CommandKind::AWAIT_CLOSED:
                return; // held in runScript
        }
    }

    void status(std::int64_t now)
    {
        const client::ClientCounters &cc = m_client.counters();
        emit(now, "event=status state=%s reg=%s epoch=%" PRIu64 " polls=%" PRIu64 " frags=%" PRIu64 " msgs=%" PRIu64
            " bytes=%" PRIu64 " keepalives=%" PRIu64 " ka_fail=%" PRIu64 " closed=%" PRIu64 " throttle=%s interval_ms=%" PRId64
            " offers=%" PRIu64 "/%" PRIu64,
            client::nameOf(m_client.state()), client::nameOf(m_registrar.state()), m_client.sessionEpoch(), m_counters.polls,
            m_counters.fragments, m_counters.egressMessages, m_counters.egressBytes, cc.keepAlivesSent, cc.keepAliveFailures,
            cc.sessionsClosed, m_paused ? "pause" : (0 == m_pollIntervalNs ? "off" : "interval"), m_pollIntervalNs / MS,
            m_counters.offersOk, m_counters.offers);
        // The money-schema templates received so far, "id:count" (the line is cut at the record size).
        char line[sizeof(runtime::LogRecord::text)];
        int used = std::snprintf(line, sizeof(line), "event=templates");
        for (std::size_t id = 0; id < m_counters.moneyTemplates.size() && used > 0 &&
             static_cast<std::size_t>(used) < sizeof(line); id++)
        {
            if (0 != m_counters.moneyTemplates[id])
            {
                used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used), " %zu:%" PRIu64, id,
                    m_counters.moneyTemplates[id]);
            }
        }
        m_out.push(runtime::LogLevel::INFO, now, "%s", line);
    }

    void emit(std::int64_t now, const char *format, ...) __attribute__((format(printf, 3, 4)))
    {
        char line[sizeof(runtime::LogRecord::text)];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        m_out.push(runtime::LogLevel::INFO, now, "%s", line);
    }

    void finish(int code)
    {
        if (!m_finished)
        {
            m_finished = true;
            m_exitCode = code;
        }
    }

    Client &m_client;
    Registrar m_registrar;
    runtime::LogRing &m_out;
    const std::uint32_t m_channels;
    const int m_defaultFragments;
    std::int64_t m_pollIntervalNs;
    int m_pollFragments;
    bool m_paused = false;
    std::int64_t m_lastPollNs = -1;
    std::int64_t m_statusIntervalNs;
    std::int64_t m_nextStatusNs = 0;
    bool m_reconnect;
    std::int64_t m_reconnectDelayNs;
    std::int64_t m_inactiveSinceNs = 0;
    std::deque<Command> m_script;
    std::int64_t m_holdUntilNs = -1;
    std::uint64_t m_awaitClosedBase = 0;
    client::ClientState m_lastState = client::ClientState::IDLE;
    client::RegistrationState m_lastRegistration = client::RegistrationState::UNREGISTERED;
    std::uint64_t m_lastNewLeaderEvents = 0;
    StubCounters m_counters;
    bool m_finished = false;
    int m_exitCode = 0;
};

}}}

#endif
