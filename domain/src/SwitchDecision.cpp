#include "cluster_tools/domain/SwitchDecision.h"

namespace cluster_tools { namespace domain {

SwitchDecision decideSwitch(const SwitchRequest &request) noexcept
{
    if (request.targetVersion == request.currentVersion)
    {
        return SwitchDecision::NOOP_ALREADY_AT_TARGET;
    }

    // 64-bit successor arithmetic: currentVersion + 1 must not overflow at INT32_MAX.
    const std::int64_t successor = static_cast<std::int64_t>(request.currentVersion) + 1;
    if (static_cast<std::int64_t>(request.targetVersion) != successor)
    {
        return SwitchDecision::REJECT_NOT_SUCCESSOR;
    }

    if (!request.jarSupportsTarget)
    {
        return SwitchDecision::FAIL_FAST_UNSUPPORTED;
    }

    return SwitchDecision::APPLY;
}

const char *decisionName(SwitchDecision decision) noexcept
{
    switch (decision)
    {
        case SwitchDecision::NOOP_ALREADY_AT_TARGET: return "NOOP_ALREADY_AT_TARGET";
        case SwitchDecision::APPLY: return "APPLY";
        case SwitchDecision::REJECT_NOT_SUCCESSOR: return "REJECT_NOT_SUCCESSOR";
        case SwitchDecision::FAIL_FAST_UNSUPPORTED: return "FAIL_FAST_UNSUPPORTED";
    }
    return "?";
}

}}
