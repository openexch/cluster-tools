#include "cluster_tools/archive/Chain.h"

#include "cluster_tools/archive/TestSeams.h"

#include <algorithm>

namespace cluster_tools { namespace archive {

using test_seams::Rule;
using test_seams::disabled;

ChainListing buildChain(std::vector<Recording> listed, std::int32_t journalStreamId)
{
    ChainListing listing;
    listing.recordings.reserve(listed.size());
    for (auto &recording : listed)
    {
        if (recording.key.streamId != journalStreamId && !disabled(Rule::CHAIN_STREAM_FILTER))
        {
            ++listing.droppedForeignStream;
            continue;
        }
        listing.recordings.push_back(recording);
    }
    std::stable_sort(listing.recordings.begin(), listing.recordings.end(),
        [](const Recording &a, const Recording &b) { return a.key.recordingId < b.key.recordingId; });
    return listing;
}

const char *recordingVerdictName(RecordingVerdict verdict) noexcept
{
    switch (verdict)
    {
        case RecordingVerdict::MALFORMED_EXTENT: return "MALFORMED_EXTENT";
        case RecordingVerdict::EMPTY: return "EMPTY";
        case RecordingVerdict::DRAINED: return "DRAINED";
        case RecordingVerdict::MTU_MISMATCH: return "MTU_MISMATCH";
        case RecordingVerdict::FOLLOW_LIVE: return "FOLLOW_LIVE";
        case RecordingVerdict::REPLAY_BOUNDED: return "REPLAY_BOUNDED";
        case RecordingVerdict::RESUME_OUTSIDE: return "RESUME_OUTSIDE";
    }
    return "?";
}

RecordingVerdict classifyRecording(const Recording &r, std::int64_t from, std::int32_t journalMtu) noexcept
{
    const bool mtuMismatch = r.key.mtuLength != journalMtu;
    if (disabled(Rule::CHAIN_MTU_FIRST) && mtuMismatch && !disabled(Rule::CHAIN_MTU_ROW))
    {
        return RecordingVerdict::MTU_MISMATCH;
    }
    // Row 1: a malformed extent is a source fault, never "empty".
    const bool malformed = r.startPosition < 0 || (!r.active() && r.stopPosition < r.startPosition) ||
        r.key.termBufferLength <= 0 || r.key.mtuLength <= 0;
    if (malformed && !disabled(Rule::CHAIN_MALFORMED_ROW))
    {
        return RecordingVerdict::MALFORMED_EXTENT;
    }
    // Row 2: empty stopped recording (09-02 bug C: the archive refuses a [s, s) replay).
    const bool empty = disabled(Rule::CHAIN_EMPTY_WIDENED)
        ? (!r.active() && r.stopPosition >= r.startPosition)
        : (!r.active() && r.stopPosition == r.startPosition);
    if (empty && !disabled(Rule::CHAIN_EMPTY_ROW))
    {
        return RecordingVerdict::EMPTY;
    }
    // Row 3: proven stopped EOF needs no replay (a zero-length bounded replay is refused).
    if (!r.active() && from == r.stopPosition && !disabled(Rule::CHAIN_DRAINED_ROW))
    {
        return RecordingVerdict::DRAINED;
    }
    // Row 4: only a recording that needs a replay is compared to the journal MTU.
    if (mtuMismatch && !disabled(Rule::CHAIN_MTU_ROW))
    {
        return RecordingVerdict::MTU_MISMATCH;
    }
    if (from < r.startPosition || (!r.active() && from > r.stopPosition))
    {
        return RecordingVerdict::RESUME_OUTSIDE;
    }
    // Row 5: active, followed live, never skipped.
    if (r.active())
    {
        return RecordingVerdict::FOLLOW_LIVE;
    }
    // Row 6.
    return RecordingVerdict::REPLAY_BOUNDED;
}

std::uint64_t replayStorageRequirement(std::int32_t termBufferLength) noexcept
{
    const std::uint64_t term = termBufferLength > 0 ? static_cast<std::uint64_t>(termBufferLength) : 0;
    return 3 * term + 16ULL * 1024 * 1024;
}

std::string describe(const RecordingKey &key)
{
    return "recording " + std::to_string(key.recordingId) + " (startTimestamp " + std::to_string(key.startTimestamp) +
        ", initialTermId " + std::to_string(key.initialTermId) + ", sessionId " + std::to_string(key.sessionId) +
        ", termLength " + std::to_string(key.termBufferLength) + ", mtu " + std::to_string(key.mtuLength) +
        ", stream " + std::to_string(key.streamId) + ")";
}

std::string describe(const Recording &recording)
{
    return describe(recording.key) + " [" + std::to_string(recording.startPosition) + ", " +
        (recording.active() ? std::string("active") : std::to_string(recording.stopPosition)) + ")";
}

}}
