#include "cluster_tools/domain/bridge/HaltMachine.h"

#include "cluster_tools/domain/bridge/TestSeams.h"

namespace cluster_tools { namespace domain { namespace bridge {

using test_seams::Rule;
using test_seams::disabled;

HaltReason wireReasonOf(BridgeHalt halt) noexcept
{
    switch (halt)
    {
        case BridgeHalt::GAP: return HaltReason::GAP;
        case BridgeHalt::EPOCH_MISMATCH: return HaltReason::EPOCH_MISMATCH;
        case BridgeHalt::SOURCE_SCHEMA: return HaltReason::SOURCE_SCHEMA;
        case BridgeHalt::CORE_REFUSED:
        case BridgeHalt::NONE: break;
    }
    return HaltReason::NONE;
}

FeedHaltFields feedHaltFor(BridgeHalt halt, std::int64_t e, std::int64_t seen, std::int64_t syncEpoch,
    std::int64_t foreignEpoch) noexcept
{
    FeedHaltFields f;
    f.reason = wireReasonOf(halt);
    f.expected = e;
    switch (halt)
    {
        case BridgeHalt::GAP:
            f.meEpochSeen = syncEpoch; // 0 while unbound
            f.seen = disabled(Rule::HALT_FIELDS_ZERO) ? e : seen; // `> e` by construction (8.3 row 4)
            break;
        case BridgeHalt::EPOCH_MISMATCH:
            f.meEpochSeen = foreignEpoch; // >= 1 by the gate's HEAD row
            f.seen = seen;
            break;
        case BridgeHalt::SOURCE_SCHEMA:
            f.meEpochSeen = syncEpoch;
            f.seen = e;
            break;
        case BridgeHalt::CORE_REFUSED:
        case BridgeHalt::NONE:
            break;
    }
    return f;
}

FeedHaltFields fallbackFeedHalt(std::int64_t e, std::int64_t syncEpoch) noexcept
{
    FeedHaltFields f;
    f.reason = HaltReason::SOURCE_SCHEMA;
    f.meEpochSeen = syncEpoch;
    f.expected = e < 1 ? 1 : e;
    f.seen = f.expected;
    return f;
}

bool feedHaltFieldsValid(const FeedHaltFields &f) noexcept
{
    switch (f.reason)
    {
        case HaltReason::GAP: return f.expected >= 1 && f.expected < f.seen;
        case HaltReason::EPOCH_MISMATCH: return 0 != f.meEpochSeen;
        case HaltReason::SOURCE_SCHEMA: return f.expected >= 1;
        case HaltReason::SOURCE_UNBOUND:
        case HaltReason::SETTLE_REFUSED: return true;
        case HaltReason::NONE: break;
    }
    return false;
}

}}}
