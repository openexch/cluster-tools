#include "cluster_tools/domain/forwarder/ResyncMachine.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

namespace cluster_tools { namespace domain { namespace forwarder {

using test_seams::Rule;
using test_seams::disabled;

namespace {

bool noTriggers()
{
    return disabled(Rule::RESYNC_ALL_TRIGGERS);
}

}

const char *nameOf(Mode mode) noexcept
{
    switch (mode)
    {
        case Mode::IDLE: return "IDLE";
        case Mode::RESYNCING: return "RESYNCING";
        case Mode::LIVE: return "LIVE";
        case Mode::OVERFLOW_WAIT: return "OVERFLOW_WAIT";
    }
    return "?";
}

const char *nameOf(Trigger trigger) noexcept
{
    switch (trigger)
    {
        case Trigger::NONE: return "none";
        case Trigger::T1: return "T1";
        case Trigger::T2: return "T2";
        case Trigger::T3: return "T3";
        case Trigger::T4: return "T4";
        case Trigger::T5: return "T5";
        case Trigger::T6: return "T6";
        case Trigger::T7: return "T7";
        case Trigger::T8: return "T8";
        case Trigger::T9: return "T9";
    }
    return "?";
}

const char *nameOf(ShapeCheck check) noexcept
{
    switch (check)
    {
        case ShapeCheck::S1: return "S1";
        case ShapeCheck::S2: return "S2";
        case ShapeCheck::S3: return "S3";
    }
    return "?";
}

void ResyncMachine::onRegistered(std::int64_t nowNs) noexcept
{
    m_probeVerified = false;
    if (noTriggers())
    {
        // ct_forwarder_nc_noresync: the naive relay that never asks the core (contract 11.4 control).
        m_mode = Mode::LIVE;
        m_lastPushedSeq = m_cursor;
        abandonRequest();
        return;
    }
    startResync(0, Trigger::T1, nowNs);
}

void ResyncMachine::onUnregistered() noexcept
{
    m_mode = Mode::IDLE;
    if (!disabled(Rule::RESYNC_D0_CLEARS_HOLD))
    {
        m_hold.clear(); // a partial reply of the old term is not trusted (NOTE-11)
    }
    abandonRequest();
    m_probeScheduled = false;
    m_probeVerified = false;
}

void ResyncMachine::resyncFromZero(Trigger trigger, std::int64_t nowNs) noexcept
{
    if (noTriggers())
    {
        return;
    }
    if (Mode::IDLE == m_mode)
    {
        m_counters.ignoredIdleTriggers++; // the RESYNC(0) at the next C3 covers it
        return;
    }
    if (Mode::OVERFLOW_WAIT == m_mode && disabled(Rule::RESYNC_D3_ZERO_AT_ONCE))
    {
        return; // RED shape of FIX-5: waits for the drain
    }
    startResync(0, trigger, nowNs);
}

void ResyncMachine::startResync(std::int64_t from, Trigger trigger, std::int64_t nowNs) noexcept
{
    m_lastTrigger = trigger;
    m_counters.resyncs[static_cast<unsigned>(trigger)]++;
    if (0 == from)
    {
        if (!disabled(Rule::RESYNC_ZERO_CLEARS_RING))
        {
            m_counters.ringClearedEntries += m_ring.clear();
        }
        m_hold.clear();
        m_cursor = 0;
    }
    m_mode = Mode::RESYNCING;
    m_resyncFrom = from;
    m_pageFrom = from;
    m_lastPushedSeq = m_cursor;
    m_resyncStartNs = nowNs;
    m_probeScheduled = false;
    abandonRequest();
    if (!m_probeVerified && !disabled(Rule::AGENT_PROBE_AT_C3))
    {
        requestProbe(nowNs);
    }
    else
    {
        requestPage(nowNs);
    }
}

void ResyncMachine::requestPage(std::int64_t) noexcept
{
    m_request.kind = RequestKind::PAGE;
    m_request.correlationId = ++m_nextCorrelation;
    m_request.fromDispatchSeq = m_pageFrom;
    m_request.maxEntries = PAGE_ENTRIES;
    m_requestDue = true;
    m_requestInFlight = false;
}

void ResyncMachine::requestProbe(std::int64_t) noexcept
{
    m_request.kind = RequestKind::PROBE;
    m_request.correlationId = ++m_nextCorrelation;
    m_request.fromDispatchSeq = m_cursor; // 6.4: never negative, maxEntries 0
    m_request.maxEntries = 0;
    m_requestDue = true;
    m_requestInFlight = false;
}

void ResyncMachine::abandonRequest() noexcept
{
    m_request.kind = RequestKind::NONE;
    m_requestDue = false;
    m_requestInFlight = false;
}

void ResyncMachine::onRequestSent(std::int64_t nowNs) noexcept
{
    if (!m_requestDue)
    {
        return;
    }
    m_requestDue = false;
    m_requestInFlight = true;
    m_requestSentNs = nowNs;
    m_counters.requestsSent++;
}

bool ResyncMachine::push(const HeldEntry &entry) noexcept
{
    RingSlot slot;
    if (!toRingSlot(entry.dispatch, entry.classification, slot))
    {
        m_counters.unroutableSkipped++;
        return true;
    }
    const std::int64_t seq = entry.dispatch.dispatchSeq;
    // F2: within one resync or LIVE period pushes are strictly ascending (seq < 1 never gets here, NOTE-2).
    if (seq <= m_lastPushedSeq)
    {
        fail(ResyncVerdict::INVARIANT, seq);
        return true;
    }
    if (!m_ring.push(slot))
    {
        return false;
    }
    m_lastPushedSeq = seq;
    m_counters.pushed++;
    return true;
}

void ResyncMachine::enterOverflow(bool ringOverflow) noexcept
{
    (ringOverflow ? m_counters.ringOverflows : m_counters.holdOverflows)++;
    // Everything held is either still pending (the RESYNC(cursor) after the drain returns it) or resolved.
    m_counters.discarded += m_hold.clear();
    abandonRequest();
    m_probeScheduled = false;
    m_mode = Mode::OVERFLOW_WAIT;
}

void ResyncMachine::shapeViolation(ShapeCheck check, std::int64_t nowNs) noexcept
{
    m_counters.shapeViolations[static_cast<unsigned>(check)]++;
    startResync(0, Trigger::T6, nowNs);
}

void ResyncMachine::fail(ResyncVerdict verdict, std::int64_t detail) noexcept
{
    if (ResyncVerdict::NONE == m_verdict)
    {
        m_verdict = verdict;
        m_verdictDetail = detail;
    }
}

void ResyncMachine::onDispatch(const Dispatch &d, const Classification &c, std::int64_t nowNs) noexcept
{
    m_counters.received++;
    // Review NOTE-2: a dispatchSeq below 1 cannot be ordered and only a corrupt core produces one (it mints from 1): it
    // is refused in every mode (never held, never pushed, no carrier, the cursor stays) and counted. The agent's codec
    // already refuses the whole frame; this is the domain's own guard.
    if (d.dispatchSeq < 1 && !disabled(Rule::RESYNC_SEQ_BELOW_ONE))
    {
        m_counters.seqBelowOne++;
        return;
    }
    const HeldEntry entry{ d, c };
    switch (m_mode)
    {
        case Mode::IDLE:
            m_counters.droppedIdle++;
            return;

        case Mode::RESYNCING:
            if (m_hold.full())
            {
                m_counters.discarded++;
                enterOverflow(false);
                return;
            }
            m_hold.push(entry);
            return;

        case Mode::OVERFLOW_WAIT:
            m_counters.discarded++;
            return;

        case Mode::LIVE:
            break;
    }

    const std::int64_t seq = d.dispatchSeq;
    if (seq <= m_cursor && !disabled(Rule::RESYNC_DUPLICATE_DROP))
    {
        m_counters.duplicates++;
        return;
    }
    const bool gap = seq > m_cursor + 1;
    if (!gap || disabled(Rule::RESYNC_T5_GAP) || noTriggers())
    {
        if (!push(entry))
        {
            m_counters.discarded++;
            enterOverflow(true);
            return;
        }
        if (seq > m_cursor)
        {
            m_cursor = seq;
        }
        return;
    }
    // T5: the gap entry is held (it arrived before the reply that will cover it) and RESYNC(cursor) runs.
    if (!disabled(Rule::RESYNC_GAP_HELD))
    {
        if (m_hold.full())
        {
            m_counters.discarded++;
            enterOverflow(false);
            return;
        }
        m_hold.push(entry);
    }
    startResync(m_cursor, Trigger::T5, nowNs);
}

void ResyncMachine::onPendingEnd(std::int64_t correlationId, std::int32_t count, std::int64_t head,
    std::int64_t nowNs) noexcept
{
    if (!m_requestInFlight || correlationId != m_request.correlationId)
    {
        m_counters.staleEnds++; // an abandoned, re-issued or foreign request's end
        return;
    }
    const RequestKind kind = m_request.kind;
    m_requestInFlight = false;
    m_request.kind = RequestKind::NONE;
    if (RequestKind::PROBE == kind)
    {
        onProbeAnswer(count, head, nowNs);
        return;
    }
    if (Mode::RESYNCING != m_mode)
    {
        m_counters.staleEnds++;
        return;
    }
    finishPage(count, head, nowNs);
}

void ResyncMachine::onProbeAnswer(std::int32_t count, std::int64_t head, std::int64_t nowNs) noexcept
{
    if (0 != count && !disabled(Rule::RESYNC_PROBE_COUNT))
    {
        fail(ResyncVerdict::PRE_S8_CORE, count); // F15: a pre-S8 core answers maxEntries 0 with a page
        return;
    }
    m_lastHead = head;
    m_lastAnswerNs = nowNs;
    m_haveAnswer = true;
    if (Mode::RESYNCING == m_mode)
    {
        m_probeVerified = true; // the C3-entry probe: now the RESYNC(0) pages
        requestPage(nowNs);
        return;
    }
    if (Mode::LIVE != m_mode)
    {
        return;
    }
    if (head > m_cursor && !disabled(Rule::RESYNC_T7) && !noTriggers())
    {
        m_counters.headProbeBehind++;
        startResync(m_cursor, Trigger::T7, nowNs);
        return;
    }
    m_counters.headProbeCovered++;
    m_nextProbeNs = nowNs + HEAD_PROBE_INTERVAL_NS;
    m_probeScheduled = true;
}

void ResyncMachine::finishPage(std::int32_t count, std::int64_t head, std::int64_t nowNs) noexcept
{
    // S1: count within the page and within what was held.
    if (!disabled(Rule::RESYNC_SHAPE_S1) &&
        (count < 0 || count > PAGE_ENTRIES || static_cast<std::size_t>(count) > m_hold.size()))
    {
        shapeViolation(ShapeCheck::S1, nowNs);
        return;
    }
    std::size_t n = count < 0 ? 0 : static_cast<std::size_t>(count);
    if (n > m_hold.size())
    {
        n = m_hold.size();
    }
    const bool fromFront = disabled(Rule::RESYNC_REPLY_LAST);
    const std::size_t start = fromFront ? 0 : m_hold.size() - n;

    // S2: strictly ascending, each above pageFrom and at or below head.
    if (!disabled(Rule::RESYNC_SHAPE_S2))
    {
        std::int64_t previous = m_pageFrom;
        for (std::size_t i = 0; i < n; i++)
        {
            const std::int64_t seq = m_hold.at(start + i).dispatch.dispatchSeq;
            if (seq <= previous || seq > head)
            {
                shapeViolation(ShapeCheck::S2, nowNs);
                return;
            }
            previous = seq;
        }
    }

    // Push the block in order; a full ring stops here (6.3 step 6) with the cursor at the last push.
    for (std::size_t i = 0; i < n; i++)
    {
        const HeldEntry &entry = m_hold.at(start + i);
        if (!push(entry))
        {
            m_counters.discarded += n - i; // the unpushed rest of the block; a later RESYNC(cursor) re-asks
            if (fromFront)
            {
                m_hold.popFront(n);
            }
            else
            {
                m_hold.popBack(n);
            }
            enterOverflow(true);
            return;
        }
        if (entry.dispatch.dispatchSeq > m_cursor)
        {
            m_cursor = entry.dispatch.dispatchSeq;
        }
    }
    if (ResyncVerdict::NONE != m_verdict)
    {
        return;
    }
    if (fromFront)
    {
        m_hold.popFront(n);
    }
    else
    {
        m_hold.popBack(n);
    }
    m_lastHead = head;
    m_counters.pages++;
    m_counters.entries += n;

    if (static_cast<std::int32_t>(n) == PAGE_ENTRIES)
    {
        m_pageFrom = m_cursor;
        requestPage(nowNs);
        return;
    }

    // Final page. S3: under FIFO nothing that arrived before the 108 can be above head.
    if (!disabled(Rule::RESYNC_SHAPE_S3))
    {
        for (std::size_t i = 0; i < m_hold.size(); i++)
        {
            if (m_hold.at(i).dispatch.dispatchSeq > head)
            {
                shapeViolation(ShapeCheck::S3, nowNs);
                return;
            }
        }
    }
    if (disabled(Rule::RESYNC_FINAL_DROP))
    {
        // RED shape (pre FIX-4): "push the rest".
        for (std::size_t i = 0; i < m_hold.size(); i++)
        {
            if (!push(m_hold.at(i)))
            {
                break;
            }
        }
        m_hold.clear();
    }
    else
    {
        // Every remaining held entry was not in a reply, so the query position resolved it.
        m_counters.heldResolved += m_hold.clear();
    }
    if (!disabled(Rule::RESYNC_CURSOR_MAX_HEAD) && head > m_cursor)
    {
        m_cursor = head;
    }
    m_mode = Mode::LIVE;
    m_lastPushedSeq = m_cursor;
    m_lastResyncEndSeq = m_cursor;
    m_lastResyncDurationNs = nowNs - m_resyncStartNs;
    m_lastAnswerNs = nowNs;
    m_haveAnswer = true;
    m_nextProbeNs = nowNs + HEAD_PROBE_INTERVAL_NS;
    m_probeScheduled = true;
}

void ResyncMachine::tick(std::int64_t nowNs) noexcept
{
    // T9: an unanswered request is re-issued with a new correlation id; every core frame restarts the timer.
    if (m_requestInFlight && !disabled(Rule::RESYNC_T9) && !noTriggers())
    {
        std::int64_t base = m_requestSentNs;
        if (m_haveCoreFrame && !disabled(Rule::RESYNC_T9_RESTART) && m_lastCoreFrameNs > base)
        {
            base = m_lastCoreFrameNs;
        }
        if (nowNs - base >= END_TIMEOUT_NS)
        {
            m_counters.timeouts++;
            m_counters.resyncs[static_cast<unsigned>(Trigger::T9)]++;
            m_request.correlationId = ++m_nextCorrelation;
            m_requestDue = true;
            m_requestInFlight = false;
        }
    }
    if (Mode::OVERFLOW_WAIT == m_mode && m_ring.depth() <= RESUME_RING_DEPTH && !disabled(Rule::RESYNC_D3_RESUME))
    {
        if (noTriggers())
        {
            m_mode = Mode::LIVE;
            m_lastPushedSeq = m_cursor;
        }
        else
        {
            startResync(m_cursor, Trigger::T8, nowNs);
        }
        return;
    }
    if (Mode::LIVE == m_mode && m_probeScheduled && !m_requestDue && !m_requestInFlight && nowNs >= m_nextProbeNs &&
        !disabled(Rule::RESYNC_HEAD_PROBE) && !noTriggers())
    {
        m_probeScheduled = false;
        requestProbe(nowNs);
    }
}

}}}
