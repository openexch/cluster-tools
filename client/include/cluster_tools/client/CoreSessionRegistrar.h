#ifndef CLUSTER_TOOLS_CLIENT_CORE_SESSION_REGISTRAR_H
#define CLUSTER_TOOLS_CLIENT_CORE_SESSION_REGISTRAR_H

#include <cstdint>
#include <exception>
#include <string>

#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/Credentials.h"

#include "com_openexchange_assets_infrastructure_generated/AdminAck.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/RegisterSession.h"

// CoreSessionRegistrar - RegisterSession (money-schema template 80) for a core session, with
// the AdminAck wait the RC2 specs require:
//   - declared after every connect AND after every new leader: the egress mask and the
//     resumable mark are leader-local (RC1 S3 README items 5 and 8), so a registration made
//     in an earlier term means nothing on the new leader;
//   - AdminAck(refTemplateId 80) ACCEPTED or DUPLICATE = registered; REJECTED = terminal (a
//     config error: the owner exits non-zero);
//   - a lost AdminAck is re-sent after resendIntervalNs (critic W12 / refuter M9: ORIGIN egress
//     is at-most-once, so without the re-send the owner would hang CONNECTED forever).
// The same correlation id is kept across re-sends of one round, so a late ack of the first
// send still resolves the round. A new round (new term or new session) takes a new id.
//
// A REJECTED AdminAck can be lost: the core closes a refused session through consensus and the
// close drops the session's egress queue, ack included. The registrar therefore also treats
// maxUnansweredSessions consecutive sessions closed by the cluster while a registration was
// pending as REJECTED (terminal): reconnecting forever would be a live process that never works.
// It also refuses to send at all when its class differs from the credential's class (the only
// reachable REJECTED cause), so that case never reaches the core.
//
// Wiring: call doWork() from the duty cycle and offer every core egress message to onEgress()
// first; it returns true when it consumed an AdminAck for this registrar. Owner rule: on
// isRejected() exit non-zero (configuration error), never reconnect.

namespace cluster_tools { namespace client {

// Bits of the money-schema EgressChannels set (money-schema.xml, choice values).
namespace core_channel {
inline constexpr std::uint32_t ACKS = 1u << 0;
inline constexpr std::uint32_t BALANCES = 1u << 1;
inline constexpr std::uint32_t SETTLEMENTS = 1u << 2;
inline constexpr std::uint32_t SNAPSHOTS = 1u << 3;
inline constexpr std::uint32_t ORDERS = 1u << 4;
inline constexpr std::uint32_t DISPATCH = 1u << 5;
inline constexpr std::uint32_t CONTROL = 1u << 6;
}

struct RegistrarSettings
{
    SessionClass sessionClass = SessionClass::UNSET;    // required
    SessionClass credentialClass = SessionClass::UNSET; // required: CoreCredentials::sessionClass of the client's credential
    std::uint32_t channels = 0;                       // core_channel bits
    std::uint8_t readerKind = 0;
    bool resumable = true; // the core refuses false (critic D16)
    std::int64_t identity = 0;
    std::int64_t sourceEpoch = 0;
    std::int64_t resendIntervalNs = 5'000'000'000LL;
    std::int64_t correlationIdBase = 0x5E55'0000'0000LL;
    // Consecutive sessions closed by the cluster with a registration pending before the
    // registrar declares the registration REJECTED (lost REJECTED ack).
    int maxUnansweredSessions = 3;
};

enum class RegistrationState : std::uint8_t
{
    UNREGISTERED, // nothing sent in this term of this session
    PENDING,      // sent, AdminAck not seen yet
    REGISTERED,   // ACCEPTED or DUPLICATE in this term of this session
    REJECTED,     // terminal
};

inline const char *nameOf(RegistrationState state) noexcept
{
    switch (state)
    {
        case RegistrationState::UNREGISTERED: return "UNREGISTERED";
        case RegistrationState::PENDING: return "PENDING";
        case RegistrationState::REGISTERED: return "REGISTERED";
        case RegistrationState::REJECTED: return "REJECTED";
    }
    return "?";
}

struct RegistrarCounters
{
    std::uint64_t sends = 0;
    std::uint64_t resends = 0;
    std::uint64_t sendBackPressured = 0;
    std::uint64_t acksAccepted = 0;
    std::uint64_t acksDuplicate = 0;
    std::uint64_t acksRejected = 0;
    std::uint64_t acksIgnored = 0;
    std::uint64_t rounds = 0;
    std::uint64_t closesWhilePending = 0;
};

template<typename Client>
class CoreSessionRegistrar
{
public:
    static constexpr std::uint16_t REGISTER_SESSION_TEMPLATE_ID = 80;

