#ifndef CLUSTER_TOOLS_CLIENT_PROTOCOL_H
#define CLUSTER_TOOLS_CLIENT_PROTOCOL_H

#include <cstdint>

// Vocabulary of the cluster client: offer result codes (numerically Aeron's, so a transport
// passes them through unchanged), client states and the typed reasons a session ends.
// Pure: no Aeron, no SBE.

namespace cluster_tools { namespace client {

// Aeron Publication::offer results (aeron/concurrent/logbuffer + Publication.h). The Aeron
// transport static_asserts these against the real constants.
inline constexpr std::int64_t OFFER_NOT_CONNECTED = -1;
inline constexpr std::int64_t OFFER_BACK_PRESSURED = -2;
inline constexpr std::int64_t OFFER_ADMIN_ACTION = -3;
inline constexpr std::int64_t OFFER_PUBLICATION_CLOSED = -4;
inline constexpr std::int64_t OFFER_MAX_POSITION_EXCEEDED = -5;
// Not Aeron: the transport caught an exception (message too long, closed client, ...).
inline constexpr std::int64_t OFFER_TRANSPORT_ERROR = -100;

// Cluster session protocol (aeron-cluster-codecs.xml schema 111), mirrored from
// io.aeron.cluster.client.AeronCluster.Configuration (1.53.1): only the major is checked
// by the consensus module.
inline constexpr std::int32_t PROTOCOL_SEMANTIC_VERSION = (0 << 16) | (3 << 8) | 0;

// ConsensusModule.Configuration.LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS, used when the
// SessionEvent OK predates the leaderHeartbeatTimeoutNs field.
inline constexpr std::int64_t LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS = 10'000'000'000LL;

enum class ClientState : std::uint8_t
{
    IDLE,                        // never connected, or reset
    CONNECTING,                  // async connect in progress (attempts + backoff inside)
    CONNECTED,                   // session open, ingress to the leader
    AWAIT_NEW_LEADER_CONNECTION, // NewLeaderEvent seen; first positive offer returns to CONNECTED
    AWAIT_NEW_LEADER,            // egress image lost or ingress NOT_CONNECTED; deadline = newLeaderTimeout
    CLOSED,                      // session over (closeReason says why); connect() opens a NEW session
    FAILED,                      // connect cycle ended without a session (connectError says why)
};

enum class OfferStatus : std::uint8_t
{
    OK,
    BACK_PRESSURED,        // incl. ADMIN_ACTION; retry later, nothing was sent
    NOT_CONNECTED,         // the ingress publication has no subscriber (election, dead leader)
    CLOSED,                // publication closed
    MAX_POSITION_EXCEEDED, // term exhausted; the client closes the session
    NOT_READY,             // no session in a state that can send (connecting, closed, ...)
    TRANSPORT_ERROR,       // the transport refused the offer (e.g. message longer than allowed)
};

struct OfferResult
{
    OfferStatus status = OfferStatus::NOT_READY;
    std::int64_t position = 0; // > 0 only when status == OK

    bool ok() const { return OfferStatus::OK == status; }
};

enum class CloseReason : std::uint8_t
{
    NONE,
    CLIENT_CLOSE,                   // close() by the owner
    SESSION_CLOSED_BY_CLUSTER,      // SessionEvent CLOSED (timeout, service close, election truncation)
    NEW_LEADER_TIMEOUT,             // AWAIT_NEW_LEADER deadline passed without a NewLeaderEvent
    NEW_LEADER_CONNECTION_TIMEOUT,  // AWAIT_NEW_LEADER_CONNECTION deadline passed without a positive offer
    MAX_POSITION_EXCEEDED,
    TRANSPORT_ERROR,                // the new leader's ingress could not be added
};

enum class ConnectError : std::uint8_t
{
    NONE,
    INVALID_SETTINGS,
    ATTEMPTS_EXHAUSTED,      // every attempt timed out or lost its connection
    AUTHENTICATION_REJECTED, // SessionEvent AUTHENTICATION_REJECTED: wrong/missing credential (terminal)
    CLUSTER_ERROR,           // SessionEvent ERROR, e.g. version or session limit (terminal)
    CHALLENGE_UNSUPPORTED,   // the cluster challenged; v1 credentials refuse challenges (terminal)
};

const char *nameOf(ClientState state) noexcept;
const char *nameOf(OfferStatus status) noexcept;
const char *nameOf(CloseReason reason) noexcept;
const char *nameOf(ConnectError error) noexcept;

}}

#endif
