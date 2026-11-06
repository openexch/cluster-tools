#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TICK_EMITTER_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TICK_EMITTER_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/BookLadder.h"
#include "cluster_tools/domain/forwarder/MarketData.h"

// TickEmitter - the coalescing MarketTick producer of RC2 S13 contract section 6, one slot per market 1..5.
// Pure: it never touches a transport; the duty cycle (step 5a, section 3) calls step() once per touched or due
// market after the whole ME poll and hands in a TickSink whose tryOffer is the core publication. Every rule E1-E11:
//   E1 the candidate comes from an OK ladder only; BROKEN / EMPTY offer nothing (suppressed{broken})
//   E2 a change = top (bid, ask) differs from lastOffered, or a newer trade AND a version move
//   E3 leading edge: a change at now >= windowEnd is offered at once, the window restarts (T_CAP_NS)
//   E4 trailing edge: a change inside the window sets trailingDue; at the window end the THEN-current candidate goes
//   E5 version <= lastOffered.version is never offered (suppressed{version_not_above}), except the E7 re-offer
//   E6 lastOffered / window / counters move on a POSITIVE tryOffer only; a failed offer retries next iteration
//   E7 each (re-)registration re-offers every market once, equal or not, version rule waived
//   E8 the epoch is the caller's (the gate requires MeLink::BOUND; the sink stamps it)
//   E9 the candidate is validated before the offer; a failure breaks the ladder and counts refused
//   E10 at most one offer per T_cap per market except retries and re-offers
//   E11 pending age: the age of a change the emitter may send and has not; 0 while BROKEN, EMPTY or gated
// 6.0: every slot field's set / reset event is in the contract table; lastOffered and windowEnd are never reset
// in-process by design (versions are log positions and grow across ME restarts and failovers).

namespace cluster_tools { namespace domain { namespace forwarder {

enum class TickEdge : std::uint8_t
{
    LEADING = 0,
    TRAILING,
    REOFFER,
    RETRY,
    COUNT
};

enum class TickSuppressed : std::uint8_t
{
    BROKEN = 0,
    UNBOUND,
    UNREGISTERED,
    VERSION_NOT_ABOVE,
    COUNT
};

// The section 3 gate as the agent evaluates it each iteration.
enum class TickGate : std::uint8_t
{
    OPEN = 0,     // REGISTERED, registration not lost, MeLink BOUND, a core leader, no stop requested
    UNREGISTERED, // the core side is not ready (or AWAIT_NEW_LEADER, or stopping)
    UNBOUND       // the ME epoch is not bound
};

const char *nameOf(TickEdge edge) noexcept;
const char *nameOf(TickSuppressed reason) noexcept;

struct TickCandidate
{
    std::int64_t bid = 0;        // best bid price, 0 when the side is empty
    std::int64_t ask = 0;
    std::int64_t tradePrice = 0;
    std::int64_t tradeId = 0;
    std::int64_t version = 0;
};

// The core publication as the emitter sees it. tryOffer returns true when the frame was accepted (positive offer).
class TickSink
{
public:
    virtual ~TickSink() = default;
    virtual bool tryOffer(int market, const TickCandidate &candidate, TickEdge edge) noexcept = 0;
};

struct TickSlotCounters
{
    std::uint64_t offered[static_cast<unsigned>(TickEdge::COUNT)] = {};          // forwarder_tick_offered_total{edge}
    std::uint64_t suppressed[static_cast<unsigned>(TickSuppressed::COUNT)] = {}; // forwarder_tick_suppressed_total{reason}
    std::uint64_t offerFailures = 0;                                             // forwarder_tick_offer_failures_total
    std::uint64_t refused = 0;                                                   // forwarder_tick_refused_total (E9)

    std::uint64_t offeredTotal() const noexcept
    {
        std::uint64_t sum = 0;
        for (auto v : offered)
        {
            sum += v;
        }
        return sum;
    }
    std::uint64_t offeredOn(TickEdge e) const noexcept { return offered[static_cast<unsigned>(e)]; }
    std::uint64_t suppressedFor(TickSuppressed r) const noexcept { return suppressed[static_cast<unsigned>(r)]; }
};

class TickEmitter
{
public:
    static constexpr std::int64_t T_CAP = T_CAP_NS;

    // Step 5a for one market: decide and (at most once) offer. `ladder` is non-const for E9 only.
    void step(int market, BookLadder &ladder, const TradeMark &mark, std::int64_t nowNs, TickGate gate,
        TickSink &sink) noexcept;

    // E7: the core link reached REGISTERED (first registration, a new core session, a re-registration).
    void onRegistered() noexcept;

    // True when the market needs a step this iteration even without a frame: a trailing edge due, a retry or a
    // re-offer pending. The agent steps every touched market plus every due one, in market order.
    bool due(int market, std::int64_t nowNs) const noexcept;

    const TickCandidate &lastOffered(int market) const noexcept { return m_slots[market - 1].lastOffered; }
    bool haveOffered(int market) const noexcept { return m_slots[market - 1].haveOffered; }
    bool trailingDue(int market) const noexcept { return m_slots[market - 1].trailingDue; }
    bool reofferDue(int market) const noexcept { return m_slots[market - 1].reofferDue; }
    bool retryDue(int market) const noexcept { return m_slots[market - 1].retryDue; }
    std::int64_t windowEndNs(int market) const noexcept { return m_slots[market - 1].windowEndNs; }
    // E11: 0 when nothing is pending; else nowNs - firstUnofferedNs.
    std::int64_t pendingAgeNs(int market, std::int64_t nowNs) const noexcept;
    const TickSlotCounters &counters(int market) const noexcept { return m_slots[market - 1].counters; }

private:
    struct Slot
    {
        TickCandidate lastOffered;
        bool haveOffered = false;
        std::int64_t windowEndNs = 0;
        bool trailingDue = false;
        bool reofferDue = false;
        bool retryDue = false;
        bool pending = false;               // E11: firstUnofferedNs is meaningful (a clock value of 0 is legal)
        std::int64_t firstUnofferedNs = 0;
        bool episode[2] = { false, false }; // suppressedEpisode{unregistered, unbound}
        TickCandidate trailingSet;          // RED shape TICK_TRAILING_NEWEST only: the state that set the flag
        TickSlotCounters counters;
    };

    static bool candidateOf(const BookLadder &ladder, const TradeMark &mark, TickCandidate &out) noexcept;
    bool isChange(const Slot &s, const TickCandidate &c) const noexcept;
    bool validate(int market, const TickCandidate &c, BreakCause &cause) const noexcept;
    void offer(int market, Slot &s, const TickCandidate &c, TickEdge edge, std::int64_t nowNs, TickSink &sink) noexcept;

    Slot m_slots[TICK_MARKETS];
};

}}}

#endif