    CoreSessionRegistrar(Client &client, RegistrarSettings settings) :
        m_client(client), m_settings(settings)
    {
    }

    // Empty = valid.
    std::string validateSettings() const
    {
        if (SessionClass::UNSET == m_settings.sessionClass)
        {
            return "registrar: sessionClass is required";
        }
        if (m_settings.credentialClass != m_settings.sessionClass)
        {
            return std::string("registrar: RegisterSession class ") + nameOf(m_settings.sessionClass) +
                " differs from the credential's class " + nameOf(m_settings.credentialClass) +
                " (the core would REJECT it)";
        }
        if (m_settings.maxUnansweredSessions <= 0)
        {
            return "registrar: maxUnansweredSessions must be > 0";
        }
        if (!m_settings.resumable)
        {
            return "registrar: the core refuses resumable=false (critic D16)";
        }
        if (m_settings.channels >= (1u << 7))
        {
            return "registrar: unknown channel bits";
        }
        if (m_settings.resendIntervalNs <= 0 || m_settings.resendIntervalNs > MAX_TIMEOUT_NS)
        {
            return "registrar: resendIntervalNs must be in (0, 1 h]";
        }
        return {};
    }

    int doWork()
    {
        if (RegistrationState::REJECTED == m_state)
        {
            return 0;
        }
        if (!m_settingsChecked)
        {
            m_settingsChecked = true;
            m_settingsError = validateSettings();
        }
        if (!m_settingsError.empty())
        {
            m_detail = m_settingsError; // never send an invalid registration
            return 0;
        }
        const std::int64_t nowNs = m_client.nanoTime();
        if (RegistrationState::PENDING == m_state && !m_closeCounted &&
            ClientState::CLOSED == m_client.state() &&
            CloseReason::SESSION_CLOSED_BY_CLUSTER == m_client.closeReason())
        {
            m_closeCounted = true;
            m_counters.closesWhilePending++;
            if (++m_consecutiveUnanswered >= m_settings.maxUnansweredSessions)
            {
                m_state = RegistrationState::REJECTED;
                m_detail = "RegisterSession unanswered in " + std::to_string(m_consecutiveUnanswered) +
                    " consecutive sessions the cluster closed: treated as REJECTED (a refused session's AdminAck "
                    "is dropped with its egress queue); configuration error, exit";
                return 1;
            }
        }
        if (m_client.sessionEpoch() != m_epoch || m_client.leadershipTermId() != m_term)
        {
            m_closeCounted = false;
            // New session or new leader: the old registration is void on this leader.
            m_epoch = m_client.sessionEpoch();
            m_term = m_client.leadershipTermId();
            m_state = RegistrationState::UNREGISTERED;
            m_correlationId = m_settings.correlationIdBase + static_cast<std::int64_t>(++m_counters.rounds);
        }
        if (!m_client.isConnected())
        {
            return 0;
        }
        const bool resendDue = RegistrationState::PENDING == m_state &&
            nowNs - m_sentAtNs >= m_settings.resendIntervalNs;
        if (RegistrationState::UNREGISTERED != m_state && !resendDue)
        {
            return 0;
        }
        const std::size_t length = encode();
        const OfferResult result = m_client.tryOffer(m_frame, length);
        if (!result.ok())
        {
            m_counters.sendBackPressured++;
            return 1; // retried on the next duty cycle
        }
        if (resendDue)
        {
            m_counters.resends++;
        }
        m_counters.sends++;
        m_state = RegistrationState::PENDING;
        m_sentAtNs = nowNs;
        return 1;
    }

