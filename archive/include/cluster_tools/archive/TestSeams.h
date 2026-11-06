#ifndef CLUSTER_TOOLS_ARCHIVE_TEST_SEAMS_H
#define CLUSTER_TOOLS_ARCHIVE_TEST_SEAMS_H

// Negative-control seams (RC2 S9a contract section 9 preamble): each rule the S9a tests pin can be
// switched off in a TEST build only, so the suite proves that the test fails for the stated reason
// when the rule is gone (the RED column of 9.1 and 9.3). The seam exists only when the translation
// unit is compiled with CT_TEST_SEAMS (the *_seamed test libraries); in every production target
// disabled() is a constexpr false and the compiler removes the branch. In a seamed build a rule is
// off when the environment variable CT_TEST_DISABLE_<RULE> is set to 1 (read once per rule).

namespace cluster_tools { namespace archive { namespace test_seams {

enum class Rule : unsigned
{
    // 4.2 recording validity table
    CHAIN_EMPTY_ROW,          // row 2: EMPTY stopped recording skipped
    CHAIN_EMPTY_WIDENED,      // RED shape: EMPTY widened to stop >= start
    CHAIN_MALFORMED_ROW,      // row 1
    CHAIN_DRAINED_ROW,        // row 3
    CHAIN_MTU_ROW,            // row 4
    CHAIN_MTU_FIRST,          // RED shape: MTU row placed before rows 2-3
    CHAIN_STREAM_FILTER,      // foreign stream descriptors dropped
    // 4.4 frame admission
    PEEK_FLAGS,
    PEEK_LENGTH,
    PEEK_SCHEMA,
    PEEK_TEMPLATE,
    PEEK_BLOCK,
    PEEK_FRAME_LENGTH,
    PEEK_HEAD,
    PEEK_OFFSETS_SHIFTED,     // RED shape: head read 8 bytes late
    PEEK_BLOCK_OFF_BY_ONE,    // RED shape: block table off by one
    // 4.5 follow decision table
    FOLLOW_ROW1_FATAL,
    FOLLOW_ROW3_REGRESSION,
    FOLLOW_ROW4_BELT,
    FOLLOW_ROW5_DRAINED,
    FOLLOW_ROW6_EARLY_EOF,
    FOLLOW_ROW7A_IMAGE_END_PROBE,
    FOLLOW_ROW8_NEVER_CONNECTED,
    FOLLOW_ROW9_ARCHIVE_GONE,
    FOLLOW_ROW10_RECORDING_ENDED,
    FOLLOW_ROW11_STALL_AFTER_STOP,
    FOLLOW_ROW12_STALL,
    FOLLOW_ROW12B_BOUNDED_STUCK,
    FOLLOW_ROW13_ARCHIVE_ERROR,
    FOLLOW_ROW15_BACKOFF,
    FOLLOW_PAUSE_RULE,
    FOLLOW_PAUSE_SUPPRESSES_CONTROL, // RED shape (review F1): limit 0 also suppresses the control call
    FOLLOW_ADVANCE_BEFORE_SINK,      // RED shape (F19): previous seq advanced before the sink answers
    FOLLOW_ROW7A_IGNORE,             // RED shape: an image end on a live follow is ignored entirely
    FOLLOW_ASYNC_DISCONNECT,         // RED shape: a lost control session (async poll) is ignored
    FOLLOW_ABORT_IS_PAUSE,           // RED shape (review F-4): a sink ABORT does not count as paused
    FOLLOW_CONTINUATION,             // RED shape (review F-1): the continuation point is lost on back-off
    FOLLOW_RELIST_RESET,             // RED shape (review F-2): the purge-race re-list is once per connection
    FOLLOW_PADDING_REACHED,          // RED shape (review N-1): trailing padding does not count as reached
    // section 5 memo
    MEMO_EPOCH_CHANGED,       // 5.3 row 1
    MEMO_CURSOR_REGRESSED,    // 5.3 row 2
    MEMO_HISTORY_CHANGED,     // 5.3 row 3
    MEMO_HEAD_TRIM,           // 5.3 row 4
    MEMO_ACTIVE_PURGE_TRIM,   // 5.3 row 5
    MEMO_POSITION_REJECTED,   // 5.3 row 6
    MEMO_SOURCE_FAULT_INVALIDATES, // 5.3 row 7 (seq regression, resume belt)
    MEMO_HOLE_VERIFY,         // 5.2 rule 5
    MEMO_VERIFIED_MARKER,     // RED shape (review F-3): a believed hole keeps forcing memo-free reads
    MEMO_PROVE_FROM_SKIP,     // 5.2 rule 1
    MEMO_SHARED_ACROSS_SOURCES, // RED shape: one memo for all sources
    MEMO_CAPACITY_CLEARS,     // RED shape: a capacity rejection clears the proofs
    MEMO_UNALIGNED_CHECK,     // 4.3 step 3
    MEMO_PURGE_RACE_RELIST,   // 4.3 step 6 re-list on INVALID_POSITION
    MEMO_KEY_WITH_START,      // RED shape: the Java key with startPosition
    // 5.5 forward rule stub of the property test
    PROPERTY_FORWARD_GREATER, // RED shape: forward when seq > last
    // archive/aeron adapter (9.3)
    ADAPTER_STOP_REPLAY,      // 4.3 step 7 stopReplay on detach
    ADAPTER_STORAGE_PRECHECK, // 4.3 step 2
    ADAPTER_RESOLVE_ENDPOINT, // 4.3 step 5: the port-0 channel is sent unresolved
    ADAPTER_INVOKER,          // 3.3: the archive Context gets no delegating invoker
    // app/journal-tail (F14)
    TAIL_TERMINATE_HANDLER,
    COUNT
};

const char *ruleName(Rule rule) noexcept;

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept;
#else
constexpr bool disabled(Rule) noexcept
{
    return false;
}
#endif

}}}

#endif
