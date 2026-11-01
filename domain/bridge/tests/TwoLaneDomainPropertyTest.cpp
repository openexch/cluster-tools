// TwoLaneDomainPropertyTest (RC2 S10' contract 3.4): N = 2 (and sometimes 3) REAL BridgeDomain lanes, each with its
// own ResumeMemo, SimSources and archive views (a lane may list a subset of the K ME nodes; every view of a node is
// built from the same node journal, so the lanes agree on content), feeding ONE stub core written against the 4R
// rule (contract 4R.1 rules 2-4), not against today's single latch slot:
//   - a dense cursor with digests (FEED_GAP above cursor + 1, FEED_UNBOUND before the bind, duplicates counted);
//   - a per-lane GAP halt table keyed by bridge identity (at most 8 records) plus ONE overflow record; a GAP FeedHalt
//     is stale per record when cursor >= seen - 1 (DUPLICATE, nothing written); a new or changed record is ACCEPTED,
//     an identical one DUPLICATE; the same identity replaces its record; a replace from ANOTHER session keeps
//     seen = max and expected = min and marks the record SESSION_SHARED, after which no session lowers it (assets
//     FeedState.recordGapHalt, eb687f1);
//   - the core's own GAP record (identity 0, FeedState.recordCoreGap): every FEED_GAP refusal and every unbound s > 1
//     latches GAP {c + 1, s} into it (an open one merges: seen = max, expected = min), exactly like the real core;
//   - each record cleared by its own reach condition cursor >= seen_i - 1; the overflow record never clears by reach
//     (an operator latch: 76 {GAP, 0} then open);
//   - in a quarter of the non-overflow runs (a side generator, the scenario's draws unchanged) the operator sends a
//     PREMATURE 76 {GAP, 0} and open while a lane's hole is open (4R.1 rule 2: self-healing by the lane's re-offer);
//   - in half the non-overflow runs (the same side generator) one extra election while a lane's hole is open and a
//     data frame is pending, noticed by the lanes 1..10 iterations late: the frames sent meanwhile reach the core above
//     cursor + 1 (FEED_GAP), so the identity-0 record opens beside a lane's record (review N2-2 non-vacuity);
//   - the BRIDGE_HALT gate bit held by the holder set {GAP, NONGAP_AWAITING_OPEN, OVERFLOW}, cleared only when the set
//     empties; the report's feedHalted = table non-empty or slot set or overflow (rule 6).
// The stub keeps a TRUTH table next to the implementation table: the truth is the 4R rule with no seam, the
// implementation is what the stub-core seams switch off. Egress goes only to the lane that sent the query, the frame or
// the FeedHalt, 1..8 duty iterations late per lane (C1: never before the next poll).
//
// The generator (per seed): per-lane source subsets; per-lane pauses of 1..200 iterations (a paused lane is not
// stepped at all: a pure domain has no driver timeout, this models the source-lag device); lanes killed and restarted
// (a new BridgeDomain, memo and session under the same identity, or under a NEW identity in the overflow shape); holes
// on one lane's nodes only while the other lane is ahead, behind or paused; nodes written behind and caught up
// hundreds of iterations later (C2); recordings stopped and succeeded later; operator re-journals (fills); elections
// that lose the uncommitted tail of every lane; and the named shapes: LANE_AHEAD (the other lane delivered e first),
// CONFIRM (the other lane delivers e while this lane decides), STALE (the FeedHalt reaches the core after the other
// lane passed seen - 1), TWO_HOLES (rk14-twoholes: lane 0 holed over [s1, s2 + 5], lane 1 at s2, lane 1 written
// behind), NESTED (two records where the FIRST record clears first: the single-slot discriminator), OVERFLOW (one lane
// restarted under 9 new identities, each halting GAP on the same hole: 8 records + the overflow record).
//
// Asserted per run (3.4 a-g; every failure text is the negative-control table's):
//   (a) every entry the core receives is cursor + 1, a duplicate, or follows a loss of that lane's own frames the lane
//       had not yet been told about ("frame below cursor + 1 after a restart" once the lane was fenced or restarted:
//       the core's cursor + 1 is below the offered seq; "frame above cursor + 1 with no unknown loss" otherwise);
//   (d) per sync a lane offers C' + 1, C' + 2, ... dense and ascending, never below C' + 1 (also after a lane_ahead fence);
//   (c) a GAP FeedHalt for e is sent only when the core's cursor was below e at the moment the lane decided ("GAP record
//       written while the other lane could deliver e"), and never while the lane's own nodes deliver e memo-free; a stale
//       FeedHalt never latches ("a stale FeedHalt latched"); every GAP record reach-clears when the run completes;
//   (e) in every episode that starts with a rotation due while the last report showed C' > last, cold walks <=
//       lane_ahead fences ("cold walks per lane_ahead fence > 1");
//   (f) the gate bit never clears while a truth record is open ("the bit cleared with a record open") or while the
//       overflow record is set ("the bit cleared under overflow"); at the end the bit is set iff a truth record, the
//       overflow or the slot is; a HALTED(GAP) lane whose hole is open (cursor < seen - 1) never runs 250 of its
//       iterations (5 s) with the bit clear ("the gate open with a hole open for > 5 s", review N2-1);
//   (b) liveness: when the union of the live lanes' nodes carries every seq the run completes with cursor == N (an
//       overflow run after the operator's 76 {GAP, 0} and open);
//   (g) non-vacuity over the seeds (C5).
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <sstream>

using namespace ct_bridge_test;

namespace {

// ------------------------------------------------------------------ the stub-core seams (test only, read once)

bool coreSeam(const char *name)
{
    char key[96];
    std::snprintf(key, sizeof(key), "CT_TEST_DISABLE_%s", name);
    const char *v = std::getenv(key);
    return nullptr != v && 0 == std::strcmp(v, "1");
}

struct CoreSeams
{
    bool staleRuleOff = coreSeam("STALE_RULE_OFF");                 // a stale GAP FeedHalt is latched anyway
    bool singleSlot = coreSeam("SINGLE_SLOT");                      // today's single slot: a second GAP is forgotten
    bool overflowReadsCleared = coreSeam("OVERFLOW_READS_CLEARED"); // the overflow record clears by reach
};

const CoreSeams &seams()
{
    static const CoreSeams s;
    return s;
}

// ------------------------------------------------------------------ the scenario

enum class Shape { RANDOM, LANE_AHEAD, CONFIRM, STALE, TWO_HOLES, NESTED, OVERFLOW };
const char *shapeName(Shape s)
{
    switch (s)
    {
        case Shape::RANDOM: return "random";
        case Shape::LANE_AHEAD: return "lane-ahead";
        case Shape::CONFIRM: return "confirm";
        case Shape::STALE: return "stale";
        case Shape::TWO_HOLES: return "two-holes";
        case Shape::NESTED: return "nested";
        case Shape::OVERFLOW: return "overflow";
    }
    return "?";
}

enum class Trig { AT, HALTED, CONFIRMING, LAST_AT };
enum class Act { WRITE, FILL, STOP, SUCCEED, KILL, RESTART, PAUSE, ELECT };

struct Event
{
    Trig trig = Trig::AT;
    int at = 0;            // AT: the iteration
    int lane = 0;          // HALTED / CONFIRMING / LAST_AT: the lane slot watched
    std::int64_t seq = 0;  // LAST_AT: fires when the lane's last == seq
    int after = -1;        // armed only once event `after` fired
    int delay = 0;         // iterations between the trigger and the action
    Act act = Act::WRITE;
    int target = 0;        // node (WRITE, FILL, STOP, SUCCEED) or lane slot (KILL, RESTART, PAUSE)
    std::int64_t arg = 0;  // WRITE: to; SUCCEED: from; RESTART: identity (0 = the same); PAUSE: length
    // run state
    int dueAt = -1;
    bool fired = false;
};

struct Scenario
{
    Shape shape = Shape::RANDOM;
    std::int64_t n = 0;
    int nodes = 2;
    int lanes = 2;
    std::vector<std::vector<int>> laneNodes;
    std::vector<std::set<std::int64_t>> holes; // per node
    std::vector<std::int64_t> written;         // per node: written at the start
    std::vector<std::uint16_t> templates;
    std::vector<int> lag;                      // per lane
    std::vector<Event> events;
    bool elections = false;
    int commitPercent = 70; // the core commits the pending log in this share of the iterations (a slow core: more is lost)

