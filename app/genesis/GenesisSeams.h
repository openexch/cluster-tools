#ifndef CLUSTER_TOOLS_APP_GENESIS_GENESIS_SEAMS_H
#define CLUSTER_TOOLS_APP_GENESIS_GENESIS_SEAMS_H

#include <cstdint>

// Negative-control seams of ct-genesis (the mechanism of the forwarder's AppSeams.h). Each rule a test pins can be
// switched off in a TEST build only (CT_TEST_SEAMS: the seamed library, the test binaries and ct_genesis_tc), so the
// suite proves the test fails for the stated reason when the rule is gone. In the production ct-genesis disabled() is
// a constexpr false and the branch is removed. In a seamed build a rule is off when CT_TEST_DISABLE_<RULE>=1. No seam
// is reachable from a setting.

namespace cluster_tools { namespace app { namespace genesis { namespace seams {

enum class Rule : unsigned
{
    EMPTINESS_CHECK, // me_genesis runs on a cluster that is not empty
    PREFIX_CHECK,    // a phase verb runs on any state (no exact-prefix rule)
    LIMITS_STEP,     // configure_risk does not send CoreLimitsSet (and does not judge the limits record)
    COUNT
};

const char *ruleName(Rule rule) noexcept;

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept;
void resetForTest() noexcept;
// ct_genesis_tc only: CT_GENESIS_TC_EXIT_AFTER_SENDS=<n> ends the process (exit 137, the SIGKILL code) right after the
// n-th command was offered: the "genesis killed in the middle" drill. 0 = never.
std::int64_t exitAfterSends() noexcept;
#else
constexpr bool disabled(Rule) noexcept
{
    return false;
}
constexpr std::int64_t exitAfterSends() noexcept
{
    return 0;
}
#endif

}}}}

#endif
