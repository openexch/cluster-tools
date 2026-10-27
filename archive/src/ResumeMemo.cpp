#include "cluster_tools/archive/ResumeMemo.h"

#include "cluster_tools/archive/TestSeams.h"

#include <algorithm>
#include <stdexcept>

namespace cluster_tools { namespace archive {

using test_seams::Rule;
using test_seams::disabled;

namespace {

bool reasonDisabled(MemoReason reason) noexcept
{
    switch (reason)
    {
        case MemoReason::EPOCH_CHANGED: return disabled(Rule::MEMO_EPOCH_CHANGED);
        case MemoReason::CURSOR_REGRESSED: return disabled(Rule::MEMO_CURSOR_REGRESSED);
        case MemoReason::HISTORY_CHANGED: return disabled(Rule::MEMO_HISTORY_CHANGED);
        case MemoReason::POSITION_REJECTED: return disabled(Rule::MEMO_POSITION_REJECTED);
        case MemoReason::SEQ_REGRESSION:
        case MemoReason::RESUME_BELT: return disabled(Rule::MEMO_SOURCE_FAULT_INVALIDATES);
        case MemoReason::HOLE_VERIFY: return disabled(Rule::MEMO_HOLE_VERIFY);
        case MemoReason::COUNT: break;
    }
    return false;
}

const Recording *findById(const std::vector<Recording> &chain, std::int64_t recordingId)
{
    for (const auto &r : chain)
    {
        if (r.key.recordingId == recordingId)
        {
            return &r;
        }
    }
    return nullptr;
}

const Recording *findByKey(const std::vector<Recording> &chain, const RecordingKey &key)
{
    for (const auto &r : chain)
    {
        if (r.key == key)
        {
            return &r;
        }
    }
    return nullptr;
}

}

ResumeMemo::ResumeMemo(std::size_t sourceCount, ArchiveMetrics *metrics) :
    m_sources(sourceCount == 0 ? 1 : sourceCount), m_metrics(metrics)
{
    if (sourceCount == 0 || sourceCount > MAX_SOURCES)
    {
        throw std::invalid_argument("ResumeMemo: 1.." + std::to_string(MAX_SOURCES) + " sources");
    }
}

ResumeMemo::SourceMemo &ResumeMemo::slot(std::size_t source)
{
    if (source >= m_sources.size())
    {
        throw std::out_of_range("ResumeMemo: source index " + std::to_string(source));
    }
    // RED shape (F9 / M3): one memo for every source.
    return disabled(Rule::MEMO_SHARED_ACROSS_SOURCES) ? m_sources[0] : m_sources[source];
}

const ResumeMemo::SourceMemo &ResumeMemo::slot(std::size_t source) const
{
    if (source >= m_sources.size())
    {
        throw std::out_of_range("ResumeMemo: source index " + std::to_string(source));
    }
    return disabled(Rule::MEMO_SHARED_ACROSS_SOURCES) ? m_sources[0] : m_sources[source];
}

const std::vector<Recording> &ResumeMemo::chain(std::size_t source) const
{
    return slot(source).chain;
}

const Proof *ResumeMemo::findProof(const SourceMemo &memo, const RecordingKey &key) const
{
    for (const auto &entry : memo.proofs)
    {
        if (entry.first == key)
        {
            return &entry.second;
        }
    }
    return nullptr;
}

void ResumeMemo::dropProof(SourceMemo &memo, const RecordingKey &key)
{
    memo.proofs.erase(std::remove_if(memo.proofs.begin(), memo.proofs.end(),
        [&](const auto &entry) { return entry.first == key; }), memo.proofs.end());
}

void ResumeMemo::recomputeMaxProven()
{
    std::int64_t max = 0;
    for (const auto &memo : m_sources)
    {
        for (const auto &entry : memo.proofs)
        {
            max = std::max(max, entry.second.seq);
        }
    }
    m_maxProven = max;
}

void ResumeMemo::invalidate(std::size_t source, MemoReason reason)
{
    if (reasonDisabled(reason))
    {
        return;
    }
    SourceMemo &memo = slot(source);
    memo.proofs.clear();
    if (nullptr != m_metrics)
    {
        m_metrics->memoInvalidated(reason);
    }
    recomputeMaxProven();
}

void ResumeMemo::invalidateAll(MemoReason reason)
{
    if (reasonDisabled(reason))
    {
        return;
    }
    for (std::size_t i = 0; i < m_sources.size(); ++i)
    {
        m_sources[i].proofs.clear();
        if (nullptr != m_metrics)
        {
            m_metrics->memoInvalidated(reason);
        }
    }
    m_maxProven = 0;
}

void ResumeMemo::onReport(std::int64_t meEpoch, std::int64_t lastAppliedJournalSeq)
{
    // Row 1: an ME re-genesis or another core. Row 2: the core restored or replaced.
    if (0 != m_epoch && meEpoch != m_epoch)
    {
        invalidateAll(MemoReason::EPOCH_CHANGED);
    }
    else if (lastAppliedJournalSeq < m_maxProven)
    {
        invalidateAll(MemoReason::CURSOR_REGRESSED);
    }
    m_epoch = meEpoch;
    m_cursor = lastAppliedJournalSeq;
}

ListingOutcome ResumeMemo::onListing(std::size_t source, const std::vector<Recording> &chain)
{
    ListingOutcome outcome;
    SourceMemo &memo = slot(source);
    if (memo.listed)
    {
        const std::vector<Recording> &prev = memo.chain;
        bool history = false;
        // A recording id seen before: same key, stopped extents fixed, start never moving back.
        for (const auto &now : chain)
        {
            const Recording *old = findById(prev, now.key.recordingId);
            if (nullptr == old)
            {
                continue;
            }
            if (!(old->key == now.key))
            {
                history = true; // id reappears with another incarnation
            }
            else if (!old->active() && (now.active() || now.stopPosition != old->stopPosition))
            {
                history = true; // stopped recording active again, or its stop moved
            }
            else if (now.startPosition < old->startPosition)
            {
                history = true; // start moved backwards
            }
            if (const Proof *p = findProof(memo, now.key); nullptr != p && !now.active() &&
                p->endPosition > now.stopPosition)
            {
                history = true; // stopped before a remembered frontier
            }
        }
        if (chain.empty() && !prev.empty())
        {
            history = true; // an archive wiped to an empty chain: retention never removes the active recording (review N-5)
        }
        // Keys gone: a leading run is retention (row 4), anything after it is a history change.
        std::size_t head = 0;
        while (head < prev.size() && nullptr == findByKey(chain, prev[head].key))
        {
            ++head;
        }
        for (std::size_t i = head; i < prev.size(); ++i)
        {
            if (nullptr == findByKey(chain, prev[i].key))
            {
                history = true; // gone from the middle (or the tail) of the chain
            }
        }
        if (!history)
        {
            for (std::size_t i = 0; i < head; ++i)
            {
                if (const Proof *p = findProof(memo, prev[i].key); nullptr != p && p->seq > m_cursor)
                {
                    history = true; // a retained recording held a proof the core does not cover
                }
            }
        }
        if (history && !disabled(Rule::MEMO_HISTORY_CHANGED))
        {
            outcome.historyChanged = true;
            invalidate(source, MemoReason::HISTORY_CHANGED);
        }
        else
        {
            // Row 4: recordings gone from the head whose every proof is <= C: drop their proofs only.
            for (std::size_t i = 0; i < head; ++i)
            {
                if (nullptr != findProof(memo, prev[i].key))
                {
                    if (disabled(Rule::MEMO_HEAD_TRIM))
                    {
                        memo.proofs.clear(); // RED shape: retention costs the whole memo (the Java behaviour)
                        break;
                    }
                    dropProof(memo, prev[i].key);
                    ++outcome.trimmedProofs;
                }
            }
            // Row 5: a start that grew (the active recording's purge): drop proofs below the new start.
            for (const auto &now : chain)
            {
                const Recording *old = findById(prev, now.key.recordingId);
                if (nullptr == old || !(old->key == now.key) || now.startPosition <= old->startPosition)
                {
                    continue;
                }
                const Proof *p = findProof(memo, now.key);
                if (disabled(Rule::MEMO_KEY_WITH_START) && nullptr != p)
                {
                    dropProof(memo, now.key); // RED shape: a key that holds startPosition loses the memo on purge
                    continue;
                }
                if (nullptr != p && p->endPosition < now.startPosition && !disabled(Rule::MEMO_ACTIVE_PURGE_TRIM))
                {
                    dropProof(memo, now.key);
                    ++outcome.trimmedProofs;
                }
            }
            if (outcome.trimmedProofs > 0 && nullptr != m_metrics)
            {
                ArchiveMetrics::add(m_metrics->memoTrims, outcome.trimmedProofs);
            }
            recomputeMaxProven();
        }
    }
    // Proofs on keys the listing no longer holds are meaningless whatever the reason.
    memo.proofs.erase(std::remove_if(memo.proofs.begin(), memo.proofs.end(),
        [&](const auto &entry) { return nullptr == findByKey(chain, entry.first); }), memo.proofs.end());
    memo.chain = chain;
    memo.listed = true;
    return outcome;
}

ResumePoint ResumeMemo::resume(std::size_t source)
{
    SourceMemo &memo = slot(source);
    const std::vector<Recording> &chain = memo.chain;
    ResumePoint point;
    point.position = chain.empty() ? 0 : chain.front().startPosition;
    // A pending hole verification forces a memo-free read (5.2 rule 5).
    const bool forcedCold = !memo.holeMarkers.empty() && !disabled(Rule::MEMO_HOLE_VERIFY);
    std::size_t k = chain.size();
    const Proof *proof = nullptr;
    if (!forcedCold)
    {
        for (std::size_t i = chain.size(); i-- > 0;)
        {
            if (const Proof *p = findProof(memo, chain[i].key); nullptr != p)
            {
                k = i;
                proof = p;
                break;
            }
        }
    }
    if (nullptr == proof)
    {
        if (nullptr != m_metrics)
        {
            m_metrics->memoResumed(ResumeKind::COLD);
        }
        return point; // chain index 0 at its start: the walk skips every seq <= C
    }
    const Recording &r = chain[k];
    point.memoFree = false;
    if (!r.active() && proof->endPosition == r.stopPosition)
    {
        point.index = k + 1;
        point.position = point.index < chain.size() ? chain[point.index].startPosition : 0;
        point.fromProof = false;
    }
    else
    {
        point.index = k;
        point.position = proof->endPosition;
        point.fromProof = true;
        point.proofSeq = proof->seq;
    }
    if (nullptr != m_metrics)
    {
        m_metrics->memoResumed(ResumeKind::PROOF);
    }
    return point;
}

bool ResumeMemo::record(std::size_t source, const RecordingKey &key, const Proof &proof)
{
    SourceMemo &memo = slot(source);
    const Recording *r = findByKey(memo.chain, key);
    if (nullptr == r)
    {
        return false; // not in the listing this read was made against
    }
    if (proof.endPosition % FRAME_ALIGNMENT != 0 || proof.endPosition < r->startPosition ||
        (!r->active() && proof.endPosition > r->stopPosition))
    {
        return false; // not a frame end position inside [start, stop]
    }
    for (auto &entry : memo.proofs)
    {
        if (!(entry.first == key))
        {
            continue;
        }
        Proof &old = entry.second;
        if (proof.seq >= old.seq && proof.endPosition >= old.endPosition)
        {
            old = proof; // monotone in s and p
        }
        else if (proof.seq <= old.seq && proof.endPosition <= old.endPosition)
        {
            return false; // an older proof: max keeps the newer one
        }
        else
        {
            dropProof(memo, key); // s and p disagree: the recording's history is inconsistent
            recomputeMaxProven();
            return false;
        }
        m_maxProven = std::max(m_maxProven, proof.seq);
        return true;
    }
    memo.proofs.emplace_back(key, proof);
    m_maxProven = std::max(m_maxProven, proof.seq);
    return true;
}

bool ResumeMemo::proveFromSkip(std::size_t source, const RecordingKey &key, const Peek &peek, std::int64_t endPosition)
{
    if (disabled(Rule::MEMO_PROVE_FROM_SKIP))
    {
        return false;
    }
    if (0 == m_epoch || peek.meEpoch != m_epoch || peek.journalSeq > m_cursor)
    {
        return false;
    }
    return record(source, key, Proof{ peek.journalSeq, peek.meEpoch, endPosition });
}

bool ResumeMemo::prove(std::size_t source, const RecordingKey &key, std::int64_t seq, std::int64_t meEpoch,
    std::int64_t endPosition)
{
    if (0 == m_epoch || meEpoch != m_epoch || seq > m_cursor || seq < 1)
    {
        return false;
    }
    return record(source, key, Proof{ seq, meEpoch, endPosition });
}

void ResumeMemo::beginSync()
{
    for (auto &memo : m_sources)
    {
        memo.holeMarkers.clear();
        memo.verifiedHoles.clear();
    }
}

HoleDecision ResumeMemo::onHole(std::size_t source, std::int64_t expectedSeq, bool readWasMemoFree)
{
    if (disabled(Rule::MEMO_HOLE_VERIFY))
    {
        return HoleDecision::BELIEVE;
    }
    SourceMemo &memo = slot(source);
    const auto contains = [](const std::vector<std::int64_t> &v, std::int64_t e) {
        return std::find(v.begin(), v.end(), e) != v.end();
    };
    if (readWasMemoFree)
    {
        // Amended 5.2 rule 5 (review F-3): a memo-free read that sees the hole verifies it. The marker
        // turns "verified": it no longer forces memo-free reads, and a later memo-read hole at the same
        // (source, e) in this sync is believed without a second invalidation.
        if (!disabled(Rule::MEMO_VERIFIED_MARKER))
        {
            memo.holeMarkers.erase(std::remove(memo.holeMarkers.begin(), memo.holeMarkers.end(), expectedSeq),
                memo.holeMarkers.end());
            if (!contains(memo.verifiedHoles, expectedSeq))
            {
                memo.verifiedHoles.push_back(expectedSeq);
            }
        }
        return HoleDecision::BELIEVE;
    }
    if (contains(memo.verifiedHoles, expectedSeq))
    {
        return HoleDecision::BELIEVE; // verified by a memo-free read earlier in this sync
    }
    if (!contains(memo.holeMarkers, expectedSeq))
    {
        memo.holeMarkers.push_back(expectedSeq);
        invalidate(source, MemoReason::HOLE_VERIFY); // once per (source, e) per sync
    }
    return HoleDecision::VERIFY_FROM_CHAIN_START;
}

void ResumeMemo::onFrameRead(std::size_t source, std::int64_t seq, bool readWasMemoFree)
{
    if (!readWasMemoFree)
    {
        return;
    }
    SourceMemo &memo = slot(source);
    memo.holeMarkers.erase(std::remove(memo.holeMarkers.begin(), memo.holeMarkers.end(), seq), memo.holeMarkers.end());
    memo.verifiedHoles.erase(std::remove(memo.verifiedHoles.begin(), memo.verifiedHoles.end(), seq), memo.verifiedHoles.end());
}

bool ResumeMemo::holeVerificationPending(std::size_t source) const
{
    return !slot(source).holeMarkers.empty();
}

bool ResumeMemo::holeVerified(std::size_t source, std::int64_t expectedSeq) const
{
    const auto &v = slot(source).verifiedHoles;
    return std::find(v.begin(), v.end(), expectedSeq) != v.end();
}

std::optional<Proof> ResumeMemo::proof(std::size_t source, const RecordingKey &key) const
{
    const Proof *p = findProof(slot(source), key);
    if (nullptr == p)
    {
        return std::nullopt;
    }
    return *p;
}

std::size_t ResumeMemo::proofCount(std::size_t source) const
{
    return slot(source).proofs.size();
}

#ifdef CT_TEST_SEAMS
void ResumeMemo::injectProofForTest(std::size_t source, const RecordingKey &key, const Proof &proof)
{
    SourceMemo &memo = slot(source);
    dropProof(memo, key);
    memo.proofs.emplace_back(key, proof);
    m_maxProven = std::max(m_maxProven, proof.seq);
}
#endif

}}
