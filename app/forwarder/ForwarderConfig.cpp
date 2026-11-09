#include "ForwarderConfig.h"

#include <cctype>
#include <charconv>
#include <limits>
#include <random>

#include "common/DriverTimeout.h"

#include "AppSeams.h"

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

std::int64_t randomIdentity()
{
    std::random_device device;
    std::uint64_t v = 0;
    while (0 == v)
    {
        v = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }
    return static_cast<std::int64_t>(v & 0x7FFF'FFFF'FFFF'FFFFULL) | 1; // non-zero, positive (the earlier shape)
}

}

const char *driverModeName(DriverMode mode) noexcept
{
    switch (mode)
    {
        case DriverMode::EMBEDDED: return "embedded";
        case DriverMode::EXTERNAL: return "external";
        case DriverMode::UNSET: break;
    }
    return "unset";
}

std::int64_t forwarderIdentity(const ForwarderConfig &config)
{
    if (app_seams::disabled(app_seams::Rule::IDENTITY_RANDOM))
    {
        return randomIdentity(); // RED shape: a random identity per start
    }
    return config.instanceId;
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
    s.clientName = "ct-forwarder-core identity=" + std::to_string(forwarderIdentity(config));
    return s;
}

client::ClientSettings meClientSettings(const ForwarderConfig &config)
{
    client::ClientSettings s;
    s.ingressEndpoints = config.meIngress;
    s.ingressChannel = config.ingressChannel;
    s.responseChannel = config.meResponseChannel;
    s.credentials = config.meCredentials.encode();
    s.clientName = "ct-forwarder-me identity=" + std::to_string(forwarderIdentity(config));
    return s;
}

ConfigResult parseForwarderConfig(const EnvGetter &getenv, const common::DirProbe &probe)
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

    // The driver mode first, then its keys, strict in both directions (C13)
    const std::string mode = trimmed(getenv("FWD_DRIVER_MODE"));
    if ("embedded" == mode)
    {
        c.driverMode = DriverMode::EMBEDDED;
    }
    else if ("external" == mode)
    {
        c.driverMode = DriverMode::EXTERNAL;
    }
    else if (mode.empty() && app_seams::disabled(app_seams::Rule::DRIVER_MODE_DEFAULT_EXTERNAL))
    {
        c.driverMode = DriverMode::EXTERNAL; // RED shape: a missing mode becomes external (the earlier behaviour)
    }
    else if (mode.empty())
    {
        e.push_back("FWD_DRIVER_MODE is required (no default): embedded (dev and demo) or external (prod, the host aeronmd)");
    }
    else
    {
        e.push_back("FWD_DRIVER_MODE must be exactly embedded or external, got '" + mode + "'");
    }
    const bool aeronDirSet = nullptr != getenv("AERON_DIR");
    const bool driverDirSet = nullptr != getenv("FWD_DRIVER_DIR");
    if (DriverMode::EXTERNAL == c.driverMode)
    {
        c.aeronDir = trimmed(getenv("AERON_DIR"));
        if (c.aeronDir.empty())
        {
            e.push_back("AERON_DIR is required with FWD_DRIVER_MODE=external (the host aeronmd's directory)");
        }
        else if (!probe.isDirectory(c.aeronDir))
        {
            e.push_back("AERON_DIR " + c.aeronDir + " is not an existing directory (is the host aeronmd running?)");
        }
        if (driverDirSet)
        {
            e.push_back("FWD_DRIVER_DIR is set but FWD_DRIVER_MODE=external uses the host aeronmd at AERON_DIR: remove it");
        }
    }
    else if (DriverMode::EMBEDDED == c.driverMode)
    {
        c.driverDir = trimmed(getenv("FWD_DRIVER_DIR"));
        if (c.driverDir.empty())
        {
            e.push_back("FWD_DRIVER_DIR is required with FWD_DRIVER_MODE=embedded (the parent of the per-instance driver dir)");
        }
        else if ('/' != c.driverDir.front())
        {
            e.push_back("FWD_DRIVER_DIR must be an absolute path, got '" + c.driverDir + "'");
        }
        else if (!probe.isDirectory(c.driverDir))
        {
            e.push_back("FWD_DRIVER_DIR " + c.driverDir + " is not an existing directory");
        }
        else if (!probe.isWritable(c.driverDir))
        {
            e.push_back("FWD_DRIVER_DIR " + c.driverDir + " is not writable");
        }
        if (aeronDirSet)
        {
            e.push_back("AERON_DIR is set but FWD_DRIVER_MODE=embedded starts its own driver: remove it");
        }
    }
    // 8.2 rule 2: the shared strict parser on the raw value, as ct-bridge (TIMEOUT_CONSTANT RED: a missing key falls back
    // to the former constant 5000)
    {
        const char *raw = getenv("FWD_DRIVER_TIMEOUT_MS");
        if ((nullptr == raw || '\0' == *raw) && app_seams::disabled(app_seams::Rule::TIMEOUT_CONSTANT))
        {
            c.driverTimeoutMs = 5000; // RED shape
        }
        else
        {
            std::string error;
            const auto timeout = common::parseDriverTimeoutMs("FWD_DRIVER_TIMEOUT_MS", raw, error);
            if (timeout)
            {
                c.driverTimeoutMs = *timeout;
            }
            else
            {
                e.push_back(error);
            }
        }
    }
    // 8.2 rule 3: the instance id, both modes
    if (const std::string id = required(getenv, "FWD_INSTANCE_ID", e); !id.empty())
    {
        std::int64_t v = 0;
        const auto parsed = std::from_chars(id.data(), id.data() + id.size(), v);
        if (parsed.ec != std::errc() || parsed.ptr != id.data() + id.size() || '+' == id.front() || v < 1)
        {
            e.push_back("FWD_INSTANCE_ID must be an integer in 1.." + std::to_string(std::numeric_limits<std::int64_t>::max()) +
                ", got '" + id + "'");
        }
        else
        {
            c.instanceId = v;
        }
    }
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
