// StitcherPropertyTest: the real BookStitcher and TickEmitter
// driven by the ME model of MePublisherModel.h with the faults of 11.2, judged against the model's history and
// the lossy core model.
//
// Model per seed: a 3-node ME whose nodes apply one shared log (each node its own books and MarketPublisher chains:
// a follower's chain advances while its frames are discarded); the leader's publishers encode every 20 ms
// (each node's scheduler thread on its own phase), the leader's lossy market-data queue is drained every 250 ms into
// a burst per ME session that reaches the forwarder's poll 1..8 polls later (the 11 timing rule; polls are 5 ms).
// Faults: lossy drops (single frames and queue-full bursts, snapshots included), below-top mutations (a 40-level
// book, 20 visible), periodic 10 s resnapshots (node-local lastPeriodic as ACS:148), session-open resnapshots, ME
// failover to a follower whose chain was sampled at other moments and that may lag the log (on in most seeds,
// off in some), a drained book (frames), forwarder ME session loss, core offer back-pressure, core leader
// changes that lose the last 1..3 positively offered ticks (RC-F7) followed by a re-registration, never-touched
// markets (and in some seeds a pre-guard ME that sends them as version-0 empty snapshots: rule U), N = 2 forwarders
// in half the seeds. Old-term frames are dropped as the agent's step 4 does, the gate is the
// gate as the agent evaluates it.
//
// Properties: S1 an OK ladder equals the model's top-20 at the ladder's version; S2 every offered tick equals the
// model top at its version, is never crossed, and versions per market per forwarder strictly increase except E7
// re-offers; S3 no offer from a BROKEN or EMPTY ladder; S4 liveness: once the model is quiet and a forwarder's ladder
// holds the final top, the core holds it within T_cap plus two polls (plus the re-registration after a tail core
// leader change); S5 offers of one market are at least T_cap apart except retries and re-offers; S6 at N = 2 the
// core's final quote equals the N = 1 run's on the same frames and at most one tick per version was applied.
// Non-vacuity (C5, C6): the test fails when any fault or recovery path never ran at >= 1000 seeds.
#include "doctest/doctest.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "MePublisherModel.h"
#include "cluster_tools/domain/forwarder/BookLadder.h"
#include "cluster_tools/domain/forwarder/TickEmitter.h"

using namespace cluster_tools::domain::forwarder;
using namespace cluster_tools::domain::forwarder::me_model;

namespace {

constexpr std::int64_t MS = 1'000'000LL;
constexpr std::int64_t POLL_NS = 5 * MS;          // one duty iteration of the model
constexpr std::int64_t PUB_NS = 20 * MS;          // MP:34 FLUSH_INTERVAL_MS
constexpr std::int64_t FLUSH_NS = 250 * MS;       // ACS:1113 MARKET_DATA_FLUSH_INTERVAL_MS
constexpr std::int64_t PERIODIC_NS = 10'000 * MS; // ACS:147 periodic resnapshot
constexpr std::int64_t NEVER_LED = -(1LL << 60);  // lastPeriodic of a node that never led: its first leader flush bumps
constexpr int NODES = 3;
constexpr int MAX_FWD = 2;
constexpr int SLOT = TICK_MARKETS + 1;

struct Rng
{
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
    std::uint64_t next()
    {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    bool chance(unsigned percent) { return next() % 100 < percent; }
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
    std::int64_t between(std::int64_t lo, std::int64_t hi) { return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo + 1))); }
};

// event times land on a poll (the model's duty iteration), never between two
std::int64_t pollAlignedMs(std::int64_t ms)
{
    return (ms / (POLL_NS / MS)) * POLL_NS;
}

enum Stat
{
    ST_BREAKS = 0,
    ST_RECOVERIES,
    ST_FAILOVERS,
    ST_FAILOVERS_NO_ME1_BROKE,
    ST_EMPTY_TRANSITIONS,
    ST_TRAILING,
    ST_LEADING,
    ST_REGRESSIONS,
    ST_REOFFERS,
    ST_OFFER_FAILURES,
    ST_LOST_REPAIRED,
    // informational
    ST_RETRIES,
    ST_STALE_TERM_DROPS,
    ST_UNVERSIONED,
    ST_CORE_STALE,
    ST_CORE_APPLIED,
    ST_FRAMES,
    ST_COMMANDS,
    ST_SESSION_LOSSES,
    ST_CORE_LEADER_CHANGES,
    ST_DROPPED,
    ST_N2_SEEDS,
    ST_COUNT
};

const char *statName(int i)
{
    static const char *names[ST_COUNT] = { "breaks", "recoveries", "failovers", "failoversWithoutMe1ThatBrokeTheChain",
        "emptyTransitions", "trailingEdges", "leadingEdges", "regressions", "reoffers", "offerFailures",
        "ticksLostAtCoreLeaderChangeRepairedByReoffer", "retries", "staleTermDrops", "unversioned", "coreStale",
        "coreApplied", "frames", "commands", "sessionLosses", "coreLeaderChanges", "dropped", "n2Seeds" };
    return names[i];
}
constexpr int NON_VACUITY_COUNT = ST_LOST_REPAIRED + 1;

