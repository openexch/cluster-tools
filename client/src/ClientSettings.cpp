#include "cluster_tools/client/ClientSettings.h"

#include "cluster_tools/client/IngressEndpoints.h"

namespace cluster_tools { namespace client {

std::string validate(const ClientSettings &settings)
{
    const ParsedEndpoints endpoints = parseIngressEndpoints(settings.ingressEndpoints);
    if (!endpoints.ok())
    {
        return "ingressEndpoints: " + endpoints.error;
    }
    if (std::string error = validateIngressBaseChannel(settings.ingressChannel); !error.empty())
    {
        return error;
    }
    if (settings.responseChannel.empty())
    {
        return "responseChannel is required (an egress channel the cluster can route back to)";
    }
    if (settings.responseChannel.substr(0, 9) != "aeron:udp" ||
        settings.responseChannel.find("endpoint=") == std::string::npos)
    {
        return "responseChannel must be aeron:udp?endpoint=<routable-ip>:<port>, got '" +
            settings.responseChannel + "'";
    }
    if (settings.responseChannel.find("endpoint=0.0.0.0") != std::string::npos)
    {
        return "responseChannel endpoint 0.0.0.0 is not routable from the cluster";
    }
    if (settings.ingressStreamId == settings.egressStreamId)
    {
        return "ingressStreamId and egressStreamId must differ";
    }
    if (settings.credentials.empty() && !settings.allowEmptyCredentials)
    {
        return "credentials are required (set allowEmptyCredentials only for a cluster without an authenticator)";
    }
    if (settings.messageTimeoutNs <= 0 || settings.messageTimeoutNs > MAX_TIMEOUT_NS)
    {
        return "messageTimeoutNs must be in (0, 1 h]";
    }
    if (settings.connectBackoffMaxNs > MAX_TIMEOUT_NS || settings.keepAliveIntervalNs > MAX_TIMEOUT_NS ||
        settings.keepAliveRetryIntervalNs > MAX_TIMEOUT_NS || settings.newLeaderTimeoutNs > MAX_TIMEOUT_NS)
    {
        return "timeouts must not exceed 1 h (deadline arithmetic)";
    }
    if (settings.maxConnectAttempts <= 0)
    {
        return "maxConnectAttempts must be > 0 (connect retries are bounded)";
    }
    if (settings.connectBackoffInitialNs <= 0 || settings.connectBackoffMaxNs < settings.connectBackoffInitialNs)
    {
        return "connect backoff must be 0 < initial <= max";
    }
    if (!settings.disableKeepAliveForTest &&
        (settings.keepAliveIntervalNs <= 0 || settings.keepAliveIntervalNs > 1'000'000'000LL))
    {
        return "keepAliveIntervalNs must be in (0, 1 s]";
    }
    if (settings.keepAliveRetryIntervalNs <= 0 || settings.keepAliveRetryIntervalNs > settings.keepAliveIntervalNs)
    {
        return "keepAliveRetryIntervalNs must be in (0, keepAliveIntervalNs]";
    }
    if (settings.newLeaderTimeoutNs < 0)
    {
        return "newLeaderTimeoutNs must be >= 0 (0 = derived from the leader heartbeat timeout)";
    }
    if (settings.connectRequestCapacity < 128)
    {
        return "connectRequestCapacity too small";
    }
    return {};
}

}}
