#include "cluster_tools/domain/forwarder/TickEmitter.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

// RC2 S13 contract section 6 (E1-E11) and the reset table 6.0. Every "RED shape" branch is a test seam that compiles
// to nothing in the production library (TestSeams.h).

namespace cluster_tools { namespace domain { namespace forwarder {

namespace seams = test_seams;

const char *nameOf(TickEdge edge) noexcept
{
    switch (edge)
    {
        case TickEdge::LEADING: return "leading";
        case TickEdge::TRAILING: return "trailing";
        case TickEdge::REOFFER: return "reoffer";
        case TickEdge::RETRY: return "retry";
        case TickEdge::COUNT: break;
    }
    return "?";
}

const char *nameOf(TickSuppressed reason) noexcept
{
    switch (reason)
    {
        case TickSuppressed::BROKEN: return "broken";
        case TickSuppressed::UNBOUND: return "unbound";
        case TickSuppressed::UNREGISTERED: return "unregistered";
        case TickSuppressed::VERSION_NOT_ABOVE: return "version_not_above";
        case TickSuppressed::COUNT: break;
    }
    return "?";
}

namespace {
inline void count(TickSlotCounters &c, TickSuppressed r) noexcept
{
    c.suppressed[static_cast<unsigned>(r)]++;
}
}

// E1: the candidate exists only while the ladder is OK.
bool TickEmitter::candidateOf(const BookLadder &ladder, const TradeMark &mark, TickCandidate &out) noexcept
{
    if (LadderState::OK != ladder.state() && !seams::disabled(seams::Rule::TICK_BROKEN_HOLD))
    {
        return false;
    }
    if (LadderState::EMPTY == ladder.state())
    {
        return false; // even the RED shape has nothing to offer from EMPTY (version 0)
    }
    out.bid = ladder.bestBid();
    out.ask = ladder.bestAsk();
    out.version = ladder.version();
    out.tradePrice = mark.price();
    out.tradeId = mark.tradeId();
    return true;
}

// E2: top changed, or a newer trade together with a version move. The first candidate ever is a change.
bool TickEmitter::isChange(const Slot &s, const TickCandidate &c) const noexcept
{
    if (!s.haveOffered)
    {
        return true;
    }
    if (c.bid != s.lastOffered.bid || c.ask != s.lastOffered.ask)
    {
        return true;
    }
    if (c.tradeId > s.lastOffered.tradeId)
    {
        if (c.version > s.lastOffered.version)
        {
            return true;
        }
        if (seams::disabled(seams::Rule::TICK_TRADE_ONLY_SILENT))
        {
            return true; // RED shape: the RC1 X6 text ("lastTradePrice changed" emits)
        }
    }
    if (seams::disabled(seams::Rule::TICK_TOP_ONLY) && c.version > s.lastOffered.version)
    {
        return true; // RED shape: a deeper-level move emits
    }
    return false;
}

// E9: a failure here is a stitcher defect (the ladder's own rules should have refused the state).
bool TickEmitter::validate(int market, const TickCandidate &c, BreakCause &cause) const noexcept
{
    if (seams::disabled(seams::Rule::TICK_VALIDATE))
    {
        return true;
    }
    if (c.bid > 0 && c.ask > 0 && c.bid >= c.ask)
    {
        cause = BreakCause::CROSSED;
        return false;
    }
    if (c.bid < 0 || c.ask < 0 || c.tradePrice < 0 || c.tradeId < 0 || c.version <= 0 || market < 1 || market > TICK_MARKETS)
    {
        cause = BreakCause::MALFORMED;
        return false;
    }
    return true;
}

// E6: everything moves on a positive offer only. A failed offer keeps the candidate for the next iteration.
void TickEmitter::offer(int market, Slot &s, const TickCandidate &c, TickEdge edge, std::int64_t nowNs, TickSink &sink) noexcept
{
    const bool ok = sink.tryOffer(market, c, edge);
    if (!ok && !seams::disabled(seams::Rule::TICK_CONFIRMED_ONLY))
    {
        s.counters.offerFailures++;
        if (TickEdge::LEADING == edge || TickEdge::RETRY == edge)
        {
            s.retryDue = true; // trailing keeps trailingDue, a re-offer keeps reofferDue
        }
        return;
    }
    if (!ok)
    {
        s.counters.offerFailures++; // RED shape: counted but the slot moves as if confirmed
    }
    s.lastOffered = c;
    s.haveOffered = true;
    // 6.0 / E10 (review F-1): the window starts on a leading, trailing or retry offer; an E7 re-offer carries no new
    // information and is outside the rate, so it leaves the window as it was
    const bool startsWindow = TickEdge::REOFFER != edge || seams::disabled(seams::Rule::TICK_REOFFER_OUTSIDE_RATE);
    if (startsWindow && !seams::disabled(seams::Rule::TICK_RATE_CAP))
    {
        s.windowEndNs = nowNs + T_CAP;
    }
    s.trailingDue = false;
    s.retryDue = false;
    s.reofferDue = false;
    s.pending = false;
    s.counters.offered[static_cast<unsigned>(edge)]++;
}

void TickEmitter::step(int market, BookLadder &ladder, const TradeMark &mark, std::int64_t nowNs, TickGate gate,
    TickSink &sink) noexcept
{
    if (market < 1 || market > TICK_MARKETS)
    {
        return;
    }
    Slot &s = m_slots[market - 1];
    const bool pendingAgeGated = !seams::disabled(seams::Rule::TICK_PENDING_AGE_GATED);

    // E1 / 6.0: no candidate from BROKEN or EMPTY; EMPTY also drops a pending trailing edge
    TickCandidate c;
    if (!candidateOf(ladder, mark, c))
    {
        if (LadderState::EMPTY == ladder.state())
        {
            s.trailingDue = false;
            s.retryDue = false;
        }
        if (s.trailingDue || s.reofferDue || s.retryDue)
        {
            count(s.counters, TickSuppressed::BROKEN); // a step that would otherwise have offered
        }
        if (pendingAgeGated)
        {
            s.pending = false;
        }
        return;
    }

    // E9 before anything else: a candidate the stitcher cannot vouch for breaks the ladder
    BreakCause cause = BreakCause::NONE;
    if (!validate(market, c, cause))
    {
        s.counters.refused++;
        ladder.breakFor(cause, nowNs);
        if (pendingAgeGated)
        {
            s.pending = false;
        }
        return;
    }

    // the section 3 gate: hold the candidate, count once per episode, no pending-age clock
    if (TickGate::OPEN != gate)
    {
        const unsigned which = TickGate::UNREGISTERED == gate ? 0u : 1u;
        if (!s.episode[which])
        {
            s.episode[which] = true;
            count(s.counters, TickGate::UNREGISTERED == gate ? TickSuppressed::UNREGISTERED : TickSuppressed::UNBOUND);
        }
        if (pendingAgeGated)
        {
            s.pending = false;
        }
        return;
    }
    if (!seams::disabled(seams::Rule::TICK_EPISODE_RESET))
    {
        s.episode[0] = false;
        s.episode[1] = false;
    }

    // E7: the re-offer goes first, equal or not, version rule waived, rate exempt
    if (s.reofferDue)
    {
        offer(market, s, c, TickEdge::REOFFER, nowNs, sink);
        return;
    }

    // E2
    if (!isChange(s, c))
    {
        s.trailingDue = false; // the then-current candidate is no longer a change (E4)
        s.retryDue = false;
        s.pending = false;
        return;
    }

    // E5: never below or at the offered version (the core would count it stale); the RED shape of a trade-only
    // change (TICK_TRADE_ONLY_SILENT off) bypasses it like the pre-decision rule did
    const bool tradeOnlyShape = seams::disabled(seams::Rule::TICK_TRADE_ONLY_SILENT) && c.bid == s.lastOffered.bid &&
        c.ask == s.lastOffered.ask && c.version <= s.lastOffered.version;
    if (c.version <= s.lastOffered.version && s.haveOffered && !tradeOnlyShape &&
        !seams::disabled(seams::Rule::TICK_VERSION_ABOVE))
    {
        count(s.counters, TickSuppressed::VERSION_NOT_ABOVE);
        s.trailingDue = false;
        s.retryDue = false;
        s.pending = false;
        return;
    }

    // E11: a change the emitter may send and has not, from the first step that saw it
    if (!s.pending)
    {
        s.pending = true;
        s.firstUnofferedNs = nowNs;
    }

    // E6 retry: the newest state, at once
    if (s.retryDue)
    {
        offer(market, s, c, TickEdge::RETRY, nowNs, sink);
        return;
    }

    // E3 / E4
    if (nowNs >= s.windowEndNs && !seams::disabled(seams::Rule::TICK_LEADING_EDGE))
    {
        const TickEdge edge = s.trailingDue ? TickEdge::TRAILING : TickEdge::LEADING;
        const TickCandidate &what = (TickEdge::TRAILING == edge && seams::disabled(seams::Rule::TICK_TRAILING_NEWEST)) ? s.trailingSet : c;
        offer(market, s, what, edge, nowNs, sink);
        return;
    }
    if (seams::disabled(seams::Rule::TICK_LEADING_EDGE) && nowNs >= s.windowEndNs && s.trailingDue)
    {
        offer(market, s, c, TickEdge::TRAILING, nowNs, sink); // RED shape: only the trailing edge ever offers
        return;
    }
    if (seams::disabled(seams::Rule::TICK_TRAILING_EDGE))
    {
        s.pending = false;
        return; // RED shape: a change inside the window is dropped
    }
    if (!s.trailingDue)
    {
        s.trailingSet = c;
    }
    s.trailingDue = true;
}

void TickEmitter::onRegistered() noexcept
{
    if (seams::disabled(seams::Rule::TICK_REOFFER_ON_REGISTER))
    {
        return;
    }
    for (Slot &s : m_slots)
    {
        if (s.haveOffered)
        {
            s.reofferDue = true; // kept across BROKEN and EMPTY until its positive offer (6.0)
        }
    }
}

bool TickEmitter::due(int market, std::int64_t nowNs) const noexcept
{
    if (market < 1 || market > TICK_MARKETS)
    {
        return false;
    }
    const Slot &s = m_slots[market - 1];
    return s.reofferDue || s.retryDue || (s.trailingDue && nowNs >= s.windowEndNs);
}

std::int64_t TickEmitter::pendingAgeNs(int market, std::int64_t nowNs) const noexcept
{
    if (market < 1 || market > TICK_MARKETS)
    {
        return 0;
    }
    const Slot &s = m_slots[market - 1];
    if (!s.pending)
    {
        return 0;
    }
    const std::int64_t age = nowNs - s.firstUnofferedNs;
    return age > 0 ? age : 0;
}

}}}
