#ifndef CLUSTER_TOOLS_DOMAIN_SWITCH_DECISION_H
#define CLUSTER_TOOLS_DOMAIN_SWITCH_DECISION_H

#include <cstdint>

// SwitchDecision — the engine-side decision table of the behaviour-versioning design,
// as a pure function. This header knows nothing about Aeron, SBE, or any wire format;
// that is the point: the table must be testable with zero infrastructure, and the same
// table is what the engine applies deterministically at a single log position.
//
// The table (design doc, "Katman 2 — engine", 2026-08-12):
//
//   targetVersion == currentVersion      -> NOOP_ALREADY_AT_TARGET
//       Duplicate/retry tolerance. Decided before anything else, and never from
//       node-local truth: a resent switch command must be a no-op on every node.
//
//   targetVersion != currentVersion + 1  -> REJECT_NOT_SUCCESSOR
//       Skips and rollbacks are rejected loudly. This outcome is computable from the
//       command fields + replicated state alone, so all nodes reach it identically —
//       which is why it outranks the node-local jar check below.
//
//   successor, but jar cannot run it     -> FAIL_FAST_UNSUPPORTED
//       The only node-local input. A *reject* here would let nodes with different
//       jars decide differently; the design instead has the node exit (CRITICAL) so
//       determinism holds and the remaining quorum lives. This function only names
//       the outcome — acting on it (log + exit) is the caller's business.
//
//   successor and jar supports it        -> APPLY
//       The switch happens, at exactly this log position.
//
// The real precondition set will grow (replicated-state preconditions per version,
// the layer-1 gateway checks stay out of scope here); this seeds the shape.

namespace cluster_tools { namespace domain {

enum class SwitchDecision
{
    NOOP_ALREADY_AT_TARGET,
    APPLY,
    REJECT_NOT_SUCCESSOR,
    FAIL_FAST_UNSUPPORTED
};

struct SwitchRequest
{
    std::int32_t currentVersion;
    std::int32_t targetVersion;
    bool jarSupportsTarget;
};

SwitchDecision decideSwitch(const SwitchRequest &request) noexcept;

// Not named toString: an ADL-visible toString returning const char* would hijack
// doctest's stringification extension point (which expects doctest::String).
const char *decisionName(SwitchDecision decision) noexcept;

}}

#endif
