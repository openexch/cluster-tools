#include "cluster_tools/domain/bridge/TestSeams.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cluster_tools { namespace domain { namespace bridge { namespace test_seams {

const char *ruleName(Rule rule) noexcept
{
    switch (rule)
    {
        case Rule::FORWARD_GREATER: return "FORWARD_GREATER";
        case Rule::TERMINAL_INCLUSIVE: return "TERMINAL_INCLUSIVE";
        case Rule::SKIP_OFF: return "SKIP_OFF";
        case Rule::EPOCH_CHECK_OFF: return "EPOCH_CHECK_OFF";
        case Rule::EPOCH_DIRECT_HALT: return "EPOCH_DIRECT_HALT";
        case Rule::ADOPT_OFF: return "ADOPT_OFF";
        case Rule::HEAD_RULE_OFF: return "HEAD_RULE_OFF";
        case Rule::REPORT_VALIDATION_OFF: return "REPORT_VALIDATION_OFF";
        case Rule::LOSS_RULE_OFF: return "LOSS_RULE_OFF";
        case Rule::LAST_FOLLOWS_REPORT: return "LAST_FOLLOWS_REPORT";
        case Rule::STAGE_TRUNCATES: return "STAGE_TRUNCATES";
        case Rule::FLUSH_BEFORE_NON_TRADE_OFF: return "FLUSH_BEFORE_NON_TRADE_OFF";
        case Rule::CAP_OFF: return "CAP_OFF";
        case Rule::IDLE_FLUSH_OFF: return "IDLE_FLUSH_OFF";
        case Rule::PRE_SWITCH_FLUSH_OFF: return "PRE_SWITCH_FLUSH_OFF";
        case Rule::BP_DROPS: return "BP_DROPS";
        case Rule::TICK_FLUSHES_ALWAYS: return "TICK_FLUSHES_ALWAYS";
        case Rule::RING_PROOF_TRADES_ONLY: return "RING_PROOF_TRADES_ONLY";
        case Rule::RING_PROOF_CURRENT_SOURCE: return "RING_PROOF_CURRENT_SOURCE";
        case Rule::RING_UNBOUNDED: return "RING_UNBOUNDED";
        case Rule::RING_KEEP_TAIL: return "RING_KEEP_TAIL";
        case Rule::KEYTABLE_UNBOUNDED: return "KEYTABLE_UNBOUNDED";
        case Rule::KEYTABLE_CLEAR_WITH_STAGING: return "KEYTABLE_CLEAR_WITH_STAGING";
        case Rule::RING_FULL_AT_ZERO: return "RING_FULL_AT_ZERO";
        case Rule::ROTATION_OFF: return "ROTATION_OFF";
        case Rule::GAP_SEEN_MAX: return "GAP_SEEN_MAX";
        case Rule::HOLE_VERIFY_OFF: return "HOLE_VERIFY_OFF";
        case Rule::REFUSAL_IGNORED: return "REFUSAL_IGNORED";
        case Rule::Q2_GAP: return "Q2_GAP";
        case Rule::S6_OFF: return "S6_OFF";
        case Rule::UNREACHABLE_AS_HOLE: return "UNREACHABLE_AS_HOLE";
        case Rule::STRIKES_OFF: return "STRIKES_OFF";
        case Rule::EPOCH_REACHABLE_ONLY: return "EPOCH_REACHABLE_ONLY";
        case Rule::HALT_CONFIRM_OFF: return "HALT_CONFIRM_OFF";
        case Rule::MALFORMED_AS_UNREACHABLE: return "MALFORMED_AS_UNREACHABLE";
        case Rule::MEMO_FAULT_STRIKES: return "MEMO_FAULT_STRIKES";
        case Rule::ROTATE_PAST_CURSOR: return "ROTATE_PAST_CURSOR";
        case Rule::BEHIND_OFF: return "BEHIND_OFF";
        case Rule::BEHIND_RESCAN_OFF: return "BEHIND_RESCAN_OFF";
        case Rule::GRACE_OFF: return "GRACE_OFF";
        case Rule::BEHIND_PARK_FIRST_CONFIGURED: return "BEHIND_PARK_FIRST_CONFIGURED";
        case Rule::DWELL_FROM_ATTACH: return "DWELL_FROM_ATTACH";
        case Rule::HALT_FIELDS_ZERO: return "HALT_FIELDS_ZERO";
        case Rule::HALT_FLUSH_OFF: return "HALT_FLUSH_OFF";
        case Rule::HALT_RESEND_OFF: return "HALT_RESEND_OFF";
        case Rule::REJECT_EXIT_AT_ONCE: return "REJECT_EXIT_AT_ONCE";
        case Rule::GAP_AUTO_RESUME_OFF: return "GAP_AUTO_RESUME_OFF";
        case Rule::SCHEMA_PARK_FOREVER: return "SCHEMA_PARK_FOREVER";
        case Rule::HALTED_IGNORES_REPORT_RULES: return "HALTED_IGNORES_REPORT_RULES";
        case Rule::RESUME_RULE_OFF: return "RESUME_RULE_OFF";
        case Rule::STORM_GUARD_OFF: return "STORM_GUARD_OFF";
        case Rule::UNBOUND_COUNTS_AS_STORM: return "UNBOUND_COUNTS_AS_STORM";
        case Rule::FENCE_OFF: return "FENCE_OFF";
        case Rule::FENCE_PER_REJECT: return "FENCE_PER_REJECT";
        case Rule::HALT_KEEP_ALWAYS: return "HALT_KEEP_ALWAYS";
        case Rule::REGISTRATION_GATE_OFF: return "REGISTRATION_GATE_OFF";
        case Rule::STEPBACK_COUNTER_OFF: return "STEPBACK_COUNTER_OFF";
        case Rule::CONFIRM_REQUERY: return "CONFIRM_REQUERY";
        case Rule::STORM_TRIED_AT_SWITCH: return "STORM_TRIED_AT_SWITCH";
        case Rule::STORM_TARGETS_CURRENT: return "STORM_TARGETS_CURRENT";
        case Rule::STALE_PREFIX_FOREIGN: return "STALE_PREFIX_FOREIGN";
        case Rule::REFUSAL_VERIFY_OFF: return "REFUSAL_VERIFY_OFF";
        case Rule::SUCCESSOR_WALK_OFF: return "SUCCESSOR_WALK_OFF";
        case Rule::STEP_BACK_OFF: return "STEP_BACK_OFF";
        case Rule::SCHEMA_RESCAN_OFF: return "SCHEMA_RESCAN_OFF";
        case Rule::SEEN_FROM_FIRST_RECORDING_ONLY: return "SEEN_FROM_FIRST_RECORDING_ONLY";
        case Rule::MARKER_NEVER_RESET: return "MARKER_NEVER_RESET";
        case Rule::RELIST_ON_EPOCH_HALT: return "RELIST_ON_EPOCH_HALT";
        case Rule::RELIST_EVERY_TICK: return "RELIST_EVERY_TICK";
        case Rule::GENESIS_EXIT_UNCOUNTED: return "GENESIS_EXIT_UNCOUNTED";
        case Rule::SUCCESSOR_WALK_WHILE_STREAMING: return "SUCCESSOR_WALK_WHILE_STREAMING";
        case Rule::FEEDHALT_PROMPT: return "FEEDHALT_PROMPT";
        case Rule::RESCAN_EXIT_A_OFF: return "RESCAN_EXIT_A_OFF";
        case Rule::MIRROR_FIRST_HALT_REASON: return "MIRROR_FIRST_HALT_REASON";
        case Rule::TAINT_VERIFY_OFF: return "TAINT_VERIFY_OFF";
        case Rule::TAINT_HALTED_EXEMPT: return "TAINT_HALTED_EXEMPT";
        case Rule::TAINT_NEVER_CLEARS: return "TAINT_NEVER_CLEARS";
        case Rule::GAP_RELATCH_OFF: return "GAP_RELATCH_OFF";
        case Rule::FOREIGN_HEAD_WALKS: return "FOREIGN_HEAD_WALKS";
        case Rule::CODEC_SWAP_TAKER_MAKER_ORDER: return "CODEC_SWAP_TAKER_MAKER_ORDER";
        case Rule::CODEC_ENUM_GET: return "CODEC_ENUM_GET";
        case Rule::CODEC_HEAD_NOT_PERMUTED: return "CODEC_HEAD_NOT_PERMUTED";
        case Rule::EGRESS_VERSION_FLOOR: return "EGRESS_VERSION_FLOOR";
        case Rule::COUNT: break;
    }
    return "?";
}

