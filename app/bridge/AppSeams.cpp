#include "AppSeams.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cluster_tools { namespace app { namespace bridge { namespace app_seams {

const char *ruleName(Rule rule) noexcept
{
    switch (rule)
    {
        case Rule::CONFIG_DEFAULTS: return "CONFIG_DEFAULTS";
        case Rule::CONFIG_HALT_ON_GAP_IGNORED: return "CONFIG_HALT_ON_GAP_IGNORED";
        case Rule::CONFIG_CLASS_CHECK_OFF: return "CONFIG_CLASS_CHECK_OFF";
        case Rule::CONFIG_DRIVER_MODE_DEFAULT: return "CONFIG_DRIVER_MODE_DEFAULT";
        case Rule::CONFIG_PORT_FORMULA: return "CONFIG_PORT_FORMULA";
        case Rule::CONFIG_MTU_DEFAULT: return "CONFIG_MTU_DEFAULT";
        case Rule::HEALTH_HALTED_FROM_STATE: return "HEALTH_HALTED_FROM_STATE";
        case Rule::ACKMAP_UNBOUNDED: return "ACKMAP_UNBOUNDED";
        case Rule::REGISTRATION_GATE_OFF: return "REGISTRATION_GATE_OFF";
        case Rule::FENCE_WAITS_REPORT_OFF: return "FENCE_WAITS_REPORT_OFF";
        case Rule::FENCE_DEFERRED: return "FENCE_DEFERRED";
        case Rule::UP_FLAG_NEVER_RESET: return "UP_FLAG_NEVER_RESET";
        case Rule::C4_RESTORES_PREVIOUS: return "C4_RESTORES_PREVIOUS";
        case Rule::LINK_MACHINE_IN_CALLBACK: return "LINK_MACHINE_IN_CALLBACK";
        case Rule::TERM_FILTER_OFF: return "TERM_FILTER_OFF";
        case Rule::UNREGISTERED_IGNORED: return "UNREGISTERED_IGNORED";
        case Rule::EXIT_CODES_ALL_70: return "EXIT_CODES_ALL_70";
        case Rule::MALFORMED_IS_CONFIG_ERROR: return "MALFORMED_IS_CONFIG_ERROR";
        case Rule::RECONNECT_WITHOUT_SESSION_DOWN: return "RECONNECT_WITHOUT_SESSION_DOWN";
        case Rule::MAX_POSITION_IS_RETRY: return "MAX_POSITION_IS_RETRY";
        case Rule::LATENCY_RECORDS_GARBAGE: return "LATENCY_RECORDS_GARBAGE";
        case Rule::READY_IGNORES_REPORT_AGE: return "READY_IGNORES_REPORT_AGE";
        case Rule::READY_COUNTS_FORWARDS_ONLY: return "READY_COUNTS_FORWARDS_ONLY";
        case Rule::TAP_SWALLOWS: return "TAP_SWALLOWS";
        case Rule::RESEND_NEW_CORRELATION: return "RESEND_NEW_CORRELATION";
        case Rule::WATCHDOG_OFF: return "WATCHDOG_OFF";
        case Rule::INVOKER_NO_BEAT: return "INVOKER_NO_BEAT";
        case Rule::CONDUCTOR_WATCH_OFF: return "CONDUCTOR_WATCH_OFF";
        case Rule::EMBEDDED_DIR_PER_PID: return "EMBEDDED_DIR_PER_PID";
        case Rule::EMBEDDED_INSTANCE_LOCK: return "EMBEDDED_INSTANCE_LOCK";
        case Rule::CONDUCTOR_REVALIDATE_OFF: return "CONDUCTOR_REVALIDATE_OFF";
        case Rule::STOP_DETACH_WHEN_LATE: return "STOP_DETACH_WHEN_LATE";
        case Rule::NEGCTL_DROP_RECORDS_2_3: return "NEGCTL_DROP_RECORDS_2_3";
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
