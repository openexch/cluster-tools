#include "cluster_tools/domain/forwarder/TestSeams.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace cluster_tools { namespace domain { namespace forwarder { namespace test_seams {

const char *ruleName(Rule rule) noexcept
{
    switch (rule)
    {
        case Rule::RULE_V12: return "RULE_V12";
        case Rule::RULE_R1: return "RULE_R1";
        case Rule::RULE_R2: return "RULE_R2";
        case Rule::RULE_R3: return "RULE_R3";
        case Rule::RULE_R4: return "RULE_R4";
        case Rule::RULE_R5: return "RULE_R5";
        case Rule::RULE_B1: return "RULE_B1";
        case Rule::RULE_B2: return "RULE_B2";
        case Rule::RULE_B3: return "RULE_B3";
        case Rule::RULE_B4: return "RULE_B4";
        case Rule::RULE_B5: return "RULE_B5";
        case Rule::RULE_B6: return "RULE_B6";
        case Rule::RULE_V1: return "RULE_V1";
        case Rule::RULE_X1: return "RULE_X1";
        case Rule::RULE_X2: return "RULE_X2";
        case Rule::RULE_A1: return "RULE_A1";
        case Rule::RULE_ORDER_B_BEFORE_R: return "RULE_ORDER_B_BEFORE_R";
        case Rule::CARRIER: return "CARRIER";
        case Rule::CARRIER_TIF: return "CARRIER_TIF";
        case Rule::RING_GROUP_CAP_48: return "RING_GROUP_CAP_48";
        case Rule::RING_RX_ISOLATION: return "RING_RX_ISOLATION";
        case Rule::RING_GROUP_RELEASES: return "RING_GROUP_RELEASES";
        case Rule::RESYNC_REPLY_LAST: return "RESYNC_REPLY_LAST";
        case Rule::RESYNC_FINAL_DROP: return "RESYNC_FINAL_DROP";
        case Rule::RESYNC_SHAPE_S1: return "RESYNC_SHAPE_S1";
        case Rule::RESYNC_SHAPE_S2: return "RESYNC_SHAPE_S2";
        case Rule::RESYNC_SHAPE_S3: return "RESYNC_SHAPE_S3";
        case Rule::RESYNC_T9_RESTART: return "RESYNC_T9_RESTART";
        case Rule::RESYNC_T9: return "RESYNC_T9";
        case Rule::RESYNC_D0_CLEARS_HOLD: return "RESYNC_D0_CLEARS_HOLD";
        case Rule::RESYNC_ZERO_CLEARS_RING: return "RESYNC_ZERO_CLEARS_RING";
        case Rule::RESYNC_T5_GAP: return "RESYNC_T5_GAP";
        case Rule::RESYNC_GAP_HELD: return "RESYNC_GAP_HELD";
        case Rule::RESYNC_DUPLICATE_DROP: return "RESYNC_DUPLICATE_DROP";
        case Rule::RESYNC_HEAD_PROBE: return "RESYNC_HEAD_PROBE";
        case Rule::RESYNC_T7: return "RESYNC_T7";
        case Rule::RESYNC_PROBE_COUNT: return "RESYNC_PROBE_COUNT";
        case Rule::RESYNC_D3_RESUME: return "RESYNC_D3_RESUME";
        case Rule::RESYNC_D3_ZERO_AT_ONCE: return "RESYNC_D3_ZERO_AT_ONCE";
        case Rule::RESYNC_CURSOR_MAX_HEAD: return "RESYNC_CURSOR_MAX_HEAD";
        case Rule::RESYNC_ALL_TRIGGERS: return "RESYNC_ALL_TRIGGERS";
        case Rule::EPOCH_ZERO_IGNORED: return "EPOCH_ZERO_IGNORED";
        case Rule::EPOCH_ZERO_FATAL: return "EPOCH_ZERO_FATAL";
        case Rule::EPOCH_MISMATCH_FATAL: return "EPOCH_MISMATCH_FATAL";
        case Rule::EPOCH_WAIT: return "EPOCH_WAIT";
        case Rule::EPOCH_PER_SESSION: return "EPOCH_PER_SESSION";
        case Rule::ADMISSION_ORIGIN_BASELINE: return "ADMISSION_ORIGIN_BASELINE";
        case Rule::ADMISSION_GLOBAL_ONLY: return "ADMISSION_GLOBAL_ONLY";
        case Rule::ADMISSION_REASON_BITS: return "ADMISSION_REASON_BITS";
        case Rule::AGENT_STALE_TERM_FILTER: return "AGENT_STALE_TERM_FILTER";
        case Rule::AGENT_EPOCH_BEFORE_REGISTER: return "AGENT_EPOCH_BEFORE_REGISTER";
        case Rule::AGENT_RELEASE_RELAY: return "AGENT_RELEASE_RELAY";
        case Rule::AGENT_SEND_INVALID: return "AGENT_SEND_INVALID";
        case Rule::AGENT_ONE_ATTEMPT_PER_CYCLE: return "AGENT_ONE_ATTEMPT_PER_CYCLE";
        case Rule::AGENT_CLUSTER_ERROR_RETRY: return "AGENT_CLUSTER_ERROR_RETRY";
        case Rule::AGENT_PROBE_AT_C3: return "AGENT_PROBE_AT_C3";
        case Rule::AGENT_ME_SESSION_BEFORE_POLL: return "AGENT_ME_SESSION_BEFORE_POLL";
        case Rule::AGENT_T2: return "AGENT_T2";
        case Rule::RESYNC_SEQ_BELOW_ONE: return "RESYNC_SEQ_BELOW_ONE";
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
        char name[96]; // no allocation: the first lookup of a rule may happen inside a counted zero-alloc window
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
#endif

}}}}
