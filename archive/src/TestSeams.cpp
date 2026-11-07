#include "cluster_tools/archive/TestSeams.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>

namespace cluster_tools { namespace archive { namespace test_seams {

const char *ruleName(Rule rule) noexcept
{
    switch (rule)
    {
        case Rule::CHAIN_EMPTY_ROW: return "CHAIN_EMPTY_ROW";
        case Rule::CHAIN_EMPTY_WIDENED: return "CHAIN_EMPTY_WIDENED";
        case Rule::CHAIN_MALFORMED_ROW: return "CHAIN_MALFORMED_ROW";
        case Rule::CHAIN_DRAINED_ROW: return "CHAIN_DRAINED_ROW";
        case Rule::CHAIN_MTU_ROW: return "CHAIN_MTU_ROW";
        case Rule::CHAIN_MTU_FIRST: return "CHAIN_MTU_FIRST";
        case Rule::CHAIN_STREAM_FILTER: return "CHAIN_STREAM_FILTER";
        case Rule::PEEK_FLAGS: return "PEEK_FLAGS";
        case Rule::PEEK_LENGTH: return "PEEK_LENGTH";
        case Rule::PEEK_SCHEMA: return "PEEK_SCHEMA";
        case Rule::PEEK_TEMPLATE: return "PEEK_TEMPLATE";
        case Rule::PEEK_BLOCK: return "PEEK_BLOCK";
        case Rule::PEEK_FRAME_LENGTH: return "PEEK_FRAME_LENGTH";
        case Rule::PEEK_HEAD: return "PEEK_HEAD";
        case Rule::PEEK_OFFSETS_SHIFTED: return "PEEK_OFFSETS_SHIFTED";
        case Rule::PEEK_BLOCK_OFF_BY_ONE: return "PEEK_BLOCK_OFF_BY_ONE";
        case Rule::FOLLOW_ROW1_FATAL: return "FOLLOW_ROW1_FATAL";
        case Rule::FOLLOW_ROW3_REGRESSION: return "FOLLOW_ROW3_REGRESSION";
        case Rule::FOLLOW_ROW4_BELT: return "FOLLOW_ROW4_BELT";
        case Rule::FOLLOW_ROW5_DRAINED: return "FOLLOW_ROW5_DRAINED";
        case Rule::FOLLOW_ROW6_EARLY_EOF: return "FOLLOW_ROW6_EARLY_EOF";
        case Rule::FOLLOW_ROW7A_IMAGE_END_PROBE: return "FOLLOW_ROW7A_IMAGE_END_PROBE";
        case Rule::FOLLOW_ROW8_NEVER_CONNECTED: return "FOLLOW_ROW8_NEVER_CONNECTED";
        case Rule::FOLLOW_ROW9_ARCHIVE_GONE: return "FOLLOW_ROW9_ARCHIVE_GONE";
        case Rule::FOLLOW_ROW10_RECORDING_ENDED: return "FOLLOW_ROW10_RECORDING_ENDED";
        case Rule::FOLLOW_ROW11_STALL_AFTER_STOP: return "FOLLOW_ROW11_STALL_AFTER_STOP";
        case Rule::FOLLOW_ROW12_STALL: return "FOLLOW_ROW12_STALL";
        case Rule::FOLLOW_ROW12B_BOUNDED_STUCK: return "FOLLOW_ROW12B_BOUNDED_STUCK";
        case Rule::FOLLOW_ROW13_ARCHIVE_ERROR: return "FOLLOW_ROW13_ARCHIVE_ERROR";
        case Rule::FOLLOW_ROW15_BACKOFF: return "FOLLOW_ROW15_BACKOFF";
        case Rule::FOLLOW_PAUSE_RULE: return "FOLLOW_PAUSE_RULE";
        case Rule::FOLLOW_PAUSE_SUPPRESSES_CONTROL: return "FOLLOW_PAUSE_SUPPRESSES_CONTROL";
        case Rule::FOLLOW_ADVANCE_BEFORE_SINK: return "FOLLOW_ADVANCE_BEFORE_SINK";
        case Rule::FOLLOW_ROW7A_IGNORE: return "FOLLOW_ROW7A_IGNORE";
        case Rule::FOLLOW_ASYNC_DISCONNECT: return "FOLLOW_ASYNC_DISCONNECT";
        case Rule::FOLLOW_ABORT_IS_PAUSE: return "FOLLOW_ABORT_IS_PAUSE";
        case Rule::FOLLOW_CONTINUATION: return "FOLLOW_CONTINUATION";
        case Rule::FOLLOW_RELIST_RESET: return "FOLLOW_RELIST_RESET";
        case Rule::FOLLOW_PADDING_REACHED: return "FOLLOW_PADDING_REACHED";
        case Rule::MEMO_EPOCH_CHANGED: return "MEMO_EPOCH_CHANGED";
        case Rule::MEMO_CURSOR_REGRESSED: return "MEMO_CURSOR_REGRESSED";
        case Rule::MEMO_HISTORY_CHANGED: return "MEMO_HISTORY_CHANGED";
        case Rule::MEMO_HEAD_TRIM: return "MEMO_HEAD_TRIM";
        case Rule::MEMO_ACTIVE_PURGE_TRIM: return "MEMO_ACTIVE_PURGE_TRIM";
        case Rule::MEMO_POSITION_REJECTED: return "MEMO_POSITION_REJECTED";
        case Rule::MEMO_SOURCE_FAULT_INVALIDATES: return "MEMO_SOURCE_FAULT_INVALIDATES";
        case Rule::MEMO_HOLE_VERIFY: return "MEMO_HOLE_VERIFY";
        case Rule::MEMO_VERIFIED_MARKER: return "MEMO_VERIFIED_MARKER";
        case Rule::MEMO_PROVE_FROM_SKIP: return "MEMO_PROVE_FROM_SKIP";
        case Rule::MEMO_SHARED_ACROSS_SOURCES: return "MEMO_SHARED_ACROSS_SOURCES";
        case Rule::MEMO_CAPACITY_CLEARS: return "MEMO_CAPACITY_CLEARS";
        case Rule::MEMO_UNALIGNED_CHECK: return "MEMO_UNALIGNED_CHECK";
        case Rule::MEMO_PURGE_RACE_RELIST: return "MEMO_PURGE_RACE_RELIST";
        case Rule::MEMO_KEY_WITH_START: return "MEMO_KEY_WITH_START";
        case Rule::PROPERTY_FORWARD_GREATER: return "PROPERTY_FORWARD_GREATER";
        case Rule::ADAPTER_STOP_REPLAY: return "ADAPTER_STOP_REPLAY";
        case Rule::ADAPTER_STORAGE_PRECHECK: return "ADAPTER_STORAGE_PRECHECK";
        case Rule::ADAPTER_RESOLVE_ENDPOINT: return "ADAPTER_RESOLVE_ENDPOINT";
        case Rule::ADAPTER_INVOKER: return "ADAPTER_INVOKER";
        case Rule::TAIL_TERMINATE_HANDLER: return "TAIL_TERMINATE_HANDLER";
        case Rule::COUNT: break;
    }
    return "?";
}

#ifdef CT_TEST_SEAMS
bool disabled(Rule rule) noexcept
{
    // 0 = not read yet, 1 = active, 2 = disabled. Read once; tests set the variable per process.
    static std::array<std::atomic<int>, static_cast<unsigned>(Rule::COUNT)> state{};
    auto &slot = state[static_cast<unsigned>(rule)];
    int value = slot.load(std::memory_order_relaxed);
    if (0 == value)
    {
        const std::string name = std::string("CT_TEST_DISABLE_") + ruleName(rule);
        const char *env = std::getenv(name.c_str());
        value = (nullptr != env && std::string(env) == "1") ? 2 : 1;
        slot.store(value, std::memory_order_relaxed);
    }
    return 2 == value;
}
#endif

}}}
