#include "cluster_tools/domain/bridge/BridgeMetrics.h"
#include "cluster_tools/domain/bridge/Types.h"

namespace cluster_tools { namespace domain { namespace bridge {

const char *haltReasonName(HaltReason reason) noexcept
{
    switch (reason)
    {
        case HaltReason::NONE: return "NONE";
        case HaltReason::GAP: return "GAP";
        case HaltReason::EPOCH_MISMATCH: return "EPOCH_MISMATCH";
        case HaltReason::SOURCE_SCHEMA: return "SOURCE_SCHEMA";
        case HaltReason::SOURCE_UNBOUND: return "SOURCE_UNBOUND";
        case HaltReason::SETTLE_REFUSED: return "SETTLE_REFUSED";
    }
    return "?";
}

const char *fenceTriggerName(FenceTrigger trigger) noexcept
{
    switch (trigger)
    {
        case FenceTrigger::NEW_LEADER: return "new_leader";
        case FenceTrigger::SESSION_REESTABLISHED: return "session_reestablished";
        case FenceTrigger::SETTLE_REJECTED_GAP: return "settle_rejected_gap";
        case FenceTrigger::SETTLE_REJECTED_UNBOUND: return "settle_rejected_unbound";
        case FenceTrigger::SETTLE_REJECTED_EPOCH: return "settle_rejected_epoch";
        case FenceTrigger::REPORT_LOSS: return "report_loss";
        case FenceTrigger::REPORT_EPOCH_CHANGED: return "report_epoch_changed";
        case FenceTrigger::REPORT_CURSOR_REGRESSED: return "report_cursor_regressed";
        case FenceTrigger::LANE_AHEAD: return "lane_ahead";
        case FenceTrigger::COUNT: break;
    }
    return "?";
}

const char *stateName(State state) noexcept
{
    switch (state)
    {
        case State::DOWN: return "DOWN";
        case State::SYNCING: return "SYNCING";
        case State::STREAMING: return "STREAMING";
        case State::HALTING: return "HALTING";
        case State::HALTED: return "HALTED";
    }
    return "?";
}

const char *bridgeHaltName(BridgeHalt halt) noexcept
{
    switch (halt)
    {
        case BridgeHalt::NONE: return "NONE";
        case BridgeHalt::GAP: return "GAP";
        case BridgeHalt::EPOCH_MISMATCH: return "EPOCH_MISMATCH";
        case BridgeHalt::SOURCE_SCHEMA: return "SOURCE_SCHEMA";
        case BridgeHalt::CORE_REFUSED: return "CORE_REFUSED";
    }
    return "?";
}

const char *fatalName(Fatal fatal) noexcept
{
    switch (fatal)
    {
        case Fatal::NONE: return "NONE";
        case Fatal::FEEDHALT_REJECTED: return "FEEDHALT_REJECTED";
        case Fatal::ENCODER_OVERFLOW: return "ENCODER_OVERFLOW";
        case Fatal::OFFER_REFUSED: return "OFFER_REFUSED";
    }
    return "?";
}

const char *verdictName(VerdictKind kind) noexcept
{
    switch (kind)
    {
        case VerdictKind::UNKNOWN: return "UNKNOWN";
        case VerdictKind::HOLED: return "HOLED";
        case VerdictKind::BAD: return "BAD";
        case VerdictKind::FOREIGN_EPOCH: return "FOREIGN_EPOCH";
        case VerdictKind::BEHIND: return "BEHIND";
        case VerdictKind::UNREACHABLE: return "UNREACHABLE";
        case VerdictKind::REFUSED_STATIC: return "REFUSED_STATIC";
    }
    return "?";
}

const char *badReasonLabel(BadReason reason) noexcept
{
    switch (reason)
    {
        case BadReason::REFUSAL_FRAGMENTED: return "fragmented";
        case BadReason::REFUSAL_LENGTH: return "length";
        case BadReason::REFUSAL_SCHEMA: return "schema";
        case BadReason::REFUSAL_TEMPLATE: return "template";
        case BadReason::REFUSAL_BLOCK: return "block";
        case BadReason::REFUSAL_FRAME_LENGTH: return "frame_length";
        case BadReason::REFUSAL_HEAD: return "head";
        case BadReason::HEAD_NOT_CONFIG: return "head_not_config";
        case BadReason::SEQ_REGRESSION: return "seq_regression";
        case BadReason::FOREIGN_EPOCH: return "foreign_epoch";
        case BadReason::MALFORMED_EXTENT: return "malformed_extent";
        case BadReason::STALE_EPOCH_PREFIX: return "stale_epoch_prefix";
        case BadReason::COUNT: break;
    }
    return "?";
}

const char *frameKindLabel(FrameKind kind) noexcept
{
    switch (kind)
    {
        case FrameKind::K61: return "61";
        case FrameKind::K62: return "62";
        case FrameKind::K63: return "63";
        case FrameKind::K64: return "64";
        case FrameKind::K67: return "67";
        case FrameKind::K68: return "68";
        case FrameKind::COUNT: break;
    }
    return "?";
}

FrameKind frameKindOf(std::uint16_t moneyTemplate) noexcept
{
    switch (moneyTemplate)
    {
        case 61: return FrameKind::K61;
        case 62: return FrameKind::K62;
        case 63: return FrameKind::K63;
        case 64: return FrameKind::K64;
        case 67: return FrameKind::K67;
        case 68: return FrameKind::K68;
        default: return FrameKind::COUNT;
    }
}

const char *deferReasonLabel(DeferReason reason) noexcept
{
    switch (reason)
    {
        case DeferReason::STATE: return "state";
        case DeferReason::COMMAND: return "command";
        case DeferReason::SOURCE: return "source";
        case DeferReason::RING_FULL: return "ring_full";
        case DeferReason::KEYTABLE_FULL: return "keytable_full";
        case DeferReason::CLOSED: return "closed";
        case DeferReason::COUNT: break;
    }
    return "?";
}

}}}
