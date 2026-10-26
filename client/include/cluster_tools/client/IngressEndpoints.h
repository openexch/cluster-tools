#ifndef CLUSTER_TOOLS_CLIENT_INGRESS_ENDPOINTS_H
#define CLUSTER_TOOLS_CLIENT_INGRESS_ENDPOINTS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The AeronCluster ingress endpoint list "id=host:port,id=host:port" (the form both the
// operator config and NewLeaderEvent/REDIRECT details carry). Strict: an empty token, a
// missing '=', a non-numeric or duplicate id, or an endpoint without host:port is an error,
// never skipped. Pure.

namespace cluster_tools { namespace client {

struct IngressMember
{
    std::int32_t id = -1;
    std::string endpoint; // host:port
};

struct ParsedEndpoints
{
    std::vector<IngressMember> members; // in input order
    std::string error;                  // empty on success

    bool ok() const { return error.empty(); }
    const IngressMember *find(std::int32_t id) const;
};

ParsedEndpoints parseIngressEndpoints(std::string_view spec);

// "aeron:udp?term-length=4m|mtu=8192" + "10.0.0.1:9302" -> "...|endpoint=10.0.0.1:9302".
// The base must be a UDP channel without its own endpoint (the client owns that parameter).
std::string ingressChannelFor(std::string_view baseChannel, std::string_view endpoint);

// Validation of an ingress base channel: empty string = valid.
std::string validateIngressBaseChannel(std::string_view baseChannel);

}}

#endif