#ifdef CT_TEST_SEAMS
namespace {
// 0 = not read yet, 1 = active, 2 = disabled.
std::array<std::atomic<int>, static_cast<unsigned>(Rule::COUNT)> g_state{};
}

bool disabled(Rule rule) noexcept
{
    auto &slot = g_state[static_cast<unsigned>(rule)];
    int value = slot.load(std::memory_order_relaxed);
    if (0 == value)
    {
        char name[96]; // no allocation: the first lookup may happen inside a callback
        std::snprintf(name, sizeof(name), "CT_TEST_DISABLE_%s", ruleName(rule));
        const char *env = std::getenv(name);
        value = (nullptr != env && std::strcmp(env, "1") == 0) ? 2 : 1;
        slot.store(value, std::memory_order_relaxed);
    }
    return 2 == value;
}

bool forceDisable(const char *names) noexcept
{
    bool ok = true;
    std::string all = nullptr == names ? "" : names;
    std::size_t start = 0;
    while (start <= all.size())
    {
        const std::size_t comma = all.find(',', start);
        const std::string name = all.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty())
        {
            bool found = false;
            for (unsigned i = 0; i < static_cast<unsigned>(Rule::COUNT); i++)
            {
                if (name == ruleName(static_cast<Rule>(i)))
                {
                    g_state[i].store(2, std::memory_order_relaxed);
                    found = true;
                }
            }
            ok = ok && found;
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return ok;
}

void resetForTest() noexcept
{
    for (auto &slot : g_state)
    {
        slot.store(0, std::memory_order_relaxed);
    }
}
#endif

}}}}
