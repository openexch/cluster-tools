#include "genesis/GenesisSeams.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cluster_tools { namespace app { namespace genesis { namespace seams {

const char *ruleName(Rule rule) noexcept
{
    switch (rule)
    {
        case Rule::EMPTINESS_CHECK: return "EMPTINESS_CHECK";
        case Rule::PREFIX_CHECK: return "PREFIX_CHECK";
        case Rule::LIMITS_STEP: return "LIMITS_STEP";
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

void resetForTest() noexcept
{
    for (auto &slot : g_state)
    {
        slot.store(0, std::memory_order_relaxed);
    }
}

std::int64_t exitAfterSends() noexcept
{
    const char *env = std::getenv("CT_GENESIS_TC_EXIT_AFTER_SENDS");
    if (nullptr == env)
    {
        return 0;
    }
    std::int64_t n = 0;
    const auto r = std::from_chars(env, env + std::strlen(env), n);
    return (r.ec == std::errc() && n > 0) ? n : 0;
}
#endif

}}}}