    std::string describe() const
    {
        std::ostringstream o;
        o << "shape=" << shapeName(shape) << " n=" << n << " nodes=" << nodes << " lanes=" << lanes
          << " elections=" << elections << " commit=" << commitPercent;
        for (int k = 0; k < nodes; ++k)
        {
            o << " | node" << k << " written=" << written[static_cast<std::size_t>(k)] << " holes=";
            std::int64_t prev = -10, start = -10;
            bool first = true;
            for (std::int64_t h : holes[static_cast<std::size_t>(k)])
            {
                if (h != prev + 1)
                {
                    if (!first)
                    {
                        o << "[" << start << "," << prev << "]";
                    }
                    start = h;
                    first = false;
                }
                prev = h;
            }
            if (!first)
            {
                o << "[" << start << "," << prev << "]";
            }
        }
        for (int l = 0; l < lanes; ++l)
        {
            o << " | lane" << l << " lag=" << lag[static_cast<std::size_t>(l)] << " nodes=";
            for (int k : laneNodes[static_cast<std::size_t>(l)])
            {
                o << k;
            }
        }
        o << " | " << events.size() << " events";
        return o.str();
    }
};

struct Gen
{
    std::mt19937_64 &rng;
    std::int64_t pick(std::int64_t lo, std::int64_t hi)
    {
        return lo >= hi ? lo : std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    }
    bool coin(double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }
};

void holeRange(Scenario &sc, int node, std::int64_t from, std::int64_t to)
{
    for (std::int64_t s = from; s <= to; ++s)
    {
        sc.holes[static_cast<std::size_t>(node)].insert(s);
    }
}

int addEvent(Scenario &sc, Event e)
{
    sc.events.push_back(e);
    return static_cast<int>(sc.events.size() - 1);
}

Event at(int iteration, Act act, int target, std::int64_t arg = 0)
{
    Event e;
    e.trig = Trig::AT;
    e.at = iteration;
    e.act = act;
    e.target = target;
    e.arg = arg;
    return e;
}

Event when(Trig trig, int lane, int delay, Act act, int target, std::int64_t arg = 0, int after = -1)
{
    Event e;
    e.trig = trig;
    e.lane = lane;
    e.delay = delay;
    e.act = act;
    e.target = target;
    e.arg = arg;
    e.after = after;
    return e;
}

Scenario generate(std::mt19937_64 &rng)
{
    Gen g{ rng };
    Scenario sc;
    const std::int64_t r = g.pick(0, 99);
    sc.shape = r < 38 ? Shape::RANDOM : r < 53 ? Shape::LANE_AHEAD : r < 65 ? Shape::CONFIRM : r < 75 ? Shape::STALE
        : r < 85 ? Shape::TWO_HOLES : r < 94 ? Shape::NESTED : Shape::OVERFLOW;
    sc.n = g.pick(30, 120);
    sc.templates.assign(static_cast<std::size_t>(sc.n + 1), TEMPLATE_TRADE);
    sc.templates[1] = TEMPLATE_CONFIG;
    for (std::int64_t s = 2; s <= sc.n; ++s)
    {
        const std::int64_t t = g.pick(0, 6);
        sc.templates[static_cast<std::size_t>(s)] = t < 4 ? TEMPLATE_TRADE : t < 5 ? TEMPLATE_TERMINAL : TEMPLATE_OUTCOME;
    }
    auto setup = [&](int nodes, int lanes) {
        sc.nodes = nodes;
        sc.lanes = lanes;
        sc.holes.assign(static_cast<std::size_t>(nodes), {});
        sc.written.assign(static_cast<std::size_t>(nodes), sc.n);
        sc.laneNodes.assign(static_cast<std::size_t>(lanes), {});
        sc.lag.clear();
        for (int l = 0; l < lanes; ++l)
        {
            sc.lag.push_back(static_cast<int>(g.pick(1, 8)));
        }
    };
    const std::int64_t n = sc.n;
    switch (sc.shape)
    {
        case Shape::RANDOM:
        {
            setup(static_cast<int>(g.pick(2, 3)), g.coin(0.15) ? 3 : 2);
            for (int l = 0; l < sc.lanes; ++l)
            {
                auto &ln = sc.laneNodes[static_cast<std::size_t>(l)];
                for (int k = 0; k < sc.nodes; ++k)
                {
                    if (g.coin(0.55))
                    {
                        ln.push_back(k);
                    }
                }
                if (ln.empty())
                {
                    ln.push_back(static_cast<int>(g.pick(0, sc.nodes - 1)));
                }
                if (g.coin(0.4))
                {
                    std::shuffle(ln.begin(), ln.end(), rng);
                }
            }
            for (int k = 0; k < sc.nodes; ++k)
            {
                for (int h = 0; h < 2; ++h)
                {
                    if (g.coin(0.35))
                    {
                        const std::int64_t a = g.pick(2, n - 1);
                        holeRange(sc, k, a, std::min(n - 1, a + g.pick(0, 12)));
                    }
                }
                if (g.coin(0.5))
                {
                    sc.written[static_cast<std::size_t>(k)] = g.pick(1, n);
                    int t = static_cast<int>(g.pick(0, 400));
                    std::int64_t to = sc.written[static_cast<std::size_t>(k)];
                    while (to < n)
                    {
                        to = std::min(n, to + g.pick(1, n));
                        t += static_cast<int>(g.pick(20, 600)); // late arrivals, hundreds of iterations later (C2)
                        addEvent(sc, at(t, Act::WRITE, k, to));
                    }
                }
                if (!sc.holes[static_cast<std::size_t>(k)].empty() && g.coin(0.5))
                {
                    addEvent(sc, at(static_cast<int>(g.pick(100, 2500)), Act::FILL, k));
                }
                if (g.coin(0.15))
                {
                    // the ME writer goes away and its successor is recorded later, overlapping the old tail
                    const int t = static_cast<int>(g.pick(30, 600));
                    addEvent(sc, at(t, Act::STOP, k));
                    addEvent(sc, at(t + static_cast<int>(g.pick(0, 400)), Act::SUCCEED, k, g.pick(1, n)));
                }
            }
            for (int l = 0; l < sc.lanes; ++l)
            {
                if (g.coin(0.3))
                {
                    const int t = static_cast<int>(g.pick(20, 1500));
                    addEvent(sc, at(t, Act::KILL, l));
                    addEvent(sc, at(t + static_cast<int>(g.pick(1, 300)), Act::RESTART, l));
                }
                for (int p = 0; p < 3; ++p)
                {
                    if (g.coin(0.25))
                    {
                        addEvent(sc, at(static_cast<int>(g.pick(0, 1500)), Act::PAUSE, l, g.pick(1, 200)));
                    }
                }
            }
            sc.elections = g.coin(0.3);
            if (sc.elections && g.coin(0.4))
            {
                sc.commitPercent = static_cast<int>(g.pick(15, 40)); // a slow core: elections lose longer tails
            }
            if (sc.elections)
            {
                // one election while a lane streams with records still to feed (the lost tail is then noticed)
                Event mid = when(Trig::LAST_AT, static_cast<int>(g.pick(0, sc.lanes - 1)), static_cast<int>(g.pick(0, 3)),
                    Act::ELECT, 0);
                mid.seq = g.pick(2, n - 2);
                addEvent(sc, mid);
            }
            break;
        }
        case Shape::LANE_AHEAD:
        {
            // lane 1 races ahead on a complete node; lane 0 (paused at the start, two nodes, both holed over [h, h + k])
            // meets its hole while its last report already shows C' > last: a lane_ahead fence, never a rotation
            setup(3, 2);
            sc.laneNodes[0] = { 0, 1 };
            sc.laneNodes[1] = { 2 };
            const std::int64_t h = g.pick(n / 3, n - 4);
            const std::int64_t k = g.pick(0, std::min<std::int64_t>(10, n - 1 - h));
            holeRange(sc, 0, h, h + k);
            holeRange(sc, 1, h, h + k);
            if (g.coin(0.5))
            {
                holeRange(sc, 1, h - 1, h - 1); // node 1 lacks one more: two different first-hole seqs
            }
            addEvent(sc, at(0, Act::PAUSE, 0, g.pick(40, 200)));
            if (g.coin(0.4))
            {
                addEvent(sc, at(static_cast<int>(g.pick(100, 600)), Act::PAUSE, 0, g.pick(1, 200)));
            }
            sc.elections = g.coin(0.15);
            break;
        }
        case Shape::CONFIRM:
        {
            // lane 0 ahead and holed over [h, h + k]; lane 1 behind at h - 1, released right when lane 0 reaches its
            // hole: lane 1 may deliver e while lane 0 decides (the confirm report then fences lane_ahead)
            setup(2, 2);
            sc.laneNodes[0] = { 0 };
            sc.laneNodes[1] = { 1 };
            const std::int64_t h = g.pick(n / 3, n - 6);
            const std::int64_t k = g.pick(2, std::min<std::int64_t>(12, n - 1 - h));
            holeRange(sc, 0, h, h + k);
            sc.written[1] = h - 1;
            Event reach = when(Trig::LAST_AT, 0, static_cast<int>(g.pick(0, 12)), Act::WRITE, 1, h + g.pick(0, k - 1));
            reach.seq = h - 1;
            const int e1 = addEvent(sc, reach);
            addEvent(sc, when(Trig::AT, 0, static_cast<int>(g.pick(20, 500)), Act::WRITE, 1, n, e1));
            if (g.coin(0.3))
            {
                Event p = at(0, Act::PAUSE, 1, g.pick(1, 60));
                addEvent(sc, p);
            }
            break;
        }
        case Shape::STALE:
        {
            // lane 0 holed over a short range, lane 1 behind at h - 1, released while lane 0 confirms: lane 0's FeedHalt
            // may reach the core after lane 1 passed seen - 1 (stale, DUPLICATE, nothing written)
            setup(2, 2);
            sc.laneNodes[0] = { 0 };
            sc.laneNodes[1] = { 1 };
            const std::int64_t h = g.pick(n / 3, n - 4);
            const std::int64_t k = g.pick(0, 2);
            holeRange(sc, 0, h, h + k);
            sc.written[1] = h - 1;
            const Trig trig = g.coin(0.5) ? Trig::CONFIRMING : Trig::HALTED;
            addEvent(sc, when(trig, 0, static_cast<int>(g.pick(0, 8)), Act::WRITE, 1, n));
            break;
        }
        case Shape::TWO_HOLES:
        {
            // rk14-twoholes: record A (lane 0, [s1, s2 + 6]) then record B (lane 1, [s2, s2 + 1]); B clears first and
            // the gate stays shut on A; A clears: exactly one reopen
            setup(2, 2);
            sc.laneNodes[0] = { 0 };
            sc.laneNodes[1] = { 1 };
            const std::int64_t s1 = g.pick(n / 4, n / 2);
            const std::int64_t s2 = s1 + g.pick(5, 30);
            if (s2 + 8 > n)
            {
                sc.n = s2 + 8 + g.pick(0, 20);
                sc.templates.resize(static_cast<std::size_t>(sc.n + 1), TEMPLATE_TRADE);
                sc.written.assign(2, sc.n);
            }
            holeRange(sc, 0, s1, s2 + 5);
            holeRange(sc, 1, s2, s2);
            sc.written[1] = s1 - 1;
            // lane 1 streams to s2 - 1 and meets its hole at s2 (s2 + 1 is written: a hole, not a live head)
            const int a = addEvent(sc, when(Trig::HALTED, 0, static_cast<int>(g.pick(5, 50)), Act::WRITE, 1, s2 + 1));
            const int b = addEvent(sc, when(Trig::HALTED, 1, static_cast<int>(g.pick(5, 250)), Act::FILL, 1, 0, a));
            const int c = addEvent(sc, when(Trig::AT, 0, 0, Act::WRITE, 1, s2 + 2, b)); // the fill, then s2 + 2 only
            addEvent(sc, when(Trig::AT, 0, static_cast<int>(g.pick(250, 500)), Act::WRITE, 1, sc.n, c));
            if (g.coin(0.3))
            {
                // -behind-dead (3.1 rule 6): lane 0 is killed while halted and never restarted
                addEvent(sc, when(Trig::HALTED, 0, static_cast<int>(g.pick(2, 20)), Act::KILL, 0));
            }
            break;
        }
        case Shape::NESTED:
        {
            // record A (lane 0, seen a2 + 1) first, record B (lane 1, seen b2 + 1 > a2 + 1) second; a fill on lane 1's
            // node passes a2 before b2: A clears first while B stays open (a single-slot core reopens here)
            setup(2, 2);
            sc.laneNodes[0] = { 0 };
            sc.laneNodes[1] = { 1 };
            const std::int64_t h = g.pick(n / 4, n / 2);
            const std::int64_t a2 = h + g.pick(0, 6);
            const std::int64_t b2 = std::min(n - 1, a2 + g.pick(2, 15));
            holeRange(sc, 0, h, a2);
            holeRange(sc, 1, h, b2);
            sc.written[1] = h - 1;
            const int a = addEvent(sc, when(Trig::HALTED, 0, static_cast<int>(g.pick(5, 30)), Act::WRITE, 1, n));
            addEvent(sc, when(Trig::HALTED, 1, static_cast<int>(g.pick(10, 300)), Act::FILL, 1, 0, a));
            break;
        }
        case Shape::OVERFLOW:
        {
            // one hole on the only node both lanes list; lane 1 is restarted under 9 new identities, each halting GAP:
            // identities 1..10 hold 8 records and the overflow record; the fill clears the 8 by reach, the overflow
            // stays an operator latch (76 {GAP, 0} then open)
            setup(1, 2);
            sc.laneNodes[0] = { 0 };
            sc.laneNodes[1] = { 0 };
            const std::int64_t h = g.pick(n / 4, n - 3);
            holeRange(sc, 0, h, std::min(n - 1, h + g.pick(0, 4)));
            int prev = -1;
            for (std::int64_t id = 3; id <= 11; ++id)
            {
                const int kill = addEvent(sc, when(Trig::HALTED, 1, static_cast<int>(g.pick(1, 10)), Act::KILL, 1, 0, prev));
                prev = addEvent(sc, when(Trig::AT, 0, static_cast<int>(g.pick(3, 40)), Act::RESTART, 1, id, kill));
            }
            addEvent(sc, when(Trig::HALTED, 1, static_cast<int>(g.pick(20, 300)), Act::FILL, 0, 0, prev));
            break;
        }
    }
    return sc;
}

// ------------------------------------------------------------------ node journals (one truth, every lane's view)

struct NodeRec
{
    std::vector<std::int64_t> seqs;
    bool stopped = false;
};

struct Node
{
    std::vector<NodeRec> recs;
    std::set<std::int64_t> holes;
    std::int64_t written = 0;

