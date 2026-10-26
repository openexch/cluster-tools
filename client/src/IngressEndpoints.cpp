#include "cluster_tools/client/IngressEndpoints.h"

#include <charconv>

namespace cluster_tools { namespace client {

const IngressMember *ParsedEndpoints::find(std::int32_t id) const
{
    for (const IngressMember &member : members)
    {
        if (member.id == id)
        {
            return &member;
        }
    }
    return nullptr;
}

ParsedEndpoints parseIngressEndpoints(std::string_view spec)
{
    ParsedEndpoints result;
    if (spec.empty())
    {
        result.error = "no ingress endpoints";
        return result;
    }
    std::size_t pos = 0;
    while (pos <= spec.size())
    {
        std::size_t comma = spec.find(',', pos);
        if (std::string_view::npos == comma)
        {
            comma = spec.size();
        }
        const std::string_view token = spec.substr(pos, comma - pos);
        const std::size_t eq = token.find('=');
        if (token.empty() || std::string_view::npos == eq || 0 == eq || eq + 1 >= token.size())
        {
            result.error = "bad ingress endpoint token '" + std::string(token) + "' (want id=host:port)";
            result.members.clear();
            return result;
        }
        std::int32_t id = -1;
        const std::string_view idText = token.substr(0, eq);
        const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
        if (parsed.ec != std::errc() || parsed.ptr != idText.data() + idText.size() || id < 0)
        {
            result.error = "bad member id in token '" + std::string(token) + "'";
            result.members.clear();
            return result;
        }
        const std::string_view endpoint = token.substr(eq + 1);
        const std::size_t colon = endpoint.rfind(':');
        if (std::string_view::npos == colon || 0 == colon || colon + 1 >= endpoint.size())
        {
            result.error = "endpoint '" + std::string(endpoint) + "' of member " + std::to_string(id) +
                " is not host:port";
            result.members.clear();
            return result;
        }
        if (nullptr != result.find(id))
        {
            result.error = "member id " + std::to_string(id) + " appears twice";
            result.members.clear();
            return result;
        }
        result.members.push_back(IngressMember{ id, std::string(endpoint) });
        pos = comma + 1;
        if (comma == spec.size())
        {
            break;
        }
    }
    return result;
}

std::string validateIngressBaseChannel(std::string_view baseChannel)
{
    if (baseChannel.substr(0, 9) != "aeron:udp")
    {
        return "ingress channel must be an aeron:udp channel, got '" + std::string(baseChannel) + "'";
    }
    if (baseChannel.find("endpoint=") != std::string_view::npos)
    {
        return "ingress channel must not carry endpoint= (the client sets it per member)";
    }
    return {};
}

std::string ingressChannelFor(std::string_view baseChannel, std::string_view endpoint)
{
    std::string channel(baseChannel);
    channel += (std::string_view::npos == baseChannel.find('?')) ? '?' : '|';
    channel += "endpoint=";
    channel += endpoint;
    return channel;
}

}}