struct Egress
{
    bool trade = false;
    std::int64_t term = 0;
    BookFrame book;
    TradeFrame tf;
};
using Burst = std::shared_ptr<const std::vector<Egress>>;


struct CoreChange
{
    std::int64_t atNs;
    int k; // 0 = a re-registration without loss (a core session loss), else the RC-F7 loss
};

// Every knob of a seed. The ME and core knobs come from their own streams so an N = 1 and an N = 2 run of one seed see
// the same frames and the same core events (S6); forwarder B never loses its ME session for the same reason (a
// session open bumps the ME's generation).
struct Params
{
    int nFwd = 1;
    bool wme1 = true;
    bool guardOff = false;
    bool faults = true;
    unsigned dropPct = 0;
    bool burstDrops = false;
    bool active[SLOT] = {};
    std::int64_t tQuiet = 0;
    std::int64_t tEnd = 0;
    unsigned cmdMax = 1;
    std::int64_t drainFrom = -1, drainTo = -1;
    int drainMarket = 0;
    std::vector<std::int64_t> failovers;
    std::vector<CoreChange> coreChanges;
    std::vector<std::int64_t> sessionLosses; // forwarder A only
    unsigned bpPct = 0;
    std::int64_t phaseNs[NODES] = {};
    bool ingressDrain = false; // ACS:1738-1740: under ingress the leader drains after every session message (RC-N1)
    bool stopShape = false;    // a failover by SIGSTOP: the old leader's queued egress arrives late
};

bool traceEnabled()
{
    const char *env = std::getenv("CT_STITCHER_PROPERTY_TRACE"); // 1 = the failure message carries the event trace
    return nullptr != env && std::atoi(env) != 0;
}

bool faultsEnabled()
{
    const char *env = std::getenv("CT_STITCHER_PROPERTY_FAULTS");
    return nullptr == env || std::atoi(env) != 0;
}

Params paramsOf(std::uint64_t seed, int forcedFwd)
{
    Rng me(seed * 4 + 1), core(seed * 4 + 2), fa(seed * 4 + 3);
    Params p;
    p.faults = faultsEnabled();
    p.nFwd = forcedFwd > 0 ? forcedFwd : (seed % 2 == 0 ? 2 : 1);
    p.wme1 = !p.faults || !me.chance(25);
    p.guardOff = p.faults && me.chance(15);
    p.dropPct = p.faults && me.chance(55) ? static_cast<unsigned>(1 + me.below(4)) : 0;
    p.burstDrops = p.faults && me.chance(30);
    int nActive = 0;
    for (int m = 1; m <= TICK_MARKETS; m++)
    {
        p.active[m] = me.chance(75);
        nActive += p.active[m] ? 1 : 0;
    }
    if (0 == nActive)
    {
        p.active[1 + static_cast<int>(me.below(TICK_MARKETS))] = true;
    }
    p.tQuiet = me.between(6, 16) * 1000 * MS;
    p.tEnd = p.tQuiet + 13'000 * MS;
    p.cmdMax = static_cast<unsigned>(1 + me.below(3));
    p.ingressDrain = me.chance(50);
    p.stopShape = p.faults && me.chance(50);
    for (int n = 0; n < NODES; n++)
    {
        p.phaseNs[n] = static_cast<std::int64_t>(me.below(4)) * POLL_NS;
    }
    if (me.chance(45))
    {
        std::vector<int> act;
        for (int m = 1; m <= TICK_MARKETS; m++)
        {
            if (p.active[m])
            {
                act.push_back(m);
            }
        }
        p.drainMarket = act[me.below(act.size())];
        p.drainFrom = me.between(1000, p.tQuiet / MS - 2500) * MS;
        p.drainTo = p.drainFrom + me.between(800, 2000) * MS;
    }
    if (p.faults)
    {
        const int nFail = me.chance(70) ? static_cast<int>(1 + me.below(3)) : 0;
        for (int i = 0; i < nFail; i++)
        {
            p.failovers.push_back(pollAlignedMs(me.between(1000, p.tQuiet / MS - 1000)));
        }
        const int nCore = core.chance(70) ? static_cast<int>(1 + core.below(2)) : 0;
        for (int i = 0; i < nCore; i++)
        {
            p.coreChanges.push_back(CoreChange{ pollAlignedMs(core.between(500, p.tQuiet / MS - 1000)), core.chance(80) ? static_cast<int>(1 + core.below(3)) : 0 });
        }
        if (core.chance(35))
        {
            p.coreChanges.push_back(CoreChange{ p.tQuiet + pollAlignedMs(core.between(1000, 3000)), static_cast<int>(1 + core.below(3)) });
        }
        const int nLoss = fa.chance(50) ? static_cast<int>(1 + fa.below(2)) : 0;
        for (int i = 0; i < nLoss; i++)
        {
            p.sessionLosses.push_back(pollAlignedMs(fa.between(500, p.tQuiet / MS - 500)));
        }
        p.bpPct = fa.chance(60) ? static_cast<unsigned>(1 + fa.below(4)) : 0;
    }
    return p;
}

class Run;