    bool has(std::int64_t s) const
    {
        for (const auto &r : recs)
        {
            if (std::find(r.seqs.begin(), r.seqs.end(), s) != r.seqs.end())
            {
                return true;
            }
        }
        return false;
    }
    // a memo-free read from last = e - 1: the first frame with seq >= e is e
    bool delivers(std::int64_t e) const
    {
        for (const auto &r : recs)
        {
            for (std::int64_t s : r.seqs)
            {
                if (s >= e)
                {
                    return s == e;
                }
            }
        }
        return false;
    }
};

Bytes recordBytes(const Scenario &sc, std::int64_t seq)
{
    return makeFrame(sc.templates[static_cast<std::size_t>(seq)], seq, EPOCH, 64 * seq);
}

// ------------------------------------------------------------------ the run

struct Stats
{
    int runs = 0, completed = 0, laneAheadFences = 0, confirmFencesAvoidedHalt = 0, staleFeedHalts = 0;
    int gapRecords = 0, gapReachCleared = 0, twoRecordOneReopen = 0, overflowRuns = 0, overflowOperatorCleared = 0;
    int restarts = 0, recoveries = 0, reopens = 0, elections = 0, duplicates = 0, pauses = 0, episodes = 0;
    int lagEgress = 0, stuckHalts = 0;
    int coreGapRecords = 0, coreAndLaneRuns = 0, prematureClears = 0, relatchedAfterClear = 0; // review N2-1 / N2-2
    int lateNoticeElections = 0; // N2-2: elections the lanes noticed 1..10 iterations late
    std::uint64_t stuckSeed = 0;
    std::vector<int> shapes = std::vector<int>(7, 0);
};
Stats g_stats;

struct Run;

struct Lane final : CorePort
{
    Run &run;
    int slot;
    int incarnation; // unique per lane object over the run (the core's session id)
    std::int64_t identity;
    std::vector<int> nodes;
    BridgeSettings settings;
    ArchiveMetrics archiveMetrics;
    std::unique_ptr<ResumeMemo> memo;
    FakeEncoder encoder;
    BridgeMetrics metrics;
    std::unique_ptr<BridgeDomain> domain;
    SimSources sources;
    int lag = 1;
    int upAt = -1;
    bool upPending = true; // onSessionUp is due at upAt (a paused lane takes it when it runs again)
    int exitSince = -1;    // the iteration since which the core's cursor has passed this lane's GAP halt (exit (a))
    int openHoleIts = 0;   // (f'): the lane's iterations HALTED(GAP) with its hole open and the gate bit clear
    bool restarted = false;
    std::deque<std::pair<int, CoreEgress>> egress;

    // (a) / (d)
    std::uint64_t offerSync = ~0ULL;
    std::int64_t nextInSync = 0;
    std::size_t knownBoundary = 0;
    std::map<std::int64_t, std::size_t> queryIndex;
    // (c): the decision in force
    bool deciding = false;
    std::int64_t decisionCursor = 0;
    std::int64_t stepCursor = 0;
    std::int64_t decisionE = 0;
    // (c) own nodes: the memo-free truth of each source when its verdict for e was taken (a decision decides on
    // verdicts that old; a re-journal after the verdict is seen by the re-scan, not by the decision: README finding)
    std::vector<std::int64_t> verdictE;
    std::vector<TimePoint> verdictSince;
    std::vector<bool> verdictDelivers;
    // (e): the lane_ahead episode in force
    bool episode = false;
    std::uint64_t epCold = 0, epFences = 0, epForwarded = 0, epSyncs = 0, epResumeBase = 0;
    std::int64_t epStartC = 0, epStartLast = 0;

    Lane(Run &r, int s, int inc, std::int64_t id, const std::vector<int> &ns, int lg);

    OfferOutcome tryOffer(const std::uint8_t *frame, std::size_t length) noexcept override;

    std::uint64_t forwarded() const
    {
        std::uint64_t n = 0;
        for (const auto &c : metrics.forwardedRecords)
        {
            n += BridgeMetrics::get(c);
        }
        return n;
    }
};

struct HaltRec
{
    std::int64_t expected = 0, seen = 0;
    int session = 0;
};

struct Pending
{
    int incarnation = 0;
    std::size_t index = 0;
    FakeFrame f;
    std::int64_t decisionCursor = -1; // FeedHalt: the core cursor when the lane decided
    std::int64_t decisionE = 0;
    bool ownDelivers = false;         // FeedHalt: a listed node delivered e memo-free at the decision
};

constexpr std::uint8_t HOLD_GAP = 1, HOLD_NONGAP = 2, HOLD_OVERFLOW = 4;
constexpr std::int64_t CORE_IDENTITY = 0; // FeedState.CORE_DETECTED_IDENTITY
constexpr int SESSION_SHARED = -1;        // FeedState.SESSION_SHARED: written by two sessions, never lowered after
// (f') 4R.1 rule 2: a halted lane re-offers within one halted report (25 iterations here) once the gate shows clear
constexpr int OPEN_HOLE_ITERATIONS = 250;
// 3.1 rule 3 / 9.4 exit (a): a halted lane leaves a GAP halt on its next report once the cursor passed seen - 1. A lane
// still halted this long after that (two re-scan periods plus the report cadence) is a stuck halt.
constexpr int STUCK_ITERATIONS = 700;

struct Run
{
    const Scenario &sc;
    std::mt19937_64 &rng;
    std::uint64_t seed;
    std::vector<Node> nodes;
    std::vector<std::unique_ptr<Lane>> lanes; // per slot; null while killed
    std::vector<std::int64_t> identityOf;     // per slot: the identity of its next start
    std::vector<int> pausedUntil;
    int nextIncarnation = 1;
    int it = 0;
    TimePoint now{};
    std::string failure;
    bool debug = std::getenv("CT_BRIDGE_TWOLANE_DEBUG") != nullptr;

