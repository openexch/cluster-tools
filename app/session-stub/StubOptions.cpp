#include "session-stub/StubOptions.h"

#include <charconv>
#include <limits>
#include <map>
#include <set>

#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "cluster_tools/runtime/DriverTimeout.h"

namespace cluster_tools { namespace app { namespace session_stub {

namespace
{

namespace ch = cluster_tools::client::core_channel;

struct ChannelName
{
    const char *name;
    std::uint32_t bit;
};

constexpr ChannelName CHANNEL_NAMES[] = {
    { "acks", ch::ACKS },
    { "balances", ch::BALANCES },
    { "settlements", ch::SETTLEMENTS },
    { "snapshots", ch::SNAPSHOTS },
    { "orders", ch::ORDERS },
    { "dispatch", ch::DISPATCH },
    { "control", ch::CONTROL },
};
constexpr std::uint32_t ALL_CHANNELS = (1u << 7) - 1;

const std::set<std::string> KNOWN = { "--driver-mode", "--instance-id", "--ingress-endpoints", "--ingress-channel",
    "--response-channel", "--channels", "--driver-dir", "--aeron-dir", "--credential-env", "--driver-timeout-ms",
    "--poll-interval-ms", "--poll-fragments", "--status-interval-ms", "--reconnect", "--reconnect-delay-ms", "--idle" };

bool parseInt64(const std::string &text, std::int64_t &value)
{
    if (text.empty() || '+' == text.front() || '-' == text.front())
    {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

// A non-negative integer setting; `positive` refuses 0 too.
void intSetting(const std::map<std::string, std::string> &given, const std::string &key, bool positive,
    std::int64_t &out, std::vector<std::string> &errors)
{
    const auto it = given.find(key);
    if (it == given.end())
    {
        return;
    }
    std::int64_t value = 0;
    if (!parseInt64(it->second, value) || (positive && 0 == value))
    {
        errors.push_back(key + " must be a " + (positive ? "positive" : "non-negative") + " integer, got '" +
            it->second + "'");
        return;
    }
    out = value;
}

}

bool parseChannelMask(const std::string &text, std::uint32_t &mask, std::string &error)
{
    if (text.empty())
    {
        error = "--channels is empty";
        return false;
    }
    if (text.front() >= '0' && text.front() <= '9')
    {
        std::uint32_t value = 0;
        const bool hex = text.size() > 2 && '0' == text[0] && ('x' == text[1] || 'X' == text[1]);
        const char *first = text.data() + (hex ? 2 : 0);
        const auto parsed = std::from_chars(first, text.data() + text.size(), value, hex ? 16 : 10);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
        {
            error = "--channels '" + text + "' is not a number";
            return false;
        }
        if (0 != (value & ~ALL_CHANNELS))
        {
            error = "--channels " + text + " names unknown channel bits (the core has 7 channels, mask < 128)";
            return false;
        }
        mask = value;
        return true;
    }
    std::uint32_t value = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t comma = text.find(',', start);
        const std::string name = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        bool found = false;
        for (const ChannelName &c : CHANNEL_NAMES)
        {
            if (name == c.name)
            {
                value |= c.bit;
                found = true;
            }
        }
        if (!found)
        {
            error = "--channels names an unknown channel '" + name +
                "' (acks, balances, settlements, snapshots, orders, dispatch, control)";
            return false;
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    mask = value;
    return true;
}

std::string channelMaskNames(std::uint32_t mask)
{
    std::string out;
    for (const ChannelName &c : CHANNEL_NAMES)
    {
        if (0 != (mask & c.bit))
        {
            out += out.empty() ? "" : ",";
            out += c.name;
        }
    }
    return out.empty() ? "none" : out;
}

std::string missingChannelParameters(const std::string &key, const std::string &channel,
    const std::vector<std::string> &required)
{
    const std::string prefix = "aeron:udp?";
    if (channel.rfind(prefix, 0) != 0)
    {
        return key + " must be an aeron:udp? channel URI, got '" + channel + "'";
    }
    std::set<std::string> named;
    std::size_t start = prefix.size();
    while (start <= channel.size())
    {
        const std::size_t bar = channel.find('|', start);
        const std::string param = channel.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        const std::size_t eq = param.find('=');
        if (eq != std::string::npos && eq + 1 < param.size())
        {
            named.insert(param.substr(0, eq));
        }
        if (bar == std::string::npos)
        {
            break;
        }
        start = bar + 1;
    }
    std::string missing;
    for (const std::string &r : required)
    {
        if (named.count(r) == 0)
        {
            missing += missing.empty() ? r : ", " + r;
        }
    }
    return missing.empty() ? std::string() : key + " must name " + missing + " explicitly, got '" + channel + "'";
}

OptionsResult parseStubOptions(const std::vector<std::string> &args)
{
    OptionsResult result;
    std::vector<std::string> &errors = result.errors;
    StubOptions &o = result.options;

    std::map<std::string, std::string> given;
    for (std::size_t i = 0; i < args.size(); i++)
    {
        const std::string &key = args[i];
        if (KNOWN.count(key) == 0)
        {
            errors.push_back("unknown option '" + key + "'");
            continue;
        }
        if (i + 1 >= args.size())
        {
            errors.push_back(key + " needs a value");
            break;
        }
        if (!given.emplace(key, args[i + 1]).second)
        {
            errors.push_back(key + " is given twice");
        }
        i++;
    }

    const auto require = [&](const std::string &key) -> const std::string * {
        const auto it = given.find(key);
        if (it == given.end() || it->second.empty())
        {
            errors.push_back(key + " is required");
            return nullptr;
        }
        return &it->second;
    };

    bool modeKnown = false;
    if (const std::string *mode = require("--driver-mode"))
    {
        if ("embedded" == *mode)
        {
            o.driverMode = DriverMode::EMBEDDED;
            modeKnown = true;
        }
        else if ("external" == *mode)
        {
            o.driverMode = DriverMode::EXTERNAL;
            modeKnown = true;
        }
        else
        {
            errors.push_back("--driver-mode must be exactly embedded or external, got '" + *mode + "'");
        }
    }
    if (modeKnown)
    {
        const bool embedded = DriverMode::EMBEDDED == o.driverMode;
        const std::string mine = embedded ? "--driver-dir" : "--aeron-dir";
        const std::string other = embedded ? "--aeron-dir" : "--driver-dir";
        if (const std::string *dir = require(mine))
        {
            (embedded ? o.driverDir : o.aeronDir) = *dir;
        }
        if (given.count(other) != 0)
        {
            errors.push_back(other + " is not a setting of driver mode " + (embedded ? "embedded" : "external"));
        }
    }

    if (const std::string *id = require("--instance-id"))
    {
        std::int64_t value = 0;
        if (!parseInt64(*id, value) || value <= 0)
        {
            errors.push_back("--instance-id must be a positive int64, got '" + *id + "'");
        }
        o.instanceId = value;
    }
    if (const std::string *endpoints = require("--ingress-endpoints"))
    {
        o.ingressEndpoints = *endpoints;
    }
    if (const std::string *channel = require("--ingress-channel"))
    {
        o.ingressChannel = *channel;
        if (std::string e = missingChannelParameters("--ingress-channel", *channel, { "term-length", "mtu" }); !e.empty())
        {
            errors.push_back(e);
        }
    }
    if (const std::string *channel = require("--response-channel"))
    {
        o.responseChannel = *channel;
        if (std::string e = missingChannelParameters("--response-channel", *channel, { "endpoint", "term-length", "mtu" });
            !e.empty())
        {
            errors.push_back(e);
        }
    }
    if (const std::string *channels = require("--channels"))
    {
        std::string error;
        if (!parseChannelMask(*channels, o.channels, error))
        {
            errors.push_back(error);
        }
    }

    if (const auto it = given.find("--credential-env"); it != given.end())
    {
        if (it->second.empty())
        {
            errors.push_back("--credential-env is empty");
        }
        o.credentialEnv = it->second;
    }
    if (const auto it = given.find("--driver-timeout-ms"); it != given.end())
    {
        std::string error;
        if (const auto value = runtime::parseDriverTimeoutMs("--driver-timeout-ms", it->second.c_str(), error))
        {
            o.driverTimeoutMs = *value;
        }
        else
        {
            errors.push_back(error);
        }
    }
    intSetting(given, "--poll-interval-ms", false, o.pollIntervalMs, errors);
    std::int64_t fragments = o.pollFragments;
    intSetting(given, "--poll-fragments", true, fragments, errors);
    if (fragments > std::numeric_limits<int>::max())
    {
        errors.push_back("--poll-fragments must fit an int fragment limit, got " + std::to_string(fragments));
    }
    o.pollFragments = static_cast<int>(fragments);
    intSetting(given, "--status-interval-ms", false, o.statusIntervalMs, errors);
    intSetting(given, "--reconnect-delay-ms", false, o.reconnectDelayMs, errors);
    if (const auto it = given.find("--reconnect"); it != given.end())
    {
        if ("on" == it->second || "off" == it->second)
        {
            o.reconnect = "on" == it->second;
        }
        else
        {
            errors.push_back("--reconnect must be on or off, got '" + it->second + "'");
        }
    }
    if (const auto it = given.find("--idle"); it != given.end())
    {
        if ("backoff" == it->second || "busy" == it->second)
        {
            o.busySpin = "busy" == it->second;
        }
        else
        {
            errors.push_back("--idle must be backoff or busy, got '" + it->second + "'");
        }
    }
    // Every interval is turned into ns on a steady clock; the client's 1 h timeout bound keeps that far from overflow.
    for (const auto &[key, value] : { std::pair<const char *, std::int64_t>{ "--poll-interval-ms", o.pollIntervalMs },
             { "--status-interval-ms", o.statusIntervalMs }, { "--reconnect-delay-ms", o.reconnectDelayMs } })
    {
        if (value > client::MAX_TIMEOUT_NS / 1'000'000LL)
        {
            errors.push_back(std::string(key) + " must be at most 3600000 (1 h)");
        }
    }
    return result;
}

}}}