class FwdSink final : public TickSink
{
public:
    Run *run = nullptr;
    int fwd = 0;
    bool tryOffer(int market, const TickCandidate &candidate, TickEdge edge) noexcept override;
};

struct Fwd
{
    BookStitcher st;
    TickEmitter em;
    FwdSink sink;
    Rng rng{ 0 };
    bool meSession = true;
    std::int64_t sessionBackNs = -1;
    std::int64_t knownTerm = 0;
    TickGate gate = TickGate::OPEN;
    std::int64_t registerAtNs = -1;
    std::multimap<std::int64_t, Burst> inbox; // due poll -> burst (bursts of one poll in arrival order)
    std::int64_t lastDueNs = 0;                 // one session never reorders: a later burst is never due before an earlier one
    bool touched[SLOT] = {};
    // S1 / S4 tracking
    bool prevOk[SLOT] = {};
    std::int64_t prevBid[SLOT] = {};
    std::int64_t prevAsk[SLOT] = {};
    std::int64_t lastTopEventNs[SLOT] = {};
    bool prevNonEmpty[SLOT] = {};
    std::uint64_t prevFromMismatch[SLOT] = {};
    // S2 / S5 tracking
    std::int64_t lastEdgeOfferNs[SLOT];
    std::int64_t lastVersionOffered[SLOT] = {}; // every positive offer, re-offers included (S2's baseline)

    Fwd()
    {
        for (int m = 0; m < SLOT; m++)
        {
            lastEdgeOfferNs[m] = NEVER_LED;
        }
    }
};

struct Node
{
    MarketBook book[SLOT];
    MePublisher pub[SLOT];
    std::size_t applied = 0;
    std::int64_t gen = 0;
    std::int64_t lastPeriodicNs = NEVER_LED;
    std::int64_t pendingTradeId[SLOT] = {};
    std::int64_t pendingTradePrice[SLOT] = {};
    std::int64_t stalledUntilNs = -1; // a follower that pauses (a GC pause shape) so a promoted node can lag the log
};

class Run
{
public:
    Run(std::uint64_t seed, int forcedFwd) : m_seed(seed), m_p(paramsOf(seed, forcedFwd)), m_me(seed * 4 + 11)
    {
        for (int f = 0; f < MAX_FWD; f++)
        {
            m_fwd[f].rng = Rng(seed * 4 + 13 + static_cast<std::uint64_t>(f));
            m_fwd[f].sink.run = this;
            m_fwd[f].sink.fwd = f;
        }
        for (int n = 0; n < NODES; n++)
        {
            for (int m = 1; m <= TICK_MARKETS; m++)
            {
                m_nodes[n].pub[m].emptyBookVersionGuard = !m_p.guardOff;
            }
        }
        for (int m = 0; m < SLOT; m++)
        {
            m_coreLastChangeNs[m] = 0;
        }
    }

    const std::string &failure() const { return m_failure; }
    const std::uint64_t *stats() const { return m_stats; }
    const Params &params() const { return m_p; }
    const LossyCoreModel &core() const { return m_core_; }

    void run()
    {
        // every forwarder opens its ME session at start: a resnapshot per open (ACS:1613-1616); first registration (E7)
        for (int f = 0; f < m_p.nFwd; f++)
        {
            m_nodes[m_leader].gen++;
            m_fwd[f].em.onRegistered();
        }
        for (m_now = 0; m_now <= m_p.tEnd && m_failure.empty(); m_now += POLL_NS)
        {
            events();
            mint();
            catchUp();
            publisherFlushes();
            leaderFlush();
            for (int f = 0; f < m_p.nFwd; f++)
            {
                pollForwarder(f);
            }
        }
        if (m_failure.empty())
        {
            judgeLiveness();
        }
        if (m_p.nFwd == 2)
        {
            m_stats[ST_N2_SEEDS]++;
        }
    }

