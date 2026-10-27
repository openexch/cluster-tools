#ifndef CLUSTER_TOOLS_ARCHIVE_FOLLOW_H
#define CLUSTER_TOOLS_ARCHIVE_FOLLOW_H

// The interfaces S9a freezes for its owner (the S9b domain, ct-journal-tail): the frame sink, the
// fault report and the archive port results the follow policy consumes (RC2 S9a contract 3.3, 4.3,
// 4.4, 4.5). Pure.

#include "cluster_tools/archive/Chain.h"
#include "cluster_tools/archive/FramePeek.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cluster_tools { namespace archive {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// What the owner's sink answers for an admitted frame (4.4). ABORT never drops a frame: it is
// redelivered on the next poll, and nothing (previous seq, belt, proofs, consumed) advances.
enum class SinkAction : std::uint8_t
{
    CONTINUE, // accepted, the poll goes on
    BREAK,    // accepted, the poll stops after it
    ABORT     // not accepted, redelivered
};

// What the owner answers for a refused frame. The bridge closes the replay (SCHEMA_REFUSED, D-1
// verdict by the owner); ct-journal-tail prints the frame and continues with the next one.
enum class RefusalAction : std::uint8_t
{
    CLOSE_REPLAY,
    SKIP_AND_CONTINUE
};

struct JournalFrame
{
    std::size_t sourceIndex = 0;
    RecordingKey key;
    std::int64_t endPosition = 0;    // header position after the frame
    const std::uint8_t *bytes = nullptr; // valid during the call only
    std::size_t length = 0;
    Peek peek;
    bool readWasMemoFree = true;     // this attach started at chain index 0 at its start (5.2 rule 5)
};

struct RefusedFrame
{
    std::size_t sourceIndex = 0;
    RecordingKey key;
    Refusal reason = Refusal::NONE;
    std::int64_t endPosition = 0;    // header position after the refused frame
    const std::uint8_t *bytes = nullptr; // valid during the call only
    std::size_t length = 0;
    bool hasPeek = false;            // rows 1-6 passed (a HEAD refusal)
    Peek peek;
};

// The owner's frame sink. noexcept: an exception must never unwind through
// aeron_subscription_controlled_poll (3.3); a throw ends the process through std::terminate,
// which the processes route to exit 70 (3.1).
class FrameSink
{
public:
    virtual ~FrameSink() = default;
    virtual SinkAction onFrame(const JournalFrame &frame) noexcept = 0;
    virtual RefusalAction onRefused(const RefusedFrame &frame) noexcept = 0;
};

// Source faults and events (4.5 verdicts, section 7 class S rows).
enum class FaultKind : std::uint8_t
{
    NONE,
    FATAL,                       // row 1: the conductor is exiting the process
    CONNECT_FAILED,              // S1
    LIST_FAILED,                 // S1 (connect level)
    MALFORMED_EXTENT,            // S8
    REPLAY_STORAGE,              // S2, no request sent
    REPLAY_ENDPOINT_UNRESOLVED,  // S2
    REPLAY_OPEN,                 // S2 (reason in openFailure)
    POSITION_REJECTED,           // S3
    SCHEMA_REFUSED,              // row 2, B (D-1)
    SEQ_REGRESSION_IN_RECORDING, // row 3, S6
    RESUME_BELT,                 // row 4, S6
    EARLY_EOF,                   // rows 6, 7a
    IMAGE_LOST,                  // rows 7, 7a
    NEVER_CONNECTED,             // row 8
    ARCHIVE_GONE,                // rows 7a, 9
    STALL,                       // row 12
    STALL_AFTER_STOP,            // row 11
    STALL_BOUNDED_STUCK,         // row 12b
    ARCHIVE_ERROR                // row 13
};
const char *faultName(FaultKind kind) noexcept;

// Archive error classes, mapped by the adapter from the client's error codes.
enum class ArchiveErrorKind : std::uint8_t
{
    NONE,
    TIMEOUT,          // the message timeout (ETIMEDOUT)
    NOT_CONNECTED,    // the control session is gone
    INVALID_POSITION,
    UNKNOWN_RECORDING,
    MAX_REPLAYS,
    STORAGE_SPACE,
    GENERIC,          // any other archive or client error
    FATAL             // the fatal monitor fired; Aeron must not be touched again
};
const char *archiveErrorName(ArchiveErrorKind kind) noexcept;

struct ArchiveError
{
    ArchiveErrorKind kind = ArchiveErrorKind::NONE;
    std::string message;
};

struct SourceFault
{
    std::size_t source = 0;
    FaultKind kind = FaultKind::NONE;
    RecordingKey key;           // the recording being read, when there is one
    std::int64_t consumed = 0;  // end position of the last accepted frame
    Refusal refusal = Refusal::NONE;
    std::uint8_t openFailure = 0; // OpenFailure for REPLAY_OPEN / REPLAY_STORAGE / ENDPOINT
    std::string detail;
};

struct AttachInfo
{
    std::size_t source = 0;
    Recording recording;
    std::size_t chainIndex = 0;
    std::int64_t from = 0;
    bool fromProof = false;
    bool memoFree = true;
};

// Owner notifications (noexcept; the observer must not call back into the follower).
class FollowObserver
{
public:
    virtual ~FollowObserver() = default;
    virtual void onFault(const SourceFault &) noexcept {}
    virtual void onAttached(const AttachInfo &) noexcept {}
    virtual void onRecordingEnded(std::size_t /*source*/, const Recording &) noexcept {}
    virtual void onChainEmpty(std::size_t /*source*/) noexcept {}
    virtual void onChainExhausted(std::size_t /*source*/) noexcept {}
    virtual void onSourceRefused(std::size_t /*source*/, const Recording & /*recording*/) noexcept {}
};

// ---- archive port results (the adapter's answers to the policy's commands)

struct ConnectPoll
{
    enum class State : std::uint8_t { PENDING, CONNECTED, FAILED } state = State::PENDING;
    ArchiveError error;
};

struct ListResult
{
    bool ok = false;
    std::vector<Recording> recordings;
    ArchiveError error;
};

struct SubscriptionResult
{
    enum class State : std::uint8_t { ADDED, STORAGE_REFUSED, FAILED } state = State::ADDED;
    std::uint64_t freeBytes = 0;
    std::uint64_t requiredBytes = 0;
    ArchiveError error;
};

struct EndpointPoll
{
    enum class State : std::uint8_t { PENDING, RESOLVED, FAILED } state = State::PENDING;
    std::string endpoint;
    ArchiveError error;
};

struct StartResult
{
    bool ok = false;
    std::int64_t replaySessionId = 0;
    ArchiveError error;
};

struct ImageObservation
{
    int imageCount = 0;
    bool endOfStream = false;   // of the image, when present
    bool imageClosed = false;
    bool subscriptionClosed = false;
    std::int64_t position = 0; // the image position, when present (padding included)
};

struct AsyncErrorPoll
{
    enum class State : std::uint8_t { NONE, ERROR, DISCONNECTED } state = State::NONE;
    std::string message;
};

struct ProbeResult
{
    enum class State : std::uint8_t { ACTIVE, STOPPED, FAILED } state = State::FAILED;
    std::int64_t position = 0; // recorded position (ACTIVE) or stop position (STOPPED)
    ArchiveError error;
};

}}

#endif
