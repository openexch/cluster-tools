#ifndef CLUSTER_TOOLS_ARCHIVE_RESUME_MEMO_H
#define CLUSTER_TOOLS_ARCHIVE_RESUME_MEMO_H

// The per-source resume memo keyed on journalSeq. RAM only, never a
// checkpoint: the owner's cursor (the bridge: the core's lastAppliedJournalSeq) is the only cursor. An owner
// that stores its own memo rows next to its cursor may hand them back at start (seed): a seeded proof is
// validated against the first listing and then treated exactly like one the library made itself, the resume
// belt included, so the library stays the only place that decides whether a proof is used. The memo decides only WHERE
// reading starts; what is offered is decided by the forward rule, so a wrong proof can
// only turn into SKIPs or a visible hole, and a hole is believed only from a memo-free read
// (5.2 rule 5), so a wrong proof can never become a FeedHalt GAP.
//
// One memo per SourceIdentity (index into the configured source list), so failover never forces a
// rescan on fail-back. A byte position is never moved to another source.

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/archive/Chain.h"
#include "cluster_tools/archive/FramePeek.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace cluster_tools { namespace archive {

// proof[key] = (s, e, p): the owner has applied every seq up to s of epoch e, and every record before the
// frame end position p on recording `key` has seq <= s (seqs strictly ascend inside a recording). The key
// holds the archive id, so a proof never matches a recording of another archive incarnation by id alone.
struct Proof
{
    std::int64_t seq = 0;
    std::int64_t epoch = 0;
    std::int64_t endPosition = 0;
    bool seeded = false; // handed over by the owner and not yet advanced by a read of this process

    bool operator==(const Proof &) const = default;
};

// The reader checkpoint value an owner reports for retention: the logical sequence is the owner's cursor; the
// recording and the position are diagnosis only and always the attached source's own (its best proof), never
// carried over from another source. NULL_POSITION / -1 when the source holds no proof yet.
struct ReaderCheckpoint
{
    std::int64_t sourceEpoch = 0;
    std::int64_t archiveId = -1;
    std::int64_t recordingId = -1;
    std::int64_t position = NULL_POSITION;
    std::int64_t logicalSeq = 0;

    bool operator==(const ReaderCheckpoint &) const = default;
};

// Where an attach starts reading (5.2 rule 3).
struct ResumePoint
{
    std::size_t index = 0;     // chain index read first; == chain size: nothing left to read
    std::int64_t position = 0; // replay start on that recording
    bool fromProof = false;    // started at (K, proof[K].p): the resume belt applies (4.5 row 4)
    std::int64_t proofSeq = 0; // proof[K].seq when fromProof; the first accepted frame must be proofSeq + 1
    bool memoFree = true;      // chain index 0 at its startPosition: holes seen on this read are believed
    bool seeded = false;       // the proof came from the owner's seed (the belt applies like to any proof)
};

struct ListingOutcome
{
    bool historyChanged = false;
    std::uint32_t trimmedProofs = 0;
};

enum class HoleDecision : std::uint8_t
{
    BELIEVE,                // the read was memo-free: the hole is real on this source
    VERIFY_FROM_CHAIN_START // the memo was invalidated (once per (source, e) per sync); re-read from chain start
};

class ResumeMemo
{
public:
    explicit ResumeMemo(std::size_t sourceCount, ArchiveMetrics *metrics = nullptr);

    std::size_t sourceCount() const noexcept { return m_sources.size(); }

    // 5.4 step 2: every FeedPositionReport (5.3 rows 1-2), or the owner's cursor. E = 0 is unbound.
    void onReport(std::int64_t epoch, std::int64_t lastAppliedJournalSeq);
    std::int64_t epoch() const noexcept { return m_epoch; }
    std::int64_t cursor() const noexcept { return m_cursor; }
    std::int64_t maxProven() const noexcept { return m_maxProven; }

    // 5.3 rows 3-5, evaluated on every listing of `source` (incl. the 4.3 step 6 re-list).
    ListingOutcome onListing(std::size_t source, const std::vector<Recording> &chain);
    const std::vector<Recording> &chain(std::size_t source) const;