    // the core: cursor, digests, the pending log
    std::int64_t bound = 0, cursor = 0;
    std::map<std::int64_t, std::uint64_t> digests;
    std::deque<Pending> pending;
    std::size_t offers = 0;
    std::vector<std::size_t> knownAt;                 // per offer index: the lane's knownBoundary when offered
    std::vector<std::pair<std::size_t, int>> losses;  // (offer index, incarnation)
    std::map<int, Lane *> byIncarnation;
    // the 4R halt state: the implementation (seamed) and the truth (the rule, no seam)
    std::map<std::int64_t, HaltRec> table, truth;
    bool overflow = false, truthOverflow = false;
    std::int64_t overflowSeen = 0, truthOverflowSeen = 0;
    std::uint8_t holders = 0;
    bool bit = false;
    std::uint8_t slotReason = 0; // the non-GAP slot (never generated: the 4R rule keeps its bit; RC-5 tests it)
    // (g)
    bool multi = false, oneClearedOneOpen = false;
    int reopens = 0, distinctHalted = 0;
    std::set<std::int64_t> haltedIdentities;
    bool overflowSeenInRun = false;
    bool coreAndLaneOpen = false; // N2-2: the identity-0 record and a lane record open together
    // N2-1: the premature operator clear (its own generator, so the scenario and the run's draws are unchanged)
    std::mt19937_64 opRng;
    bool prematureArmed = false, prematureDone = false, relatchCounted = false;
    int prematureAt = -1;
    // N2-2: one extra LATE-NOTICE election per armed run (half the non-overflow runs, all draws from the side
    // generator): the lanes learn of the new leader 1..10 iterations after the uncommitted tail was lost, so a lane's
    // next frames reach the new leader above cursor + 1 (the real FEED_GAP path: frames sent before the bridge noticed
    // the loss) and the core's identity-0 record opens; the scenario's draws are unchanged
    bool lateNoticeArmed = false;
    int noticeAt = -1;
    int extraElectAt = -1; // one extra late-notice election while a lane's hole is open (the core and a lane record)
    bool extraElectDone = false;
    struct Deferred
    {
        int slot, incarnation, upDelta;
    };
    std::vector<Deferred> deferredNotice;

    Run(const Scenario &s, std::mt19937_64 &r, std::uint64_t sd) : sc(s), rng(r), seed(sd), opRng(sd ^ 0x76c1ea2ULL)
    {
        prematureArmed = Shape::OVERFLOW != sc.shape && 0 == opRng() % 4;
        lateNoticeArmed = Shape::OVERFLOW != sc.shape && 0 == opRng() % 2;
        nodes.resize(static_cast<std::size_t>(sc.nodes));
        for (int k = 0; k < sc.nodes; ++k)
        {
            Node &nd = nodes[static_cast<std::size_t>(k)];
            nd.holes = sc.holes[static_cast<std::size_t>(k)];
            nd.recs.push_back({});
            for (std::int64_t s2 = 1; s2 <= sc.written[static_cast<std::size_t>(k)]; ++s2)
            {
                if (!nd.holes.count(s2))
                {
                    nd.recs.back().seqs.push_back(s2);
                }
            }
            nd.written = sc.written[static_cast<std::size_t>(k)];
        }
        lanes.resize(static_cast<std::size_t>(sc.lanes));
        pausedUntil.assign(static_cast<std::size_t>(sc.lanes), -1);
        for (int l = 0; l < sc.lanes; ++l)
        {
            identityOf.push_back(l + 1);
            start(l);
        }
    }

    void fail(const std::string &why)
    {
        if (failure.empty())
        {
            failure = why;
        }
    }

    // a new lane object (a process start): its archive views built from the node journals as they are now
    void start(int slot)
    {
        const int inc = nextIncarnation++;
        auto lane = std::make_unique<Lane>(*this, slot, inc, identityOf[static_cast<std::size_t>(slot)],
            sc.laneNodes[static_cast<std::size_t>(slot)], sc.lag[static_cast<std::size_t>(slot)]);
        for (std::size_t i = 0; i < lane->nodes.size(); ++i)
        {
            const Node &nd = nodes[static_cast<std::size_t>(lane->nodes[i])];
            SimArchive &a = *lane->sources.archives[i];
            for (const NodeRec &r : nd.recs)
            {
                SimRecording &rec = a.record();
                for (std::int64_t s2 : r.seqs)
                {
                    a.append(rec, recordBytes(sc, s2));
                }
                if (r.stopped)
                {
                    a.stop(rec);
                }
            }
        }
        lane->sources.start(now, 0);
        lane->upAt = it + 1;
        byIncarnation[inc] = lane.get();
        lanes[static_cast<std::size_t>(slot)] = std::move(lane);
    }

    void kill(int slot)
    {
        Lane *l = lanes[static_cast<std::size_t>(slot)].get();
        if (nullptr != l)
        {
            byIncarnation.erase(l->incarnation);
            lanes[static_cast<std::size_t>(slot)].reset(); // its log entries stay in the core's log; its egress is gone
        }
    }

    template<typename F>
    void eachView(int node, F f)
    {
        for (auto &lp : lanes)
        {
            if (!lp)
            {
                continue;
            }
            for (std::size_t i = 0; i < lp->nodes.size(); ++i)
            {
                if (lp->nodes[i] == node)
                {
                    f(*lp->sources.archives[i]);
                }
            }
        }
    }

    void write(int node, std::int64_t to)
    {
        Node &nd = nodes[static_cast<std::size_t>(node)];
        for (std::int64_t s2 = nd.written + 1; s2 <= to; ++s2)
        {
            if (nd.holes.count(s2) || nd.recs.empty() || nd.recs.back().stopped)
            {
                continue;
            }
            nd.recs.back().seqs.push_back(s2);
            const Bytes b = recordBytes(sc, s2);
            eachView(node, [&](SimArchive &a) { a.append(a.recordings.back(), b); });
        }
        nd.written = std::max(nd.written, to);
    }

    // an operator re-journal: one complete recording of everything written so far, the holes gone
    void fill(int node)
    {
        Node &nd = nodes[static_cast<std::size_t>(node)];
        nd.holes.clear();
        nd.recs.clear();
        nd.recs.push_back({});
        for (std::int64_t s2 = 1; s2 <= nd.written; ++s2)
        {
            nd.recs.back().seqs.push_back(s2);
        }
        eachView(node, [&](SimArchive &a) {
            a.recordings.clear();
            SimRecording &rec = a.record();
            for (std::int64_t s2 = 1; s2 <= nd.written; ++s2)
            {
                a.append(rec, recordBytes(sc, s2));
            }
        });
    }

    void stopNode(int node)
    {
        Node &nd = nodes[static_cast<std::size_t>(node)];
        if (nd.recs.empty() || nd.recs.back().stopped)
        {
            return;
        }
        nd.recs.back().stopped = true;
        eachView(node, [&](SimArchive &a) { a.stop(a.recordings.back()); });
    }

    void succeed(int node, std::int64_t from)
    {
        Node &nd = nodes[static_cast<std::size_t>(node)];
        stopNode(node);
        NodeRec r;
        for (std::int64_t s2 = std::min(from, nd.written + 1); s2 <= nd.written; ++s2)
        {
            if (!nd.holes.count(s2))
            {
                r.seqs.push_back(s2);
            }
        }
        nd.recs.push_back(r);
        eachView(node, [&](SimArchive &a) {
            SimRecording &rec = a.record();
            for (std::int64_t s2 : r.seqs)
            {
                a.append(rec, recordBytes(sc, s2));
            }
        });
    }

    // ---------------------------------------------------------------- the core

    OfferOutcome offer(Lane &lane, const std::uint8_t *frame, std::size_t length)
    {
        const FakeFrame f = decodeFake(frame, length);
        const std::size_t index = offers++;
        knownAt.push_back(lane.knownBoundary);
        if (T_QUERY_FEED_POSITION == f.moneyTemplate && !lane.queryIndex.count(f.correlationId))
        {
            lane.queryIndex[f.correlationId] = index;
        }
        if (!f.records.empty())
        {
            // (d): per sync the offers are C' + 1, C' + 2, ... dense and ascending, never below C' + 1
            if (lane.domain->syncs() != lane.offerSync)
            {
                lane.offerSync = lane.domain->syncs();
                lane.nextInSync = lane.domain->syncCursor() + 1;
            }
            for (const FakeRecord &r : f.records)
            {
                if (r.seq != lane.nextInSync)
                {
                    fail("lane " + std::to_string(lane.slot) + ": sync from " + std::to_string(lane.domain->syncCursor()) +
                        " offered " + std::to_string(r.seq) + " where " + std::to_string(lane.nextInSync) + " was due");
                }
                lane.nextInSync = r.seq + 1;
            }
        }
        Pending p;
        p.incarnation = lane.incarnation;
        p.index = index;
        p.f = f;
        if (T_FEED_HALT == f.moneyTemplate)
        {
            const bool dec = lane.deciding;
            p.decisionCursor = dec ? lane.decisionCursor : lane.stepCursor;
            p.decisionE = f.expected;
            p.ownDelivers = false;
            for (std::size_t i = 0; i < lane.nodes.size(); ++i)
            {
                const bool d = lane.verdictE[i] == f.expected ? lane.verdictDelivers[i]
                                                               : nodes[static_cast<std::size_t>(lane.nodes[i])].delivers(f.expected);
                p.ownDelivers = p.ownDelivers || d;
            }
        }
        pending.push_back(p);
        return OfferOutcome::OK;
    }

    void toLane(int incarnation, const CoreEgress &e)
    {
        auto itL = byIncarnation.find(incarnation);
        if (itL != byIncarnation.end())
        {
            itL->second->egress.emplace_back(it, e);
        }
    }

    void reject(int incarnation, std::int64_t s, std::uint8_t reason, std::int64_t detail)
    {
        if (debug)
        {
            std::printf("it %d reject inc %d seq %lld reason %d detail %lld cursor %lld\n", it, incarnation,
                static_cast<long long>(s), reason, static_cast<long long>(detail), static_cast<long long>(cursor));
        }
        CoreEgress e;
        e.kind = CoreEgress::REJECT;
        e.reject.tradeId = s;
        e.reject.marketId = static_cast<std::int32_t>(s);
        e.reject.journalPosition = s;
        e.reject.reasonRaw = reason;
        e.reject.detail = detail;
        toLane(incarnation, e);
    }

