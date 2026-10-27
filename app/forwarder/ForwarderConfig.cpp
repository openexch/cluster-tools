#include "ForwarderConfig.h"

#include <cctype>
#include <charconv>

namespace cluster_tools { namespace app { namespace forwarder {

namespace {

std::string trimmed(const char *raw)
{
    if (nullptr == raw)
    {
        return {};
    }
    std::string s(raw);
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !notSpace(static_cast<unsigned char>(s.back())))
    {
        s.pop_back();
    }
    std::size_t start = 0;
    while (start < s.size() && !notSpace(static_cast<unsigned char>(s[start])))
    {
        start++;
    }
    return s.substr(start);
}

std::string required(const EnvGetter &getenv, const char *name, std::vector<std::string> &errors)
{
    std::string value = trimmed(getenv(name));
    if (value.empty())
    {
        errors.push_back(std::string(name) + " is required (no default)");
    }
    return value;
}

client::CoreCredentials credential(const EnvGetter &getenv, const char *name, std::vector<std::string> &errors)
{
    const std::string spec = trimmed(getenv(name));
    if (spec.empty())
    {
        errors.push_back(std::string(name) + " is required (keyId:FORWARDER:secretHex)");
        return {};
    }
    const client::CredentialsResult parsed = client::parseCoreCredentials(spec);
    if (!parsed.ok())
    {
        errors.push_back(std::string(name) + ": " + parsed.error);
        return {};
    }
    if (client::SessionClass::FORWARDER != parsed.credentials.sessionClass)
    {
        errors.push_back(std::string(name) + " is a " + client::nameOf(parsed.credentials.sessionClass) +
            " credential; the forwarder needs FORWARDER");
        return {};
    }
    return parsed.credentials;
}

}

std::string endpointOf(const std::string &channel)
{
    const std::size_t at = channel.find("endpoint=");
    if (std::string::npos == at)
    {
        return {};
    }
    const std::size_t start = at + 9;
    const std::size_t end = channel.find('|', start);
    return channel.substr(start, std::string::npos == end ? std::string::npos : end - start);
}

int endpointPort(const std::string &channel)
{
    const std::string endpoint = endpointOf(channel);
    const std::size_t colon = endpoint.rfind(':');
    if (std::string::npos == colon || colon + 1 >= endpoint.size())
    {
        return -1;
    }
    int port = -1;
    const char *first = endpoint.data() + colon + 1;
    const char *last = endpoint.data() + endpoint.size();
    const auto r = std::from_chars(first, last, port);
    return (r.ec == std::errc() && r.ptr == last && port >= 0 && port <= 65535) ? port : -1;
}

client::ClientSettings coreClientSettings(const ForwarderConfig &config)
{
    client::ClientSettings s;
    s.ingressEndpoints = config.coreIngress;
    s.ingressChannel = config.ingressChannel;
    s.responseChannel = config.coreResponseChannel;
    s.credentials = config.coreCredentials.encode();
    s.clientName = "ct-forwarder-core";
    return s;
}

client::ClientSettings meClientSettings(const ForwarderConfig &config)
{
    client::ClientSettings s;
    s.ingressEndpoints = config.meIngress;
    s.ingressChannel = config.ingressChannel;
    s.responseChannel = config.meResponseChannel;
    s.credentials = config.meCredentials.encode();
    s.clientName = "ct-forwarder-me";
    return s;
}

ConfigResult parseForwarderConfig(const EnvGetter &getenv)
{
    ConfigResult r;
    ForwarderConfig &c = r.config;
    std::vector<std::string> &e = r.errors;
    c.coreIngress = required(getenv, "FWD_CORE_INGRESS", e);
    c.coreResponseChannel = required(getenv, "FWD_CORE_RESPONSE_CHANNEL", e);
    c.coreCredentials = credential(getenv, "AE_AUTH_CREDENTIAL", e);
    c.meIngress = required(getenv, "FWD_ME_INGRESS", e);
    c.meResponseChannel = required(getenv, "FWD_ME_RESPONSE_CHANNEL", e);
    c.meCredentials = credential(getenv, "ME_AUTH_CREDENTIAL", e);
    c.ingressChannel = required(getenv, "FWD_INGRESS_CHANNEL", e);
    c.aeronDir = required(getenv, "AERON_DIR", e);
    const std::string bind = required(getenv, "FWD_HTTP_BIND", e);
    if (!e.empty())
    {
        return r;
    }

    // Each session's settings through the client's own validation (endpoints, ingress base channel,
    // response channel form, 0.0.0.0 refused, credentials present).
    if (const std::string error = client::validate(coreClientSettings(c)); !error.empty())
    {
        e.push_back("core: " + error);
    }
    if (const std::string error = client::validate(meClientSettings(c)); !error.empty())
    {
        e.push_back("ME: " + error);
    }
    if (endpointOf(c.coreResponseChannel) == endpointOf(c.meResponseChannel) && 0 != endpointPort(c.coreResponseChannel))
    {
        e.push_back("FWD_CORE_RESPONSE_CHANNEL and FWD_ME_RESPONSE_CHANNEL name the same endpoint " +
            endpointOf(c.coreResponseChannel) + " (allowed only when both use port 0)");
    }
    const std::size_t colon = bind.rfind(':');
    int port = -1;
    if (std::string::npos != colon && colon > 0 && colon + 1 < bind.size())
    {
        const char *first = bind.data() + colon + 1;
        const char *last = bind.data() + bind.size();
        const auto parsed = std::from_chars(first, last, port);
        if (parsed.ec != std::errc() || parsed.ptr != last)
        {
            port = -1;
        }
    }
    if (port < 1 || port > 65535)
    {
        e.push_back("FWD_HTTP_BIND must be host:port with a port in 1..65535, got '" + bind + "'");
    }
    else
    {
        c.httpHost = bind.substr(0, colon);
        c.httpPort = static_cast<std::uint16_t>(port);
    }
    return r;
}

}}}