    // the sink's view
    std::int64_t now() const { return m_now; }
    void fail(const std::string &what)
    {
        if (m_failure.empty())
        {
            std::ostringstream o;
            o << what << " (seed " << m_seed << ", t=" << m_now / MS << " ms, nFwd " << m_p.nFwd << ")";
            if (m_tracing)
            {
                o << "\n" << m_trace.str();
            }
            m_failure = o.str();
        }
    }
    template <class F>
    void trace(F &&line)
    {
        if (m_tracing)
        {
            m_trace << m_now / MS << " ";
            line(m_trace);
            m_trace << "\n";
        }
    }
    Fwd &fwd(int f) { return m_fwd[f]; }
    const PublishedTop *historyAt(int market, std::int64_t version) const
    {
        auto it = m_history[market].find(version);
        return it == m_history[market].end() ? nullptr : &it->second;
    }
    bool backPressure(int f)
    {
        return m_p.bpPct > 0 && m_now < m_p.tQuiet && m_fwd[f].rng.chance(m_p.bpPct);
    }
    void coreOffer(int f, int market, const TickCandidate &c, TickEdge edge)
    {
        (void)f;
        const LossyCoreModel::Market before = m_core_.market(market);
        const LossyCoreModel::Verdict v = m_core_.offer(market, c);
        trace([&](std::ostream &t) { t << "fwd" << f << " m" << market << " offer " << nameOf(edge) << " " << c.bid << "/" << c.ask << " v" << c.version << " -> " << (LossyCoreModel::Verdict::APPLIED == v ? "applied" : "stale"); });
        if (LossyCoreModel::Verdict::INVALID == v)
        {
            fail("S2: the core counted an offered tick invalid");
            return;
        }
        if (LossyCoreModel::Verdict::APPLIED == v)
        {
            m_stats[ST_CORE_APPLIED]++;
            if (m_core_.duplicateApplied())
            {
                fail("S6: the core applied two ticks of one version");
            }
            const LossyCoreModel::Market &after = m_core_.market(market);
            if (after.bestBid != before.bestBid || after.bestAsk != before.bestAsk)
            {
                m_coreLastChangeNs[market] = m_now;
            }
            if (TickEdge::REOFFER == edge && m_lostMarkets.erase(market) > 0)
            {
                m_stats[ST_LOST_REPAIRED]++;
            }
        }
        else
        {
            m_stats[ST_CORE_STALE]++;
        }
    }
    void countEdge(TickEdge edge)
    {
        switch (edge)
        {
            case TickEdge::LEADING: m_stats[ST_LEADING]++; break;
            case TickEdge::TRAILING: m_stats[ST_TRAILING]++; break;
            case TickEdge::REOFFER: m_stats[ST_REOFFERS]++; break;
            case TickEdge::RETRY: m_stats[ST_RETRIES]++; break;
            case TickEdge::COUNT: break;
        }
    }

private:
    // ---- scheduled faults ----
    void events()
    {
        for (std::int64_t t : m_p.failovers)
        {
            if (t == m_now)
            {
                failover();
            }
        }
        for (const CoreChange &c : m_p.coreChanges)
        {
            if (c.atNs == m_now)
            {
                coreLeaderChange(c.k);
            }
        }
        for (std::int64_t t : m_p.sessionLosses)
        {
            if (t == m_now && m_fwd[0].meSession)
            {
                Fwd &f = m_fwd[0];
                f.meSession = false;
                f.st.onMeSessionEnd(m_now);
                f.inbox.clear();
                f.lastDueNs = 0;
                f.gate = TickGate::UNBOUND;
                f.sessionBackNs = m_now + f.rng.between(2, 40) * POLL_NS;
                m_stats[ST_SESSION_LOSSES]++;
                for (int m = 1; m <= TICK_MARKETS; m++)
                {
                    noteTop(f, m);
                }
            }
        }
        for (int f = 0; f < m_p.nFwd; f++)
        {
            Fwd &w = m_fwd[f];
            if (!w.meSession && w.sessionBackNs == m_now)
            {
                w.meSession = true;
                w.gate = w.registerAtNs >= 0 ? TickGate::UNREGISTERED : TickGate::OPEN;
                m_nodes[m_leader].gen++; // the session open triggers a resnapshot
            }
            if (w.registerAtNs == m_now)
            {
                w.registerAtNs = -1;
                w.gate = w.meSession ? TickGate::OPEN : TickGate::UNBOUND;
                w.em.onRegistered();
                trace([&](std::ostream &t) { t << "fwd" << f << " registered"; });
                m_lastReRegisterNs = m_now;
            }
        }
    }

    void failover()
    {
        m_stats[ST_FAILOVERS]++;
        trace([&](std::ostream &t) { t << "ME failover term " << m_term + 1 << (m_p.wme1 ? " (W-ME1)" : " (no W-ME1)"); });
        if (m_p.stopShape && !(m_omsQueue.empty() && m_mdQueue.empty()))
        {
            // SIGSTOP shape: egress the old leader wrote before the stop reaches the sessions after the new leader's
            // first frames; the forwarder's step 4 term filter must drop it
            auto late = std::make_shared<std::vector<Egress>>();
            late->insert(late->end(), m_omsQueue.begin(), m_omsQueue.end());
            late->insert(late->end(), m_mdQueue.begin(), m_mdQueue.end());
            for (int f = 0; f < m_p.nFwd; f++)
            {
                if (m_fwd[f].meSession)
                {
                    m_fwd[f].inbox.emplace(m_now + m_fwd[f].rng.between(20, 120) * POLL_NS, late);
                }
            }
        }
        m_mdQueue.clear(); // otherwise the old leader dies before its next flush; bursts already on the wire still arrive
        m_omsQueue.clear();
        int next = m_leader;
        while (next == m_leader)
        {
            next = static_cast<int>(m_me.below(NODES));
        }
        m_leader = next;
        m_term++;
        if (m_p.wme1)
        {
            m_nodes[m_leader].gen++; // requestResnapshot() after isLeader = true
        }
        else
        {
            m_noMe1WindowEndNs = m_now + 1500 * MS;
            m_noMe1Counted = false;
        }
    }

