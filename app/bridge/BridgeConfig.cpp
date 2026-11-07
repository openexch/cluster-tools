#include "BridgeConfig.h"

#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

#include "common/DriverTimeout.h"

#include "AppSeams.h"

namespace cluster_tools { namespace app { namespace bridge {

using app_seams::Rule;
using app_seams::disabled;

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

const std::vector<std::string> &configKeys()
{
    static const std::vector<std::string> keys = {
        "BRIDGE_ME_JOURNAL_ARCHIVES",
        "BRIDGE_JOURNAL_CONTROL_STREAM",
        "BRIDGE_JOURNAL_STREAM",
        "BRIDGE_JOURNAL_CHANNEL",
        "BRIDGE_REPLAY_STREAM",
        "BRIDGE_JOURNAL_MTU",
        "BRIDGE_LOCAL_HOST",
        "BRIDGE_DRIVER_MODE",
        "AERON_DIR",
        "BRIDGE_DRIVER_DIR",
        "BRIDGE_DRIVER_TIMEOUT_MS",
        "BRIDGE_AE_CLUSTER_ADDRESSES",
        "BRIDGE_AE_PORT_BASE",
        "CLUSTER_PORT_STRIDE",
        "BRIDGE_AE_EGRESS_ENDPOINT",
        "AE_AUTH_CREDENTIAL",
        "BRIDGE_INSTANCE_ID",
        "BRIDGE_HTTP_BIND",
        "BRIDGE_HALT_ON_GAP",
    };
    return keys;
}

DirProbe systemDirProbe()
{
    DirProbe p;
    p.isDirectory = [](const std::string &path) {
        struct stat st{};
        return 0 == ::stat(path.c_str(), &st) && S_ISDIR(st.st_mode);
    };
    p.isWritable = [](const std::string &path) { return 0 == ::access(path.c_str(), W_OK | X_OK); };
    return p;
}

namespace {

std::string trimmed(const char *raw)
{
    if (nullptr == raw)
    {
        return {};
    }
    std::string s(raw);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    {
        s.pop_back();
    }
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    {
        start++;
    }
    return s.substr(start);
}

template<typename T>
std::optional<T> parseInteger(const std::string &text)
{
    if (text.empty() || '+' == text.front())
    {
        return std::nullopt;
    }
    T value{};
    const char *first = text.data();
    const char *last = text.data() + text.size();
    const auto r = std::from_chars(first, last, value);
    if (r.ec != std::errc() || r.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

bool isIpv4OrLocalhost(const std::string &host)
{
    if ("localhost" == host)
    {
        return true;
    }
    in_addr addr{};
    return 1 == ::inet_pton(AF_INET, host.c_str(), &addr);
}

// Errors keyed by the variable they concern, emitted in table order (stable within one key).
class Problems
{
public:
    void add(const std::string &key, std::string message)
    {
        m_items.emplace_back(indexOf(key), std::move(message));
    }
    // An error from the archive parser: keyed by the first variable it names.
    void addMentioning(std::string message)
    {
        std::size_t bestAt = std::string::npos;
        std::size_t bestLength = 0;
        int best = static_cast<int>(configKeys().size());
        for (std::size_t i = 0; i < configKeys().size(); ++i)
        {
            const std::string &key = configKeys()[i];
            const std::size_t at = message.find(key);
            if (std::string::npos == at)
            {
                continue;
            }
            if (at < bestAt || (at == bestAt && key.size() > bestLength))
            {
                bestAt = at;
                bestLength = key.size();
                best = static_cast<int>(i);
            }
        }
        m_items.emplace_back(best, std::move(message));
    }
    std::vector<std::string> sorted() const
    {
        std::vector<std::pair<int, std::string>> items = m_items;
        std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<std::string> out;
        for (auto &item : items)
        {
            out.push_back(item.second);
        }
        return out;
    }
    bool has(const std::string &key) const
    {
        const int index = indexOf(key);
        for (const auto &item : m_items)
        {
            if (item.first == index)
            {
                return true;
            }
        }
        return false;
    }
    bool empty() const { return m_items.empty(); }

private:
    static int indexOf(const std::string &key)
    {
        const auto &keys = configKeys();
        const auto it = std::find(keys.begin(), keys.end(), key);
        return static_cast<int>(it - keys.begin());
    }
    std::vector<std::pair<int, std::string>> m_items;
};

}

std::int64_t ingressPort(std::int32_t base, std::int32_t stride, std::size_t memberIndex) noexcept
{
    const std::int64_t offset = disabled(Rule::CONFIG_PORT_FORMULA) ? 0 : CLIENT_FACING_OFFSET;
    return static_cast<std::int64_t>(base) + static_cast<std::int64_t>(memberIndex) * stride + offset;
}

std::string ingressChannel()
{
    return disabled(Rule::CONFIG_MTU_DEFAULT) ? INGRESS_CHANNEL_MTU1408 : INGRESS_CHANNEL;
}

std::string responseChannel(const std::string &egressEndpoint)
{
    return "aeron:udp?endpoint=" + egressEndpoint + "|term-length=4m";
}

client::ClientSettings coreClientSettings(const BridgeConfig &config)
{
    client::ClientSettings s;
    s.ingressEndpoints = config.ingressEndpoints;
    s.ingressChannel = ingressChannel();
    s.responseChannel = responseChannel(config.egressEndpoint);
    s.credentials = config.credentials.encode();
    s.clientName = "ct-bridge";
    s.keepAliveIntervalNs = KEEPALIVE_INTERVAL_NS;
    s.messageTimeoutNs = MESSAGE_TIMEOUT_NS;
    s.maxConnectAttempts = MAX_CONNECT_ATTEMPTS;
    return s;
}

ConfigResult parseBridgeConfig(const EnvGetter &getenv, const DirProbe &probe)
{
    ConfigResult result;
    BridgeConfig &c = result.config;
    Problems problems;
    auto present = [&](const char *key) { return nullptr != getenv(key); };
    auto value = [&](const char *key) { return trimmed(getenv(key)); };
    auto required = [&](const char *key) {
        std::string v = value(key);
        if (v.empty())
        {
            problems.add(key, std::string(key) + " is required (no default)");
        }
        return v;
    };

    // ---- the driver mode first: it decides which driver keys the archive scope requires
    const std::string mode = value("BRIDGE_DRIVER_MODE");
    if ("embedded" == mode)
    {
        c.driverMode = DriverMode::EMBEDDED;
    }
    else if ("external" == mode)
    {
        c.driverMode = DriverMode::EXTERNAL;
    }
    else if (mode.empty() && disabled(Rule::CONFIG_DRIVER_MODE_DEFAULT))
    {
        c.driverMode = DriverMode::EMBEDDED; // RED shape: a missing mode becomes embedded
    }
    else if (mode.empty())
    {
        problems.add("BRIDGE_DRIVER_MODE",
            "BRIDGE_DRIVER_MODE is required (no default): embedded (dev and demo) or external (prod, the host aeronmd)");
    }
    else
    {
        problems.add("BRIDGE_DRIVER_MODE", "BRIDGE_DRIVER_MODE must be exactly embedded or external, got '" + mode + "'");
    }

    // ---- the S9a 4.1 archive keys (BRIDGE_HALT_ON_GAP present is refused there), scoped by the mode (W0)
    const archive::SettingsScope scope = DriverMode::EMBEDDED == c.driverMode ? archive::SettingsScope::BRIDGE_EMBEDDED :
        DriverMode::EXTERNAL == c.driverMode ? archive::SettingsScope::BRIDGE_EXTERNAL : archive::SettingsScope::TOOL;
    const archive::ArchiveSettingsResult archive = archive::parseArchiveSettings(
        [&](const std::string &key) -> std::optional<std::string> {
            if ("BRIDGE_HALT_ON_GAP" == key && disabled(Rule::CONFIG_HALT_ON_GAP_IGNORED))
            {
                return std::nullopt; // RED shape: the key is never looked at
            }
            const char *raw = getenv(key.c_str());
            if (nullptr == raw)
            {
                return std::nullopt;
            }
            return trimmed(raw);
        },
        scope);
    for (const std::string &error : archive.errors)
    {
        problems.addMentioning(error);
    }
    if (archive.ok())
    {
        c.archive = *archive.settings;
    }

    // ---- driver keys per mode (C13: strict in both directions)
    if (DriverMode::EMBEDDED == c.driverMode)
    {
        if (present("AERON_DIR"))
        {
            problems.add("AERON_DIR", "AERON_DIR is set but BRIDGE_DRIVER_MODE=embedded starts its own driver: remove it");
        }
        const std::string dir = value("BRIDGE_DRIVER_DIR");
        if (!dir.empty() && '/' == dir.front() && !problems.has("BRIDGE_DRIVER_DIR"))
        {
            if (!probe.isDirectory(dir))
            {
                problems.add("BRIDGE_DRIVER_DIR", "BRIDGE_DRIVER_DIR " + dir + " is not an existing directory");
            }
            else if (!probe.isWritable(dir))
            {
                problems.add("BRIDGE_DRIVER_DIR", "BRIDGE_DRIVER_DIR " + dir + " is not writable");
            }
        }
        c.driverDir = dir;
    }
    else if (DriverMode::EXTERNAL == c.driverMode)
    {
        const std::string dir = value("AERON_DIR");
        if (dir.empty())
        {
            problems.add("AERON_DIR", "AERON_DIR is required with BRIDGE_DRIVER_MODE=external (the host aeronmd's directory)");
        }
        else if (!probe.isDirectory(dir))
        {
            problems.add("AERON_DIR", "AERON_DIR " + dir + " is not an existing directory (is the host aeronmd running?)");
        }
        c.aeronDir = dir;
    }
    {
        std::string error;
        const auto timeout = common::parseDriverTimeoutMs("BRIDGE_DRIVER_TIMEOUT_MS", getenv("BRIDGE_DRIVER_TIMEOUT_MS"), error);
        if (timeout)
        {
            c.driverTimeoutMs = *timeout;
        }
        else
        {
            problems.add("BRIDGE_DRIVER_TIMEOUT_MS", error);
        }
    }

    // ---- the core cluster
    if (const std::string list = required("BRIDGE_AE_CLUSTER_ADDRESSES"); !list.empty())
    {
        std::size_t start = 0;
        while (start <= list.size())
        {
            const std::size_t comma = list.find(',', start);
            const std::string host = trimmed(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start).c_str());
            if (host.empty() || std::string::npos != host.find(':') || std::string::npos != host.find('='))
            {
                problems.add("BRIDGE_AE_CLUSTER_ADDRESSES", "BRIDGE_AE_CLUSTER_ADDRESSES entry '" + host +
                    "' must be a host (the port comes from BRIDGE_AE_PORT_BASE and CLUSTER_PORT_STRIDE)");
            }
            c.coreHosts.push_back(host);
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
        if (c.coreHosts.empty() || c.coreHosts.size() > 16)
        {
            problems.add("BRIDGE_AE_CLUSTER_ADDRESSES", "BRIDGE_AE_CLUSTER_ADDRESSES must list 1..16 members, got " +
                std::to_string(c.coreHosts.size()));
        }
    }
    {
        std::string base = value("BRIDGE_AE_PORT_BASE");
        if (base.empty() && disabled(Rule::CONFIG_DEFAULTS))
        {
            base = "9000"; // RED shape: a default fills the port base
        }
        if (base.empty())
        {
            problems.add("BRIDGE_AE_PORT_BASE", "BRIDGE_AE_PORT_BASE is required (no default)");
        }
        else if (const auto v = parseInteger<std::int32_t>(base); !v || *v < 1 || *v > 65535)
        {
            problems.add("BRIDGE_AE_PORT_BASE", "BRIDGE_AE_PORT_BASE must be 1..65535, got '" + base + "'");
        }
        else
        {
            c.corePortBase = *v;
        }
    }
    if (const std::string stride = required("CLUSTER_PORT_STRIDE"); !stride.empty())
    {
        if (const auto v = parseInteger<std::int32_t>(stride); !v || *v < 0 || *v > 65535)
        {
            problems.add("CLUSTER_PORT_STRIDE", "CLUSTER_PORT_STRIDE must be 0..65535, got '" + stride + "'");
        }
        else
        {
            c.portStride = *v;
        }
    }
    if (!problems.has("BRIDGE_AE_CLUSTER_ADDRESSES") && !problems.has("BRIDGE_AE_PORT_BASE") &&
        !problems.has("CLUSTER_PORT_STRIDE") && 0 != c.corePortBase && !c.coreHosts.empty())
    {
        std::string endpoints;
        std::vector<std::pair<std::string, std::int64_t>> seen;
        for (std::size_t i = 0; i < c.coreHosts.size(); ++i)
        {
            const std::int64_t port = ingressPort(c.corePortBase, c.portStride, i);
            if (port < 1 || port > 65535)
            {
                problems.add("CLUSTER_PORT_STRIDE", "member " + std::to_string(i) + " ingress port " + std::to_string(port) +
                    " (BRIDGE_AE_PORT_BASE + i * CLUSTER_PORT_STRIDE + 2) is outside 1..65535");
                continue;
            }
            const std::string canonical = archive::canonicalHost(c.coreHosts[i]);
            for (const auto &[host, p] : seen)
            {
                if (host == canonical && p == port)
                {
                    problems.add("CLUSTER_PORT_STRIDE", "members share the ingress endpoint " + c.coreHosts[i] + ":" +
                        std::to_string(port) + " (CLUSTER_PORT_STRIDE " + std::to_string(c.portStride) + ")");
                }
            }
            seen.emplace_back(canonical, port);
            endpoints += (i ? "," : "") + std::to_string(i) + "=" + c.coreHosts[i] + ":" + std::to_string(port);
        }
        c.ingressEndpoints = endpoints;
    }
    if (const std::string ep = required("BRIDGE_AE_EGRESS_ENDPOINT"); !ep.empty())
    {
        const std::size_t colon = ep.rfind(':');
        const std::string host = std::string::npos == colon ? std::string() : ep.substr(0, colon);
        const auto port = std::string::npos == colon ? std::nullopt : parseInteger<std::int32_t>(ep.substr(colon + 1));
        if (host.empty() || !port || *port < 0 || *port > 65535)
        {
            problems.add("BRIDGE_AE_EGRESS_ENDPOINT", "BRIDGE_AE_EGRESS_ENDPOINT must be host:port (port 0..65535), got '" + ep + "'");
        }
        else if ("0.0.0.0" == host)
        {
            problems.add("BRIDGE_AE_EGRESS_ENDPOINT",
                "BRIDGE_AE_EGRESS_ENDPOINT host 0.0.0.0 is not routable from the cluster (name this host's address)");
        }
        else if (archive::isLoopbackHost(host))
        {
            for (const std::string &core : c.coreHosts)
            {
                if (!core.empty() && !archive::isLoopbackHost(core))
                {
                    problems.add("BRIDGE_AE_EGRESS_ENDPOINT", "BRIDGE_AE_EGRESS_ENDPOINT " + ep + " is loopback but core member " +
                        core + " is not: the core would send this session's egress to itself");
                    break;
                }
            }
        }
        c.egressEndpoint = ep;
    }
    if (const std::string spec = value("AE_AUTH_CREDENTIAL"); spec.empty())
    {
        problems.add("AE_AUTH_CREDENTIAL", "AE_AUTH_CREDENTIAL is required (keyId:BRIDGE:secretHex)");
    }
    else
    {
        const client::CredentialsResult parsed = client::parseCoreCredentials(spec);
        if (!parsed.ok())
        {
            problems.add("AE_AUTH_CREDENTIAL", "AE_AUTH_CREDENTIAL: " + parsed.error);
        }
        else if (client::SessionClass::BRIDGE != parsed.credentials.sessionClass && !disabled(Rule::CONFIG_CLASS_CHECK_OFF))
        {
            problems.add("AE_AUTH_CREDENTIAL", std::string("AE_AUTH_CREDENTIAL is a ") +
                client::nameOf(parsed.credentials.sessionClass) + " credential; ct-bridge needs BRIDGE");
        }
        else
        {
            c.credentials = parsed.credentials;
        }
    }
    if (const std::string id = required("BRIDGE_INSTANCE_ID"); !id.empty())
    {
        if (const auto v = parseInteger<std::int64_t>(id); !v || *v < 1)
        {
            problems.add("BRIDGE_INSTANCE_ID", "BRIDGE_INSTANCE_ID must be an integer in 1.." +
                std::to_string(std::numeric_limits<std::int64_t>::max()) + ", got '" + id + "'");
        }
        else
        {
            c.instanceId = *v;
        }
    }
    if (const std::string bind = required("BRIDGE_HTTP_BIND"); !bind.empty())
    {
        const std::size_t colon = bind.rfind(':');
        const std::string host = std::string::npos == colon ? std::string() : bind.substr(0, colon);
        const auto port = std::string::npos == colon ? std::nullopt : parseInteger<std::int32_t>(bind.substr(colon + 1));
        if (host.empty() || !port || *port < 0 || *port > 65535)
        {
            problems.add("BRIDGE_HTTP_BIND", "BRIDGE_HTTP_BIND must be host:port (an IPv4 address, written out; port 0..65535), got '" +
                bind + "'");
        }
        else if (!isIpv4OrLocalhost(host))
        {
            problems.add("BRIDGE_HTTP_BIND", "BRIDGE_HTTP_BIND host '" + host + "' is not an IPv4 address");
        }
        else
        {
            c.httpHost = host;
            c.httpPort = static_cast<std::uint16_t>(*port);
        }
    }
    // the client's own validation of the assembled session settings (endpoints, channels, credential present)
    if (problems.empty())
    {
        if (const std::string error = client::validate(coreClientSettings(c)); !error.empty())
        {
            problems.add("BRIDGE_AE_EGRESS_ENDPOINT", "core session settings: " + error);
        }
    }
    result.errors = problems.sorted();
    return result;
}

}}}