    void gapCheck(const Pending &p, std::int64_t s)
    {
        bool excused = false;
        for (const auto &[l, inc] : losses)
        {
            if (inc == p.incarnation && l < p.index && l >= knownAt[p.index])
            {
                excused = true;
                break;
            }
        }
        if (excused)
        {
            return;
        }
        const auto itL = byIncarnation.find(p.incarnation);
        const bool fencedOrRestarted = itL == byIncarnation.end() || itL->second->restarted ||
            itL->second->metrics.totalFences() > 0;
        const std::string what = " (the core's cursor + 1 = " + std::to_string(cursor + 1) + " is below the offered seq " +
            std::to_string(s) + "; lane incarnation " + std::to_string(p.incarnation) +
            ", no unknown loss of its own frames before it)";
        fail(fencedOrRestarted ? "frame below cursor + 1 after a restart" + what
                               : "frame above cursor + 1 with no unknown loss" + what);
    }

    void reachClear()
    {
        // the truth (the 4R rule, no seam)
        const std::size_t truthBefore = truth.size();
        for (auto itR = truth.begin(); itR != truth.end();)
        {
            if (cursor >= itR->second.seen - 1)
            {
                ++g_stats.gapReachCleared;
                itR = truth.erase(itR);
            }
            else
            {
                ++itR;
            }
        }
        if (truth.size() < truthBefore && !truth.empty() && multi)
        {
            oneClearedOneOpen = true;
        }
        // the implementation
        for (auto itR = table.begin(); itR != table.end();)
        {
            itR = cursor >= itR->second.seen - 1 ? table.erase(itR) : std::next(itR);
        }
        if (overflow && seams().overflowReadsCleared && cursor >= overflowSeen - 1)
        {
            overflow = false; // RED shape: the overflow record reads as cleared
        }
        if (table.empty())
        {
            holders &= static_cast<std::uint8_t>(~HOLD_GAP);
        }
        if (!overflow)
        {
            holders &= static_cast<std::uint8_t>(~HOLD_OVERFLOW);
        }
        if (bit && 0 == holders)
        {
            bit = false;
            ++reopens;
            ++g_stats.reopens;
            if (!truth.empty())
            {
                const auto &[id, rec] = *truth.begin();
                fail("the bit cleared with a record open (cursor " + std::to_string(cursor) + ", identity " +
                    std::to_string(id) + " record {" + std::to_string(rec.expected) + ", " + std::to_string(rec.seen) +
                    "} still open)");
            }
            else if (truthOverflow)
            {
                fail("the bit cleared under overflow (cursor " + std::to_string(cursor) + ", overflow seen " +
                    std::to_string(truthOverflowSeen) + ")");
            }
            if (multi && oneClearedOneOpen)
            {
                ++g_stats.twoRecordOneReopen;
            }
            multi = false;
            oneClearedOneOpen = false;
        }
    }

    bool admit(const Pending &p, const FakeRecord &r, std::uint16_t moneyTemplate)
    {
        const std::int64_t s = r.seq;
        if (0 == bound)
        {
            if (1 == s && T_ME_ENGINE_CONFIG == moneyTemplate && EPOCH == r.epoch)
            {
                bound = EPOCH;
                cursor = 1;
                digests[1] = StubCore::digestOf(r, moneyTemplate);
                reachClear();
                return true;
            }
            gapCheck(p, s);
            reject(p.incarnation, s, 8, 1);
            if (s > 1)
            {
                recordCoreGap(1, s); // D2 row 3: a hole at 1 (AssetsEngine 1467)
            }
            return false;
        }
        if (s <= cursor)
        {
            const auto d = digests.find(s);
            if (d != digests.end() && d->second != StubCore::digestOf(r, moneyTemplate))
            {
                fail("duplicate mismatch at seq " + std::to_string(s));
            }
            ++g_stats.duplicates;
            return true;
        }
        if (s > cursor + 1)
        {
            gapCheck(p, s);
            reject(p.incarnation, s, 6, cursor + 1);
            recordCoreGap(cursor + 1, s); // D1 row 5: FEED_GAP, then latch(GAP, c + 1, s) (AssetsEngine 1506-1507)
            return false;
        }
        cursor = s;
        digests[s] = StubCore::digestOf(r, moneyTemplate);
        reachClear();
        return true;
    }

    FeedPositionReport reportFor(std::int64_t corr) const
    {
        FeedPositionReport r;
        r.correlationId = corr;
        r.meEpoch = bound;
        r.lastAppliedJournalSeq = cursor;
        const bool halted = !table.empty() || overflow || 0 != slotReason;
        r.haltReasonRaw = halted ? (0 != slotReason ? slotReason : 1) : 0;
        r.feedHaltedRaw = halted ? 1 : 0;
        r.feedHalted = halted;
        return r;
    }

    void applyFeedHalt(const Pending &p)
    {
        const FakeFrame &f = p.f;
        CoreEgress e;
        e.kind = CoreEgress::ACK;
        e.ack.correlationId = f.correlationId;
        e.ack.refTemplateId = T_FEED_HALT;
        if (HaltReason::GAP != f.reason || !(f.expected >= 1 && f.expected < f.seen))
        {
            e.ack.resultRaw = HaltReason::GAP == f.reason ? ADMIN_REJECTED : ADMIN_DUPLICATE; // non-GAP: not generated
            toLane(p.incarnation, e);
            return;
        }
        const std::int64_t id = f.identity;
        // (c): a GAP FeedHalt only when the cursor was below e when the lane decided, and its own nodes lack e
        if (p.decisionCursor >= f.expected)
        {
            fail("GAP record written while the other lane could deliver e (FeedHalt {" + std::to_string(f.expected) + ", " +
                std::to_string(f.seen) + "} from identity " + std::to_string(id) + ": the core's cursor was " +
                std::to_string(p.decisionCursor) + " when the lane decided)");
        }
        if (p.ownDelivers)
        {
            fail("halt GAP at " + std::to_string(f.expected) + " although a memo-free read of the lane's own node delivers it");
        }
        haltedIdentities.insert(id);
        // the truth
        const bool truthStale = cursor >= f.seen - 1;
        if (truthStale)
        {
            ++g_stats.staleFeedHalts;
        }
        else
        {
            auto t = truth.find(id);
            if (t != truth.end())
            {
                if (t->second.session != p.incarnation)
                {
                    // another session (or a record already shared): never weakened (S2-3, FeedState.recordGapHalt)
                    t->second.seen = std::max(t->second.seen, f.seen);
                    t->second.expected = std::min(t->second.expected, f.expected);
                    t->second.session = SESSION_SHARED;
                }
                else
                {
                    t->second.seen = f.seen;
                    t->second.expected = f.expected;
                }
            }
            else if (truth.size() < 8)
            {
                truth[id] = HaltRec{ f.expected, f.seen, p.incarnation };
                ++g_stats.gapRecords;
            }
            else
            {
                truthOverflow = true;
                truthOverflowSeen = std::max(truthOverflowSeen, f.seen);
                overflowSeenInRun = true;
            }
            noteTruthShape();
        }
        // the implementation
        const bool stale = !seams().staleRuleOff && truthStale;
        bool accepted = false;
        if (stale)
        {
            e.ack.resultRaw = ADMIN_DUPLICATE;
        }
        else if (seams().singleSlot && !table.empty())
        {
            e.ack.resultRaw = ADMIN_DUPLICATE; // RED shape (today's AE 1332-1338): a second GAP is acked and forgotten
        }
        else if (auto t = table.find(id); t != table.end())
        {
            const bool shared = t->second.session != p.incarnation;
            const HaltRec next = shared
                ? HaltRec{ std::min(t->second.expected, f.expected), std::max(t->second.seen, f.seen), SESSION_SHARED }
                : HaltRec{ f.expected, f.seen, p.incarnation };
            if (t->second.expected == next.expected && t->second.seen == next.seen && t->second.session == next.session)
            {
                e.ack.resultRaw = ADMIN_DUPLICATE;
            }
            else
            {
                t->second = next;
                accepted = true;
            }
        }
        else if (table.size() < 8)
        {
            table[id] = HaltRec{ f.expected, f.seen, p.incarnation };
            accepted = true;
        }
        else
        {
            overflow = true;
            overflowSeen = std::max(overflowSeen, f.seen);
            accepted = true;
        }
        if (accepted)
        {
            e.ack.resultRaw = ADMIN_ACCEPTED;
            if (truthStale)
            {
                fail("a stale FeedHalt latched (FeedHalt {" + std::to_string(f.expected) + ", " + std::to_string(f.seen) +
                    "} from identity " + std::to_string(id) + " ACCEPTED at cursor " + std::to_string(cursor) + " >= seen - 1)");
            }
            bit = true;
            holders |= HOLD_GAP;
            if (overflow)
            {
                holders |= HOLD_OVERFLOW;
            }
            if (prematureDone && !relatchCounted)
            {
                relatchCounted = true;
                ++g_stats.relatchedAfterClear;
            }
        }
        toLane(p.incarnation, e);
    }

    void noteTruthShape()
    {
        if (truth.size() >= 2)
        {
            multi = true;
            if (truth.count(CORE_IDENTITY))
            {
                coreAndLaneOpen = true;
            }
        }
    }

    // FeedState.recordCoreGap (assets eb687f1 FIX-1): the identity-0 record of a GAP the core detected itself; an open
    // one merges (seen = max, expected = min), so its reach never opens the gate below a hole the core refused a frame at
    void recordCoreGap(std::int64_t expected, std::int64_t seen)
    {
        // the truth
        if (auto t = truth.find(CORE_IDENTITY); t != truth.end())
        {
            t->second.seen = std::max(t->second.seen, seen);
            t->second.expected = std::min(t->second.expected, expected);
        }
        else if (truth.size() < 8)
        {
            truth[CORE_IDENTITY] = HaltRec{ expected, seen, 0 };
            ++g_stats.coreGapRecords;
        }
        else
        {
            truthOverflow = true;
            truthOverflowSeen = std::max(truthOverflowSeen, seen);
            overflowSeenInRun = true;
        }
        noteTruthShape();
        // the implementation
        bool wrote = false;
        if (auto t = table.find(CORE_IDENTITY); t != table.end())
        {
            const HaltRec merged{ std::min(t->second.expected, expected), std::max(t->second.seen, seen), 0 };
            wrote = merged.expected != t->second.expected || merged.seen != t->second.seen;
            t->second = merged;
        }
        else if (seams().singleSlot && !table.empty())
        {
            // RED shape (the S7 single slot): the core's own second GAP is suppressed
        }
        else if (table.size() < 8)
        {
            table[CORE_IDENTITY] = HaltRec{ expected, seen, 0 };
            wrote = true;
        }
        else
        {
            overflow = true;
            overflowSeen = std::max(overflowSeen, seen);
            wrote = true;
        }
        if (wrote)
        {
            bit = true;
            holders |= HOLD_GAP;
            if (overflow)
            {
                holders |= HOLD_OVERFLOW;
            }
        }
    }

