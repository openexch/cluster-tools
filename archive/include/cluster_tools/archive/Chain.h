#ifndef CLUSTER_TOOLS_ARCHIVE_CHAIN_H
#define CLUSTER_TOOLS_ARCHIVE_CHAIN_H

// The recording chain model (RC2 S9a contract 4.2): source identity, recording key, the chain
// listing and the recording validity table. Pure.

#include <cstdint>
#include <string>
#include <vector>

namespace cluster_tools { namespace archive {

// Aeron's NULL_POSITION: the stop position of an active recording.
inline constexpr std::int64_t NULL_POSITION = -1;
inline constexpr std::int64_t FRAME_ALIGNMENT = 32;

// (endpoint, controlStreamId, journalStreamId, channel), ArchiveJournalSource.java:31-32.
struct SourceIdentity
{
    std::string endpoint; // host:port of the archive control channel
    std::int32_t controlStreamId = 0;
    std::int32_t journalStreamId = 0;
    std::string channel;  // the listRecordingsForUri channel fragment

    bool operator==(const SourceIdentity &) const = default;
};

// CHANGE against Java: startPosition is NOT part of the key. Retention purges the active
// recording's head, and the Java key turned every purge into a memo loss; the start moving
// BACKWARDS is a history change (ResumeMemo, 5.3 row 3).
struct RecordingKey
{
    std::int64_t recordingId = -1;
    std::int64_t startTimestamp = 0;
    std::int32_t initialTermId = 0;
    std::int32_t sessionId = 0;
    std::int32_t termBufferLength = 0;
    std::int32_t mtuLength = 0;
    std::int32_t streamId = 0;

    bool operator==(const RecordingKey &) const = default;
};

struct Recording
{
    RecordingKey key;
    std::int64_t startPosition = 0;
    std::int64_t stopPosition = NULL_POSITION;

    bool active() const noexcept { return NULL_POSITION == stopPosition; }
    bool operator==(const Recording &) const = default;
};

struct ChainListing
{
    std::vector<Recording> recordings; // recordingId order
    std::uint32_t droppedForeignStream = 0;
};

// listRecordingsForUri(0, INT32_MAX, channel, journalStream) result -> chain: recordingId order,
// a descriptor whose streamId != journalStream dropped and counted (the URI match is a substring
// match, the stream filter is what makes the chain exact).
ChainListing buildChain(std::vector<Recording> listed, std::int32_t journalStreamId);

// The 4.2 validity table, first match. EMPTY and DRAINED come before the MTU row so a recording
// that needs no replay can never refuse a source (review N4).
enum class RecordingVerdict : std::uint8_t
{
    MALFORMED_EXTENT, // 1: source fault, never skipped
    EMPTY,            // 2: stopped, stop == start: skipped without a replay
    DRAINED,          // 3: stopped, resume position == stop: no replay opened
    MTU_MISMATCH,     // 4: the source is refused until its chain changes
    FOLLOW_LIVE,      // 5: active, followed live (length INT64_MAX)
    REPLAY_BOUNDED,   // 6: replayed [from, stop)
    RESUME_OUTSIDE    // a resume position outside [start, stop]: POSITION_REJECTED (never a skip)
};

const char *recordingVerdictName(RecordingVerdict verdict) noexcept;

RecordingVerdict classifyRecording(const Recording &recording, std::int64_t from, std::int32_t journalMtu) noexcept;

// Bytes the replay image of a recording needs in the driver directory (4.3 step 2): three terms
// plus 16 MiB of headroom.
std::uint64_t replayStorageRequirement(std::int32_t termBufferLength) noexcept;

std::string describe(const RecordingKey &key);
std::string describe(const Recording &recording);

}}

#endif
