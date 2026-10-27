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
