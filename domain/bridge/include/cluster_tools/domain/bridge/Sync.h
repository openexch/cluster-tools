#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_SYNC_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_SYNC_H

// Cursor and sync: the report validity rules of 7.2, the one outstanding QueryFeedPosition
// with the ring tail it covers (the exact report-loss rule), and the periodic report rules of a
// streaming sync as a pure function. The forward rule itself is in BridgeDomain::onFrame.

#include "cluster_tools/domain/bridge/Types.h"

#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

enum class ReportFault : std::uint8_t
{
    NONE,
    NEGATIVE_CURSOR,
    NEGATIVE_EPOCH,
    EPOCH_CURSOR_PAIR,  // (E == 0) != (C == 0), the S7 2.1 invariant
    HALT_REASON_RANGE,  // haltReasonRaw > 5
    HALTED_RAW,         // feedHaltedRaw not 0 or 1 (a raw BoolFlag)
    HALTED_MISMATCH     // feedHalted != (haltReasonRaw != 0)
};
const char *reportFaultName(ReportFault fault) noexcept;

ReportFault validateReport(const FeedPositionReport &report) noexcept;

// The outstanding query (7.2): one at a time; re-sent with the same correlation id until answered, so its
// coversSeq (the ring tail when the FIRST send returned OK) never exceeds what any answer covers.
struct QueryTracker
{
    bool wanted = false;       // a query is due: offered (outstanding) or waiting for an OK offer
    bool offered = false;      // the first send returned OK; an answer with `correlationId` is expected
    std::int64_t correlationId = 0;
    std::int64_t coversSeq = 0;
    TimePoint firstSentAt{};
    TimePoint nextResendAt{};
    bool timeoutCounted = false;

    void reset() noexcept { *this = QueryTracker{}; }
};

// The periodic report rules of a streaming sync (7.2, in order). `coversSeq` is the outstanding query's,
// `previousCursor` the previous report's C (or -1 when there was none in this process).
enum class ReportRule : std::uint8_t
{
    ACCEPT,
    LOSS,               // rule 1: C' < coversSeq
    EPOCH_CHANGED,      // rule 2
    CURSOR_REGRESSED    // rule 3
};
ReportRule periodicRule(std::int64_t cursor, std::int64_t epoch, std::int64_t coversSeq, std::int64_t syncEpoch,
    std::int64_t previousCursor) noexcept;

}}}

#endif
