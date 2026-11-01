#ifndef CLUSTER_TOOLS_APP_BRIDGE_APP_SEAMS_H
#define CLUSTER_TOOLS_APP_BRIDGE_APP_SEAMS_H

// Negative-control seams of ct-bridge's app layer (RC2 S9c section 12, the light method; the same mechanism as the
// S9b domain seams and the S12 forwarder seams). Each rule a S9c test pins can be switched off in a TEST build only,
// so the suite proves that the test fails for the stated reason when the rule is gone. The seam exists only when the
// translation unit is compiled with CT_TEST_SEAMS (ct_bridge_pure_seamed and the app test / nc / tc targets); in the
// production ct-bridge disabled() is a constexpr false and the branch is removed. In a seamed build a rule is off
// when CT_TEST_DISABLE_<RULE>=1 (read once per rule) or when forceDisable() named it. REGISTRATION_GATE_OFF carries
// the domain rule's name on purpose: one variable switches the gate off in both layers.

namespace cluster_tools { namespace app { namespace bridge { namespace app_seams {

enum class Rule : unsigned
{
    // 12.1 config, render, ack map
    CONFIG_DEFAULTS,            // a default fills BRIDGE_AE_PORT_BASE
    CONFIG_HALT_ON_GAP_IGNORED, // BRIDGE_HALT_ON_GAP is not looked at
    CONFIG_CLASS_CHECK_OFF,     // a non-BRIDGE credential passes
    CONFIG_DRIVER_MODE_DEFAULT, // a missing BRIDGE_DRIVER_MODE becomes embedded
    CONFIG_PORT_FORMULA,        // the ingress port formula without the client-facing offset (+2)
    CONFIG_MTU_DEFAULT,         // the ingress channel without mtu=8192 (the 2026-09-02 B shape)
    HEALTH_HALTED_FROM_STATE,   // /health halted from the state (DOWN with a kept halt reads false)
    ACKMAP_UNBOUNDED,           // the ack map grows without the 1M bound
    // 12.2 the agent
    REGISTRATION_GATE_OFF,      // onSessionUp at CONNECTED, before the registration ack
    FENCE_WAITS_REPORT_OFF,     // a NewLeaderEvent re-ups the session (onSessionUp) instead of fencing
    FENCE_DEFERRED,             // the NewLeader fence is applied after poll() returned (a flag)
    UP_FLAG_NEVER_RESET,        // upReportedFor is never reset (a re-registration in the same term is not re-upped)
    C4_RESTORES_PREVIOUS,       // C4 -> CONNECTED restores C3 without the new term's ack
    LINK_MACHINE_IN_CALLBACK,   // onStateChange runs the link machine (domain calls inside the invoker)
    TERM_FILTER_OFF,            // rule S off: frames of another term reach the domain
    UNREGISTERED_IGNORED,       // OrderReject UNREGISTERED_SESSION is only counted
    EXIT_CODES_ALL_70,          // every stop is 70 (the S9a F4 reading)
    MALFORMED_IS_CONFIG_ERROR,  // a malformed 17 / 110 / 114 stops the process 78
    RECONNECT_WITHOUT_SESSION_DOWN, // a session loss reconnects without onSessionDown
    MAX_POSITION_IS_RETRY,      // MAX_POSITION_EXCEEDED maps to RETRY
    LATENCY_RECORDS_GARBAGE,    // a future / negative trade timestamp is recorded
    READY_IGNORES_REPORT_AGE,   // /ready has no report-age bit
    READY_COUNTS_FORWARDS_ONLY, // backlog progress = forwards only (a skip walk reads not ready)
    TAP_SWALLOWS,               // the observer tap counts a fault but does not forward it
    RESEND_NEW_CORRELATION,     // the app re-registers (new correlation id) when an ack is late
    DUPLICATE_IDENTITY_EXITS,   // RC2 S10': a REJECTED DUPLICATE_IDENTITY (44) exits 78 at once (no bounded retry)
    // main (W4)
    WATCHDOG_OFF,               // the duty watchdog never fires
    INVOKER_NO_BEAT,            // the archive invoker does not store the heartbeat
    CONDUCTOR_WATCH_OFF,        // the client-conductor heartbeat check never fires
    EMBEDDED_DIR_PER_PID,       // the embedded driver dir is per pid (a stale sibling is left behind)
    EMBEDDED_INSTANCE_LOCK,     // no instance lock: two same-id embedded starts both reclaim the dir (Q3)
    CONDUCTOR_REVALIDATE_OFF,   // the heartbeat record is not re-validated: a reclaimed, re-used slot reads healthy
    STOP_DETACH_WHEN_LATE,      // a late stop still runs stopReplay: a blocked call + stopReplay pass the 7 s join bound
    // test builds (never a test RED): ct_bridge_negctl
    NEGCTL_DROP_RECORDS_2_3,    // record templates 2 and 3 are dropped before the domain (the A02-T2 control)
    COUNT
};

const char *ruleName(Rule rule) noexcept;

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept;
// Test builds only: switch rules off by name (comma-separated); names the app layer does not know are ignored and
// reported through `unknown` (the domain's forceDisable takes the rest).
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
