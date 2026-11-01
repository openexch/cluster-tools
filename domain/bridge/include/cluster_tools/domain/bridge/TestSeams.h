#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_TEST_SEAMS_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_TEST_SEAMS_H

// Negative-control seams of the bridge domain and of its codec (RC2 S9b section 13, the light method; the same
// mechanism as S9a's archive seams and S12's forwarder seams). Each rule a S9b test pins can be switched off in a
// TEST build only, so the suite proves that the test fails for the stated reason when the rule is gone. The seam
// exists only when the translation unit is compiled with CT_TEST_SEAMS (ct_bridge_domain_seamed,
// ct_bridge_codec_seamed); in every production target disabled() is a constexpr false and the branch is removed.
// In a seamed build a rule is off when the environment variable CT_TEST_DISABLE_<RULE> is 1 (read once per rule)
// or when forceDisable() named it.

namespace cluster_tools { namespace domain { namespace bridge { namespace test_seams {

enum class Rule : unsigned
{
    // 7.3 per-frame table and 7.2 reports (13.1, 13.4)
    FORWARD_GREATER,        // RED shape: seq > last forwards (a hole is forwarded)
    TERMINAL_INCLUSIVE,     // RED shape: the Java inclusive terminal boundary (a 63 at seq == last is offered)
    SKIP_OFF,               // seq <= last is offered again
    EPOCH_CHECK_OFF,        // row 1 off: a foreign epoch is forwarded / skipped
    EPOCH_DIRECT_HALT,      // RED shape (pre D-S9b-1): the first foreign record halts EPOCH_MISMATCH at once
    ADOPT_OFF,              // an unbound sync never adopts the epoch of its seq 1
    HEAD_RULE_OFF,          // row 3 off: a non-config seq 1 is offered
    REPORT_VALIDATION_OFF,  // 7.2 validity off
    LOSS_RULE_OFF,          // 7.2 rule 1 off
    LAST_FOLLOWS_REPORT,    // RED shape: a report with C' > last moves last (a G1 violation)
    // 6.1 staging (13.2)
    STAGE_TRUNCATES,        // RED shape: the staged copy loses its last byte
    FLUSH_BEFORE_NON_TRADE_OFF,
    CAP_OFF,                // the 64 cap is not flushed
    IDLE_FLUSH_OFF,         // an empty poll does not flush
    PRE_SWITCH_FLUSH_OFF,   // a source command is released while trades are staged
    BP_DROPS,               // RED shape: back-pressure drops (a staged batch, or the single record advances)
    TICK_FLUSHES_ALWAYS,    // RED shape (pre FIX-8): tick flushes a healthy partial batch
    // 6.2 ring and key table (13.3)
    RING_PROOF_TRADES_ONLY,
    RING_PROOF_CURRENT_SOURCE,
    RING_UNBOUNDED,
    RING_KEEP_TAIL,
    KEYTABLE_UNBOUNDED,
    KEYTABLE_CLEAR_WITH_STAGING,
    RING_FULL_AT_ZERO,      // RED shape (pre N-4): full means 0 free slots
    // section 8 failover (13.5, 13.6)
    ROTATION_OFF,           // RED shape: halt on the first verdict
    GAP_SEEN_MAX,           // RED shape: GAP seen = the max over the holed sources
    HOLE_VERIFY_OFF,        // a memo-read hole is believed (a false GAP)
    REFUSAL_IGNORED,        // RED shape: a refused frame is skipped like Java T5
    Q2_GAP,                 // RED shape: a mixed rotation halts GAP
    S6_OFF,                 // a second memo-free SEQ_REGRESSION is not BAD (loops)
    UNREACHABLE_AS_HOLE,    // RED shape: an unreachable source counts as holed
    STRIKES_OFF,            // transient strikes never make a source unreachable
    EPOCH_REACHABLE_ONLY,   // RED shape (pre FIX-1): EPOCH_MISMATCH when every REACHABLE source is foreign
    HALT_CONFIRM_OFF,       // RED shape (pre FIX-4): no confirming report before a halt
    MALFORMED_AS_UNREACHABLE, // RED shape (pre FIX-11)
    MEMO_FAULT_STRIKES,     // RED shape (pre FIX-13): POSITION_REJECTED / RESUME_BELT count as strikes
    ROTATE_PAST_CURSOR,     // RED shape (pre FIX-16): rotate although a report shows C' > last
    BEHIND_OFF,             // no BEHIND verdict (the 08-12 zombie)
    BEHIND_RESCAN_OFF,      // RED shape: an all-behind park is re-derived every dwell
    GRACE_OFF,              // RED shape: a hole with a lagging source halts at once
    BEHIND_PARK_FIRST_CONFIGURED, // RED shape (pre FIX-5)
    DWELL_FROM_ATTACH,      // RED shape (pre FIX-6): the dwell starts at the attach (a stale backlog counts)
    // section 9 halt machine (13.7)
    HALT_FIELDS_ZERO,       // RED shape: GAP with seen == expected
    HALT_FLUSH_OFF,         // RED shape: the FeedHalt is offered before the pre-halt staging
    HALT_RESEND_OFF,        // the FeedHalt is never re-offered
    REJECT_EXIT_AT_ONCE,    // RED shape (pre FIX-10): fatal before the fallback FeedHalt is acked
    GAP_AUTO_RESUME_OFF,
    SCHEMA_PARK_FOREVER,
    HALTED_IGNORES_REPORT_RULES, // RED shape (pre FIX-3)
    RESUME_RULE_OFF,        // RED shape (pre FIX-2): a halted bridge streams after a reconnect
    // 9.6 storm guard (13.8)
    STORM_GUARD_OFF,
    UNBOUND_COUNTS_AS_STORM, // RED shape (BLOCKER-1)
    // section 10 fences (13.9)
    FENCE_OFF,              // offers continue after NEW_LEADER
    FENCE_PER_REJECT,       // RED shape (pre FIX-9): one fence (and query) per SettleRejected
    HALT_KEEP_ALWAYS,       // RED shape: a fence keeps the halt even when C' < e - 1
    REGISTRATION_GATE_OFF,  // a sync starts without the new term's registration
    STEPBACK_COUNTER_OFF,
    // the 2026-10-31 review fixes
    CONFIRM_REQUERY,        // RED shape (review BLOCKER-1): every decision re-queries and the source keeps reading
    STORM_TRIED_AT_SWITCH,  // RED shape (review FIX-2): a source is "tried" when switched to; no CORE_REFUSED retry
    STORM_TARGETS_CURRENT,  // RED shape (review NOTE-1): the storm rotation may target the current source
    STALE_PREFIX_FOREIGN,   // RED shape (review NOTE-8): a foreign frame before any own-epoch frame is FOREIGN
    REFUSAL_VERIFY_OFF,     // a refusal on a memo read is believed at once (a false SOURCE_SCHEMA)
    // RC2 S10' section 5 (F-1), 4.1 rule 8 and 3.3
    SUCCESSOR_WALK_OFF, // S10' 5.1 R1 off: a halted re-scan decides HOLED / BAD on the holed recording (F-1)
    STEP_BACK_OFF, // S10' 5.1 R2 off: no memo-free re-read after a FILL successor's f + 1 is missing
    SCHEMA_RESCAN_OFF, // S10' 5.1 R0 off: HALTED(SOURCE_SCHEMA) never re-scans
    SEEN_FROM_FIRST_RECORDING_ONLY, // RED shape: the walk keeps the first recording's seen (not the min)
    MARKER_NEVER_RESET, // RED shape: successorsTried / steppedBack survive a sync start
    RELIST_ON_EPOCH_HALT, // RED shape: HALTED(EPOCH_MISMATCH) re-scans and re-lists
    RELIST_EVERY_TICK, // RED shape: a halted bridge re-lists its source on every tick
    GENESIS_EXIT_UNCOUNTED, // S10' 4.1 rule 8 off: the genesis exit of EPOCH_MISMATCH is not counted
    SUCCESSOR_WALK_WHILE_STREAMING, // RED shape: a streaming hole walks the later recordings instead of rotating
    FEEDHALT_PROMPT,
    RESCAN_EXIT_A_OFF,
    MIRROR_FIRST_HALT_REASON, // RED shape (4R rule 6): the core halt mirror keeps the first halted report's reason // RED shape (lane C W2b finding): a re-scan's reports never take exit (a) at a quiet head // a timing seam, not a rule (tc_slowhalt, S10' 3.3): off = the FeedHalt is offered 5 s after HALTING
    // adapters/sbe BridgeCodec (13.11-13.16)
    CODEC_SWAP_TAKER_MAKER_ORDER, // RED shape: takerOrderId and makerOrderId swapped
    CODEC_ENUM_GET,         // RED shape: an E::get path (raw 2..255 throws or clamps)
    CODEC_HEAD_NOT_PERMUTED, // RED shape: the head copied in schema 4 order
    EGRESS_VERSION_FLOOR,   // RED shape: a v6 report accepted
    COUNT
};

const char *ruleName(Rule rule) noexcept;

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept;
// Test builds only: switch rules off by name (comma-separated); false if a name is unknown.
bool forceDisable(const char *commaSeparatedNames) noexcept;
// Test builds only: switch every rule back on and forget the environment reads.
void resetForTest() noexcept;
#else
constexpr bool disabled(Rule) noexcept
{
    return false;
}
#endif

}}}}

#endif
