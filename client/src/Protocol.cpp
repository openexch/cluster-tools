#include "cluster_tools/client/Protocol.h"

namespace cluster_tools { namespace client {

const char *nameOf(ClientState state) noexcept
{
    switch (state)
    {
        case ClientState::IDLE: return "IDLE";
        case ClientState::CONNECTING: return "CONNECTING";
        case ClientState::CONNECTED: return "CONNECTED";
        case ClientState::AWAIT_NEW_LEADER_CONNECTION: return "AWAIT_NEW_LEADER_CONNECTION";
        case ClientState::AWAIT_NEW_LEADER: return "AWAIT_NEW_LEADER";
        case ClientState::CLOSED: return "CLOSED";
        case ClientState::FAILED: return "FAILED";
    }
    return "?";
}

const char *nameOf(OfferStatus status) noexcept
{
    switch (status)
    {
        case OfferStatus::OK: return "OK";
        case OfferStatus::BACK_PRESSURED: return "BACK_PRESSURED";
        case OfferStatus::NOT_CONNECTED: return "NOT_CONNECTED";
        case OfferStatus::CLOSED: return "CLOSED";
        case OfferStatus::MAX_POSITION_EXCEEDED: return "MAX_POSITION_EXCEEDED";
        case OfferStatus::NOT_READY: return "NOT_READY";
        case OfferStatus::TRANSPORT_ERROR: return "TRANSPORT_ERROR";
    }
    return "?";
}

const char *nameOf(CloseReason reason) noexcept
{
    switch (reason)
    {
        case CloseReason::NONE: return "NONE";
        case CloseReason::CLIENT_CLOSE: return "CLIENT_CLOSE";
        case CloseReason::SESSION_CLOSED_BY_CLUSTER: return "SESSION_CLOSED_BY_CLUSTER";
        case CloseReason::NEW_LEADER_TIMEOUT: return "NEW_LEADER_TIMEOUT";
        case CloseReason::NEW_LEADER_CONNECTION_TIMEOUT: return "NEW_LEADER_CONNECTION_TIMEOUT";
        case CloseReason::MAX_POSITION_EXCEEDED: return "MAX_POSITION_EXCEEDED";
        case CloseReason::TRANSPORT_ERROR: return "TRANSPORT_ERROR";
    }
    return "?";
}

const char *nameOf(ConnectError error) noexcept
{
    switch (error)
    {
        case ConnectError::NONE: return "NONE";
        case ConnectError::INVALID_SETTINGS: return "INVALID_SETTINGS";
        case ConnectError::ATTEMPTS_EXHAUSTED: return "ATTEMPTS_EXHAUSTED";
        case ConnectError::AUTHENTICATION_REJECTED: return "AUTHENTICATION_REJECTED";
        case ConnectError::CLUSTER_ERROR: return "CLUSTER_ERROR";
        case ConnectError::CHALLENGE_UNSUPPORTED: return "CHALLENGE_UNSUPPORTED";
    }
    return "?";
}

}}