    void apply(const Pending &p)
    {
        const FakeFrame &f = p.f;
        if (T_QUERY_FEED_POSITION == f.moneyTemplate)
        {
            CoreEgress e;
            e.kind = CoreEgress::REPORT;
            e.report = reportFor(f.correlationId);
            toLane(p.incarnation, e);
            return;
        }
        if (T_FEED_HALT == f.moneyTemplate)
        {
            applyFeedHalt(p);
            return;
        }
        for (const FakeRecord &r : f.records)
        {
            if (!admit(p, r, f.moneyTemplate))
            {
                losses.emplace_back(p.index, p.incarnation); // a 62 stops at the first refusal
                break;
            }
        }
    }

    void commit()
    {
        while (!pending.empty())
        {
            const Pending p = pending.front();
            pending.pop_front();
            apply(p);
        }
    }

    // the scenario's elections are noticed at once; only the extra election below is late (a late notice on every
    // election trips the 9.6 storm guard on repeated FEED_GAP refusals at one cursor: a carried finding, README)
    void elect() { elect(rng, false); }

    // g: the generator of this election's own draws (the scenario's rng, or the side generator for the extra one)
    void elect(std::mt19937_64 &g, bool late)
    {
        ++g_stats.elections;
        noticeNow(); // a second election inside a late-notice window: the first one is noticed now
        for (const Pending &p : pending)
        {
            if (!p.f.records.empty())
            {
                losses.emplace_back(p.index, p.incarnation);
            }
        }
        pending.clear();
        const bool dropEgress = std::uniform_int_distribution<int>(0, 1)(g) == 0;
        for (auto &lp : lanes)
        {
            if (!lp)
            {
                continue;
            }
            if (dropEgress)
            {
                lp->egress.clear();
            }
            const int upDelta = static_cast<int>(std::uniform_int_distribution<int>(1, 20)(g));
            if (late)
            {
                deferredNotice.push_back({ lp->slot, lp->incarnation, upDelta });
                continue;
            }
            notice(*lp, upDelta);
        }
        if (late && !deferredNotice.empty())
        {
            ++g_stats.lateNoticeElections;
            noticeAt = it + 1 + static_cast<int>(opRng() % 10);
        }
        if (debug)
        {
            std::printf("it %d election late %d noticeAt %d\n", it, late ? 1 : 0, noticeAt);
        }
    }

    void notice(Lane &l, int upDelta)
    {
        l.knownBoundary = offers;
        l.domain->onFence(FenceTrigger::NEW_LEADER, now);
        l.upAt = it + upDelta;
        l.upPending = true;
    }

    void noticeNow()
    {
        for (const Deferred &d : deferredNotice)
        {
            Lane *l = lanes[static_cast<std::size_t>(d.slot)].get();
            if (l && l->incarnation == d.incarnation)
            {
                notice(*l, d.upDelta);
            }
        }
        deferredNotice.clear();
        noticeAt = -1;
    }

    // the operator, in an overflow run: 76 {GAP, 0} clears the table and the overflow, then open (clearAll)
    void operatorClearAndOpen()
    {
        ++g_stats.overflowOperatorCleared;
        table.clear();
        truth.clear();
        overflow = false;
        truthOverflow = false;
        holders = HOLD_NONGAP; // 76 {GAP, 0} adds NONGAP_AWAITING_OPEN
        holders = 0;           // open: clearAll
        bit = false;
    }

    // N2-1, the operator in any run: a PREMATURE 76 {GAP, 0} (the table, the identity-0 record and the overflow are
    // cleared) and open while a lane's hole is still open; the truth forgets the records too (the operator's clear is
    // not a reach-clear): the halted lanes must re-latch by themselves, (f') checks they do
    void operatorPrematureClear()
    {
        ++g_stats.prematureClears;
        table.clear();
        truth.clear();
        overflow = false;
        truthOverflow = false;
        holders = 0;
        bit = false;
        multi = false;
        oneClearedOneOpen = false;
    }

    bool pendingData() const
    {
        for (const Pending &p : pending)
        {
            if (!p.f.records.empty())
            {
                return true;
            }
        }
        return false;
    }

    bool laneStreaming() const
    {
        for (const auto &lp : lanes)
        {
            if (lp && it >= lp->upAt && State::STREAMING == lp->domain->view().state)
            {
                return true;
            }
        }
        return false;
    }