    void coreLeaderChange(int k)
    {
        m_stats[ST_CORE_LEADER_CHANGES]++;
        trace([&](std::ostream &t) { t << "core leader change k=" << k; });
        if (k > 0)
        {
            for (int m : m_core_.leaderChange(k))
            {
                m_lostMarkets.insert(m);
                m_coreLastChangeNs[m] = m_now;
            }
        }
        for (int f = 0; f < m_p.nFwd; f++)
        {
            Fwd &w = m_fwd[f];
            w.gate = TickGate::UNREGISTERED;
            w.registerAtNs = m_now + w.rng.between(1, 3) * POLL_NS;
        }
    }

    // ---- the ME ----
    void mint()
    {
        m_mintedThisPoll = 0;
        Node &L = m_nodes[m_leader];
        advance(L, 64);
        if (L.applied < m_log.size() || m_now >= m_p.tQuiet)
        {
            return;
        }
        const bool drain = m_p.drainMarket > 0 && m_now >= m_p.drainFrom && m_now < m_p.drainTo;
        const unsigned n = static_cast<unsigned>(m_me.below(m_p.cmdMax + 1));
        for (unsigned i = 0; i < n; i++)
        {
            Command c;
            if (drain)
            {
                c.market = m_p.drainMarket;
                c.op = Op::FILL;
                c.bidSide = m_me.chance(50);
                c.qty = 1000;
            }
            else
            {
                do
                {
                    c.market = 1 + static_cast<int>(m_me.below(TICK_MARKETS));
                } while (!m_p.active[c.market]);
                const unsigned r = static_cast<unsigned>(m_me.below(100));
                c.op = r < 45 ? Op::ADD : (r < 80 ? Op::CANCEL : Op::FILL);
                c.bidSide = m_me.chance(50);
                const std::int64_t idx = m_me.chance(50) ? static_cast<std::int64_t>(m_me.below(40)) : static_cast<std::int64_t>(m_me.below(4));
                c.price = c.bidSide ? 1390 - 10 * idx : 1400 + 10 * idx;
                c.qty = Op::FILL == c.op ? m_me.between(1, 15) : m_me.between(1, 20);
            }
            const std::int64_t pos = m_nextLogPos;
            std::int64_t tradePrice = 0;
            if (!L.book[c.market].apply(c, pos, tradePrice))
            {
                continue; // a no-op command: rejected by the ME, never logged
            }
            m_nextLogPos += 1 + static_cast<std::int64_t>(m_me.below(20));
            m_log.push_back(c);
            m_logPos.push_back(pos);
            L.applied = m_log.size();
            m_mintedThisPoll++;
            m_history[c.market][L.book[c.market].bookVersion()] = L.book[c.market].published();
            m_stats[ST_COMMANDS]++;
            if (tradePrice > 0)
            {
                L.pendingTradeId[c.market] = m_nextTradeId++;
                L.pendingTradePrice[c.market] = tradePrice;
            }
        }
    }

    void advance(Node &n, std::size_t maxN)
    {
        std::size_t done = 0;
        while (n.applied < m_log.size() && done < maxN)
        {
            const Command &c = m_log[n.applied];
            std::int64_t tp = 0;
            n.book[c.market].apply(c, m_logPos[n.applied], tp);
            n.applied++;
            done++;
        }
    }

    void catchUp()
    {
        for (int i = 0; i < NODES; i++)
        {
            if (i == m_leader)
            {
                continue;
            }
            Node &n = m_nodes[i];
            if (m_now < n.stalledUntilNs)
            {
                continue;
            }
            if (m_now < m_p.tQuiet && m_me.chance(2))
            {
                n.stalledUntilNs = m_now + m_me.between(20, 120) * POLL_NS;
                continue;
            }
            advance(n, m_me.below(m_p.cmdMax * 2 + 1));
        }
    }

    void publisherFlushes()
    {
        for (int i = 0; i < NODES; i++)
        {
            Node &n = m_nodes[i];
            if ((m_now + m_p.phaseNs[i]) % PUB_NS != 0)
            {
                continue;
            }
            const bool leader = i == m_leader;
            for (int m = 1; m <= TICK_MARKETS; m++)
            {
                if (n.pendingTradeId[m] > 0)
                {
                    if (leader)
                    {
                        Egress e;
                        e.trade = true;
                        e.term = m_term;
                        e.tf = book_fixtures::trades(m, n.pendingTradeId[m], n.pendingTradePrice[m]);
                        m_omsQueue.push_back(e);
                    }
                    n.pendingTradeId[m] = 0;
                }
                Egress e;
                if (!n.pub[m].flush(n.gen, m, n.book[m].published(), e.book))
                {
                    continue;
                }
                if (!leader)
                {
                    continue; // ACS:360-367 followerSkipped: the chain advanced, the frame is gone
                }
                e.term = m_term;
                if (m_now < m_p.tQuiet && m_p.dropPct > 0 && m_me.chance(m_p.dropPct))
                {
                    m_stats[ST_DROPPED]++;
                    continue; // ACS:373-383 the lossy queue drops
                }
                m_mdQueue.push_back(e);
            }
        }
    }