    // True when the message was an AdminAck for this registrar's RegisterSession.
    bool onEgress(const EgressMessage &message)
    {
        namespace money = com::openexchange::assets::infrastructure::generated;
        if (message.length < money::MessageHeader::encodedLength())
        {
            return false;
        }
        try
        {
            char *buffer = reinterpret_cast<char *>(const_cast<std::uint8_t *>(message.payload));
            money::MessageHeader header(buffer, 0, message.length, 0);
            if (header.schemaId() != money::MessageHeader::sbeSchemaId() ||
                header.templateId() != money::AdminAck::sbeTemplateId())
            {
                return false;
            }
            money::AdminAck ack(buffer, money::MessageHeader::encodedLength(), message.length,
                header.blockLength(), header.version());
            if (ack.refTemplateId() != REGISTER_SESSION_TEMPLATE_ID || ack.correlationId() != m_correlationId)
            {
                m_counters.acksIgnored++;
                return false;
            }
            const std::uint8_t result = ack.resultRaw();
            m_lastReason = ack.reason();
            if (result == static_cast<std::uint8_t>(money::AdminResult::ACCEPTED) ||
                result == static_cast<std::uint8_t>(money::AdminResult::DUPLICATE))
            {
                (result == static_cast<std::uint8_t>(money::AdminResult::ACCEPTED) ?
                    m_counters.acksAccepted : m_counters.acksDuplicate)++;
                if (RegistrationState::REJECTED != m_state)
                {
                    m_state = RegistrationState::REGISTERED;
                    m_consecutiveUnanswered = 0;
                }
            }
            else
            {
                m_counters.acksRejected++;
                m_state = RegistrationState::REJECTED;
                m_detail = "RegisterSession REJECTED by the core (reason " + std::to_string(m_lastReason) +
                    "): class/credential/resumable mismatch is a configuration error";
            }
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    // RC2 S12 (contract 4.1 C3 -> C2): the core says this session is not registered on this leader (an
    // OrderReject UNREGISTERED_SESSION or UNAUTHORIZED_CLASS naming one of the owner's commands, e.g. between a
    // D8b revoke and its close). Starts a new round (new correlation id) in the same session and term; the next
    // doWork() sends it. REJECTED stays terminal.
    void requestReRegistration()
    {
        if (RegistrationState::REJECTED == m_state)
        {
            return;
        }
        m_state = RegistrationState::UNREGISTERED;
        m_correlationId = m_settings.correlationIdBase + static_cast<std::int64_t>(++m_counters.rounds);
    }

    RegistrationState state() const { return m_state; }
    // Registered in the client's CURRENT, CONNECTED session and term (what readiness must
    // check): a closed session keeps its epoch and term until the next connect, so the state
    // alone would report a registration that no longer exists.
    bool isRegistered() const
    {
        return RegistrationState::REGISTERED == m_state && m_client.isConnected() &&
            m_client.sessionEpoch() == m_epoch && m_client.leadershipTermId() == m_term;
    }
    bool isRejected() const { return RegistrationState::REJECTED == m_state; }
    std::int64_t correlationId() const { return m_correlationId; }
    const std::string &detail() const { return m_detail; }
    const RegistrarCounters &counters() const { return m_counters; }
    int consecutiveUnansweredSessions() const { return m_consecutiveUnanswered; }

private:
    std::size_t encode()
    {
        namespace money = com::openexchange::assets::infrastructure::generated;
        money::RegisterSession encoder;
        encoder.wrapAndApplyHeader(reinterpret_cast<char *>(m_frame), 0, sizeof(m_frame))
            .correlationId(m_correlationId)
            .sessionClass(static_cast<money::SessionClass::Value>(static_cast<std::uint8_t>(m_settings.sessionClass)))
            .readerKind(m_settings.readerKind)
            .resumable(m_settings.resumable ? money::BoolFlag::TRUE : money::BoolFlag::FALSE)
            .identity(m_settings.identity)
            .sourceEpoch(m_settings.sourceEpoch);
        encoder.channels().clear()
            .acks(0 != (m_settings.channels & core_channel::ACKS))
            .balances(0 != (m_settings.channels & core_channel::BALANCES))
            .settlements(0 != (m_settings.channels & core_channel::SETTLEMENTS))
            .snapshots(0 != (m_settings.channels & core_channel::SNAPSHOTS))
            .orders(0 != (m_settings.channels & core_channel::ORDERS))
            .dispatch(0 != (m_settings.channels & core_channel::DISPATCH))
            .control(0 != (m_settings.channels & core_channel::CONTROL));
        return money::MessageHeader::encodedLength() + encoder.encodedLength();
    }

    Client &m_client;
    RegistrarSettings m_settings;
    RegistrationState m_state = RegistrationState::UNREGISTERED;
    std::uint64_t m_epoch = 0;
    std::int64_t m_term = -1;
    std::int64_t m_correlationId = 0;
    std::int64_t m_sentAtNs = 0;
    std::uint8_t m_lastReason = 0;
    std::string m_detail;
    RegistrarCounters m_counters;
    bool m_settingsChecked = false;
    std::string m_settingsError;
    bool m_closeCounted = false;
    int m_consecutiveUnanswered = 0;
    std::uint8_t m_frame[128] = {};
};

}}

#endif
