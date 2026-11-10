#ifndef CLUSTER_TOOLS_APP_COMMON_RETIRED_SETTINGS_H
#define CLUSTER_TOOLS_APP_COMMON_RETIRED_SETTINGS_H

#include <array>
#include <string>
#include <vector>

// The environment variables retired by the rename of the engine to core, in ONE table shared by ct-bridge and
// ct-forwarder (the core's RetiredSettings carries the same names). A retired name that is still set means a
// deployment was written for the old names: the process refuses to start (exit 78, the config refusal) with one line
// per retired name naming the new one, before it reads any other setting. There is no dual read and no fallback: the
// old name is never honoured. A variable counts as set when it is present at all, an empty value included.

namespace cluster_tools { namespace app { namespace common {

struct RetiredSetting
{
    const char *retired;
    const char *replacement;
};

inline constexpr std::array<RetiredSetting, 4> RETIRED_SETTINGS = { {
    { "AE_AUTH_CREDENTIAL", "CORE_AUTH_CREDENTIAL" },
    { "BRIDGE_AE_EGRESS_ENDPOINT", "BRIDGE_CORE_EGRESS_ENDPOINT" },
    { "BRIDGE_AE_CLUSTER_ADDRESSES", "BRIDGE_CORE_CLUSTER_ADDRESSES" },
    { "BRIDGE_AE_PORT_BASE", "BRIDGE_CORE_PORT_BASE" },
} };

inline std::string retiredSettingLine(const RetiredSetting &s)
{
    return std::string("the environment variable ") + s.retired + " is retired; it is now " + s.replacement +
        " (rename the setting; the old name is never read)";
}

// One refusal line per retired name present in the environment, in table order; empty when none is.
template<typename Getenv>
std::vector<std::string> retiredSettingRefusals(const Getenv &getenv)
{
    std::vector<std::string> lines;
    for (const RetiredSetting &s : RETIRED_SETTINGS)
    {
        if (nullptr != getenv(s.retired))
        {
            lines.push_back(retiredSettingLine(s));
        }
    }
    return lines;
}

}}}

#endif
