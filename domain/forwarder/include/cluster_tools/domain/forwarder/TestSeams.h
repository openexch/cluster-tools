#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TEST_SEAMS_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TEST_SEAMS_H

// Negative-control seams of the forwarder (RC2 S12, light method; the same mechanism as S9a's
// archive seams). Each rule a S12 test pins can be switched off in a TEST build only, so the suite
// proves that the test fails for the stated reason when the rule is gone. The seam exists only when
// the translation unit is compiled with CT_TEST_SEAMS (the *_seamed test libraries and the
// ct_forwarder_nc_* / ct_forwarder_tc_* binaries); in every production target disabled() is a
// constexpr false and the branch is removed. In a seamed build a rule is off when the environment
// variable CT_TEST_DISABLE_<RULE> is 1 (read once per rule), or when forceDisable() named it (the
// negative-control binaries of contract 11.4 preset their rules that way).

namespace cluster_tools { namespace domain { namespace forwarder { namespace test_seams {

enum class Rule : unsigned
{
    // 5.4 classification, one seam per rule (the rule is treated as holding when off)
    RULE_V12,
    RULE_R1,
    RULE_R2,
    RULE_R3,
    RULE_R4,
    RULE_R5,
    RULE_B1,
    RULE_B2,
    RULE_B3,
    RULE_B4,
    RULE_B5,
    RULE_B6,
    RULE_V1,
    RULE_X1,
    RULE_X2,
    RULE_A1,
    RULE_ORDER_B_BEFORE_R,  // RED shape (FIX-10): the B rules evaluated before the R group
    // 5.5 carrier
    CARRIER,                // class C sent verbatim (the ME would decode-drop it)
    CARRIER_TIF,            // RED shape: the carrier keeps the dispatch's tif (no forced cause 4)
    // 5.3 / 7 ring drain
    RING_GROUP_CAP_48,      // RED shape: a release run is not cut at 48
    RING_RX_ISOLATION,      // RED shape: an RX entry joins its neighbours' frame
    RING_GROUP_RELEASES,    // RED shape: every release travels alone
    // 4.3 / section 6 resync machine
    RESYNC_REPLY_LAST,      // RED shape: the reply block taken from the FRONT of hold
    RESYNC_FINAL_DROP,      // RED shape (pre FIX-4): remaining held entries pushed at the final page
    RESYNC_SHAPE_S1,
    RESYNC_SHAPE_S2,
    RESYNC_SHAPE_S3,
    RESYNC_T9_RESTART,      // RED shape: the 108 timer is not restarted by core frames
    RESYNC_T9,              // the 108 timeout never fires
    RESYNC_D0_CLEARS_HOLD,
    RESYNC_ZERO_CLEARS_RING,
    RESYNC_T5_GAP,          // a live gap is pushed (no resync)
    RESYNC_GAP_HELD,        // RED shape (NOTE-11): the gap entry is dropped instead of held
    RESYNC_DUPLICATE_DROP,  // seq <= cursor pushed again
    RESYNC_HEAD_PROBE,      // no periodic head probe in LIVE
    RESYNC_T7,              // head > cursor ignored
    RESYNC_PROBE_COUNT,     // a probe 108 with count != 0 accepted (F15 off)
    RESYNC_D3_RESUME,       // D3 never resumes
    RESYNC_D3_ZERO_AT_ONCE, // RED shape (FIX-5): RESYNC(0) in D3 waits for the drain
    RESYNC_CURSOR_MAX_HEAD, // RED shape: the final page leaves the cursor at the last reply seq
    RESYNC_ALL_TRIGGERS,    // T1-T9 all disabled (ct_forwarder_nc_noresync)
    // 4.2 epoch binder
    EPOCH_ZERO_IGNORED,     // meEpoch 0 in M2 binds 0
    EPOCH_ZERO_FATAL,       // meEpoch 0 in M3 accepted
    EPOCH_MISMATCH_FATAL,
    EPOCH_WAIT,             // no reconnect after 5 s unverified
    EPOCH_PER_SESSION,      // a new ME session reuses the old verification
    // W16 admission watcher
    ADMISSION_ORIGIN_BASELINE, // RED shape (NOTE-4): ORIGIN triggers like a BROADCAST
    ADMISSION_GLOBAL_ONLY,     // per-market AdmissionState triggers
    ADMISSION_REASON_BITS,     // RED shape: only gateState compared
    // app agent
    AGENT_STALE_TERM_FILTER,   // FIX-1 rule S
    AGENT_EPOCH_BEFORE_REGISTER, // RED shape: RegisterSession sent before the ME epoch is bound
    AGENT_RELEASE_RELAY,       // RELEASE_ID entries discarded (ct_forwarder_nc_norelease)
    AGENT_SEND_INVALID,        // classes I and C discarded (ct_forwarder_nc_refuse, the pre-review rule)
    AGENT_ONE_ATTEMPT_PER_CYCLE, // RED shape: a back-pressured offer is retried in place
    AGENT_CLUSTER_ERROR_RETRY, // RED shape (FIX-7): a session-limit CLUSTER_ERROR exits 78
    AGENT_PROBE_AT_C3,         // RED shape: RESYNC(0) at C3 without the head probe
    AGENT_ME_SESSION_BEFORE_POLL, // RED shape (review FIX-1): the ME session starts only at supervision, after the poll
    AGENT_T2,                  // the property model's ME-loss recovery (T2) switched off (review FIX-4)
    RESYNC_SEQ_BELOW_ONE,      // RED shape (review NOTE-2): a dispatchSeq below 1 is pushed / held
    // RC2 S13 4.2 frame validation (MarketData.cpp)
    STITCH_HEADER_BLOCK,               // RED shape: a header blockLength below the template's accepted (4.2)
    STITCH_GROUP_BLOCK_LENGTH,         // RED shape (CL-F-9): a group blockLength below the entry size accepted (22, 25, 26)
    STITCH_COUNT_CAP,                  // RED shape: a group count above 20 (22) or 80 (25) accepted
    STITCH_FRAME_LENGTH,               // RED shape: a frame shorter than header + block + groups accepted
    STITCH_UNVERSIONED_EMPTY,          // RED shape (rule U): bookVersion 0 with two empty groups is malformed
    STITCH_VALUE_CHECK,                // RED shape: price / qty / count <= 0 accepted (22, 25)
    STITCH_DELTA_VERSION,              // RED shape (RC-N3): bookVersion <= fromVersion (or 0) accepted on a 25
    STITCH_RAW_ENUM_CHECK,             // RED shape: a raw side above 1 or updateType above 2 accepted
    STITCH_TRADE_VALIDATE,             // RED shape: a 26 entry with tradeId <= 0 or price <= 0 accepted
    // RC2 S13 section 4 ladder (BookLadder.cpp)
    STITCH_CHAIN_CHECK,                // RED shape: a delta with fromVersion != version applied anyway (4.1 rule 3)
    STITCH_STRICT_LEVEL_OPS,           // RED shape: NEW of a present price updates, UPDATE / DELETE of an absent one is lenient
    STITCH_DEPTH_CHECK,                // RED shape: more than 20 levels at the frame end accepted
    STITCH_40_SLOTS,                   // RED shape (CL-N-11): the insert bound check caps a side at 20 mid frame (DEPTH on the 21st)
    LADDER_RESET_ON_SESSION_END,       // RED shape: an ME session end keeps every ladder and trade mark (4.1 rule 6)
    // RC2 S13 section 6 emitter (TickEmitter.cpp)
    TICK_BROKEN_HOLD,                  // RED shape: a BROKEN ladder's (discarded) state is offered (E1)
    TICK_VALIDATE,                     // RED shape: no validation before the offer (E9)
    TICK_TOP_ONLY,                     // RED shape: any version move is a change (deeper levels emit)
    TICK_TRADE_ONLY_SILENT,            // RED shape: a newer trade is a change without a version move (the RC1 X6 text)
    TICK_LEADING_EDGE,                 // RED shape: every change waits for the window end
    TICK_TRAILING_NEWEST,              // RED shape: the trailing offer carries the state that set the flag
    TICK_TRAILING_EDGE,                // RED shape: a change inside the window is dropped (no flag)
    TICK_RATE_CAP,                     // RED shape: the window never starts (every change offered at once)
    TICK_VERSION_ABOVE,                // RED shape: a candidate at or below lastOffered.version is offered (E5)
    TICK_CONFIRMED_ONLY,               // RED shape: a failed offer moves lastOffered (E6)
    TICK_REOFFER_ON_REGISTER,          // RED shape: a (re-)registration re-offers nothing (E7)
    TICK_REOFFER_OUTSIDE_RATE,         // RED shape (review F-1): a re-offer restarts the T_cap window (6.0, E10)
    TICK_PENDING_AGE_GATED,            // RED shape: the pending-age clock keeps running while BROKEN / EMPTY / gated (E11)
    TICK_EPISODE_RESET,                // RED shape: the suppressed episode flag never clears (6.0)
    // RC2 S13 sections 3, 7.3, 10 agent integration (app/forwarder ForwarderAgent.h)
    AGENT_ME_STALE_TERM_FILTER,        // RED shape (F-S13-8): 22 / 25 / 26 of an ended ME term reach the ladder
    AGENT_TICK_GATE,                   // RED shape: ticks offered without the section 3 gate (unbound, unregistered)
    AGENT_TICK_AFTER_POLL,             // RED shape (F-S13-4): the emitter steps inside the ME poll callback, per frame
    AGENT_KNOWN_TEMPLATES,             // RED shape (F-S13-5): 23 / 24 / 27 counted as unknown ME templates
    AGENT_BOOK_STALE,                  // RED shape: forwarder_ticks_ready never drops (no BOOK_STALE conditions)
    AGENT_TICK_BEFORE_REQUEST,         // RED shape (CL-F-3): step 5a runs before step 5 (ticks ahead of the 65)
    AGENT_TICK_REJECT_HANDLED,         // RED shape (F-S13-9): a core reject naming 60 is an unknown core template
    COUNT
};

const char *ruleName(Rule rule) noexcept;

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept;
// Test builds only: switch rules off by name (comma-separated), e.g. a negative-control binary's preset.
// Returns false if a name is unknown.
bool forceDisable(const char *commaSeparatedNames) noexcept;
#else
constexpr bool disabled(Rule) noexcept
{
    return false;
}
#endif

}}}}

#endif
