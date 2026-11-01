#include "cluster_tools/domain/bridge/BridgeDomain.h"

#include "cluster_tools/archive/FramePeek.h"
#include "cluster_tools/domain/bridge/TestSeams.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <string>

namespace cluster_tools { namespace domain { namespace bridge {

using archive::FaultKind;
using archive::HoleDecision;
using archive::RefusalAction;
using archive::SinkAction;
using test_seams::Rule;
using test_seams::disabled;

namespace {

std::int64_t randomBase()
{
    std::random_device rd;
    const std::uint64_t hi = rd();
    const std::uint64_t lo = rd();
    // positive, with room below INT64_MAX for a process's lifetime of correlation ids
    return static_cast<std::int64_t>(((hi << 32) ^ lo) & 0x3fff'ffff'ffff'0000ULL);
}

BadReason badFor(archive::Refusal r) noexcept
{
    switch (r)
    {
        case archive::Refusal::FRAGMENTED: return BadReason::REFUSAL_FRAGMENTED;
        case archive::Refusal::LENGTH: return BadReason::REFUSAL_LENGTH;
        case archive::Refusal::SCHEMA: return BadReason::REFUSAL_SCHEMA;
        case archive::Refusal::TEMPLATE: return BadReason::REFUSAL_TEMPLATE;
        case archive::Refusal::BLOCK: return BadReason::REFUSAL_BLOCK;
        case archive::Refusal::FRAME_LENGTH: return BadReason::REFUSAL_FRAME_LENGTH;
        case archive::Refusal::HEAD: return BadReason::REFUSAL_HEAD;
        case archive::Refusal::NONE:
        case archive::Refusal::COUNT: break;
    }
    return BadReason::REFUSAL_SCHEMA;
}

constexpr std::uint32_t ROW_BITS = bit(Degraded::NO_REACHABLE_SOURCE) | bit(Degraded::HOLE_WITH_LAGGING_SOURCE);

std::size_t settleLabel(std::uint8_t raw) noexcept
{
    return raw <= 8 ? raw : SETTLE_REASON_LABELS - 1;
}

}

// ------------------------------------------------------------------ construction

BridgeDomain::BridgeDomain(const BridgeSettings &settings, archive::ResumeMemo &memo,
    const archive::ArchiveMetrics &archive, FrameEncoder &encoder, CorePort &core, BridgeMetrics &metrics) :
    m_settings(settings), m_memo(memo), m_archive(archive), m_encoder(encoder), m_core(core), m_metrics(metrics),
    m_output(*this), m_stager(encoder), m_ring(settings.ringCapacity),
    m_keys(settings.sourceCount, settings.keyTableCapacity), m_verdicts(settings.sourceCount)
{
    if (settings.sourceCount == 0 || settings.sourceCount > archive::MAX_SOURCES)
    {
        throw std::invalid_argument("BridgeDomain: sourceCount must be 1.." + std::to_string(archive::MAX_SOURCES));
    }
    if (memo.sourceCount() != settings.sourceCount)
    {
        throw std::invalid_argument("BridgeDomain: the memo has another source count");
    }
    if (settings.fragmentLimit <= 0 || settings.transientStrikes < 1 || settings.stormFences < 1)
    {
        throw std::invalid_argument("BridgeDomain: fragmentLimit, transientStrikes and stormFences must be >= 1");
    }
    m_correlation = 0 != settings.correlationBase ? settings.correlationBase : randomBase();
    m_lastForwardSource = NO_SOURCE;
    publish();
}

// ------------------------------------------------------------------ helpers

bool BridgeDomain::feeding() const noexcept
{
    if (m_awaitSync || Fatal::NONE != m_fatal)
    {
        return false;
    }
    return State::STREAMING == m_state || (State::HALTED == m_state && m_rescan);
}

int BridgeDomain::fragmentLimit() const noexcept
{
    int limit = 0;
    if (Fatal::NONE == m_fatal && !m_closed && feeding() && SourceCommandKind::NONE == m_command.kind &&
        !m_switchInFlight && !m_ring.full() && !m_keysFull && (!m_confirming || disabled(Rule::CONFIRM_REQUERY)))
    {
        // review BLOCKER-1: a confirming bridge pauses like one waiting for any outstanding answer, so the aborted
        // frame is not redelivered (and the decision not re-run) before the confirming report arrives
        limit = m_settings.fragmentLimit;
    }
    m_lastLimit = limit;
    return limit;
}

SinkAction BridgeDomain::defer(DeferReason why) noexcept
{
    BridgeMetrics::add(m_metrics.framesDeferred[static_cast<std::size_t>(why)]);
    return SinkAction::ABORT;
}

bool BridgeDomain::handleStep(StepResult r) noexcept
{
    switch (r)
    {
        case StepResult::OK:
            return true;
        case StepResult::RETRY:
        case StepResult::NO_ROOM:
            BridgeMetrics::add(m_metrics.backpressureAborts);
            return false;
        case StepResult::CLOSED:
            BridgeMetrics::add(m_metrics.backpressureAborts);
            m_closed = true; // no feed reads until S9c reports onSessionDown / onSessionUp
            return false;
        case StepResult::REFUSED:
            m_fatal = Fatal::OFFER_REFUSED;
            return false;
        case StepResult::ENCODER_FAILED:
            m_fatal = Fatal::ENCODER_OVERFLOW;
            return false;
    }
    return false;
}

StepResult BridgeDomain::flushStaging() noexcept
{
    return m_stager.flush(m_output);
}

void BridgeDomain::dropStaging() noexcept
{
    const std::size_t n = m_stager.drop();
    if (n > 0)
    {
        BridgeMetrics::add(m_metrics.stagedDropped, n);
    }
}

StepResult BridgeDomain::Output::emit(const std::uint8_t *frame, std::size_t length, std::uint16_t moneyTemplate,
    const OfferedRecord *records, std::size_t n) noexcept
{
    BridgeDomain &d = m_d;
    if (d.m_ring.freeSlots() < n && !disabled(Rule::RING_UNBOUNDED) && !disabled(Rule::RING_FULL_AT_ZERO))
    {
        return StepResult::NO_ROOM;
    }
    switch (d.m_core.tryOffer(frame, length))
    {
        case OfferOutcome::OK:
            break;
        case OfferOutcome::RETRY:
            return StepResult::RETRY;
        case OfferOutcome::CLOSED:
            return StepResult::CLOSED;
        case OfferOutcome::REFUSED:
            return StepResult::REFUSED;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        RingEntry e;
        e.seq = records[i].seq;
        e.endPosition = records[i].endPosition;
        e.sourceIndex = records[i].source;
        e.moneyTemplate = moneyTemplate;
        e.recordingKeyIndex = records[i].keyIndex;
        d.m_ring.push(e);
        d.m_offeredTail = std::max(d.m_offeredTail, records[i].seq);
        if (d.m_storm.active && records[i].seq == d.m_storm.cursor + 1 && !disabled(Rule::STORM_TRIED_AT_SWITCH))
        {
            d.m_storm.tried |= 1u << records[i].source; // the source offered C' + 1: tried (9.6, 2026-10-31)
        }
    }
    const FrameKind kind = frameKindOf(moneyTemplate);
    if (FrameKind::COUNT != kind)
    {
        BridgeMetrics::add(d.m_metrics.forwarded[static_cast<std::size_t>(kind)]);
        BridgeMetrics::add(d.m_metrics.forwardedRecords[static_cast<std::size_t>(kind)], n);
    }
    BridgeMetrics::set(d.m_metrics.lastOfferedSeq, d.m_offeredTail);
    d.m_lastForwardAt = d.m_now;
    if (d.m_ring.full())
    {
        BridgeMetrics::add(d.m_metrics.ringFullPauses);
        if (!d.m_query.wanted)
        {
            d.wantQueryNow(); // 6.2: an immediate query, so the report that pops comes at once
        }
    }
    return StepResult::OK;
}

void BridgeDomain::logVerdict(std::size_t source, const Verdict &v) noexcept
{
    VerdictEvent ev;
    ev.source = source;
    ev.e = m_verdicts.e();
    ev.kind = v.kind;
    ev.bad = v.bad;
    ev.seq = v.seq;
    ev.epoch = v.epoch;
    ev.fault = v.fault;
    if (m_logSize == VERDICT_LOG)
    {
        m_logHead = (m_logHead + 1) % VERDICT_LOG; // the oldest event is overwritten (counted)
        --m_logSize;
        BridgeMetrics::add(m_metrics.verdictLogDropped);
    }
    m_log[(m_logHead + m_logSize) % VERDICT_LOG] = ev;
    ++m_logSize;
}

std::size_t BridgeDomain::drainVerdictEvents(VerdictEvent *out, std::size_t max) noexcept
{
    std::size_t n = 0;
    while (n < max && m_logSize > 0)
    {
        out[n++] = m_log[m_logHead];
        m_logHead = (m_logHead + 1) % VERDICT_LOG;
        --m_logSize;
    }
    return n;
}

void BridgeDomain::publish() noexcept
{
    const bool halted = State::HALTED == m_state || State::HALTING == m_state;
    (void)halted;
    BridgeMetrics::set(m_metrics.halted, m_halt.active ? 1 : 0); // review NOTE-3: a kept halt is a halt
    BridgeMetrics::set(m_metrics.haltReason, static_cast<std::int64_t>(m_halt.active ? m_halt.halt : BridgeHalt::NONE));
    BridgeMetrics::set(m_metrics.coreHaltReason, m_coreHaltRaw);
    BridgeMetrics::set(m_metrics.coreCursor, m_C);
    BridgeMetrics::set(m_metrics.meEpoch, m_syncEpoch);
    BridgeMetrics::set(m_metrics.inflightRingDepth, static_cast<std::int64_t>(m_ring.size()));
    BridgeMetrics::set(m_metrics.stagedTrades, static_cast<std::int64_t>(m_stager.size()));
    BridgeMetrics::set(m_metrics.degraded, m_degraded);
    for (std::size_t i = 0; i < m_settings.sourceCount; ++i)
    {
        BridgeMetrics::set(m_metrics.sourceVerdicts[i], static_cast<std::int64_t>(m_verdicts.at(i).kind));
    }
}

BridgeView BridgeDomain::view() const noexcept
{
    BridgeView v;
    v.state = m_state;
    v.halt = m_halt.active ? m_halt.halt : BridgeHalt::NONE;
    v.coreMirror = m_coreHaltRaw <= 5 ? static_cast<HaltReason>(m_coreHaltRaw) : HaltReason::NONE;
    v.syncEpoch = m_syncEpoch;
    v.cursor = m_C;
    v.last = m_last;
    v.expected = m_last + 1;
    v.attached = m_current;
    v.reportThisTerm = m_reportThisTerm;
    v.degraded = m_degraded;
    v.ringDepth = m_ring.size();
    v.staged = m_stager.size();
    v.lastForwardAt = m_lastForwardAt;
    v.fatal = m_fatal;
    v.lastReportAt = m_lastReportAt;
    v.consumedPosition = m_consumedPosition;
    v.syncCursor = m_syncCursor;
    v.relistsSinceHalt = m_relistsSinceHalt;
    return v;
}

// ------------------------------------------------------------------ the frame path (7.3)

void BridgeDomain::noteAccepted(const archive::JournalFrame &frame) noexcept
{
    m_consumedPosition = frame.endPosition; // W2a (S9c 4.3 bit 8: a skip walk is progress)
    m_acceptedSinceAttach = true;
    m_dwellRunning = false;
    m_strikes[frame.sourceIndex] = 0;
    if (0 != m_syncEpoch && frame.peek.meEpoch == m_syncEpoch)
    {
        m_ownEpochSeen = true;
    }
    if (m_tainted[frame.sourceIndex] && m_readMemoFree && !m_relist[frame.sourceIndex].walking &&
        frame.key.recordingId > m_taintRecording[frame.sourceIndex] && !disabled(Rule::TAINT_NEVER_CLEARS))
    {
        // S10' 5.1 taint, reset path (C4): a linear memo-free read accepted a frame of a recording after the bad / stale
        // one, so it passed that recording without a refusal (a linear read ends at one: BAD) or no longer meets it
        m_tainted[frame.sourceIndex] = false;
        m_taintRecording[frame.sourceIndex] = 0;
    }
    // S9a 11a.13: the first seq of a recording read from its start, against the earlier recordings' first seqs
    if (!(frame.key == m_readKey))
    {
        m_readKey = frame.key;
        m_expectFirstSeq = true; // the walk enters a later recording at its startPosition
    }
    if (!m_expectFirstSeq)
    {
        return;
    }
    m_expectFirstSeq = false;
    StepBack &sb = m_stepBack[frame.sourceIndex];
    const std::int64_t id = frame.key.recordingId;
    for (std::size_t i = 0; i < sb.used; ++i)
    {
        if (sb.recordingId[i] == id)
        {
            return; // already known
        }
    }
    bool stepBack = false;
    for (std::size_t i = 0; i < sb.used; ++i)
    {
        if (sb.recordingId[i] < id && frame.peek.journalSeq < sb.firstSeq[i])
        {
            stepBack = true;
        }
    }
    if (stepBack && !disabled(Rule::STEPBACK_COUNTER_OFF))
    {
        BridgeMetrics::add(m_metrics.recordingStartStepBack);
    }
    sb.recordingId[sb.next] = id;
    sb.firstSeq[sb.next] = frame.peek.journalSeq;
    sb.next = (sb.next + 1) % StepBack::SLOTS;
    sb.used = std::min(sb.used + 1, StepBack::SLOTS);
}

void BridgeDomain::noteForward(const archive::JournalFrame &frame) noexcept
{
    m_verdicts.clearDynamic();
    m_verdicts.atE(e());
    clearWait();
    m_degraded &= ~(ROW_BITS | bit(Degraded::NO_SOURCE_BEYOND_E));
    m_regressions.fill(0);
    m_lastForwardSource = frame.sourceIndex;
    if (m_rescan && m_halt.active && frame.peek.journalSeq >= m_halt.e)
    {
        // 9.4 (b): the re-scan delivered e; the core applies the exactly-next entry while latched and reach-clears
        // its latch (S10' 4R: a GAP reach-clear with no other holder reopens the gate; SOURCE_SCHEMA keeps it)
        Relist &r = m_relist[frame.sourceIndex];
        if (r.walking && r.e == m_halt.e)
        {
            r.deliveredE = true; // S10' 5.1 R1: e came from a recording after the holed one (the R2 precondition)
            BridgeMetrics::add(m_metrics.haltSuccessorAttaches);
        }
        leaveHalt(true);
        m_state = State::STREAMING;
    }
}

SinkAction BridgeDomain::onFrame(const archive::JournalFrame &frame) noexcept
{
    // row 0
    if (Fatal::NONE != m_fatal || !feeding())
    {
        return defer(DeferReason::STATE);
    }
    if (m_closed)
    {
        return defer(DeferReason::CLOSED);
    }
    if (SourceCommandKind::NONE != m_command.kind || m_switchInFlight)
    {
        return defer(DeferReason::COMMAND);
    }
    if (!m_attached || frame.sourceIndex != m_current)
    {
        return defer(DeferReason::SOURCE);
    }
    if (m_ring.full())
    {
        BridgeMetrics::add(m_metrics.ringFullPauses);
        if (!m_query.wanted)
        {
            wantQueryNow();
        }
        return defer(DeferReason::RING_FULL);
    }
    if (m_keysFull)
    {
        return defer(DeferReason::KEYTABLE_FULL);
    }
    const archive::Peek &p = frame.peek;
    const std::int64_t seq = p.journalSeq;

    // S10' 5.1 R1: the walk skips the rest of a recording that lacks e or is bad before e: its frames are consumed, never
    // forwarded, never decided on (the next recording of the listing is read from its start)
    if (walkSkips(frame.sourceIndex, frame.key))
    {
        if (seq > m_last)
        {
            Relist &r = m_relist[frame.sourceIndex];
            r.walkedMax = std::max(r.walkedMax, seq);
        }
        m_consumedPosition = frame.endPosition;
        m_acceptedSinceAttach = true;
        return SinkAction::CONSUME; // no memo proof from a recording the walk left (5.2 item 3)
    }

    // row 1: a foreign epoch makes the source unusable for this sync, even where it would be a skip (D-S9b-1)
    if (0 != m_syncEpoch && p.meEpoch != m_syncEpoch && !disabled(Rule::EPOCH_CHECK_OFF))
    {
        if (!handleStep(flushStaging()))
        {
            return SinkAction::ABORT;
        }
        if (disabled(Rule::EPOCH_DIRECT_HALT))
        {
            enterHalting(BridgeHalt::EPOCH_MISMATCH, seq, p.meEpoch); // RED shape: the first foreign record halts
            return SinkAction::ABORT;
        }
        if (taintVerify(m_current))
        {
            return SinkAction::ABORT; // S10' 5.1: a foreign frame on a tainted source's non-linear read is verified first
        }
        Verdict v;
        if (walkVerdict(frame.sourceIndex, v))
        {
            // S10' 5.1 R1: a foreign recording ends a re-scan walk; the verdict at e is the walk's (HOLED with the min
            // seen, or BAD when a walked recording refused e), never the genesis-only class (C9)
            setVerdict(m_current, v);
            rotate();
            return SinkAction::ABORT;
        }
        if (!m_ownEpochSeen && !disabled(Rule::STALE_PREFIX_FOREIGN) &&
            relistEligible(frame.readWasMemoFree, frame.laterRecordings) && frame.laterRecordings)
        {
            // S10' 5.1 R1: a stale older-epoch prefix is a recording bad before e: the re-scan walks past it into the later
            // recordings of the listing (an end without e decides BAD, the admin class)
            Relist &r = m_relist[m_current];
            if (r.e != e() && !disabled(Rule::MARKER_NEVER_RESET))
            {
                r = Relist{};
                r.e = e();
                r.holedKey = frame.key;
            }
            if (!r.badAtE)
            {
                r.badAtE = true;
                r.bad = BadReason::STALE_EPOCH_PREFIX;
            }
            taint(m_current, frame.key);
            if (!r.walking)
            {
                r.walking = true;
                BridgeMetrics::add(m_metrics.haltRelists[m_current]);
                ++m_relistsSinceHalt;
            }
            r.skipKey = frame.key;
            m_consumedPosition = frame.endPosition;
            m_acceptedSinceAttach = true;
            return SinkAction::CONSUME;
        }
        if (!m_ownEpochSeen && !disabled(Rule::STALE_PREFIX_FOREIGN))
        {
            // review NOTE-8 (amended 2026-10-31): a foreign frame before any frame of the bound epoch on this read is
            // a stale prefix (an unwiped older recording at the chain head), not a re-genesis: BAD, so the outcome is
            // SOURCE_SCHEMA (admin class), never the genesis-only EPOCH_MISMATCH
            v.kind = VerdictKind::BAD;
            v.bad = BadReason::STALE_EPOCH_PREFIX;
        }
        else
        {
            v.kind = VerdictKind::FOREIGN_EPOCH;
            v.bad = BadReason::FOREIGN_EPOCH;
            v.epoch = p.meEpoch;
        }
        v.seq = seq;
        setVerdict(m_current, v);
        rotate();
        return SinkAction::ABORT;
    }

    // row 2: SKIP (the follower records the skip proof itself when seq <= C)
    if (seq <= m_last && !disabled(Rule::SKIP_OFF))
    {
        if (disabled(Rule::TERMINAL_INCLUSIVE) && archive::TEMPLATE_TERMINAL == p.templateId && seq == m_last)
        {
            // RED shape: the Java bridge's inclusive terminal boundary re-offers the 63 at the cursor
            const std::size_t n = m_encoder.encodeRecord(frame.bytes, frame.length, m_frame, sizeof(m_frame));
            if (n > 0)
            {
                (void)m_core.tryOffer(m_frame, n);
            }
        }
        if (p.templateId >= 1 && p.templateId <= SKIP_KIND_LABELS)
        {
            BridgeMetrics::add(m_metrics.skipped[p.templateId - 1]);
        }
        noteAccepted(frame);
        return SinkAction::CONTINUE;
    }

    // row 3: only a 68 FRESH can bind; a well-formed seq 1 of any other kind would latch the core genesis-only
    if (seq == m_last + 1 && 0 == m_last && archive::TEMPLATE_CONFIG != p.templateId && !disabled(Rule::HEAD_RULE_OFF))
    {
        if (!handleStep(flushStaging()))
        {
            return SinkAction::ABORT;
        }
        Verdict v;
        v.kind = VerdictKind::BAD;
        v.bad = BadReason::HEAD_NOT_CONFIG;
        v.seq = seq;
        setVerdict(m_current, v);
        rotate();
        return SinkAction::ABORT;
    }

    // row 4: FORWARD (G1: exactly last + 1, nothing else)
    const bool forward = disabled(Rule::FORWARD_GREATER) ? seq > m_last
        : (seq == m_last + 1 || (disabled(Rule::SKIP_OFF) && seq <= m_last));
    if (forward)
    {
        std::uint32_t keyIndex = 0;
        if (!m_keys.intern(frame.sourceIndex, frame.key, keyIndex))
        {
            if (!handleStep(flushStaging()))
            {
                return SinkAction::ABORT;
            }
            m_keysFull = true; // cleared by the report that empties the ring (6.2)
            BridgeMetrics::add(m_metrics.ringFullPauses);
            if (!m_query.wanted)
            {
                wantQueryNow();
            }
            return defer(DeferReason::KEYTABLE_FULL);
        }
        OfferedRecord meta;
        meta.seq = seq;
        meta.endPosition = frame.endPosition;
        meta.source = static_cast<std::uint16_t>(frame.sourceIndex);
        meta.keyIndex = keyIndex;
        const ForwardResult r = m_stager.forward(frame.bytes, frame.length, p.templateId, meta, m_output);
        if (!handleStep(r.step))
        {
            return SinkAction::ABORT; // back-pressure: nothing advanced, the frame is redelivered
        }
        if (seq > m_last)
        {
            m_last = seq;
        }
        if (0 == m_syncEpoch && !disabled(Rule::ADOPT_OFF))
        {
            m_syncEpoch = p.meEpoch; // an unbound sync adopts the epoch of its seq 1
        }
        noteForward(frame);
        (void)handleStep(r.flush); // a back-pressured cap flush keeps the 64; tick retries it
        noteAccepted(frame);
        return SinkAction::CONTINUE;
    }

    // row 5: a hole at e = last + 1, never a forward
    if (!handleStep(flushStaging()))
    {
        return SinkAction::ABORT;
    }
    if (!m_halt.active && stepBackDue(frame.sourceIndex, &frame.key))
    {
        stepBack(frame.sourceIndex); // S10' 5.1 R2: an earlier recording of this chain holds e
        return SinkAction::ABORT;
    }
    if (taintVerify(frame.sourceIndex))
    {
        return SinkAction::ABORT; // S10' 5.1: a hole on a tainted source's non-linear read is verified first
    }
    const bool memoFree = disabled(Rule::HOLE_VERIFY_OFF) ? true : frame.readWasMemoFree;
    if (HoleDecision::VERIFY_FROM_CHAIN_START == m_memo.onHole(frame.sourceIndex, e(), memoFree))
    {
        BridgeMetrics::add(m_metrics.holeVerifications);
        requestReattach();
        return SinkAction::ABORT;
    }
    Verdict v;
    v.kind = VerdictKind::HOLED;
    v.seq = seq;
    if (relistEligible(frame.readWasMemoFree, frame.laterRecordings))
    {
        // S10' 5.1 R1: a halted re-scan's memo-free read walks past the hole into the later recordings of the listed
        // chain (an in-place successor: FILL, RESTART); the verdict at e is derived only after the last one
        Relist &r = m_relist[frame.sourceIndex];
        if (r.e != e() && !disabled(Rule::MARKER_NEVER_RESET))
        {
            r = Relist{};
            r.e = e();
            r.holedKey = frame.key;
        }
        if (0 == r.seen || (seq < r.seen && !disabled(Rule::SEEN_FROM_FIRST_RECORDING_ONLY)))
        {
            r.seen = seq; // 5.2 item 4: the FeedHalt's seen is the min over the recordings walked
        }
        if (frame.laterRecordings)
        {
            if (!r.walking)
            {
                r.walking = true;
                BridgeMetrics::add(m_metrics.haltRelists[frame.sourceIndex]);
                ++m_relistsSinceHalt;
            }
            r.skipKey = frame.key; // the rest of this recording is consumed (walkSkips)
            r.walkedMax = std::max(r.walkedMax, seq); // what the earlier recordings hold past the hole (R2)
            m_consumedPosition = frame.endPosition;
            m_acceptedSinceAttach = true;
            return SinkAction::CONSUME; // consumed, never forwarded (G1), never a memo proof (5.2 item 3)
        }
        (void)walkVerdict(frame.sourceIndex, v); // the last recording of the listing was walked
    }
    setVerdict(m_current, v);
    rotate();
    return SinkAction::ABORT;
}

RefusalAction BridgeDomain::onRefused(const archive::RefusedFrame &frame) noexcept
{
    if (Fatal::NONE != m_fatal || !feeding())
    {
        return RefusalAction::CLOSE_REPLAY;
    }
    if (disabled(Rule::REFUSAL_IGNORED))
    {
        return RefusalAction::SKIP_AND_CONTINUE; // RED shape: skipped like the Java T5
    }
    (void)handleStep(flushStaging()); // retried in tick if back-pressured; the switch waits for it
    if (frame.sourceIndex == m_current && !m_switchInFlight && walkSkips(frame.sourceIndex, frame.key))
    {
        m_consumedPosition = frame.endPosition; // S10' 5.1 R1: the rest of a recording the walk left
        return RefusalAction::SKIP_AND_CONTINUE;
    }
    if (frame.sourceIndex == m_current && !m_switchInFlight)
    {
        // a refused frame on a read that resumed from the memo is verified like a hole (8.6): a memo-free read may
        // reach a valid e in an earlier recording without crossing the refused copy (13.10 (c)); once per (source, e)
        if (!m_readMemoFree && m_refusalVerifiedAt[m_current] != e() && !disabled(Rule::REFUSAL_VERIFY_OFF))
        {
            m_refusalVerifiedAt[m_current] = e();
            m_memo.invalidate(m_current, archive::MemoReason::HOLE_VERIFY);
            BridgeMetrics::add(m_metrics.refusalVerifications);
            requestReattach();
            return RefusalAction::CLOSE_REPLAY;
        }
        if (relistEligible(m_readMemoFree, frame.laterRecordings))
        {
            // S10' 5.1 R0 / R1: a halted re-scan walks past the bad copy into the later recordings (valid copies of e
            // in a successor); BAD is derived only after the last recording
            Relist &r = m_relist[m_current];
            if (r.e != e() && !disabled(Rule::MARKER_NEVER_RESET))
            {
                r = Relist{};
                r.e = e();
                r.holedKey = frame.key;
            }
            if (!r.badAtE)
            {
                r.badAtE = true;
                r.bad = badFor(frame.reason);
            }
            taint(m_current, frame.key);
            if (frame.laterRecordings)
            {
                if (!r.walking)
                {
                    r.walking = true;
                    BridgeMetrics::add(m_metrics.haltRelists[m_current]);
                    ++m_relistsSinceHalt;
                }
                r.skipKey = frame.key; // a recording bad before (or at) e delivers nothing: the rest is consumed
                m_consumedPosition = frame.endPosition;
                return RefusalAction::SKIP_AND_CONTINUE;
            }
            r.successorsTried = true;
        }
        Verdict v;
        v.kind = VerdictKind::BAD;
        v.bad = badFor(frame.reason);
        v.seq = frame.hasPeek ? frame.peek.journalSeq : 0;
        setVerdict(m_current, v);
        rotate();
    }
    return RefusalAction::CLOSE_REPLAY;
}

void BridgeDomain::onPollDone(int fragments, TimePoint now) noexcept
{
    m_now = now;
    if (0 == fragments && m_lastLimit > 0 && !m_stager.empty() && feeding() && !disabled(Rule::IDLE_FLUSH_OFF))
    {
        (void)handleStep(flushStaging()); // the empty-poll flush (S9a 3.2 step 4)
    }
}

SourceCommand BridgeDomain::takeSourceCommand() noexcept
{
    if (SourceCommandKind::NONE == m_command.kind)
    {
        return {};
    }
    if (!m_stager.empty() && !disabled(Rule::PRE_SWITCH_FLUSH_OFF))
    {
        return {}; // held until staging is empty (S9a 6.3 N13); tick flushes
    }
    const SourceCommand cmd = m_command;
    m_command = {};
    if (SourceCommandKind::SWITCH == cmd.kind)
    {
        if (cmd.index == m_current && !m_switchInFlight)
        {
            return {};
        }
        m_switchInFlight = true;
        m_switchTarget = cmd.index;
        m_attached = false;
        m_sourceSince = m_now;
    }
    else
    {
        m_acceptedSinceAttach = false;
        m_attached = false; // frames resume at the new onAttached
    }
    m_resumePending = true;
    m_dwellRunning = false;
    return cmd;
}

// ------------------------------------------------------------------ observer (8.5)

void BridgeDomain::switchDone(std::size_t source) noexcept
{
    m_switchInFlight = false;
    m_current = source;
    m_attached = false;
    m_live = false;
    m_exhausted = false;
    m_acceptedSinceAttach = false;
    m_resumePending = true;
    m_sourceSince = m_now;
    m_dwellRunning = false;
}

void BridgeDomain::onAttached(const archive::AttachInfo &info) noexcept
{
    if (m_switchInFlight && info.source == m_switchTarget)
    {
        switchDone(info.source);
    }
    m_current = info.source;
    m_consumedPosition = info.from; // W2a
    m_attached = true;
    m_live = true;
    m_exhausted = false;
    if (m_resumePending)
    {
        if (info.source < m_settings.sourceCount)
        {
            m_relist[info.source].walking = false; // S10' 5.1: a walk lives on the read it began on
            m_relist[info.source].skipKey = {};
        }
        m_acceptedSinceAttach = false; // a read from a resume point; a successor attach keeps the flag
        m_resumePending = false;
        m_ownEpochSeen = info.fromProof || !info.memoFree; // a memo resume (proof meEpoch == E) implies own-epoch frames
        m_readMemoFree = info.memoFree;
    }
    m_attachMemoFree = info.memoFree;
    m_sourceSince = m_now;
    m_dwellRunning = false;
    m_readKey = info.recording.key;
    m_expectFirstSeq = info.from == info.recording.startPosition;
    if (info.source < m_settings.sourceCount && VerdictKind::REFUSED_STATIC == m_verdicts.at(info.source).kind)
    {
        m_verdicts.clearSource(info.source); // the source attached again: its static refusal is over
    }
}

void BridgeDomain::onRecordingEnded(std::size_t, const archive::Recording &) noexcept
{
    BridgeMetrics::add(m_metrics.recordingsEnded);
}

void BridgeDomain::onChainEmpty(std::size_t source) noexcept
{
    if (m_switchInFlight && source == m_switchTarget)
    {
        switchDone(source);
    }
    if (source == m_current)
    {
        m_exhausted = true;
    }
}

void BridgeDomain::onChainExhausted(std::size_t source) noexcept
{
    onChainEmpty(source);
}

void BridgeDomain::onSourceRefused(std::size_t source, const archive::Recording &) noexcept
{
    if (m_switchInFlight && source == m_switchTarget)
    {
        switchDone(source);
    }
    if (source >= m_settings.sourceCount)
    {
        return;
    }
    Verdict v;
    v.kind = VerdictKind::REFUSED_STATIC;
    setVerdict(source, v);
    if (source == m_current)
    {
        m_attached = false;
        rotate();
    }
}

void BridgeDomain::onFault(const archive::SourceFault &fault) noexcept
{
    if (Fatal::NONE != m_fatal || fault.source >= m_settings.sourceCount)
    {
        return;
    }
    const std::size_t s = fault.source;
    if (m_switchInFlight && s == m_switchTarget)
    {
        switchDone(s);
    }
    if (s == m_current)
    {
        m_live = false;
        m_attached = false;
        m_resumePending = true; // the follower backs off and resumes from the memo
    }
    if (!feeding() || s != m_current)
    {
        return;
    }
    Verdict v;
    auto unreachable = [&]() {
        v.kind = VerdictKind::UNREACHABLE;
        v.fault = fault.kind;
        m_strikes[s] = 0;
        setVerdict(s, v);
        rotate();
    };
    switch (fault.kind)
    {
        case FaultKind::NONE:
        case FaultKind::FATAL:          // the conductor is exiting the process
        case FaultKind::SCHEMA_REFUSED: // the verdict came from onRefused
            return;
        case FaultKind::CONNECT_FAILED:
        case FaultKind::LIST_FAILED:
        case FaultKind::ARCHIVE_GONE:
        case FaultKind::NEVER_CONNECTED:
        case FaultKind::ARCHIVE_ERROR:
            unreachable();
            return;
        case FaultKind::MALFORMED_EXTENT:
            if (disabled(Rule::MALFORMED_AS_UNREACHABLE))
            {
                unreachable();
                return;
            }
            v.kind = VerdictKind::BAD;
            v.bad = BadReason::MALFORMED_EXTENT;
            setVerdict(s, v);
            rotate();
            return;
        case FaultKind::POSITION_REJECTED:
        case FaultKind::RESUME_BELT:
            if (!disabled(Rule::MEMO_FAULT_STRIKES))
            {
                return; // S9a invalidated the memo and re-reads memo-free; not a strike
            }
            [[fallthrough]];
        case FaultKind::REPLAY_STORAGE:
        case FaultKind::REPLAY_ENDPOINT_UNRESOLVED:
        case FaultKind::REPLAY_OPEN:
        case FaultKind::EARLY_EOF:
        case FaultKind::IMAGE_LOST:
        case FaultKind::STALL:
        case FaultKind::STALL_AFTER_STOP:
        case FaultKind::STALL_BOUNDED_STUCK:
            if (++m_strikes[s] >= m_settings.transientStrikes && !disabled(Rule::STRIKES_OFF))
            {
                unreachable();
            }
            return;
        case FaultKind::SEQ_REGRESSION_IN_RECORDING:
            if (m_attachMemoFree && ++m_regressions[s] >= 2 && !disabled(Rule::S6_OFF))
            {
                v.kind = VerdictKind::BAD;
                v.bad = BadReason::SEQ_REGRESSION;
                setVerdict(s, v);
                rotate();
            }
            return;
    }
}

// ------------------------------------------------------------------ failover (8.1-8.4)

void BridgeDomain::setVerdict(std::size_t source, Verdict v) noexcept
{
    m_verdicts.atE(e());
    if (m_verdicts.set(source, v, m_now))
    {
        if (VerdictKind::BAD == v.kind || VerdictKind::FOREIGN_EPOCH == v.kind)
        {
            BridgeMetrics::add(m_metrics.sourceBad[static_cast<std::size_t>(v.bad)]);
        }
        logVerdict(source, m_verdicts.at(source));
    }
    publish();
}

void BridgeDomain::requestSwitch(std::size_t index, bool failover) noexcept
{
    if (index == m_current && !m_switchInFlight)
    {
        return;
    }
    m_command.kind = SourceCommandKind::SWITCH;
    m_command.index = index;
    BridgeMetrics::add(failover ? m_metrics.sourceFailovers : m_metrics.sourceParks);
    if (!m_stager.empty())
    {
        (void)handleStep(flushStaging()); // the command is held until staging is empty
    }
}

void BridgeDomain::requestReattach() noexcept
{
    if (SourceCommandKind::SWITCH == m_command.kind)
    {
        return; // the switch attaches fresh from the memo anyway
    }
    m_command.kind = SourceCommandKind::REATTACH;
    m_command.index = m_current;
    if (!m_stager.empty())
    {
        (void)handleStep(flushStaging());
    }
}

void BridgeDomain::clearWait() noexcept
{
    m_waitRow = DecisionRow::UNDECIDED;
    m_confirming = false;
    m_degraded &= ~ROW_BITS;
}

void BridgeDomain::rotate() noexcept
{
    if (!feeding() || SourceCommandKind::SWITCH == m_command.kind || m_switchInFlight)
    {
        return; // a rotation is under way (or the bridge is not streaming)
    }
    if (disabled(Rule::ROTATION_OFF))
    {
        // RED shape: halt on the first verdict
        const Verdict &v = m_verdicts.at(m_current);
        if (VerdictKind::HOLED == v.kind)
        {
            enterHalting(BridgeHalt::GAP, v.seq, 0);
        }
        else if (VerdictKind::FOREIGN_EPOCH == v.kind)
        {
            enterHalting(BridgeHalt::EPOCH_MISMATCH, v.seq, v.epoch);
        }
        else if (VerdictKind::BAD == v.kind)
        {
            enterHalting(BridgeHalt::SOURCE_SCHEMA, e(), 0);
        }
        return;
    }
    if (lanePastLast() && !disabled(Rule::ROTATE_PAST_CURSOR))
    {
        fence(FenceTrigger::LANE_AHEAD, false); // review FIX-16: another lane is past last: a sync from C'
        return;
    }
    const std::size_t n = m_settings.sourceCount;
    const bool probe = m_stormProbe && m_storm.active && m_last <= m_storm.cursor;
    for (std::size_t k = 1; k < n; ++k)
    {
        const std::size_t i = (m_current + k) % n;
        if (probe && 0 != (m_storm.tried & (1u << i)))
        {
            continue; // a CORE_REFUSED retry reads only the sources that never offered C' + 1
        }
        if (VerdictKind::UNKNOWN == m_verdicts.at(i).kind)
        {
            requestSwitch(i, true);
            return;
        }
    }
    if (probe)
    {
        enterCoreRefused(m_storm.cursor + 1); // no untried source can deliver C' + 1 now: park again, nothing re-offered
        return;
    }
    runDecision();
}

void BridgeDomain::runDecision() noexcept
{
    const bool graceExpired = m_verdicts.hasFirst() && m_now - m_verdicts.firstAt() >= m_settings.holeGrace;
    const Decision d = decide(m_verdicts, graceExpired, m_lastForwardSource);
    switch (d.row)
    {
        case DecisionRow::UNDECIDED:
            return;
        case DecisionRow::NO_REACHABLE:
            m_waitRow = d.row;
            m_degraded |= bit(Degraded::NO_REACHABLE_SOURCE);
            if (NO_SOURCE != d.target && d.target != m_current)
            {
                requestSwitch(d.target, false);
            }
            publish();
            return;
        case DecisionRow::ALL_BEHIND:
            if (DecisionRow::ALL_BEHIND != m_waitRow)
            {
                m_behindRescanAt = m_now + m_settings.behindRescan;
            }
            m_waitRow = d.row;
            m_degraded &= ~ROW_BITS;
            if (lanePastLast())
            {
                m_degraded |= bit(Degraded::NO_SOURCE_BEYOND_E);
            }
            if (disabled(Rule::BEHIND_RESCAN_OFF))
            {
                // RED shape: no rescan bound: every source is re-derived at the next dwell (a rotation every 10 s)
                m_verdicts.clearDynamic();
                m_waitRow = DecisionRow::UNDECIDED;
                return;
            }
            if (NO_SOURCE != d.target && d.target != m_current)
            {
                requestSwitch(d.target, false);
            }
            publish();
            return;
        case DecisionRow::HOLE_GRACE:
            m_waitRow = d.row;
            m_degraded |= bit(Degraded::HOLE_WITH_LAGGING_SOURCE);
            if (NO_SOURCE != d.target && d.target != m_current)
            {
                requestSwitch(d.target, false);
            }
            publish();
            return;
        case DecisionRow::GAP:
        case DecisionRow::EPOCH:
        case DecisionRow::SCHEMA:
            confirmThenHalt(d);
            return;
    }
}

void BridgeDomain::expireVerdicts() noexcept
{
    bool expired = false;
    for (std::size_t i = 0; i < m_settings.sourceCount; ++i)
    {
        const Verdict &v = m_verdicts.at(i);
        if (VerdictKind::UNREACHABLE == v.kind && m_now - v.since >= m_settings.unreachableRetry)
        {
            m_verdicts.clearSource(i); // a returning source is tried again
            expired = true;
        }
    }
    if (expired && DecisionRow::UNDECIDED != m_waitRow && !m_confirming)
    {
        rotate();
    }
}

void BridgeDomain::behindDwell() noexcept
{
    if (disabled(Rule::BEHIND_OFF))
    {
        return;
    }
    const bool paused = m_lastLimit <= 0 || SourceCommandKind::NONE != m_command.kind || m_switchInFlight;
    const bool live = (m_attached && m_live) || m_exhausted;
    const bool backlogZero = m_exhausted || m_archive.sourceBacklogBytes.load(std::memory_order_relaxed) <= 0;
    const TimePoint earliest = m_sourceSince +
        (disabled(Rule::DWELL_FROM_ATTACH) ? std::chrono::milliseconds(0) : m_settings.probeInterval);
    if (paused || !live || !backlogZero || !m_stager.empty() || m_now < earliest ||
        VerdictKind::UNKNOWN != m_verdicts.at(m_current).kind)
    {
        m_dwellRunning = false;
        return;
    }
    if (!m_dwellRunning)
    {
        m_dwellRunning = true;
        m_dwellStart = m_now;
        return;
    }
    if (m_now - m_dwellStart >= m_settings.behindDwell)
    {
        m_dwellRunning = false;
        if (!m_halt.active && stepBackDue(m_current, nullptr))
        {
            stepBack(m_current); // S10' 5.1 R2: the successor ended at f, an earlier recording holds f + 1
            return;
        }
        if (taintVerify(m_current))
        {
            return; // S10' 5.1: BEHIND on a tainted source's non-linear read is verified first
        }
        Verdict v;
        v.kind = VerdictKind::BEHIND;
        setVerdict(m_current, v);
        rotate();
    }
}

// ------------------------------------------------------------------ the halt machine (section 9)

void BridgeDomain::confirmThenHalt(const Decision &d) noexcept
{
    if (disabled(Rule::HALT_CONFIRM_OFF))
    {
        enterHalting(DecisionRow::GAP == d.row ? BridgeHalt::GAP
                : DecisionRow::EPOCH == d.row ? BridgeHalt::EPOCH_MISMATCH : BridgeHalt::SOURCE_SCHEMA,
            d.seen, d.epochSeen);
        return;
    }
    // review FIX-4: confirm before halting: another lane may have delivered e meanwhile. Review BLOCKER-1: one query
    // per (row, e): while that confirmation is outstanding a re-derived decision keeps it (and its correlation id);
    // only a changed decision asks again
    if (m_confirming && d.row == m_confirmDecision.row && e() == m_confirmE && !disabled(Rule::CONFIRM_REQUERY))
    {
        m_confirmDecision = d;
        return;
    }
    m_waitRow = d.row;
    m_confirming = true;
    m_confirmDecision = d;
    m_confirmE = e();
    wantQueryNow();
}

void BridgeDomain::enterHalting(BridgeHalt halt, std::int64_t seen, std::int64_t foreignEpoch) noexcept
{
    clearWait();
    m_command = {};
    if (m_rescan && m_halt.active)
    {
        if (halt == m_halt.halt && e() == m_halt.e)
        {
            // 9.4 (b): the re-scan ended in the same verdict at the same e: back to HALTED, no new FeedHalt
            m_rescan = false;
            m_state = State::HALTED;
            m_nextRescanAt = m_now + m_settings.haltRescan;
            m_nextPeriodicAt = m_now + m_settings.reportIntervalHalted;
            publish();
            return;
        }
    }
    m_rescan = false;
    HaltRecord h;
    h.active = true;
    h.halt = halt;
    h.e = e();
    h.seen = seen;
    h.foreignEpoch = foreignEpoch;
    h.syncEpoch = m_syncEpoch;
    h.wire = feedHaltFor(halt, h.e, seen, m_syncEpoch, foreignEpoch);
    h.correlationId = nextCorrelation();
    h.needOffer = true;
    m_halt = h;
    m_relistsSinceHalt = 0;
    m_haltingSince = m_now;
    m_state = State::HALTING;
    publish();
    driveHalting();
}

void BridgeDomain::driveHalting() noexcept
{
    if (State::HALTING != m_state || Fatal::NONE != m_fatal)
    {
        return;
    }
    if (!disabled(Rule::HALT_FLUSH_OFF))
    {
        if (!handleStep(flushStaging())) // H2: the pre-halt trades go first (retried on back-pressure)
        {
            return;
        }
    }
    if (disabled(Rule::FEEDHALT_PROMPT) && m_now - m_haltingSince < std::chrono::milliseconds(5'000))
    {
        return; // ct_bridge_tc_slowhalt (S10' 3.3, a timing device): the FeedHalt 5 s after the confirming report
    }
    offerFeedHalt();
    if (m_halt.offeredOnce)
    {
        m_state = State::HALTED;
        m_nextPeriodicAt = m_now + m_settings.reportIntervalHalted;
        m_nextRescanAt = m_now + m_settings.haltRescan;
        publish();
    }
}

void BridgeDomain::offerFeedHalt() noexcept
{
    if (!m_halt.sendsFeedHalt())
    {
        return;
    }
    const FeedHaltFields &f = m_halt.wire;
    const std::size_t n = m_encoder.encodeFeedHalt(m_halt.correlationId, f.reason, f.meEpochSeen, f.expected, f.seen,
        m_settings.identity, m_frame, sizeof(m_frame));
    if (0 == n)
    {
        m_fatal = Fatal::ENCODER_OVERFLOW;
        return;
    }
    const OfferOutcome o = m_core.tryOffer(m_frame, n);
    if (OfferOutcome::OK != o)
    {
        (void)handleStep(OfferOutcome::RETRY == o ? StepResult::RETRY
                : OfferOutcome::CLOSED == o ? StepResult::CLOSED : StepResult::REFUSED);
        return;
    }
    BridgeMetrics::add(m_metrics.feedHaltsSent[static_cast<std::size_t>(f.reason)]);
    m_halt.offeredOnce = true;
    m_halt.needOffer = false;
    m_halt.nextOfferAt = m_now + m_settings.feedHaltResend;
}

void BridgeDomain::enterCoreRefused(std::int64_t refusedSeq) noexcept
{
    clearWait();
    m_command = {};
    dropStaging();
    HaltRecord h;
    h.active = true;
    h.halt = BridgeHalt::CORE_REFUSED;
    h.e = refusedSeq;
    h.syncEpoch = m_syncEpoch;
    h.acked = true; // CORE_REFUSED sends no FeedHalt: the core already latched (cpp 2.7)
    h.offeredOnce = true;
    m_halt = h;
    m_rescan = false;
    m_stormProbe = false;
    m_state = State::HALTED;
    m_nextPeriodicAt = m_now + m_settings.reportIntervalHalted;
    m_nextRescanAt = m_now + m_settings.haltRescan;
    BridgeMetrics::add(m_metrics.coreRefused);
    publish();
}

void BridgeDomain::keepHalt() noexcept
{
    // a fence or a session loss while HALTING or HALTED: the halt is kept (H1); the resumption rule decides
    m_rescan = false;
    m_rescanPending = false;
    m_stormRetryPending = false;
    if (m_halt.sendsFeedHalt())
    {
        m_halt.needOffer = true;
        m_halt.acked = false;
    }
}

void BridgeDomain::leaveHalt(bool recovered) noexcept
{
    if (recovered && m_halt.active)
    {
        BridgeMetrics::add(m_metrics.haltRecovered[static_cast<std::size_t>(m_halt.halt)]);
    }
    m_halt = HaltRecord{};
    m_rescan = false;
    m_rescanPending = false;
}

// ------------------------------------------------------------------ S10' section 5: re-list while halted

bool BridgeDomain::rescanReason() const noexcept
{
    switch (m_halt.halt)
    {
        case BridgeHalt::GAP: return true;
        case BridgeHalt::SOURCE_SCHEMA: return !disabled(Rule::SCHEMA_RESCAN_OFF); // R0
        case BridgeHalt::EPOCH_MISMATCH: return disabled(Rule::RELIST_ON_EPOCH_HALT); // never (RED shape: re-scans)
        case BridgeHalt::CORE_REFUSED: // the storm retry has its own cadence (9.6)
        case BridgeHalt::NONE: return false;
    }
    return false;
}

bool BridgeDomain::relistEligible(bool memoFree, bool laterRecordings) const noexcept
{
    (void)laterRecordings;
    if (disabled(Rule::SUCCESSOR_WALK_OFF) || !memoFree)
    {
        return false; // a memo read verifies first (8.6): the walk is memo-free
    }
    if (!m_halt.active)
    {
        return disabled(Rule::SUCCESSOR_WALK_WHILE_STREAMING) && State::STREAMING == m_state; // RED shape only
    }
    if (!m_rescan)
    {
        return false; // only a re-scan (or the resumption sync it is) re-lists: bounded by haltRescan (N-5)
    }
    return BridgeHalt::GAP == m_halt.halt || BridgeHalt::SOURCE_SCHEMA == m_halt.halt ||
        (BridgeHalt::EPOCH_MISMATCH == m_halt.halt && disabled(Rule::RELIST_ON_EPOCH_HALT));
}

bool BridgeDomain::walkVerdict(std::size_t source, Verdict &v) noexcept
{
    if (!m_halt.active || !m_rescan || source >= m_settings.sourceCount)
    {
        return false;
    }
    Relist &r = m_relist[source];
    if (!r.walking || r.e != e())
    {
        return false;
    }
    r.successorsTried = true;
    if (r.badAtE)
    {
        // a walked recording refused its copy of e or a seq before it (R0): BAD (SOURCE_SCHEMA), never a GAP: "every
        // recording lacks e or is bad before e" (5.1 R1)
        v = Verdict{};
        v.kind = VerdictKind::BAD;
        v.bad = r.bad;
        return true;
    }
    v = Verdict{};
    v.kind = VerdictKind::HOLED;
    v.seq = r.seen; // 5.2 item 4: the min over the recordings walked
    return true;
}

void BridgeDomain::taint(std::size_t source, const archive::RecordingKey &key) noexcept
{
    m_tainted[source] = true;
    m_taintRecording[source] = std::max(m_taintRecording[source], key.recordingId);
}

bool BridgeDomain::taintVerify(std::size_t source) noexcept
{
    // S10' 5.1 (R1 consequence): a re-scan walk went past a bad or stale recording of this source, so reads that are not
    // linear (a memo resume, or the walk read itself once streaming) may sit beyond it. Before a HOLED / BEHIND / FOREIGN
    // verdict on such a read is believed, ONE linear memo-free read from the chain start verifies it, once per (source, e):
    // the bad recording then decides the admin class BAD as for any linear read (C9: never the genesis-only class by a
    // detour). Also while halted (review F1): a GAP re-scan reads only the current source memo-free and rotates to the
    // others by memo resume; only the halted re-scan's memo-free read is exempt, as it IS the walk (its verdict at e is
    // walkVerdict's: BAD when a walked recording was bad, R0 / R1). The reset path is noteAccepted (C4).
    if (disabled(Rule::TAINT_VERIFY_OFF) || source >= m_settings.sourceCount || !m_tainted[source])
    {
        return false;
    }
    if (m_halt.active && disabled(Rule::TAINT_HALTED_EXEMPT))
    {
        return false; // RED shape: the pre-review exemption of every read while a halt is active
    }
    const bool linear = m_readMemoFree && (m_halt.active || !m_relist[source].walking);
    if (linear || m_taintVerifiedAt[source] == e())
    {
        return false;
    }
    m_taintVerifiedAt[source] = e();
    m_memo.invalidate(source, archive::MemoReason::HOLE_VERIFY);
    BridgeMetrics::add(m_metrics.holeVerifications);
    clearWait();
    requestReattach();
    return true;
}

bool BridgeDomain::walkSkips(std::size_t source, const archive::RecordingKey &key) const noexcept
{
    if (!m_halt.active || !m_rescan || source >= m_settings.sourceCount)
    {
        return false;
    }
    const Relist &r = m_relist[source];
    return r.walking && r.e == e() && key == r.skipKey;
}

bool BridgeDomain::stepBackDue(std::size_t source, const archive::RecordingKey *key) const noexcept
{
    if (disabled(Rule::STEP_BACK_OFF) || source >= m_settings.sourceCount)
    {
        return false;
    }
    const Relist &r = m_relist[source];
    // R2 (F-2 trigger): the successor delivered e..f (f = last), f + 1 is missing on it (a hole, or its end), and the
    // HOLED verdict at e recorded seen <= f + 1: an earlier recording of the chain holds a seq in (e, f + 1]. HOLED only
    // (contract R2): a SOURCE_SCHEMA walk records no seen, so a FILL successor holding only a valid e re-halts
    // SOURCE_SCHEMA at e + 1 (the linear truth is BAD); the runbook's fix for SOURCE_SCHEMA is a RESTART-shaped re-journal
    // (review F1-SCHEMA-FILL-NOSTEPBACK, carried as documentation)
    if (!r.deliveredE || r.steppedBack || 0 == r.seen || r.seen > e() || e() > r.walkedMax || e() <= r.e)
    {
        return false;
    }
    return nullptr == key || !(*key == r.holedKey);
}

void BridgeDomain::stepBack(std::size_t source) noexcept
{
    Relist &r = m_relist[source];
    r.steppedBack = true;
    BridgeMetrics::add(m_metrics.haltStepbacks);
    // the 8.6 pair: invalidate the source's memo, then REATTACH: a memo-free re-read from the chain start; seq <= f
    // SKIPs, f + 1 is FORWARDed from the earlier recording, the successor is SKIPped when it is reached again
    m_memo.invalidate(source, archive::MemoReason::HOLE_VERIFY);
    clearWait();
    requestReattach();
}

void BridgeDomain::resetRelist() noexcept
{
    if (!disabled(Rule::MARKER_NEVER_RESET))
    {
        m_relist.fill(Relist{});
    }
}

bool BridgeDomain::haltExitReached(std::int64_t cursor) const noexcept
{
    switch (m_halt.halt)
    {
        case BridgeHalt::GAP:
            return !disabled(Rule::GAP_AUTO_RESUME_OFF) && cursor >= m_halt.seen - 1;
        case BridgeHalt::SOURCE_SCHEMA:
        case BridgeHalt::CORE_REFUSED:
            return !disabled(Rule::SCHEMA_PARK_FOREVER) && cursor >= m_halt.e;
        case BridgeHalt::EPOCH_MISMATCH:
        case BridgeHalt::NONE:
            return false;
    }
    return false;
}

void BridgeDomain::startRescan() noexcept
{
    m_rescanPending = true;
    m_awaitSync = true;
    m_nextRescanAt = m_now + m_settings.haltRescan;
    wantQueryNow();
}

void BridgeDomain::startStormRetry() noexcept
{
    m_stormRetryPending = true;
    m_awaitSync = true;
    m_nextRescanAt = m_now + m_settings.haltRescan;
    wantQueryNow();
}

bool BridgeDomain::stormRotatable() const noexcept
{
    return m_coreHalted && (static_cast<std::uint8_t>(HaltReason::SOURCE_SCHEMA) == m_coreHaltRaw ||
                               static_cast<std::uint8_t>(HaltReason::SETTLE_REFUSED) == m_coreHaltRaw);
}

std::size_t BridgeDomain::untriedStormSource(bool allowCurrent) const noexcept
{
    const std::size_t n = m_settings.sourceCount;
    // k < n: never the current source (review NOTE-1); the RED shape lets it wrap round to the current one
    const std::size_t last = disabled(Rule::STORM_TARGETS_CURRENT) ? n : n - 1;
    for (std::size_t k = 1; k <= last; ++k)
    {
        const std::size_t i = (m_current + k) % n;
        if (0 == (m_storm.tried & (1u << i)) && (allowCurrent || 0 == (m_storm.visited & (1u << i))))
        {
            return i;
        }
    }
    if (allowCurrent && 0 == (m_storm.tried & (1u << m_current)))
    {
        return m_current;
    }
    return NO_SOURCE;
}

std::size_t BridgeDomain::ringSourceOf(std::int64_t seq) const noexcept
{
    for (std::size_t i = 0; i < m_ring.size(); ++i)
    {
        const RingEntry &r = m_ring.at(i);
        if (r.seq == seq)
        {
            return r.sourceIndex;
        }
        if (r.seq > seq)
        {
            break;
        }
    }
    return NO_SOURCE;
}

// ------------------------------------------------------------------ queries and reports (7.2)

void BridgeDomain::wantQueryNow() noexcept
{
    m_query.reset();
    m_query.wanted = true;
    m_query.correlationId = nextCorrelation();
    tryOfferQuery();
}

void BridgeDomain::offerQueryFrame() noexcept
{
    const std::size_t n = m_encoder.encodeQuery(m_query.correlationId, m_frame, sizeof(m_frame));
    if (0 == n)
    {
        m_fatal = Fatal::ENCODER_OVERFLOW;
        return;
    }
    const OfferOutcome o = m_core.tryOffer(m_frame, n);
    if (OfferOutcome::OK == o)
    {
        BridgeMetrics::add(m_metrics.reportQueries);
        if (!m_query.offered)
        {
            m_query.offered = true;
            m_query.firstSentAt = m_now;
            m_query.coversSeq = m_offeredTail; // every seq offered OK before this query (S9a 5.5 ordering)
        }
        m_query.nextResendAt = m_now + m_settings.queryResend;
        return;
    }
    (void)handleStep(OfferOutcome::RETRY == o ? StepResult::RETRY
            : OfferOutcome::CLOSED == o ? StepResult::CLOSED : StepResult::REFUSED);
}

void BridgeDomain::tryOfferQuery() noexcept
{
    if (m_query.wanted && !m_query.offered && Fatal::NONE == m_fatal)
    {
        offerQueryFrame();
    }
}

void BridgeDomain::popAndProve(std::int64_t cursor) noexcept
{
    m_ring.popThrough(cursor, [&](const RingEntry &r) {
        if (disabled(Rule::RING_PROOF_TRADES_ONLY) && T_ME_JOURNAL_TRADE != r.moneyTemplate &&
            T_ME_JOURNAL_TRADE_BATCH != r.moneyTemplate)
        {
            return;
        }
        const std::size_t source = disabled(Rule::RING_PROOF_CURRENT_SOURCE) ? m_current : r.sourceIndex;
        (void)m_memo.prove(source, m_keys.key(r.sourceIndex, r.recordingKeyIndex), r.seq, m_syncEpoch, r.endPosition);
    });
}

void BridgeDomain::acceptReport(std::int64_t cursor, std::int64_t epoch) noexcept
{
    m_memo.onReport(epoch, cursor);
    popAndProve(cursor); // with the epoch the entries were offered under (the sync's, before a new sync sets it)
    if (m_ring.empty() && (m_stager.empty() || disabled(Rule::KEYTABLE_CLEAR_WITH_STAGING)))
    {
        m_keys.clear();
        m_keysFull = false;
    }
    m_C = cursor;
    m_E = epoch;
    m_haveReport = true;
    if (m_storm.active && cursor > m_storm.cursor)
    {
        m_storm = StormRecord{}; // the record resets when C' advances
        m_stormProbe = false;
    }
    if (disabled(Rule::LAST_FOLLOWS_REPORT) && cursor > m_last)
    {
        m_last = cursor; // RED shape: a G1 violation
    }
}

void BridgeDomain::beginNormalSync(std::int64_t cursor, std::int64_t epoch) noexcept
{
    acceptReport(cursor, epoch);
    if (!disabled(Rule::RING_KEEP_TAIL))
    {
        m_ring.clear(); // entries > C' are possibly lost: re-read
    }
    dropStaging(); // empty: dropped by the fence or never filled
    if (m_ring.empty())
    {
        m_keys.clear();
        m_keysFull = false;
    }
    m_memo.beginSync();
    resetRelist(); // S10' 5.1: successorsTried / steppedBack reset at every sync start (C4)
    m_syncCursor = cursor;
    m_last = cursor;
    m_syncEpoch = epoch;
    m_offeredTail = cursor;
    ++m_syncs;
    BridgeMetrics::add(m_metrics.epochs);
    m_verdicts.clearDynamic();
    m_verdicts.atE(cursor + 1);
    clearWait();
    m_regressions.fill(0);
    m_strikes.fill(0);
    m_refusalVerifiedAt.fill(0);
    m_taintVerifiedAt.fill(0);
    m_state = State::STREAMING;
    m_rescan = false;
    m_nextPeriodicAt = m_now + m_settings.reportInterval;
    // rewind the current source to the memo proof for C whenever it accepted a frame since its attach: its read
    // position may be past C + 1, and the next frame would look like a hole
    if (m_acceptedSinceAttach && !m_switchInFlight && SourceCommandKind::NONE == m_command.kind)
    {
        requestReattach();
    }
}

void BridgeDomain::syncStart(std::int64_t cursor, std::int64_t epoch) noexcept
{
    m_awaitSync = false;
    if (m_rescanPending)
    {
        // 9.4 (b): a GAP re-scan: a normal sync from C' with the halt kept (state HALTED, reading on)
        m_rescanPending = false;
        if (haltExitReached(cursor) && epoch == m_halt.syncEpoch && !disabled(Rule::RESCAN_EXIT_A_OFF))
        {
            leaveHalt(true); // the re-scan's own report shows exit (a): no walk needed
            beginNormalSync(cursor, epoch);
            publish();
            return;
        }
        const HaltRecord kept = m_halt;
        beginNormalSync(cursor, epoch);
        m_halt = kept;
        m_state = State::HALTED;
        m_rescan = true;
        if (BridgeHalt::SOURCE_SCHEMA == kept.halt)
        {
            // S10' 5.1 R0 / R1: the re-scan of a SOURCE_SCHEMA halt reads every source memo-free from its chain start (a
            // memo resume could land past a bad copy below e in the very recording the walk must leave)
            for (std::size_t i = 0; i < m_settings.sourceCount; ++i)
            {
                m_memo.invalidate(i, archive::MemoReason::HOLE_VERIFY);
            }
        }
        if (!m_switchInFlight && SourceCommandKind::NONE == m_command.kind && !disabled(Rule::SUCCESSOR_WALK_OFF))
        {
            // S10' 5.1 R1: every re-scan re-lists the current source's chain (a successor recorded since the last listing
            // is seen), even when nothing was accepted since the attach, and reads it memo-free from its start (the walk
            // is memo-free; one listing per re-scan instead of a memo read followed by the 8.6 verification)
            m_memo.invalidate(m_current, archive::MemoReason::HOLE_VERIFY);
            requestReattach();
        }
        publish();
        return;
    }
    if (m_stormRetryPending && m_halt.active && BridgeHalt::CORE_REFUSED == m_halt.halt)
    {
        // 9.6 as amended 2026-10-31 (review FIX-2): a rotatable CORE_REFUSED re-tries the sources that never offered
        // C' + 1, reading ONLY them (rotate() is restricted while m_stormProbe); nothing tried is re-offered
        m_stormRetryPending = false;
        m_refusalFenceSinceSync = false;
        if (haltExitReached(cursor) || epoch != m_halt.syncEpoch || (m_haveReport && cursor < m_C))
        {
            // the core moved on (or was replaced) meanwhile: the ordinary resumption below decides
        }
        else
        {
            const std::size_t target = untriedStormSource(true);
            leaveHalt(false);
            beginNormalSync(cursor, epoch);
            if (NO_SOURCE == target)
            {
                enterCoreRefused(cursor + 1);
                publish();
                return;
            }
            m_stormProbe = true;
            m_storm.visited = 0;
            BridgeMetrics::add(m_metrics.stormRotations);
            if (target != m_current)
            {
                m_command = {};
                m_command.kind = SourceCommandKind::SWITCH;
                m_command.index = target;
            }
            publish();
            return;
        }
    }
    m_stormRetryPending = false;
    // 9.6: the storm counts sync starts that follow one or more refusal fences
    bool stormDue = false;
    if (m_refusalFenceSinceSync && !disabled(Rule::STORM_GUARD_OFF))
    {
        const bool unboundSilent = 0 == epoch && 0 == cursor && !m_coreHalted && !disabled(Rule::UNBOUND_COUNTS_AS_STORM);
        if (!unboundSilent) // review BLOCKER-1: the genesis race S7 4.1 designs to heal never counts
        {
            // the source that offered C' + 1 in the sync now ending (the ring still holds its entry: it is cleared
            // by beginNormalSync below)
            const std::size_t offeredBy = ringSourceOf(cursor + 1);
            const bool offeredNext = NO_SOURCE != offeredBy;
            if (offeredNext && m_storm.active && m_storm.cursor == cursor)
            {
                ++m_storm.count;
            }
            else
            {
                m_storm = StormRecord{};
                m_storm.active = true;
                m_storm.cursor = cursor;
                m_storm.count = 1;
                if (disabled(Rule::STORM_TRIED_AT_SWITCH))
                {
                    m_storm.tried = 1u << m_current;
                }
            }
            if (offeredNext)
            {
                m_storm.tried |= 1u << offeredBy; // 9.6 as amended 2026-10-31: tried = it offered C' + 1
            }
            stormDue = m_storm.count >= m_settings.stormFences;
        }
    }
    m_refusalFenceSinceSync = false;
    // 7.2 the resumption rule with a kept halt (one rule for onSessionUp after DOWN and for every fence)
    if (m_halt.active)
    {
        bool leave;
        if (disabled(Rule::RESUME_RULE_OFF))
        {
            leave = true; // RED shape: a halted bridge streams after a reconnect
        }
        else if (disabled(Rule::HALT_KEEP_ALWAYS))
        {
            leave = false;
        }
        else
        {
            leave = cursor < m_halt.e - 1 || epoch != m_halt.syncEpoch || (m_haveReport && cursor < m_C);
        }
        if (!leave)
        {
            acceptReport(cursor, epoch);
            if (haltExitReached(cursor))
            {
                leaveHalt(true);
                beginNormalSync(cursor, epoch);
                publish();
                return;
            }
            m_state = State::HALTED;
            m_nextPeriodicAt = m_now + m_settings.reportIntervalHalted;
            // the re-scan cadence is NOT restarted by a fence or a reconnect: leader changes faster than haltRescan
            // would otherwise postpone the GAP re-scan and the CORE_REFUSED retry forever
            if (m_halt.sendsFeedHalt() && !disabled(Rule::HALT_RESEND_OFF))
            {
                m_halt.needOffer = true;
                m_halt.acked = false;
                offerFeedHalt(); // re-offered after the new term's report
            }
            publish();
            return;
        }
        // S10' 4.1 rule 8: HALTED(EPOCH_MISMATCH) left on a report with a new epoch or a regressed cursor is the genesis
        // exit (the dual genesis of 4.1 rule 6): counted as a recovery; any other leave re-forwards below e uncounted
        const bool genesisExit = BridgeHalt::EPOCH_MISMATCH == m_halt.halt &&
            (epoch != m_halt.syncEpoch || (m_haveReport && cursor < m_C)) && !disabled(Rule::GENESIS_EXIT_UNCOUNTED);
        leaveHalt(genesisExit); // the prefix below e is re-forwarded, the verdict at e re-derived
    }
    beginNormalSync(cursor, epoch);
    if (stormDue)
    {
        // the next untried source the storm has not switched to yet, never the current one (review NOTE-1: a SWITCH
        // to the current source would drop the sync start's REATTACH)
        const std::size_t untried = stormRotatable() ? untriedStormSource(false) : NO_SOURCE;
        if (NO_SOURCE != untried)
        {
            m_storm.visited |= 1u << untried;
            if (disabled(Rule::STORM_TRIED_AT_SWITCH))
            {
                m_storm.tried |= 1u << untried; // RED shape (review FIX-2)
            }
            BridgeMetrics::add(m_metrics.stormRotations);
            m_command = {};
            m_command.kind = SourceCommandKind::SWITCH;
            m_command.index = untried;
        }
        else
        {
            enterCoreRefused(cursor + 1);
        }
    }
    publish();
}

void BridgeDomain::fence(FenceTrigger trigger, bool external) noexcept
{
    (void)external;
    BridgeMetrics::add(m_metrics.fences[static_cast<std::size_t>(trigger)]);
    if (disabled(Rule::FENCE_OFF))
    {
        return; // RED shape: offers continue after a fence
    }
    if (FenceTrigger::NEW_LEADER == trigger || FenceTrigger::SESSION_REESTABLISHED == trigger)
    {
        m_registered = false; // S9c calls onSessionUp again when the new term's registration is acknowledged
        m_reportThisTerm = false;
    }
    const bool refusal = isRefusalFence(trigger);
    if (refusal)
    {
        m_refusalFenceSinceSync = true;
        if (m_awaitSync && !m_rescanPending && !disabled(Rule::FENCE_PER_REJECT))
        {
            return; // review FIX-9: the outstanding query was offered after every earlier offer, its answer covers them
        }
    }
    if (State::DOWN == m_state)
    {
        return; // onSessionUp queries
    }
    dropStaging();
    m_command = {};
    clearWait();
    if (State::HALTING == m_state || State::HALTED == m_state)
    {
        keepHalt();
        m_state = State::HALTED;
    }
    else
    {
        m_state = State::SYNCING;
    }
    m_rescan = false;
    m_rescanPending = false;
    m_awaitSync = true;
    wantQueryNow(); // a fresh correlation id: an answer to an older query is stale
    publish();
}

void BridgeDomain::streamingReport(std::int64_t cursor, std::int64_t epoch, std::int64_t covers,
    std::int64_t previous) noexcept
{
    switch (periodicRule(cursor, epoch, covers, m_syncEpoch, previous))
    {
        case ReportRule::LOSS:
            fence(FenceTrigger::REPORT_LOSS, false);
            return;
        case ReportRule::EPOCH_CHANGED:
            fence(FenceTrigger::REPORT_EPOCH_CHANGED, false);
            return;
        case ReportRule::CURSOR_REGRESSED:
            fence(FenceTrigger::REPORT_CURSOR_REGRESSED, false);
            return;
        case ReportRule::ACCEPT:
            break;
    }
    acceptReport(cursor, epoch); // rule 4: last does NOT move (G1)
    if (m_rescan && m_halt.active && haltExitReached(cursor) && !disabled(Rule::RESCAN_EXIT_A_OFF))
    {
        // S10' (lane C W2b finding): a re-scan whose report shows the exit reached (9.4 (a): GAP C' >= seen - 1,
        // SOURCE_SCHEMA C' >= e) leaves the halt now, also at a quiet journal head where no record >= e is forwarded
        leaveHalt(true);
        beginNormalSync(cursor, epoch);
        return;
    }
    if (m_confirming)
    {
        const Decision d = m_confirmDecision;
        if (cursor >= e())
        {
            fence(FenceTrigger::LANE_AHEAD, false); // another lane delivered e meanwhile: no halt
            return;
        }
        enterHalting(DecisionRow::GAP == d.row ? BridgeHalt::GAP
                : DecisionRow::EPOCH == d.row ? BridgeHalt::EPOCH_MISMATCH : BridgeHalt::SOURCE_SCHEMA,
            d.seen, d.epochSeen);
        return;
    }
    if (cursor > m_last)
    {
        if (DecisionRow::ALL_BEHIND == m_waitRow)
        {
            m_degraded |= bit(Degraded::NO_SOURCE_BEYOND_E); // evidence that e exists: another lane delivered it
        }
        if (DecisionRow::UNDECIDED != m_waitRow)
        {
            rotate(); // 8.3 row 2 "at once when a report shows C' > last": with FIX-16 a sync from C'
        }
    }
    else if (DecisionRow::ALL_BEHIND == m_waitRow)
    {
        m_degraded &= ~bit(Degraded::NO_SOURCE_BEYOND_E);
    }
}

void BridgeDomain::haltedReport(std::int64_t cursor, std::int64_t epoch, std::int64_t previous) noexcept
{
    if (!disabled(Rule::HALTED_IGNORES_REPORT_RULES))
    {
        // review FIX-3: an epoch change or a cursor regression fences a halted bridge too (a core re-genesis)
        if (0 != m_halt.syncEpoch && 0 != epoch && epoch != m_halt.syncEpoch)
        {
            fence(FenceTrigger::REPORT_EPOCH_CHANGED, false);
            return;
        }
        if (previous >= 0 && cursor < previous)
        {
            fence(FenceTrigger::REPORT_CURSOR_REGRESSED, false);
            return;
        }
    }
    acceptReport(cursor, epoch);
    if (haltExitReached(cursor))
    {
        leaveHalt(true); // 9.4: the core decided (another lane, an operator fill or skip)
        beginNormalSync(cursor, epoch);
        return;
    }
    relatchIfCleared(cursor);
}

void BridgeDomain::relatchIfCleared(std::int64_t cursor) noexcept
{
    // S10' 4R.1 rule 2 (review N2-1): "a premature 76 {GAP, 0} is self-healing". The 76 removes this lane's record while
    // its hole is open, and `open` reopens the gate; the halted report then shows feedHalted 0 with C' < seen - 1. The
    // acked FeedHalt is re-offered (the same wire and correlation id, at most once per feedHaltResend), so the open hole
    // re-latches within one halted report interval. GAP only: a non-GAP clear keeps NONGAP_AWAITING_OPEN, and the `open`
    // after it is the operator's decision (rule 4). The core's identity-0 record or another lane's record holding the
    // gate shows feedHalted 1: nothing is re-offered until that one clears.
    if (disabled(Rule::GAP_RELATCH_OFF) || State::HALTED != m_state || BridgeHalt::GAP != m_halt.halt ||
        !m_halt.sendsFeedHalt() || m_halt.fallback || !m_halt.acked || m_coreHalted || haltExitReached(cursor) ||
        m_now < m_halt.nextOfferAt)
    {
        return;
    }
    m_halt.acked = false;
    m_halt.needOffer = true; // tick offers it (State::HALTED, also during a re-scan)
}

void BridgeDomain::onReport(const FeedPositionReport &report, TimePoint now) noexcept
{
    m_now = now;
    if (Fatal::NONE != m_fatal)
    {
        return;
    }
    if (!m_query.offered || report.correlationId != m_query.correlationId)
    {
        BridgeMetrics::add(m_metrics.reportStale);
        return;
    }
    if (ReportFault::NONE != validateReport(report))
    {
        BridgeMetrics::add(m_metrics.reportInvalid);
        m_degraded |= bit(Degraded::REPORT_INVALID);
        m_query.nextResendAt = now + m_settings.invalidReportResend;
        publish();
        return;
    }
    const std::int64_t covers = m_query.coversSeq;
    m_query.reset();
    m_lastReportAt = now; // W2a: an accepted report (the outstanding query's correlation id, valid)
    m_degraded &= ~(bit(Degraded::REPORT_INVALID) | bit(Degraded::REPORT_TIMEOUT));
    const bool keepFirst = disabled(Rule::MIRROR_FIRST_HALT_REASON) && m_coreHalted && 0 != m_coreHaltRaw &&
        1 == report.feedHaltedRaw;
    m_coreHalted = 1 == report.feedHaltedRaw;
    if (!keepFirst)
    {
        m_coreHaltRaw = report.haltReasonRaw; // 4R rule 6: with a GAP record and a non-GAP slot, the higher exit class
    }
    const std::int64_t previous = m_haveReport ? m_C : -1;
    const std::int64_t cursor = report.lastAppliedJournalSeq;
    const std::int64_t epoch = report.meEpoch;
    const bool halted = State::HALTED == m_state || State::HALTING == m_state;
    m_nextPeriodicAt = now + (halted ? m_settings.reportIntervalHalted : m_settings.reportInterval);
    if (m_awaitSync)
    {
        if (!m_registered && !disabled(Rule::REGISTRATION_GATE_OFF))
        {
            publish();
            return; // no feed offer until the new term's registration and a report after it (cpp 2.6)
        }
        m_reportThisTerm = true;
        syncStart(cursor, epoch);
        publish();
        return;
    }
    m_reportThisTerm = true;
    switch (m_state)
    {
        case State::STREAMING:
            streamingReport(cursor, epoch, covers, previous);
            break;
        case State::HALTED:
            if (m_rescan)
            {
                streamingReport(cursor, epoch, covers, previous);
                if (m_rescan && m_halt.active)
                {
                    relatchIfCleared(cursor); // a re-scan still holds the halt (no exit, no fence)
                }
            }
            else
            {
                haltedReport(cursor, epoch, previous);
            }
            break;
        case State::HALTING:
            haltedReport(cursor, epoch, previous);
            break;
        case State::DOWN:
        case State::SYNCING:
            break;
    }
    publish();
}

void BridgeDomain::onMalformedEgress(std::uint16_t templateId, TimePoint now) noexcept
{
    m_now = now;
    if (T_FEED_POSITION_REPORT == templateId)
    {
        BridgeMetrics::add(m_metrics.reportInvalid);
        m_degraded |= bit(Degraded::REPORT_INVALID);
        if (m_query.offered)
        {
            m_query.nextResendAt = now + m_settings.invalidReportResend;
        }
        publish();
    }
}

void BridgeDomain::onSettleRejected(const SettleRejected &reject, TimePoint now) noexcept
{
    m_now = now;
    BridgeMetrics::add(m_metrics.settleRejected[settleLabel(reject.reasonRaw)]);
    if (Fatal::NONE != m_fatal)
    {
        return;
    }
    switch (reject.reasonRaw)
    {
        case static_cast<std::uint8_t>(RejectReason::TRADE_GAP):
        case static_cast<std::uint8_t>(RejectReason::FEED_GAP):
            fence(FenceTrigger::SETTLE_REJECTED_GAP, false);
            break;
        case static_cast<std::uint8_t>(RejectReason::FEED_UNBOUND):
            fence(FenceTrigger::SETTLE_REJECTED_UNBOUND, false);
            break;
        case static_cast<std::uint8_t>(RejectReason::ME_EPOCH_MISMATCH):
            fence(FenceTrigger::SETTLE_REJECTED_EPOCH, false);
            break;
        default:
            break; // counted only: the core latched SETTLE_REFUSED, the next report's C' < coversSeq fences
    }
}

void BridgeDomain::onAdminAck(const AdminAck &ack, TimePoint now) noexcept
{
    m_now = now;
    if (T_FEED_HALT != ack.refTemplateId || !m_halt.active || ack.correlationId != m_halt.correlationId ||
        Fatal::NONE != m_fatal)
    {
        return;
    }
    if (ADMIN_ACCEPTED == ack.resultRaw || ADMIN_DUPLICATE == ack.resultRaw)
    {
        if (m_halt.fallback)
        {
            m_fatal = Fatal::FEEDHALT_REJECTED; // the gate is closed; S9c exits 70
        }
        m_halt.acked = true;
        m_halt.needOffer = false;
        publish();
        return;
    }
    if (ADMIN_REJECTED == ack.resultRaw)
    {
        // a bridge bug (9.2 makes every frame valid): the core did NOT latch, GLOBAL may be open
        BridgeMetrics::add(m_metrics.feedHaltRejected);
        if (m_halt.fallback || disabled(Rule::REJECT_EXIT_AT_ONCE))
        {
            m_fatal = Fatal::FEEDHALT_REJECTED;
            publish();
            return;
        }
        m_halt.fallback = true;
        m_halt.wire = fallbackFeedHalt(m_halt.e, m_halt.syncEpoch);
        m_halt.correlationId = nextCorrelation();
        m_halt.acked = false;
        m_halt.needOffer = true;
        if (State::HALTED == m_state)
        {
            offerFeedHalt();
        }
        publish();
    }
}

void BridgeDomain::onSessionUp(TimePoint now) noexcept
{
    m_now = now;
    if (Fatal::NONE != m_fatal)
    {
        return;
    }
    m_registered = true;
    m_closed = false;
    if (State::DOWN == m_state)
    {
        m_state = m_halt.active ? State::HALTED : State::SYNCING;
        m_awaitSync = true;
        wantQueryNow();
    }
    else if (m_awaitSync)
    {
        wantQueryNow(); // the answer must come after this term's registration
    }
    publish();
}

void BridgeDomain::onSessionDown(TimePoint now) noexcept
{
    m_now = now;
    dropStaging();
    m_query.reset();
    m_registered = false;
    m_reportThisTerm = false;
    m_lastReportAt = TimePoint{}; // W2a: the report age restarts with the next session (checklist C4)
    m_awaitSync = false;
    m_closed = false;
    m_command = {};
    clearWait();
    if (State::HALTING == m_state || State::HALTED == m_state)
    {
        keepHalt(); // a HALTING halt whose staging was dropped is a kept halt (7.2)
    }
    m_rescan = false;
    m_rescanPending = false;
    m_state = State::DOWN;
    publish();
}

void BridgeDomain::onFence(FenceTrigger trigger, TimePoint now) noexcept
{
    m_now = now;
    if (Fatal::NONE != m_fatal || FenceTrigger::COUNT == trigger)
    {
        return;
    }
    fence(trigger, true);
}

// ------------------------------------------------------------------ tick

void BridgeDomain::tick(TimePoint now) noexcept
{
    m_now = now;
    if (Fatal::NONE != m_fatal)
    {
        publish();
        return;
    }
    // the query: offer, re-send every 500 ms until answered, time out at 5 s (degraded, still re-sent)
    if (m_query.wanted && !m_query.offered)
    {
        tryOfferQuery();
    }
    else if (m_query.offered)
    {
        if (now >= m_query.nextResendAt)
        {
            offerQueryFrame();
        }
        if (!m_query.timeoutCounted && now - m_query.firstSentAt >= m_settings.reportTimeout)
        {
            m_query.timeoutCounted = true;
            BridgeMetrics::add(m_metrics.reportTimeouts);
            m_degraded |= bit(Degraded::REPORT_TIMEOUT);
        }
    }
    if (!m_query.wanted && !m_awaitSync &&
        (State::STREAMING == m_state || State::HALTED == m_state || State::HALTING == m_state) && now >= m_nextPeriodicAt)
    {
        const bool halted = State::HALTED == m_state || State::HALTING == m_state;
        m_nextPeriodicAt = now + (halted && !m_rescan ? m_settings.reportIntervalHalted : m_settings.reportInterval);
        wantQueryNow();
    }
    // staging: only a back-pressured flush is retried (FIX-8), or a held source command's pre-command flush
    if (!m_stager.empty() && (feeding() || State::HALTING == m_state))
    {
        if (m_stager.flushPending() || SourceCommandKind::NONE != m_command.kind ||
            disabled(Rule::TICK_FLUSHES_ALWAYS))
        {
            (void)handleStep(flushStaging());
        }
    }
    if (State::HALTING == m_state)
    {
        driveHalting();
    }
    if (State::HALTED == m_state && !m_awaitSync && Fatal::NONE == m_fatal)
    {
        if (m_halt.sendsFeedHalt() && !m_halt.acked && (m_halt.needOffer || now >= m_halt.nextOfferAt) &&
            !(disabled(Rule::HALT_RESEND_OFF) && m_halt.offeredOnce))
        {
            offerFeedHalt(); // every 5 s until the AdminAck (a v1/v2 core never acks: bounded, visible)
        }
        if (rescanReason() && !m_rescan && !m_rescanPending && now >= m_nextRescanAt)
        {
            startRescan(); // 9.4 (b); S10' 5.1 R0: SOURCE_SCHEMA re-scans too
        }
        if (disabled(Rule::RELIST_EVERY_TICK) && !m_rescan && !m_rescanPending && SourceCommandKind::NONE == m_command.kind)
        {
            m_command.kind = SourceCommandKind::REATTACH; // RED shape: a re-list on every tick while halted
            m_command.index = m_current;
        }
        if (BridgeHalt::CORE_REFUSED == m_halt.halt && !m_stormRetryPending && now >= m_nextRescanAt &&
            !disabled(Rule::STORM_TRIED_AT_SWITCH) && m_storm.active && stormRotatable() &&
            NO_SOURCE != untriedStormSource(true))
        {
            startStormRetry(); // 9.6 as amended 2026-10-31: re-try the untried sources on the haltRescan cadence
        }
    }
    if (feeding())
    {
        expireVerdicts();
        if (DecisionRow::HOLE_GRACE == m_waitRow && !m_confirming && m_verdicts.hasFirst() &&
            now - m_verdicts.firstAt() >= m_settings.holeGrace)
        {
            runDecision();
        }
        if (DecisionRow::ALL_BEHIND == m_waitRow && !m_confirming && now >= m_behindRescanAt &&
            !disabled(Rule::BEHIND_RESCAN_OFF))
        {
            // 8.3 row 2: one rotation per behindRescan: the next other source in configured order (cyclic, a rescan
            // pointer) is re-derived; when it is behind too the bridge parks back on the last live source
            const std::size_t n = m_settings.sourceCount;
            for (std::size_t k = 1; k < n; ++k)
            {
                const std::size_t i = (m_rescanNext + k) % n;
                if (i != m_current && VerdictKind::REFUSED_STATIC != m_verdicts.at(i).kind)
                {
                    m_verdicts.clearSource(i);
                    m_rescanNext = i;
                    break;
                }
            }
            m_behindRescanAt = now + m_settings.behindRescan;
            rotate();
        }
        behindDwell();
    }
    publish();
}

}}}