    // 5.2 rule 3 against the last listing. Counts bridge_memo_resumes_total{kind}.
    ResumePoint resume(std::size_t source);

    // 5.2 rule 1: an accepted frame with epoch == E != 0 and journalSeq <= C read on `key` of
    // `source` at frame end position `endPosition`. Returns true when a proof was recorded.
    bool proveFromSkip(std::size_t source, const RecordingKey &key, const Peek &peek, std::int64_t endPosition);

    // 5.2 rule 2: a ring proof after a report raised the cursor; applied to `source` only.
    bool prove(std::size_t source, const RecordingKey &key, std::int64_t seq, std::int64_t epoch,
        std::int64_t endPosition);

    // The memo seed: the owner's stored proof (recording key with its archive id, end position, sequence) for
    // `source`, handed over at start after onReport. Refused (false) without a report, for another epoch, for
    // seq < 1 or above the cursor, or for a position that is not a frame end. An accepted seed waits for the
    // source's next listing: a key the listing does not hold (a recording that is gone, another archive id) or
    // a position outside the recording's extent drops it; otherwise it becomes a proof marked seeded. The
    // bridge never calls this: its memo stays RAM only.
    bool seed(std::size_t source, const RecordingKey &key, std::int64_t seq, std::int64_t epoch, std::int64_t endPosition);
    std::size_t pendingSeeds(std::size_t source) const;

    // The reader checkpoint for `source` at the owner's logical sequence (see ReaderCheckpoint).
    ReaderCheckpoint readerCheckpoint(std::size_t source, std::int64_t logicalSeq) const;

    // 5.3 rows 6-7 (and rows 1-3 internally): drop every proof of `source`.
    void invalidate(std::size_t source, MemoReason reason);

    // 5.2 rule 5. beginSync() starts a sync (a FeedPositionReport after a fence): markers clear.
    void beginSync();
    HoleDecision onHole(std::size_t source, std::int64_t expectedSeq, bool readWasMemoFree);
    // An accepted frame of a read: clears the (source, seq) marker when the read is memo-free.
    void onFrameRead(std::size_t source, std::int64_t seq, bool readWasMemoFree);
    bool holeVerificationPending(std::size_t source) const;
    bool holeVerified(std::size_t source, std::int64_t expectedSeq) const;

    std::optional<Proof> proof(std::size_t source, const RecordingKey &key) const;
    std::size_t proofCount(std::size_t source) const;

#ifdef CT_TEST_SEAMS
    // Test only: a raw proof that bypasses the validation of prove(), to reach the 4.3 step 3
    // alignment check (a proof from "an older implementation or unseen history change").
    void injectProofForTest(std::size_t source, const RecordingKey &key, const Proof &proof);
#endif

private:
    struct SourceMemo
    {
        std::vector<Recording> chain;
        bool listed = false;
        std::vector<std::pair<RecordingKey, Proof>> proofs;
        std::vector<std::pair<RecordingKey, Proof>> seeds;  // accepted seeds waiting for the next listing
        std::vector<std::int64_t> holeMarkers;   // e values whose verification is pending this sync (forces memo-free reads)
        std::vector<std::int64_t> verifiedHoles; // e values a memo-free read believed this sync (amended 5.2 rule 5)
    };

    SourceMemo &slot(std::size_t source);
    const SourceMemo &slot(std::size_t source) const;
    bool record(std::size_t source, const RecordingKey &key, const Proof &proof);
    void adoptSeeds(SourceMemo &memo);
    void dropProof(SourceMemo &memo, const RecordingKey &key);
    const Proof *findProof(const SourceMemo &memo, const RecordingKey &key) const;
    void recomputeMaxProven();
    void invalidateAll(MemoReason reason);

    std::vector<SourceMemo> m_sources;
    ArchiveMetrics *m_metrics;
    std::int64_t m_epoch = 0;
    std::int64_t m_cursor = 0;
    std::int64_t m_maxProven = 0;
};

}}

#endif
