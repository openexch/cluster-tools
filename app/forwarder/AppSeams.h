#ifndef CLUSTER_TOOLS_APP_FORWARDER_APP_SEAMS_H
#define CLUSTER_TOOLS_APP_FORWARDER_APP_SEAMS_H

// Negative-control seams of ct-forwarder's app layer (RC2 S10' section 8.3; the mechanism of the bridge's AppSeams.h and
// the S12 domain seams). Each driver-mode rule a test pins can be switched off in a TEST build only, so the suite proves
// the test fails for the stated reason when the rule is gone. The seam exists only when the translation unit is
// compiled with CT_TEST_SEAMS (ct_forwarder_pure_seamed and the test / nc / tc targets); in the production ct-forwarder
// disabled() is a constexpr false and the branch is removed. In a seamed build a rule is off when
// CT_TEST_DISABLE_<RULE>=1 (read once per rule) or when forceDisable() named it. No seam is reachable from a config key.

namespace cluster_tools { namespace app { namespace forwarder { namespace app_seams {

enum class Rule : unsigned
{
    DRIVER_MODE_DEFAULT_EXTERNAL, // a missing FWD_DRIVER_MODE becomes external (the pre-S10' shape)
    TIMEOUT_CONSTANT,             // a missing FWD_DRIVER_TIMEOUT_MS falls back to the old constant 5000
    IDENTITY_RANDOM,              // the identity is a random value per start (the S12 shape), not FWD_INSTANCE_ID
    CONDUCTOR_WATCH_OFF,          // the client-conductor heartbeat check never fires
    EMBEDDED_DIR_PER_PID,         // the embedded driver dir is per pid (a stale sibling is left behind)
    EMBEDDED_INSTANCE_LOCK,       // no instance lock: two same-id embedded starts both reclaim the dir
    STORAGE_STARVED_EXIT_OFF,     // S10' Q1: a publication starved of storage space never ends the process (alive forever)
    COUNT
};

const char *ruleName(Rule rule) noexcept;

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept;
// Test builds only: switch rules off by name (comma-separated); false when a name is unknown (unless allowUnknown).
bool forceDisable(const char *commaSeparatedNames, bool allowUnknown = false) noexcept;
void resetForTest() noexcept;
#else
constexpr bool disabled(Rule) noexcept
{
    return false;
}
#endif

}}}}

#endif
