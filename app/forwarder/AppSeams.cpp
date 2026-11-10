#include "AppSeams.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cluster_tools { namespace app { namespace forwarder { namespace app_seams {

const char *ruleName(Rule rule) noexcept
{
    switch (rule)
    {
        case Rule::DRIVER_MODE_DEFAULT_EXTERNAL: return "DRIVER_MODE_DEFAULT_EXTERNAL";
        case Rule::TIMEOUT_CONSTANT: return "TIMEOUT_CONSTANT";
        case Rule::IDENTITY_RANDOM: return "IDENTITY_RANDOM";
        case Rule::CONDUCTOR_WATCH_OFF: return "CONDUCTOR_WATCH_OFF";
        case Rule::EMBEDDED_DIR_PER_PID: return "EMBEDDED_DIR_PER_PID";
        case Rule::EMBEDDED_INSTANCE_LOCK: return "EMBEDDED_INSTANCE_LOCK";
        case Rule::STORAGE_STARVED_EXIT_OFF: return "STORAGE_STARVED_EXIT_OFF";
        case Rule::EMBEDDED_LIVE_CHECK: return "EMBEDDED_LIVE_CHECK";
        case Rule::DRIVER_START_STORAGE_78: return "DRIVER_START_STORAGE_78";
        case Rule::RETIRED_NAMES_IGNORED: return "RETIRED_NAMES_IGNORED";
        case Rule::COUNT: break;
    }
    return "?";
}

#ifdef CT_TEST_SEAMS
namespace {
std::array<std::atomic<int>, static_cast<std::size_t>(Rule::COUNT)> g_state{}; // 0 unread, 1 on, 2 off
}

bool disabled(Rule rule) noexcept
{
    auto &slot = g_state[static_cast<std::size_t>(rule)];
    int value = slot.load(std::memory_order_relaxed);
    if (0 == value)
    {
        char name[96];
        std::snprintf(name, sizeof(name), "CT_TEST_DISABLE_%s", ruleName(rule));
        const char *env = std::getenv(name);
        value = (nullptr != env && std::strcmp(env, "1") == 0) ? 2 : 1;
        slot.store(value, std::memory_order_relaxed);
    }
    return 2 == value;
}

bool forceDisable(const char *names, bool allowUnknown) noexcept
{
    bool ok = true;
    const std::string all = nullptr == names ? "" : names;
    std::size_t start = 0;
    while (start <= all.size())
    {
        const std::size_t comma = all.find(',', start);
        const std::string name = all.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty())
        {
            bool found = false;
            for (unsigned i = 0; i < static_cast<unsigned>(Rule::COUNT); i++)
            {
                if (name == ruleName(static_cast<Rule>(i)))
                {
                    g_state[i].store(2, std::memory_order_relaxed);
                    found = true;
                }
            }
            ok = ok && (found || allowUnknown);
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return ok;
}

void resetForTest() noexcept
{
    for (auto &slot : g_state)
    {
        slot.store(0, std::memory_order_relaxed);
    }
}
#endif

}}}}
