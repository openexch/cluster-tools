#include "cluster_tools/archive/ArchiveSettings.h"

#include "cluster_tools/archive/FramePeek.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace cluster_tools { namespace archive {

std::string ArchiveEndpoint::text() const
{
    const bool v6 = host.find(':') != std::string::npos;
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::string canonicalHost(const std::string &host)
{
    std::string h = host;
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']')
    {
        h = h.substr(1, h.size() - 2);
    }
    for (char &c : h)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (h == "localhost")
    {
        return "127.0.0.1";
    }
    // Every IPv4 form inet_aton accepts (127.1, 0x7f.1, 2130706433) to dotted quad; IPv6 to its
    // RFC 5952 text; a v4-mapped IPv6 address to its IPv4 form (review N-12).
    in_addr v4{};
    if (h.find(':') == std::string::npos && ::inet_aton(h.c_str(), &v4) != 0)
    {
        char text[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &v4, text, sizeof(text));
        return text;
    }
    in6_addr v6{};
    if (::inet_pton(AF_INET6, h.c_str(), &v6) == 1)
    {
        if (IN6_IS_ADDR_V4MAPPED(&v6))
        {
            std::memcpy(&v4, &v6.s6_addr[12], 4);
            char text[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &v4, text, sizeof(text));
            return text;
        }
        char text[INET6_ADDRSTRLEN];
        ::inet_ntop(AF_INET6, &v6, text, sizeof(text));
        return text;
    }
    return h; // a host name other than localhost: compared case-insensitively
}

bool isLoopbackHost(const std::string &host) noexcept
{
    try
    {
        const std::string h = canonicalHost(host);
        return h == "::1" || h.rfind("127.", 0) == 0; // canonical IPv4 is a dotted quad: 127/8
    }
    catch (...)
    {
        return false;
    }
}

namespace {

std::optional<std::int64_t> parseInt(const std::string &text)
{
    if (text.empty() || text.size() > 19)
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return std::nullopt; // no sign, no space, no hex: strict
        }
    }
    errno = 0;
    char *end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end != text.c_str() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<ArchiveEndpoint> parseEndpoint(const std::string &text, std::string &why)
{
    std::string host;
    std::string port;
    if (!text.empty() && text.front() == '[')
    {
        const auto close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':')
        {
            why = "IPv6 endpoint must be [addr]:port";
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        port = text.substr(close + 2);
    }
    else
    {
        const auto colon = text.rfind(':');
        if (colon == std::string::npos || text.find(':') != colon)
        {
            why = "must be host:port";
            return std::nullopt;
        }
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
    }
    if (host.empty())
    {
        why = "empty host";
        return std::nullopt;
    }
    const auto value = parseInt(port);
    if (!value || *value < 1 || *value > 65535)
    {
        why = "port must be 1..65535";
        return std::nullopt;
    }
    return ArchiveEndpoint{ host, static_cast<std::uint16_t>(*value) };
}

}

ArchiveSettingsResult parseArchiveSettings(const SettingLookup &lookup, SettingsScope scope)
{
    ArchiveSettingsResult result;
    ArchiveSettings s;
    auto &errors = result.errors;

    auto required = [&](const std::string &key) -> std::optional<std::string> {
        auto value = lookup(key);
        if (!value)
        {
            errors.push_back(key + " is required");
            return std::nullopt;
        }
        if (value->empty())
        {
            errors.push_back(key + " is empty");
            return std::nullopt;
        }
        return value;
    };
    auto positiveInt32 = [&](const std::string &key) -> std::int32_t {
        auto text = required(key);
        if (!text)
        {
            return 0;
        }
        const auto value = parseInt(*text);
        if (!value || *value < 1 || *value > std::numeric_limits<std::int32_t>::max())
        {
            errors.push_back(key + " must be an int32 > 0, got '" + *text + "'");
            return 0;
        }
        return static_cast<std::int32_t>(*value);
    };

    if (lookup("BRIDGE_HALT_ON_GAP"))
    {
        errors.push_back("BRIDGE_HALT_ON_GAP is not a setting any more (the core enforces density; cpp: F5): remove it");
    }

    if (auto list = required("BRIDGE_ME_JOURNAL_ARCHIVES"))
    {
        std::size_t start = 0;
        bool bad = false;
        while (start <= list->size())
        {
            const auto comma = list->find(',', start);
            const std::string item = list->substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            std::string why;
            const auto endpoint = parseEndpoint(item, why);
            if (!endpoint)
            {
                errors.push_back("BRIDGE_ME_JOURNAL_ARCHIVES entry '" + item + "': " + why);
                bad = true;
            }
            else
            {
                for (const auto &seen : s.archives)
                {
                    if (seen.port == endpoint->port && canonicalHost(seen.host) == canonicalHost(endpoint->host))
                    {
                        errors.push_back("BRIDGE_ME_JOURNAL_ARCHIVES lists " + endpoint->text() + " twice");
                        bad = true;
                    }
                }
                s.archives.push_back(*endpoint);
            }
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
        if (!bad && (s.archives.empty() || s.archives.size() > 16))
        {
            errors.push_back("BRIDGE_ME_JOURNAL_ARCHIVES must list 1..16 archives, got " + std::to_string(s.archives.size()));
        }
    }
    s.controlStreamId = positiveInt32("BRIDGE_JOURNAL_CONTROL_STREAM");
    s.journalStreamId = positiveInt32("BRIDGE_JOURNAL_STREAM");
    if (auto channel = required("BRIDGE_JOURNAL_CHANNEL"))
    {
        if (channel->rfind("aeron:", 0) != 0)
        {
            errors.push_back("BRIDGE_JOURNAL_CHANNEL must start with aeron:, got '" + *channel + "'");
        }
        s.journalChannel = *channel;
    }
    s.replayStreamId = positiveInt32("BRIDGE_REPLAY_STREAM");
    if (s.replayStreamId != 0 && s.replayStreamId == s.journalStreamId)
    {
        errors.push_back("BRIDGE_REPLAY_STREAM must differ from BRIDGE_JOURNAL_STREAM");
    }
    if (auto host = required("BRIDGE_LOCAL_HOST"))
    {
        s.localHost = *host;
        if (isLoopbackHost(*host))
        {
            for (const auto &archive : s.archives)
            {
                if (!isLoopbackHost(archive.host))
                {
                    errors.push_back("BRIDGE_LOCAL_HOST " + *host + " is loopback but archive " + archive.text() +
                        " is not: the archive would send control responses and the replay to itself");
                    break;
                }
            }
        }
    }
    s.journalMtu = positiveInt32("BRIDGE_JOURNAL_MTU");
    if (s.journalMtu != 0 &&
        (s.journalMtu % 32 != 0 || s.journalMtu < static_cast<std::int32_t>(MAX_FRAME_LENGTH + 32) || s.journalMtu > 65504))
    {
        errors.push_back("BRIDGE_JOURNAL_MTU must be a multiple of 32 in [" + std::to_string(MAX_FRAME_LENGTH + 32) +
            ", 65504], got " + std::to_string(s.journalMtu));
    }
    if (SettingsScope::BRIDGE == scope)
    {
        if (auto dir = required("BRIDGE_DRIVER_DIR"))
        {
            if (dir->front() != '/')
            {
                errors.push_back("BRIDGE_DRIVER_DIR must be an absolute path, got '" + *dir + "'");
            }
            s.driverDir = *dir;
        }
    }
    if (errors.empty())
    {
        result.settings = std::move(s);
    }
    return result;
}

}}
