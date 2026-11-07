#include "cluster_tools/domain/bridge/Sync.h"

#include "cluster_tools/domain/bridge/TestSeams.h"

namespace cluster_tools { namespace domain { namespace bridge {

using test_seams::Rule;
using test_seams::disabled;

const char *reportFaultName(ReportFault fault) noexcept
{
    switch (fault)
    {
        case ReportFault::NONE: return "none";
        case ReportFault::NEGATIVE_CURSOR: return "negative_cursor";
        case ReportFault::NEGATIVE_EPOCH: return "negative_epoch";
        case ReportFault::EPOCH_CURSOR_PAIR: return "epoch_cursor_pair";
        case ReportFault::HALT_REASON_RANGE: return "halt_reason_range";
        case ReportFault::HALTED_RAW: return "halted_raw";
        case ReportFault::HALTED_MISMATCH: return "halted_mismatch";
    }
    return "?";
}

ReportFault validateReport(const FeedPositionReport &r) noexcept
{
    if (disabled(Rule::REPORT_VALIDATION_OFF))
    {
        return ReportFault::NONE;
    }
    if (r.lastAppliedJournalSeq < 0)
    {
        return ReportFault::NEGATIVE_CURSOR;
    }
    if (r.meEpoch < 0)
    {
        return ReportFault::NEGATIVE_EPOCH;
    }
    if ((0 == r.meEpoch) != (0 == r.lastAppliedJournalSeq))
    {
        return ReportFault::EPOCH_CURSOR_PAIR;
    }
    if (r.haltReasonRaw > 5)
    {
        return ReportFault::HALT_REASON_RANGE;
    }
    if (r.feedHaltedRaw > 1)
    {
        return ReportFault::HALTED_RAW;
    }
    if ((1 == r.feedHaltedRaw) != (0 != r.haltReasonRaw))
    {
        return ReportFault::HALTED_MISMATCH;
    }
    return ReportFault::NONE;
}

ReportRule periodicRule(std::int64_t cursor, std::int64_t epoch, std::int64_t coversSeq, std::int64_t syncEpoch,
    std::int64_t previousCursor) noexcept
{
    if (cursor < coversSeq && !disabled(Rule::LOSS_RULE_OFF))
    {
        return ReportRule::LOSS;
    }
    if (0 != syncEpoch && 0 != epoch && epoch != syncEpoch)
    {
        return ReportRule::EPOCH_CHANGED;
    }
    if (previousCursor >= 0 && cursor < previousCursor)
    {
        return ReportRule::CURSOR_REGRESSED;
    }
    return ReportRule::ACCEPT;
}

}}}
