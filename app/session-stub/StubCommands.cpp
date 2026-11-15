#include "session-stub/StubCommands.h"

#include <charconv>
#include <limits>
#include <sstream>

#include "cluster_tools/client/ClientSettings.h"

namespace cluster_tools { namespace app { namespace session_stub {

namespace
{

std::vector<std::string> words(const std::string &line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    std::string w;
    while (in >> w)
    {
        out.push_back(w);
    }
    return out;
}

// A non-negative count of ms, at most 1 h (the client's timeout bound keeps now + interval far from overflow).
bool parseMs(const std::string &text, std::int64_t &value)
{
    if (text.empty() || '+' == text.front() || '-' == text.front())
    {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() &&
        value <= client::MAX_TIMEOUT_NS / 1'000'000LL;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

}

CommandResult parseCommand(const std::string &line)
{
    CommandResult r;
    r.command.text = line;
    const std::vector<std::string> w = words(line);
    if (w.empty() || '#' == w[0].front())
    {
        return r; // NONE
    }
    const std::string &verb = w[0];
    const auto bad = [&](const std::string &why) {
        r.error = why;
        return r;
    };

    if ("throttle" == verb)
    {
        if (w.size() == 2 && "off" == w[1])
        {
            r.command.kind = CommandKind::THROTTLE_OFF;
            return r;
        }
        if (w.size() == 2 && "pause" == w[1])
        {
            r.command.kind = CommandKind::THROTTLE_PAUSE;
            return r;
        }
        if ((w.size() == 2 || w.size() == 3) && parseMs(w[1], r.command.valueMs) && r.command.valueMs > 0)
        {
            r.command.kind = CommandKind::THROTTLE;
            if (w.size() == 3)
            {
                std::int64_t n = 0;
                if (!parseMs(w[2], n) || n <= 0 || n > std::numeric_limits<int>::max())
                {
                    return bad("throttle: the fragment limit must be a positive int, got '" + w[2] + "'");
                }
                r.command.fragments = static_cast<int>(n);
            }
            return r;
        }
        return bad("throttle needs off, pause, or <interval_ms> [<fragments>] with interval_ms > 0");
    }
    if ("offer" == verb)
    {
        if (w.size() != 2 || w[1].size() % 2 != 0)
        {
            return bad("offer needs one even-length hex string");
        }
        for (std::size_t i = 0; i < w[1].size(); i += 2)
        {
            const int hi = hexValue(w[1][i]);
            const int lo = hexValue(w[1][i + 1]);
            if (hi < 0 || lo < 0)
            {
                return bad("offer: '" + w[1] + "' is not hex");
            }
            r.command.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        }
        r.command.kind = CommandKind::OFFER;
        return r;
    }
    if ("status" == verb && w.size() == 1)
    {
        r.command.kind = CommandKind::STATUS;
        return r;
    }
    if ("sleep" == verb)
    {
        if (w.size() != 2 || !parseMs(w[1], r.command.valueMs))
        {
            return bad("sleep needs <ms> (0..3600000)");
        }
        r.command.kind = CommandKind::SLEEP;
        return r;
    }
    if ("await" == verb)
    {
        if (w.size() != 3 || ("registered" != w[1] && "closed" != w[1]) || !parseMs(w[2], r.command.valueMs))
        {
            return bad("await needs registered|closed <timeout_ms> (0..3600000)");
        }
        r.command.kind = "registered" == w[1] ? CommandKind::AWAIT_REGISTERED : CommandKind::AWAIT_CLOSED;
        return r;
    }
    if ("reconnect" == verb)
    {
        if (w.size() != 2 || ("on" != w[1] && "off" != w[1]))
        {
            return bad("reconnect needs on or off");
        }
        r.command.kind = CommandKind::RECONNECT;
        r.command.flag = "on" == w[1];
        return r;
    }
    if ("close" == verb && w.size() == 1)
    {
        r.command.kind = CommandKind::CLOSE;
        return r;
    }
    if ("quit" == verb && w.size() == 1)
    {
        r.command.kind = CommandKind::QUIT;
        return r;
    }
    return bad("unknown command '" + verb + "'");
}

}}}
