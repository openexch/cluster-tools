#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <climits>
#include <cstdint>
#include <vector>

#include "cluster_tools/domain/SwitchDecision.h"

using cluster_tools::domain::SwitchDecision;
using cluster_tools::domain::SwitchRequest;
using cluster_tools::domain::decideSwitch;
using cluster_tools::domain::decisionName;

TEST_CASE("same target is a no-op regardless of jar capability")
{
    // Duplicate/retry tolerance: a resent SwitchBehavior{target == current} must be a
    // no-op on every node, and must not consult node-local truth to say so.
    CHECK(decideSwitch({ 3, 3, true }) == SwitchDecision::NOOP_ALREADY_AT_TARGET);
    CHECK(decideSwitch({ 3, 3, false }) == SwitchDecision::NOOP_ALREADY_AT_TARGET);
    CHECK(decideSwitch({ 0, 0, true }) == SwitchDecision::NOOP_ALREADY_AT_TARGET);
}

TEST_CASE("successor with a capable jar applies")
{
    CHECK(decideSwitch({ 3, 4, true }) == SwitchDecision::APPLY);
    CHECK(decideSwitch({ 0, 1, true }) == SwitchDecision::APPLY);
}

TEST_CASE("successor without a capable jar fails fast, not reject")
{
    // The one node-local branch: rejecting would let differently-jarred nodes diverge;
    // the design has the node exit instead. The domain only names the outcome.
    CHECK(decideSwitch({ 3, 4, false }) == SwitchDecision::FAIL_FAST_UNSUPPORTED);
}

TEST_CASE("forward skips are rejected")
{
    CHECK(decideSwitch({ 3, 5, true }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
    CHECK(decideSwitch({ 3, 42, true }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
}

TEST_CASE("rollbacks are rejected")
{
    CHECK(decideSwitch({ 3, 2, true }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
    CHECK(decideSwitch({ 3, 0, true }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
}

TEST_CASE("non-successor rejection outranks jar incapability")
{
    // A skip/rollback is rejectable from command fields + replicated state alone — every
    // node computes it identically, so the deterministic reject wins over the node-local
    // fail-fast. (Precedence case from the design's Katman 2 table.)
    CHECK(decideSwitch({ 3, 5, false }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
    CHECK(decideSwitch({ 3, 2, false }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
}

TEST_CASE("successor comparison does not overflow at INT32_MAX")
{
    // current = INT32_MAX has no representable successor: nothing can be APPLY/FAIL_FAST.
    CHECK(decideSwitch({ INT32_MAX, INT32_MAX, false }) == SwitchDecision::NOOP_ALREADY_AT_TARGET);
    CHECK(decideSwitch({ INT32_MAX, INT32_MIN, true }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
    CHECK(decideSwitch({ INT32_MAX, 0, true }) == SwitchDecision::REJECT_NOT_SUCCESSOR);
}

TEST_CASE("the table partitions every input into exactly one outcome")
{
    const std::vector<std::int32_t> versions{ -2, -1, 0, 1, 2, 3, 41, 42, INT32_MAX - 1, INT32_MAX };

    for (const std::int32_t current : versions)
    {
        for (const std::int32_t target : versions)
        {
            for (const bool jar : { false, true })
            {
                const SwitchDecision decision = decideSwitch({ current, target, jar });
                const bool isSame = target == current;
                const bool isSuccessor =
                    static_cast<std::int64_t>(target) == static_cast<std::int64_t>(current) + 1;

                CAPTURE(current);
                CAPTURE(target);
                CAPTURE(jar);
                CAPTURE(decisionName(decision));

                if (isSame)
                {
                    CHECK(decision == SwitchDecision::NOOP_ALREADY_AT_TARGET);
                }
                else if (!isSuccessor)
                {
                    CHECK(decision == SwitchDecision::REJECT_NOT_SUCCESSOR);
                }
                else if (jar)
                {
                    CHECK(decision == SwitchDecision::APPLY);
                }
                else
                {
                    CHECK(decision == SwitchDecision::FAIL_FAST_UNSUPPORTED);
                }
            }
        }
    }
}

TEST_CASE("decisionName names every outcome")
{
    CHECK(std::string(decisionName(SwitchDecision::NOOP_ALREADY_AT_TARGET)) == "NOOP_ALREADY_AT_TARGET");
    CHECK(std::string(decisionName(SwitchDecision::APPLY)) == "APPLY");
    CHECK(std::string(decisionName(SwitchDecision::REJECT_NOT_SUCCESSOR)) == "REJECT_NOT_SUCCESSOR");
    CHECK(std::string(decisionName(SwitchDecision::FAIL_FAST_UNSUPPORTED)) == "FAIL_FAST_UNSUPPORTED");
}
