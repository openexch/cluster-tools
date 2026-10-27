#include "cluster_tools/archive/ArchiveMetrics.h"

namespace cluster_tools { namespace archive {

const char *stallLabel(StallReason reason) noexcept
{
    switch (reason)
    {
        case StallReason::NEVER_CONNECTED: return "never_connected";
        case StallReason::IMAGE_LOST: return "image_lost";
        case StallReason::ARCHIVE_GONE: return "archive_gone";
        case StallReason::STALL: return "stall";
        case StallReason::STALL_AFTER_STOP: return "stall_after_stop";
        case StallReason::BOUNDED_STUCK: return "bounded_stuck";
        case StallReason::EARLY_EOF: return "early_eof";
        case StallReason::ARCHIVE_ERROR: return "archive_error";
        case StallReason::COUNT: break;
    }
    return "unknown";
}

const char *openFailureLabel(OpenFailure failure) noexcept
{
    switch (failure)
    {
        case OpenFailure::STORAGE: return "storage";
        case OpenFailure::ENDPOINT: return "endpoint";
        case OpenFailure::MAX_REPLAYS: return "max_replays";
        case OpenFailure::STORAGE_SPACE: return "storage_space";
        case OpenFailure::INVALID_POSITION: return "invalid_position";
        case OpenFailure::ARCHIVE_ERROR: return "archive_error";
        case OpenFailure::TIMEOUT: return "timeout";
        case OpenFailure::COUNT: break;
    }
    return "unknown";
}

const char *memoReasonLabel(MemoReason reason) noexcept
{
    switch (reason)
    {
        case MemoReason::EPOCH_CHANGED: return "epoch_changed";
        case MemoReason::CURSOR_REGRESSED: return "cursor_regressed";
        case MemoReason::HISTORY_CHANGED: return "history_changed";
        case MemoReason::POSITION_REJECTED: return "position_rejected";
        case MemoReason::SEQ_REGRESSION: return "seq_regression";
        case MemoReason::RESUME_BELT: return "resume_belt";
        case MemoReason::HOLE_VERIFY: return "hole_verify";
        case MemoReason::COUNT: break;
    }
    return "unknown";
}

}}