    void leaderFlush()
    {
        const bool timer = m_now % FLUSH_NS == 0;
        if (!timer && !(m_p.ingressDrain && m_mintedThisPoll > 0))
        {
            return;
        }
        Node &L = m_nodes[m_leader];
        if (timer && m_now - L.lastPeriodicNs >= PERIODIC_NS) // ACS:1236-1243, node-local, on the flush timer
        {
            L.lastPeriodicNs = m_now;
            L.gen++;
        }
        if (m_now < m_p.tQuiet && m_p.burstDrops && m_me.chance(3) && !m_mdQueue.empty())
        {
            const std::size_t keep = m_me.below(m_mdQueue.size());
            m_stats[ST_DROPPED] += m_mdQueue.size() - keep;
            m_mdQueue.resize(keep); // queue full for a moment: a run of frames lost
        }
        if (m_omsQueue.empty() && m_mdQueue.empty())
        {
            return;
        }
        auto burst = std::make_shared<std::vector<Egress>>();
        burst->insert(burst->end(), m_omsQueue.begin(), m_omsQueue.end()); // OMS lane first (ACS:448-449)
        burst->insert(burst->end(), m_mdQueue.begin(), m_mdQueue.end());
        m_omsQueue.clear();
        m_mdQueue.clear();
        for (int f = 0; f < m_p.nFwd; f++)
        {
            Fwd &w = m_fwd[f];
            if (!w.meSession)
            {
                continue;
            }
            w.lastDueNs = std::max(w.lastDueNs, m_now + w.rng.between(1, 8) * POLL_NS);
            w.inbox.emplace(w.lastDueNs, burst);
        }
    }

    // ---- the forwarder ----
    void noteTop(Fwd &w, int m)
    {
        const BookLadder &l = w.st.ladder(m);
        const bool ok = LadderState::OK == l.state();
        const std::int64_t bid = ok ? l.bestBid() : -1, ask = ok ? l.bestAsk() : -1;
        if (ok != w.prevOk[m] || bid != w.prevBid[m] || ask != w.prevAsk[m])
        {
            w.lastTopEventNs[m] = m_now;
            w.prevOk[m] = ok;
            w.prevBid[m] = bid;
            w.prevAsk[m] = ask;
        }
        const bool nonEmpty = ok && l.bidCount() + l.askCount() > 0;
        if (ok && !nonEmpty && w.prevNonEmpty[m])
        {
            m_stats[ST_EMPTY_TRANSITIONS]++;
        }
        w.prevNonEmpty[m] = nonEmpty;
    }

    void checkS1(Fwd &w, int m)
    {
        const BookLadder &l = w.st.ladder(m);
        if (LadderState::OK != l.state())
        {
            return;
        }
        const PublishedTop *h = historyAt(m, l.version());
        std::ostringstream o;
        if (nullptr == h)
        {
            o << "S1: an OK ladder differs from the model at version " << l.version() << " (market " << m << ": the model never published that version)";
            fail(o.str());
            return;
        }
        bool same = l.bidCount() == static_cast<int>(h->bids.size()) && l.askCount() == static_cast<int>(h->asks.size());
        for (int i = 0; same && i < l.bidCount(); i++)
        {
            same = l.bid(i).price == h->bids[i].price && l.bid(i).quantity == h->bids[i].quantity && l.bid(i).orderCount == h->bids[i].orderCount;
        }
        for (int i = 0; same && i < l.askCount(); i++)
        {
            same = l.ask(i).price == h->asks[i].price && l.ask(i).quantity == h->asks[i].quantity && l.ask(i).orderCount == h->asks[i].orderCount;
        }
        if (!same)
        {
            o << "S1: an OK ladder differs from the model at version " << l.version() << " (market " << m << ", ladder " << l.bidCount() << "x" << l.askCount()
              << " levels, model " << h->bids.size() << "x" << h->asks.size() << ")";
            fail(o.str());
        }
    }

    void pollForwarder(int f)
    {
        Fwd &w = m_fwd[f];
        while (!w.inbox.empty() && w.inbox.begin()->first <= m_now)
        {
            Burst burst = w.inbox.begin()->second;
            w.inbox.erase(w.inbox.begin());
            if (!w.meSession)
            {
                continue;
            }
            for (const Egress &e : *burst)
            {
                if (e.term < w.knownTerm)
                {
                    m_stats[ST_STALE_TERM_DROPS]++; // step 4: forwarder_me_stale_term_total
                    continue;
                }
                w.knownTerm = e.term;
                if (e.trade)
                {
                    w.st.onTradeFrame(e.tf);
                    continue;
                }
                m_stats[ST_FRAMES]++;
                LadderOutcome outcome = LadderOutcome::IGNORED;
                const int m = w.st.onBookFrame(e.book, m_now, &outcome);
                if (0 == m)
                {
                    fail("the model produced a frame the stitcher dropped before any ladder");
                    return;
                }
                w.touched[m] = true;
                trace([&](std::ostream &t) {
                    t << "fwd" << f << " m" << m << (e.book.snapshot ? " 22 v" : " 25 v") << e.book.bookVersion << " term " << e.term << " -> " << nameOf(w.st.ladder(m).state())
                      << " " << w.st.ladder(m).bestBid() << "/" << w.st.ladder(m).bestAsk() << " v" << w.st.ladder(m).version();
                });
                if (LadderOutcome::SNAPSHOT_APPLIED == outcome || LadderOutcome::DELTA_APPLIED == outcome)
                {
                    checkS1(w, m);
                }
                const std::uint64_t fm = w.st.ladder(m).counters().breaks(BreakCause::FROM_MISMATCH);
                if (fm != w.prevFromMismatch[m])
                {
                    w.prevFromMismatch[m] = fm;
                    if (m_now < m_noMe1WindowEndNs && !m_noMe1Counted)
                    {
                        m_noMe1Counted = true;
                        m_stats[ST_FAILOVERS_NO_ME1_BROKE]++;
                    }
                }
                noteTop(w, m);
            }
        }
        for (int m = 1; m <= TICK_MARKETS; m++)
        {
            if (w.touched[m] || w.em.due(m, m_now))
            {
                w.em.step(m, w.st.ladder(m), w.st.mark(m), m_now, w.gate, w.sink);
                noteTop(w, m); // E9 may have broken the ladder
            }
            w.touched[m] = false;
        }
    }