    bool laneHoleOpen() const
    {
        for (const auto &lp : lanes)
        {
            if (lp && it >= lp->upAt && State::HALTED == lp->domain->view().state &&
                BridgeHalt::GAP == lp->domain->view().halt && cursor < lp->domain->halt().seen - 1)
            {
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------- events

    bool triggered(const Event &e)
    {
        switch (e.trig)
        {
            case Trig::AT: return it >= e.at;
            case Trig::HALTED:
            case Trig::CONFIRMING:
            case Trig::LAST_AT:
            {
                Lane *l = lanes[static_cast<std::size_t>(e.lane)].get();
                if (nullptr == l || it < l->upAt)
                {
                    return false;
                }
                const BridgeView v = l->domain->view();
                if (Trig::HALTED == e.trig)
                {
                    return (State::HALTED == v.state || State::HALTING == v.state) && BridgeHalt::GAP == v.halt;
                }
                if (Trig::CONFIRMING == e.trig)
                {
                    return l->domain->confirming();
                }
                return v.last >= e.seq; // LAST_AT: the lane forwarded up to seq (it stands at seq + 1)
            }
        }
        return false;
    }

    void fireEvents()
    {
        auto &events = const_cast<std::vector<Event> &>(sc.events);
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            Event &e = events[i];
            if (e.fired)
            {
                continue;
            }
            if (e.after >= 0 && !events[static_cast<std::size_t>(e.after)].fired)
            {
                continue;
            }
            if (e.dueAt < 0)
            {
                if (!triggered(e))
                {
                    continue;
                }
                e.dueAt = (Trig::AT == e.trig && e.after < 0 ? e.at : it) + e.delay;
            }
            if (it < e.dueAt)
            {
                continue;
            }
            e.fired = true;
            if (debug)
            {
                std::printf("it %d event %zu act %d target %d arg %lld\n", it, i, static_cast<int>(e.act), e.target,
                    static_cast<long long>(e.arg));
            }
            switch (e.act)
            {
                case Act::WRITE: write(e.target, e.arg); break;
                case Act::FILL: fill(e.target); break;
                case Act::STOP: stopNode(e.target); break;
                case Act::SUCCEED: succeed(e.target, e.arg); break;
                case Act::KILL: kill(e.target); break;
                case Act::RESTART:
                    if (!lanes[static_cast<std::size_t>(e.target)])
                    {
                        if (0 != e.arg)
                        {
                            identityOf[static_cast<std::size_t>(e.target)] = e.arg;
                        }
                        ++g_stats.restarts;
                        start(e.target);
                        lanes[static_cast<std::size_t>(e.target)]->restarted = true;
                    }
                    break;
                case Act::ELECT: elect(); break;
                case Act::PAUSE:
                    ++g_stats.pauses;
                    pausedUntil[static_cast<std::size_t>(e.target)] = it + static_cast<int>(e.arg);
                    break;
            }
        }
    }

    // ---------------------------------------------------------------- one lane's duty iteration

    void deliver(Lane &l)
    {
        while (!l.egress.empty() && l.egress.front().first + l.lag <= it)
        {
            const CoreEgress e = l.egress.front().second;
            l.egress.pop_front();
            ++g_stats.lagEgress;
            switch (e.kind)
            {
                case CoreEgress::REPORT:
                {
                    const auto q = l.queryIndex.find(e.report.correlationId);
                    if (q != l.queryIndex.end())
                    {
                        l.knownBoundary = std::max(l.knownBoundary, q->second);
                    }
                    l.domain->onReport(e.report, now);
                    break;
                }
                case CoreEgress::REJECT:
                    l.knownBoundary = std::max(l.knownBoundary, offers);
                    l.domain->onSettleRejected(e.reject, now);
                    break;
                case CoreEgress::ACK: l.domain->onAdminAck(e.ack, now); break;
            }
        }
    }

    void stepLane(Lane &l)
    {
        if (l.upPending && it >= l.upAt)
        {
            l.upPending = false;
            l.domain->onSessionUp(now);
        }
        const bool confirmingBefore = l.domain->confirming(); // before the egress: the confirming report fences in it
        const std::uint64_t aheadBeforeEgress = l.metrics.fenceCount(FenceTrigger::LANE_AHEAD);
        deliver(l);
        l.stepCursor = cursor;
        const BridgeView pre = l.domain->view();
        const std::uint64_t cold0 = l.archiveMetrics.resumes(ResumeKind::COLD);
        const std::uint64_t proof0 = l.archiveMetrics.resumes(ResumeKind::PROOF);
        const std::uint64_t ahead0 = l.metrics.fenceCount(FenceTrigger::LANE_AHEAD);
        const std::uint64_t failovers0 = BridgeMetrics::get(l.metrics.sourceFailovers);
        const std::uint64_t fwd0 = l.forwarded();

        const int limit = l.domain->fragmentLimit();
        const int n = l.sources.doWork(*l.domain, limit, now);
        l.domain->onPollDone(n, now);
        const SourceCommand cmd = l.domain->takeSourceCommand();
        if (SourceCommandKind::SWITCH == cmd.kind)
        {
            l.sources.switchTo(cmd.index, now);
        }
        else if (SourceCommandKind::REATTACH == cmd.kind)
        {
            l.sources.reattach(now);
        }
        l.domain->tick(now);

        const BridgeView v = l.domain->view();
        const bool halted = (State::HALTED == v.state || State::HALTING == v.state) && BridgeHalt::NONE != v.halt;
        const std::uint64_t ahead1 = l.metrics.fenceCount(FenceTrigger::LANE_AHEAD);
        g_stats.laneAheadFences += static_cast<int>(ahead1 - aheadBeforeEgress);
        if (confirmingBefore && ahead1 > aheadBeforeEgress && !halted)
        {
            ++g_stats.confirmFencesAvoidedHalt;
        }
        if (BridgeHalt::GAP == v.halt && cursor >= l.domain->halt().seen - 1)
        {
            if (l.exitSince < 0)
            {
                l.exitSince = it;
            }
        }
        else
        {
            l.exitSince = -1;
        }
        // (f') 4R.1 rule 2 (review N2-1): a premature 76 {GAP, 0} is self-healing: a HALTED(GAP) lane whose hole is open
        // never runs OPEN_HOLE_ITERATIONS of its iterations with the gate bit clear
        if (State::HALTED == v.state && BridgeHalt::GAP == v.halt && !bit && cursor < l.domain->halt().seen - 1)
        {
            if (++l.openHoleIts > OPEN_HOLE_ITERATIONS)
            {
                fail("the gate open with a hole open for > 5 s (lane " + std::to_string(l.slot) + ", halt {" +
                    std::to_string(l.domain->halt().e) + ", " + std::to_string(l.domain->halt().seen) + "}, cursor " +
                    std::to_string(cursor) + (prematureDone ? ", after a premature 76 {GAP, 0}" : "") + ")");
            }
        }
        else
        {
            l.openHoleIts = 0;
        }
        // (c) the decision in force: the cursor when the lane began confirming (or halted without confirming)
        // a kept halt (also through a re-scan sync, whose re-offers repeat the same decision) keeps it in force
        const bool decidingNow = l.domain->confirming() || halted || BridgeHalt::NONE != v.halt;
        if (decidingNow && l.deciding && BridgeHalt::NONE != v.halt && l.domain->halt().e != l.decisionE &&
            !l.domain->confirming())
        {
            l.deciding = false; // a new halt at another e: a new decision
        }
        if (decidingNow && !l.deciding)
        {
            l.deciding = true;
            l.decisionCursor = l.stepCursor;
            l.decisionE = BridgeHalt::NONE != v.halt ? l.domain->halt().e : v.expected;
        }
        else if (!decidingNow)
        {
            l.deciding = false;
        }
        {
            const std::int64_t ve = l.domain->verdicts().e();
            for (std::size_t i = 0; i < l.nodes.size(); ++i)
            {
                const Verdict &vv = l.domain->verdicts().at(i);
                if (VerdictKind::UNKNOWN != vv.kind && (l.verdictE[i] != ve || l.verdictSince[i] != vv.since))
                {
                    l.verdictE[i] = ve;
                    l.verdictSince[i] = vv.since;
                    l.verdictDelivers[i] = nodes[static_cast<std::size_t>(l.nodes[i])].delivers(ve);
                }
            }
        }
        // (e) a rotation due while the last report showed C' > last opens an episode until the next forward
        const bool pastCursor = pre.cursor > pre.last && State::STREAMING == pre.state;
        const bool failover = BridgeMetrics::get(l.metrics.sourceFailovers) > failovers0;
        if (!l.episode && pastCursor && (ahead1 > ahead0 || failover))
        {
            l.episode = true;
            ++g_stats.episodes;
            l.epCold = cold0;
            l.epFences = ahead0;
            l.epForwarded = fwd0;
            l.epSyncs = l.domain->syncs();
            l.epResumeBase = ~0ULL;
            l.epStartC = pre.cursor;
            l.epStartLast = pre.last;
        }
        if (l.episode)
        {
            // the episode is the cost of the rotation that was due with C' > last: it ends at the first resume after
            // the sync from C' started (the fence's own resume), at the next forward, or at a halt
            const std::uint64_t resumes = l.archiveMetrics.resumes(ResumeKind::COLD) + l.archiveMetrics.resumes(ResumeKind::PROOF);
            if (l.domain->syncs() > l.epSyncs && ~0ULL == l.epResumeBase)
            {
                l.epResumeBase = cold0 + proof0; // the resumes before the step in which the sync from C' started
            }
            const bool resumedAfterSync = ~0ULL != l.epResumeBase && resumes > l.epResumeBase;
            if (resumedAfterSync || l.forwarded() > l.epForwarded || halted)
            {
                l.episode = false;
                const std::uint64_t colds = l.archiveMetrics.resumes(ResumeKind::COLD) - l.epCold;
                const std::uint64_t fences = ahead1 - l.epFences;
                if (colds > fences)
                {
                    fail("cold walks per lane_ahead fence > 1 (lane " + std::to_string(l.slot) + ": " + std::to_string(colds) +
                        " cold walks over " + std::to_string(fences) + " lane_ahead fences in an episode that began with C' " +
                        std::to_string(l.epStartC) + " > last " + std::to_string(l.epStartLast) + ")");
                }
            }
        }
        if (debug)
        {
            std::printf("it %d lane %d id %lld state %s last %lld C %lld halt %s wait %s core %lld bit %d table %zu ov %d "
                        "cold %llu proof %llu ahead %llu fo %llu verify %llu syncs %llu cur %zu cmd %d fwd %llu ep %d he %lld hs %lld rep %llu stale %llu\n",
                it, l.slot, static_cast<long long>(l.identity), stateName(v.state), static_cast<long long>(v.last),
                static_cast<long long>(v.cursor), bridgeHaltName(v.halt), decisionRowName(l.domain->waiting()),
                static_cast<long long>(cursor), bit ? 1 : 0, table.size(), overflow ? 1 : 0,
                (unsigned long long)l.archiveMetrics.resumes(ResumeKind::COLD),
                (unsigned long long)l.archiveMetrics.resumes(ResumeKind::PROOF), (unsigned long long)ahead1,
                (unsigned long long)BridgeMetrics::get(l.metrics.sourceFailovers),
                (unsigned long long)BridgeMetrics::get(l.metrics.holeVerifications),
                (unsigned long long)l.domain->syncs(), l.domain->current(), static_cast<int>(cmd.kind),
                (unsigned long long)l.forwarded(), l.episode ? 1 : 0, (long long)l.domain->halt().e, (long long)l.domain->halt().seen, (unsigned long long)BridgeMetrics::get(l.metrics.reportQueries), (unsigned long long)BridgeMetrics::get(l.metrics.reportStale));
        }
    }

    bool covered() const
    {
        for (std::int64_t s = 1; s <= sc.n; ++s)
        {
            bool any = false;
            for (const auto &lp : lanes)
            {
                if (!lp)
                {
                    continue;
                }
                for (int k : lp->nodes)
                {
                    any = any || nodes[static_cast<std::size_t>(k)].has(s);
                }
            }
            if (!any)
            {
                return false;
            }
        }
        return true;
    }

    bool done() const
    {
        if (cursor != sc.n || bit)
        {
            return false;
        }
        for (const auto &lp : lanes)
        {
            if (!lp)
            {
                continue;
            }
            const BridgeView v = lp->domain->view();
            const bool stuck = lp->exitSince >= 0 && it - lp->exitSince >= STUCK_ITERATIONS; // recorded at the end
            if (!stuck && (v.last != sc.n || v.staged != 0 || BridgeHalt::NONE != v.halt || State::STREAMING != v.state))
            {
                return false;
            }
        }
        return true;
    }
};

Lane::Lane(Run &r, int s, int inc, std::int64_t id, const std::vector<int> &ns, int lg) :
    run(r), slot(s), incarnation(inc), identity(id), nodes(ns), lag(lg)
{
    settings = testSettings(nodes.size());
    settings.identity = identity;
    settings.correlationBase = 1'000'000LL * inc;
    settings.ringCapacity = 4096;
    settings.keyTableCapacity = 64;
    settings.reportInterval = 1'000ms;
    settings.reportIntervalHalted = 500ms;
    settings.queryResend = 50ms;
    settings.reportTimeout = 500ms;
    settings.invalidReportResend = 500ms;
    settings.feedHaltResend = 500ms;
    settings.haltRescan = 6'000ms;
    settings.behindDwell = 1'000ms;
    settings.behindRescan = 6'000ms;
    settings.holeGrace = 6'000ms;
    settings.unreachableRetry = 1'000ms;
    settings.probeInterval = 1'000ms;
    memo = std::make_unique<ResumeMemo>(nodes.size(), &archiveMetrics);
    domain = std::make_unique<BridgeDomain>(settings, *memo, archiveMetrics, encoder, *this, metrics);
    FollowSettings fs;
    fs.backoff = 100ms;
    fs.probeInterval = 1'000ms;
    fs.neverConnectedTimeout = 1'500ms;
    fs.endpointResolveTimeout = 1'000ms;
    sources.build(nodes.size(), *memo, archiveMetrics, domain.get(), fs);
    encoder.keepRecords = false;
    verdictE.assign(nodes.size(), 0);
    verdictSince.assign(nodes.size(), TimePoint{});
    verdictDelivers.assign(nodes.size(), false);
}

OfferOutcome Lane::tryOffer(const std::uint8_t *frame, std::size_t length) noexcept
{
    return run.offer(*this, frame, length);
}

constexpr int MAX_ITERATIONS = 16'000;

std::string runScenario(std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    Scenario sc = generate(rng);
    for (Event &e : sc.events)
    {
        e.fired = false;
        e.dueAt = -1;
    }
    Run run(sc, rng, seed);
    ++g_stats.runs;
    ++g_stats.shapes[static_cast<std::size_t>(sc.shape)];
    auto failText = [&](const std::string &why) {
        return "seed " + std::to_string(seed) + ": " + why + " :: " + sc.describe();
    };
    bool operatorDone = false;
    for (run.it = 0; run.it < MAX_ITERATIONS; ++run.it)
    {
        run.now += 20ms;
        run.fireEvents();
        if (run.noticeAt >= 0 && run.it >= run.noticeAt)
        {
            run.noticeNow();
        }
        if (sc.elections && std::uniform_int_distribution<int>(0, 199)(rng) == 0)
        {
            run.elect();
        }
        if (std::uniform_int_distribution<int>(0, 99)(rng) < sc.commitPercent)
        {
            run.commit();
        }
        for (auto &lp : run.lanes)
        {
            if (lp && run.it >= run.pausedUntil[static_cast<std::size_t>(lp->slot)])
            {
                run.stepLane(*lp);
            }
        }
        if (!run.failure.empty())
        {
            return failText(run.failure);
        }
        // N2-1: the premature operator clear, 0..100 iterations after a lane's hole is first seen open
        if (run.prematureArmed && !run.prematureDone)
        {
            if (run.prematureAt < 0 && run.laneHoleOpen())
            {
                run.prematureAt = run.it + std::uniform_int_distribution<int>(0, 100)(run.opRng);
            }
            if (run.prematureAt >= 0 && run.it >= run.prematureAt)
            {
                run.prematureDone = true;
                run.operatorPrematureClear();
            }
        }
        // N2-2: in a late-notice run, one extra late-notice election 0..50 iterations after a lane's hole is first seen
        // open while another lane streams (the side generator draws it), so the core's own record can open beside a lane's record
        if (run.lateNoticeArmed && !run.extraElectDone)
        {
            if (run.extraElectAt < 0 && run.laneHoleOpen() && run.laneStreaming())
            {
                run.extraElectAt = run.it + static_cast<int>(run.opRng() % 51);
            }
            // fired once an uncommitted data frame is pending (the lost tail the late notice hides), at most 200 late
            if (run.extraElectAt >= 0 && run.it >= run.extraElectAt &&
                (run.pendingData() || run.it >= run.extraElectAt + 200))
            {
                run.extraElectDone = true;
                run.elect(run.opRng, true);
            }
        }
        // the operator, in an overflow run, once every hole is passed: 76 {GAP, 0} then open
        if (!operatorDone && run.cursor == sc.n && (run.overflow || run.truthOverflow))
        {
            {
                // (f): the overflow record never cleared by reach
                if (!run.bit)
                {
                    return failText("the bit cleared under overflow (at the operator step)");
                }
                operatorDone = true;
                run.operatorClearAndOpen();
            }
        }
        if (run.done())
        {
            break;
        }
    }
    run.commit();
    if (run.overflowSeenInRun)
    {
        ++g_stats.overflowRuns;
    }
    if (run.coreAndLaneOpen)
    {
        ++g_stats.coreAndLaneRuns;
    }
    for (auto &lp : run.lanes)
    {
        if (!lp)
        {
            continue;
        }
        for (const auto &c : lp->metrics.haltRecovered)
        {
            g_stats.recoveries += static_cast<int>(BridgeMetrics::get(c));
        }
    }
    if (!run.failure.empty())
    {
        return failText(run.failure);
    }
    // 9.4 exit (a): a halted lane that keeps its GAP halt after the cursor passed seen - 1 fails the run (strict by default
    // since lane A's re-scan exit-(a) fix, S10' W4; RED RESCAN_EXIT_A_OFF). The counter stays for the summary line.
    for (auto &lp : run.lanes)
    {
        if (lp && lp->exitSince >= 0 && run.it - lp->exitSince >= STUCK_ITERATIONS)
        {
            ++g_stats.stuckHalts;
            if (g_stats.stuckSeed == 0)
            {
                g_stats.stuckSeed = seed;
            }
            return failText("a halted lane kept its GAP halt after the cursor passed seen - 1 (lane " +
                std::to_string(lp->slot) + ", halt {" + std::to_string(lp->domain->halt().e) + ", " +
                std::to_string(lp->domain->halt().seen) + "}, cursor " + std::to_string(run.cursor) + ")");
        }
    }
    // (b) liveness
    if (run.covered() && run.cursor != sc.n)
    {
        std::string lanes;
        for (auto &lp : run.lanes)
        {
            if (!lp)
            {
                lanes += " [dead]";
                continue;
            }
            const BridgeView v = lp->domain->view();
            lanes += std::string(" [") + stateName(v.state) + " halt " + bridgeHaltName(v.halt) + " last " +
                std::to_string(v.last) + " C " + std::to_string(v.cursor) + " waiting " +
                decisionRowName(lp->domain->waiting()) + "]";
        }
        return failText("liveness: the lanes' nodes carry every seq but the core stopped at " + std::to_string(run.cursor) +
            " of " + std::to_string(sc.n) + lanes);
    }
    // (c) every GAP record reach-cleared once the run completed; (f) the bit at the end iff a record is open
    if (run.cursor == sc.n && !run.truth.empty())
    {
        return failText("a GAP record still open at cursor N");
    }
    const bool shouldHold = !run.truth.empty() || run.truthOverflow || 0 != run.slotReason;
    if (run.bit != shouldHold)
    {
        return failText(run.bit ? "the gate bit is set at the end with no record open"
                                : "the bit cleared with a record open (at the end)");
    }
    if (run.cursor == sc.n)
    {
        ++g_stats.completed;
    }
    if (std::getenv("CT_BRIDGE_TWOLANE_SUMMARY"))
    {
        std::printf("seed %llu %s cursor %lld/%lld covered %d it %d overflow %d operator %d identities %zu\n",
            static_cast<unsigned long long>(seed), shapeName(sc.shape), static_cast<long long>(run.cursor),
            static_cast<long long>(sc.n), run.covered() ? 1 : 0, run.it, run.overflowSeenInRun ? 1 : 0, operatorDone ? 1 : 0,
            run.haltedIdentities.size());
    }
    return {};
}

}

TEST_CASE("TwoLaneDomainPropertyTest.twoLanesFeedTheCoreExactlyOnceAndTheGateReopensOnlyWhenEveryHoleCleared")
{
    int runs = 200;
    if (const char *env = std::getenv("CT_BRIDGE_TWOLANE_RUNS"))
    {
        runs = std::atoi(env);
    }
    std::uint64_t base = 0x2a4e5eedULL;
    if (const char *env = std::getenv("CT_BRIDGE_TWOLANE_SEED"))
    {
        base = std::strtoull(env, nullptr, 0);
    }
    g_stats = Stats{};
    int failures = 0;
    std::vector<std::string> reported;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; ++i)
    {
        const std::string result = runScenario(base + static_cast<std::uint64_t>(i));
        if (!result.empty())
        {
            ++failures;
            if (reported.size() < 8)
            {
                reported.push_back(result);
            }
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("runs " << g_stats.runs << " (seed base " << base << ", " << wall << " s): completed " << g_stats.completed
            << "; shapes random " << g_stats.shapes[0] << " lane-ahead " << g_stats.shapes[1] << " confirm "
            << g_stats.shapes[2] << " stale " << g_stats.shapes[3] << " two-holes " << g_stats.shapes[4] << " nested "
            << g_stats.shapes[5] << " overflow " << g_stats.shapes[6] << "; lane_ahead fences " << g_stats.laneAheadFences
            << ", confirm fences that avoided a halt " << g_stats.confirmFencesAvoidedHalt << ", lane_ahead episodes "
            << g_stats.episodes << ", stale FeedHalts " << g_stats.staleFeedHalts << ", GAP records " << g_stats.gapRecords
            << ", GAP records reach-cleared " << g_stats.gapReachCleared << ", reopens " << g_stats.reopens
            << ", two-record episodes with one reopen " << g_stats.twoRecordOneReopen << ", overflow runs "
            << g_stats.overflowRuns << " (operator cleared " << g_stats.overflowOperatorCleared << "), restarts "
            << g_stats.restarts << ", pauses " << g_stats.pauses << ", elections " << g_stats.elections
            << ", recoveries " << g_stats.recoveries << ", duplicates " << g_stats.duplicates << ", egress delivered late "
            << g_stats.lagEgress << "; stuck GAP halts (exit (a) not taken) " << g_stats.stuckHalts << " (first seed "
            << g_stats.stuckSeed << "); late-notice elections " << g_stats.lateNoticeElections << ", core GAP records " << g_stats.coreGapRecords << ", runs with a core and a lane record "
            << g_stats.coreAndLaneRuns << ", premature clears " << g_stats.prematureClears << " (re-latched "
            << g_stats.relatchedAfterClear << "); failures " << failures);
    for (const std::string &r : reported)
    {
        MESSAGE("FAILED " << r);
    }
    CHECK(failures == 0);
    if (failures == 0 && runs >= 400)
    {
        // (g) non-vacuity (C5): every mechanism the property is about ran, and recovered
        CHECK(g_stats.laneAheadFences > 0);
        CHECK(g_stats.episodes > 0);
        CHECK(g_stats.confirmFencesAvoidedHalt > 0);
        CHECK(g_stats.staleFeedHalts > 0);
        CHECK(g_stats.gapReachCleared > 0);
        CHECK(g_stats.twoRecordOneReopen > 0);
        CHECK(g_stats.overflowRuns > 0);
        CHECK(g_stats.overflowOperatorCleared > 0);
        CHECK(g_stats.restarts > 0);
        CHECK(g_stats.recoveries > 0);
        CHECK(g_stats.completed > runs / 2);
        // review N2-2: the identity-0 record ran, also together with a lane record; N2-1: premature clears re-latched
        CHECK(g_stats.coreGapRecords > 0);
        CHECK(g_stats.coreAndLaneRuns > 0);
        CHECK(g_stats.prematureClears > 0);
        CHECK(g_stats.relatchedAfterClear > 0);
    }
}