    // ---- S4 / S6 at the end ----
    void judgeLiveness()
    {
        for (int f = 0; f < m_p.nFwd; f++)
        {
            for (int m = 1; m <= TICK_MARKETS; m++)
            {
                const BookLadder &l = m_fwd[f].st.ladder(m);
                m_stats[ST_BREAKS] += l.counters().breaksTotal();
                m_stats[ST_RECOVERIES] += l.counters().recoveries;
                m_stats[ST_REGRESSIONS] += l.counters().versionRegressions;
                m_stats[ST_UNVERSIONED] += l.counters().unversioned;
                m_stats[ST_OFFER_FAILURES] += m_fwd[f].em.counters(m).offerFailures;
            }
        }
        const Node &L = m_nodes[m_leader];
        for (int m = 1; m <= TICK_MARKETS; m++)
        {
            const PublishedTop &fin = L.book[m].published();
            if (m_history[m].empty())
            {
                continue; // never versioned: no snapshot, nothing to reach (rule U keeps it EMPTY)
            }
            // every forwarder ends OK at the final top; the clock runs from the LAST one to get there: until then the
            // slower forwarder's offers of older (true) states can displace the quote at the core (6.2), and the N = 1
            // companion run of every N = 2 seed checks the sharp single-forwarder bound on the same frames
            std::int64_t lastHolderNs = -1;
            for (int f = 0; f < m_p.nFwd; f++)
            {
                const Fwd &w = m_fwd[f];
                const BookLadder &l = w.st.ladder(m);
                std::ostringstream o;
                if (LadderState::OK != l.state() || l.bestBid() != fin.bestBid() || l.bestAsk() != fin.bestAsk())
                {
                    o << "S4: forwarder " << f << "'s ladder does not hold the final top of market " << m << " at the end (state " << nameOf(l.state())
                      << ", " << l.bestBid() << "/" << l.bestAsk() << " v" << l.version() << ", final " << fin.bestBid() << "/" << fin.bestAsk() << " v" << fin.bookVersion << ")";
                    fail(o.str());
                    return;
                }
                lastHolderNs = std::max(lastHolderNs, w.lastTopEventNs[m]);
            }
            std::ostringstream o;
            const LossyCoreModel::Market &c = m_core_.market(m);
            const std::int64_t deadline = std::max(lastHolderNs, m_lastReRegisterNs) + T_CAP_NS + 2 * POLL_NS;
            if (c.bestBid != fin.bestBid() || c.bestAsk != fin.bestAsk() || m_coreLastChangeNs[m] > deadline)
            {
                o << "S4: the final top did not reach the core within T_cap (market " << m << ": core " << c.bestBid << "/" << c.bestAsk << " v" << c.bookVersion
                  << ", model " << fin.bestBid() << "/" << fin.bestAsk() << " v" << fin.bookVersion << ", ladder final at " << lastHolderNs / MS
                  << " ms, core last change at " << m_coreLastChangeNs[m] / MS << " ms, deadline " << deadline / MS << " ms)";
                fail(o.str());
                return;
            }
        }
    }

    std::uint64_t m_seed;
    Params m_p;
    Rng m_me;
    Node m_nodes[NODES];
    int m_leader = 0;
    std::int64_t m_term = 1;
    std::vector<Command> m_log;
    std::vector<std::int64_t> m_logPos;
    std::int64_t m_nextLogPos = 1;
    std::int64_t m_nextTradeId = 1;
    std::unordered_map<std::int64_t, PublishedTop> m_history[SLOT];
    std::vector<Egress> m_mdQueue;
    std::vector<Egress> m_omsQueue;
    Fwd m_fwd[MAX_FWD];
    LossyCoreModel m_core_;
    std::int64_t m_coreLastChangeNs[SLOT];
    std::set<int> m_lostMarkets;
    std::int64_t m_lastReRegisterNs = 0;
    std::int64_t m_noMe1WindowEndNs = -1;
    bool m_noMe1Counted = true;
    std::int64_t m_now = 0;
    unsigned m_mintedThisPoll = 0;
    std::string m_failure;
    bool m_tracing = traceEnabled();
    std::ostringstream m_trace;
    std::uint64_t m_stats[ST_COUNT] = {};
};

bool FwdSink::tryOffer(int market, const TickCandidate &c, TickEdge edge) noexcept
{
    Fwd &w = run->fwd(fwd);
    const BookLadder &l = w.st.ladder(market);
    std::ostringstream o;
    // S3
    if (LadderState::OK != l.state())
    {
        o << "S3: a tick was offered while the ladder was " << nameOf(l.state()) << " (market " << market << ", version " << c.version << ")";
        run->fail(o.str());
        return false;
    }
    // S2
    if (c.bid > 0 && c.ask > 0 && c.bid >= c.ask)
    {
        o << "S2: a crossed tick was offered (market " << market << ", " << c.bid << "/" << c.ask << " v" << c.version << ")";
        run->fail(o.str());
        return false;
    }
    const PublishedTop *h = run->historyAt(market, c.version);
    if (nullptr == h || h->bestBid() != c.bid || h->bestAsk() != c.ask)
    {
        o << "S2: an offered tick differs from the model top at version " << c.version << " (market " << market << ", tick " << c.bid << "/" << c.ask;
        if (nullptr != h)
        {
            o << ", model " << h->bestBid() << "/" << h->bestAsk();
        }
        o << ")";
        run->fail(o.str());
        return false;
    }
    if (TickEdge::REOFFER != edge && c.version <= w.lastVersionOffered[market])
    {
        o << "S2: a version did not increase (market " << market << ", " << c.version << " after " << w.lastVersionOffered[market] << ", edge " << nameOf(edge) << ")";
        run->fail(o.str());
        return false;
    }
    // the transport: a back-pressured offer is nothing (E6)
    if (run->backPressure(fwd))
    {
        run->trace([&](std::ostream &t) { t << "fwd" << fwd << " m" << market << " offer " << nameOf(edge) << " v" << c.version << " back-pressured"; });
        return false;
    }
    w.lastVersionOffered[market] = c.version;
    // S5 on the positive offers
    if (TickEdge::LEADING == edge || TickEdge::TRAILING == edge)
    {
        if (w.lastEdgeOfferNs[market] != NEVER_LED && run->now() - w.lastEdgeOfferNs[market] < T_CAP_NS)
        {
            o << "S5: two edge offers of market " << market << " within T_cap (" << (run->now() - w.lastEdgeOfferNs[market]) / MS << " ms apart)";
            run->fail(o.str());
            return false;
        }
        w.lastEdgeOfferNs[market] = run->now();
    }
    run->countEdge(edge);
    run->coreOffer(fwd, market, c, edge);
    return true;
}

int seedCount()
{
    const char *env = std::getenv("CT_FORWARDER_PROPERTY_SEEDS");
    return nullptr != env ? std::atoi(env) : 10'000;
}

} // namespace

TEST_CASE("StitcherPropertyTest.properties")
{
    const int seeds = seedCount();
    const char *first = std::getenv("CT_FORWARDER_PROPERTY_FIRST_SEED"); // reproduce one failing seed
    const int firstSeed = nullptr != first ? std::atoi(first) : 1;
    std::uint64_t totals[ST_COUNT] = {};
    std::uint64_t s6Compared = 0;
    for (int seed = firstSeed; seed < firstSeed + seeds; seed++)
    {
        Run run(static_cast<std::uint64_t>(seed), 0);
        run.run();
        REQUIRE_MESSAGE(run.failure().empty(), run.failure());
        for (int i = 0; i < ST_COUNT; i++)
        {
            totals[i] += run.stats()[i];
        }
        if (run.params().nFwd == 2)
        {
            // S6: the same frames (the ME and core streams do not depend on N) through one forwarder
            Run single(static_cast<std::uint64_t>(seed), 1);
            single.run();
            REQUIRE_MESSAGE(single.failure().empty(), single.failure());
            for (int m = 1; m <= TICK_MARKETS; m++)
            {
                const auto &a = run.core().market(m), &b = single.core().market(m);
                if (a.bestBid != b.bestBid || a.bestAsk != b.bestAsk)
                {
                    std::ostringstream o;
                    o << "S6: the N = 2 core quote differs from the N = 1 run (seed " << seed << ", market " << m << ": " << a.bestBid << "/" << a.bestAsk
                      << " vs " << b.bestBid << "/" << b.bestAsk << ")";
                    REQUIRE_MESSAGE(false, o.str());
                }
            }
            s6Compared++;
        }
    }
    std::ostringstream summary;
    summary << "seeds=" << seeds << " s6Compared=" << s6Compared;
    for (int i = 0; i < ST_COUNT; i++)
    {
        summary << " " << statName(i) << "=" << totals[i];
    }
    MESSAGE(summary.str());
    // non-vacuity (C5, C6): every fault and every recovery path ran, recoveries > 0 among them
    if (seeds >= 1000)
    {
        for (int i = 0; i < NON_VACUITY_COUNT; i++)
        {
            std::ostringstream o;
            o << "non-vacuity: " << statName(i) << " is 0 after " << seeds << " seeds";
            CHECK_MESSAGE(totals[i] > 0, o.str());
        }
    }
}
