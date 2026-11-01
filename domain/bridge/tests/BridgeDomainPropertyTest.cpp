// BridgeDomainPropertyTest (RC2 S9b 13.10): seeded random scenarios driven through S9a's REAL FollowPolicy +
// FollowerLoop + ResumeMemo over SimArchive sources, with the REAL BridgeDomain as the owner (replacing S9a's stub
// owner) and the stub core of BridgeTestSupport.h, which models the real core's outcomes (review FIX-12):
// SettleRejected FEED_GAP for every frame after a refusal and FEED_UNBOUND while unbound, decode poison that latches
// with NO SettleRejected, a 62 applied entry by entry stopping at the first refusal, FeedHalt per D3a, a v1/v2 phase
// that drops 68 and 66 and reports 0/0/NONE, and egress lost on a leader change.
//
// The scenario generator: 1-3 sources; re-journal step-backs; per-source holes; a whole-box hole; the K8 shape (a
// memo read sees a hole a memo-free read does not); padding; refused frames on a subset (also a superseded corrupt
// copy before good data); a foreign-epoch prefix recording on a subset; a dead node at its head; recordings stopped
// and succeeded mid-run; elections losing the uncommitted tail; one stub-core-refused seq (poisoned copies on a
// subset, or a core-only rule on every copy); back-pressure bursts; the ring forced small in 10 % of the runs; a
// v1/v2 phase before the core switches to 3.
//
// Asserted per run (13.10 a-j): (a) per sync the offered seqs are C + 1, C + 2, ... dense and ascending (staged
// ones included); (b) every entry the stub core receives is cursor + 1, a duplicate, or follows a loss the bridge
// had not yet been told about; (c) every halt matches the memo-free ground truth at e (no source delivers e; GAP
// only with every source holed or behind; EPOCH_MISMATCH only with every source foreign; SOURCE_SCHEMA only with a
// bad or foreign source); (d) liveness: with a clean source and no core refusal the run completes, cursor == N, no
// halt; (e) staging is dropped only at fences and session losses; (f) the ring never exceeds its capacity; (g) with
// every source behind, at most one failover per behindRescan; (h) non-vacuity counters over the seeds; (i) a
// stub-core refusal ends in CORE_REFUSED unless a source carries a copy the core accepts, the refused seq offered at
// most stormFences x sources times; (j) a v1/v2 phase never ends in a halt and the run binds after the switch.
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

using namespace ct_bridge_test;

namespace {

constexpr std::int64_t FOREIGN = 9;

struct Run
{
    std::int64_t from;
    std::int64_t to;
    std::int64_t epoch = EPOCH;
};

struct Scenario
{
    std::int64_t n = 0;
    std::size_t sources = 0;
    std::int64_t wholeBoxHole = 0;
    std::vector<std::vector<Run>> runs;
    std::vector<std::int64_t> badSeq;
    std::vector<int> badFirstCopy;
    std::vector<std::vector<std::int64_t>> pads;
    std::vector<int> deferredRun;
    std::vector<int> stopAt;
    std::vector<int> successorDelay;
    std::vector<std::uint16_t> templates; // per seq (index 0 unused)
    std::int64_t poisonSeq = 0;           // 0 = none
    std::vector<int> poisoned;            // per source: its copy of poisonSeq is poisoned
    std::int64_t coreRefuseSeq = 0;       // a core-only rule refuses every copy of this seq
    bool elections = false;
    bool smallRing = false;
    double retryProbability = 0;
    int v2Until = -1;                     // iteration the core switches to 3 (-1: v3 from the start)
    bool foreignPrefix = false;
    bool slow = false;
    int lag = 1;             // FIX-1 (2026-10-31 review): egress reaches the domain 1..8 duty iterations after the commit
    std::int64_t foreignTail = 0; // the ME was re-genesised without the wipe: epoch 9 records from this seq on, on every source
    bool postHaltEvents = false;  // FIX-3: an operator fill, a second lane past e, a core re-genesis after a halt
    std::int64_t startCursor = 0; // the core starts bound at this cursor (0: unbound)           // some source holds an old-epoch recording at its chain head (core bound at 1)

    std::string describe() const
    {
        std::ostringstream o;
        o << "n=" << n << " hole=" << wholeBoxHole << " poison=" << poisonSeq << " coreRefuse=" << coreRefuseSeq
          << " elections=" << elections << " smallRing=" << smallRing << " retry=" << retryProbability
          << " v2Until=" << v2Until << " slow=" << slow << " startCursor=" << startCursor << " lag=" << lag
          << " foreignTail=" << foreignTail << " postHalt=" << postHaltEvents;
        for (std::size_t s = 0; s < sources; ++s)
        {
            o << " | src" << s << (badSeq[s] ? " bad=" + std::to_string(badSeq[s]) : "")
              << (poisoned[s] ? " poisoned" : "") << ":";
            for (const auto &r : runs[s])
            {
                o << " [" << r.from << "," << r.to << (r.epoch != EPOCH ? "@9" : "") << "]";
            }
            if (deferredRun[s] >= 0)
            {
                o << " deferred#" << deferredRun[s] << "@" << stopAt[s] << "+" << successorDelay[s];
            }
        }
        return o.str();
    }
};

Scenario generate(std::mt19937_64 &rng)
{
    auto pick = [&](std::int64_t lo, std::int64_t hi) {
        return lo >= hi ? lo : std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    auto coin = [&](double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; };
    Scenario sc;
    sc.n = pick(8, 60);
    sc.sources = static_cast<std::size_t>(pick(1, 3));
    sc.wholeBoxHole = coin(0.12) ? pick(2, sc.n - 1) : 0;
    sc.runs.resize(sc.sources);
    sc.badSeq.assign(sc.sources, 0);
    sc.badFirstCopy.assign(sc.sources, 0);
    sc.pads.resize(sc.sources);
    sc.deferredRun.assign(sc.sources, -1);
    sc.stopAt.assign(sc.sources, 0);
    sc.successorDelay.assign(sc.sources, 0);
    sc.poisoned.assign(sc.sources, 0);
    sc.templates.assign(static_cast<std::size_t>(sc.n + 1), TEMPLATE_TRADE);
    sc.templates[1] = TEMPLATE_CONFIG;
    for (std::int64_t s = 2; s <= sc.n; ++s)
    {
        const int t = static_cast<int>(pick(0, 7));
        sc.templates[static_cast<std::size_t>(s)] = t < 4 ? TEMPLATE_TRADE : static_cast<std::uint16_t>(t - 2); // 2, 3, 4, 5->4
        if (sc.templates[static_cast<std::size_t>(s)] == 5)
        {
            sc.templates[static_cast<std::size_t>(s)] = TEMPLATE_RELEASE;
        }
    }
    for (std::size_t s = 0; s < sc.sources; ++s)
    {
        auto &runs = sc.runs[s];
        if (coin(0.1))
        {
            runs.push_back({ 1, pick(1, sc.n), FOREIGN }); // an unwiped old-epoch recording at the chain head
        }
        if (!sc.wholeBoxHole && sc.n >= 16 && coin(0.3))
        {
            const std::int64_t a = pick(4, sc.n - 8);
            runs.push_back({ 1, a + 5 });
            runs.push_back({ a - 2, a });
            runs.push_back({ a + 3, sc.n }); // K8: a memo read resumed after the short run sees a false hole
            if (1 == sc.sources && coin(0.5))
            {
                sc.startCursor = a; // the core already at a: the short run is proven by its skips at the first read
            }
        }
        else
        {
            std::int64_t from = 1;
            for (int guard = 0; guard < 10; ++guard)
            {
                const bool last = runs.size() >= 5 || coin(0.3);
                std::int64_t to = last ? sc.n : pick(from - 1, sc.n);
                if (sc.wholeBoxHole && from <= sc.wholeBoxHole && sc.wholeBoxHole <= to)
                {
                    runs.push_back({ from, sc.wholeBoxHole - 1 });
                    from = sc.wholeBoxHole + 1;
                    continue;
                }
                runs.push_back({ from, to });
                if (last && to >= sc.n)
                {
                    break;
                }
                const double r = std::uniform_real_distribution<double>(0, 1)(rng);
                if (r < 0.45)
                {
                    from = to + 1;
                }
                else if (r < 0.85)
                {
                    from = pick(std::max<std::int64_t>(1, runs.back().from), to + 1); // re-journal step back
                }
                else
                {
                    from = to + 1 + pick(1, 3); // a per-source hole across the boundary
                }
                from = std::min(from, sc.n);
                if (sc.wholeBoxHole && from == sc.wholeBoxHole)
                {
                    from = sc.wholeBoxHole + 1;
                }
            }
            if (runs.empty() || runs.back().to < sc.n)
            {
                runs.push_back({ from, sc.n });
                if (sc.wholeBoxHole && runs.back().from <= sc.wholeBoxHole && sc.wholeBoxHole <= sc.n)
                {
                    runs.back() = { sc.wholeBoxHole + 1, sc.n };
                }
            }
        }
        if (coin(0.15))
        {
            // a dead node at its head: it stops below n and never records again
            const std::int64_t deadAt = pick(1, sc.n - 1);
            std::vector<Run> cut;
            for (const Run &r : runs)
            {
                if (r.epoch != EPOCH || r.from > deadAt)
                {
                    if (r.epoch != EPOCH)
                    {
                        cut.push_back(r);
                    }
                    continue;
                }
                cut.push_back({ r.from, std::min(r.to, deadAt), r.epoch });
            }
            runs = cut;
        }
        if (coin(0.15))
        {
            sc.badSeq[s] = pick(1, sc.n);
            sc.badFirstCopy[s] = coin(0.4) ? 1 : 0;
        }
        for (int p = 0; p < 2; ++p)
        {
            if (coin(0.3))
            {
                sc.pads[s].push_back(pick(1, sc.n));
            }
        }
        if (!runs.empty())
        {
            const Run lastRun = runs.back();
            if (lastRun.epoch == EPOCH && lastRun.to - lastRun.from >= 3 && coin(0.3))
            {
                const std::int64_t mid = pick(lastRun.from + 1, lastRun.to - 1);
                const std::int64_t next = pick(std::max<std::int64_t>(lastRun.from, mid - 2), mid + 1);
                runs.back() = { lastRun.from, mid };
                runs.push_back({ next, lastRun.to });
                sc.deferredRun[s] = static_cast<int>(runs.size() - 1);
                sc.stopAt[s] = static_cast<int>(pick(10, 150));
                // recorded within 5 s of the start (holeGrace is 6 s here): a successor later than the grace is a
                // blind period D-S9b-3 bounds on purpose (the halt is then the right outcome, not a liveness defect)
                sc.successorDelay[s] = static_cast<int>(pick(0, 100));
            }
        }
    }
    if (coin(0.1))
    {
        sc.poisonSeq = pick(2, sc.n);
        bool any = false;
        for (std::size_t s = 0; s < sc.sources; ++s)
        {
            sc.poisoned[s] = coin(0.6) ? 1 : 0;
            any = any || sc.poisoned[s];
        }
        if (!any)
        {
            sc.poisoned[0] = 1;
        }
    }
    else if (coin(0.04))
    {
        sc.coreRefuseSeq = pick(2, sc.n);
    }
    sc.elections = 0 == sc.poisonSeq && 0 == sc.coreRefuseSeq && (coin(0.4) || 0 != sc.startCursor);
    sc.smallRing = coin(0.1);
    sc.slow = coin(0.3); // a small poll budget and frequent reports: the read and the cursor interleave (K8 proofs)
    sc.retryProbability = coin(0.3) ? 0.05 : 0.0;
    sc.v2Until = coin(0.1) ? static_cast<int>(pick(100, 1500)) : -1;
    sc.lag = static_cast<int>(pick(1, 8));
    sc.postHaltEvents = coin(0.6);
    if (0 == sc.poisonSeq && 0 == sc.coreRefuseSeq && sc.n >= 6 && coin(0.08))
    {
        // the ME was re-genesised and its old archives kept: own-epoch records first, then epoch 9 from a seq on
        sc.foreignTail = pick(2, sc.n);
    }
    if (0 != sc.startCursor)
    {
        sc.v2Until = -1;
        sc.slow = true;
    }
    for (const auto &runs : sc.runs)
    {
        for (const Run &r : runs)
        {
            if (r.epoch != EPOCH)
            {
                // 15b.3: an unbound sync that meets a stale FRESH of another epoch at seq 1 latches the core
                // SOURCE_UNBOUND (the genesis runbook wipes the archives); the foreign-prefix shape is the bound case
                sc.foreignPrefix = true;
                sc.v2Until = -1;
            }
        }
    }
    return sc;
}

Bytes recordFor(const Scenario &sc, std::size_t source, std::int64_t seq, std::int64_t epoch)
{
    const std::uint16_t t = sc.templates[static_cast<std::size_t>(seq)];
    Bytes b = TEMPLATE_RELEASE == t ? makeRelease(seq, epoch, static_cast<std::uint16_t>(seq % 3))
                                    : makeFrame(t, seq, epoch, 64 * seq);
    if (TEMPLATE_RELEASE == t)
    {
        putLe64(b, FRAME_OFFSET_EGRESS_SEQ, 64 * seq);
    }
    if (seq == sc.poisonSeq && sc.poisoned[source] && epoch == EPOCH)
    {
        b.back() = FLAG_POISON;
    }
    return b;
}

void buildRun(const Scenario &sc, std::size_t s, SimArchive &archive, std::size_t i)
{
    const auto &runs = sc.runs[s];
    SimRecording &rec = archive.record();
    std::size_t badRun = runs.size();
    for (std::size_t j = 0; j < runs.size(); ++j)
    {
        if (runs[j].epoch == EPOCH && runs[j].from <= sc.badSeq[s] && sc.badSeq[s] <= runs[j].to &&
            sc.badSeq[s] != sc.wholeBoxHole)
        {
            badRun = j;
            if (sc.badFirstCopy[s])
            {
                break;
            }
        }
    }
    for (std::int64_t seq = runs[i].from; seq <= runs[i].to; ++seq)
    {
        if (seq == sc.wholeBoxHole && runs[i].epoch == EPOCH)
        {
            continue;
        }
        for (std::int64_t padAt : sc.pads[s])
        {
            if (padAt == seq)
            {
                archive.pad(rec, 64);
            }
        }
        const std::int64_t epoch = sc.foreignTail && seq >= sc.foreignTail && runs[i].epoch == EPOCH ? FOREIGN : runs[i].epoch;
        Bytes frame = recordFor(sc, s, seq, epoch);
        if (seq == sc.badSeq[s] && i == badRun)
        {
            putLe16(frame, 4, 3); // a schema 3 frame
        }
        archive.append(rec, frame);
    }
}

void build(const Scenario &sc, std::size_t s, SimArchive &archive)
{
    const auto &runs = sc.runs[s];
    const std::size_t upTo = sc.deferredRun[s] >= 0 ? static_cast<std::size_t>(sc.deferredRun[s]) : runs.size();
    for (std::size_t i = 0; i < upTo; ++i)
    {
        buildRun(sc, s, archive, i);
        if (i + 1 < upTo)
        {
            archive.stop(archive.recordings.back());
        }
    }
}

// Ground truth: a memo-free read of source s from last = e - 1 (as the archives are now).
enum class Truth { DELIVERS, HOLE, BAD, FOREIGN, NOTHING };
const char *truthName(Truth t)
{
    switch (t)
    {
        case Truth::DELIVERS: return "delivers";
        case Truth::HOLE: return "hole";
        case Truth::BAD: return "bad";
        case Truth::FOREIGN: return "foreign";
        case Truth::NOTHING: return "nothing";
    }
    return "?";
}

// S10' 5.3: some recording holds a valid own-epoch copy of seq
bool holdsValid(const SimArchive &archive, std::int64_t seq, std::int64_t epoch)
{
    for (const auto &rec : archive.recordings)
    {
        for (const auto &f : rec.frames)
        {
            if (f.bytes.empty())
            {
                continue;
            }
            const Admission a = admitFrame(f.flags, f.bytes.data(), f.bytes.size());
            if (a.accepted() && a.peek.journalSeq == seq && (0 == epoch || a.peek.meEpoch == epoch))
            {
                return true;
            }
        }
    }
    return false;
}

Truth memoFree(const SimArchive &archive, std::int64_t e, std::int64_t epoch)
{
    bool ownSeen = false; // NOTE-8 (2026-10-31): a foreign frame before any own-epoch frame is a stale prefix: BAD
    for (const auto &rec : archive.recordings)
    {
        for (const auto &f : rec.frames)
        {
            if (f.bytes.empty())
            {
                continue;
            }
            const Admission a = admitFrame(f.flags, f.bytes.data(), f.bytes.size());
            if (!a.accepted())
            {
                return Truth::BAD;
            }
            if (0 != epoch && a.peek.meEpoch != epoch)
            {
                return ownSeen ? Truth::FOREIGN : Truth::BAD;
            }
            ownSeen = true;
            if (a.peek.journalSeq < e)
            {
                continue;
            }
            return a.peek.journalSeq == e ? Truth::DELIVERS : Truth::HOLE;
        }
    }
    return Truth::NOTHING;
}

struct Stats
{
    int runs = 0, completed = 0, gaps = 0, schemas = 0, epochs = 0, mixed = 0, coreRefused = 0, behindFailovers = 0;
    int verifications = 0, storms = 0, fences = 0, ringFullPauses = 0, elections = 0, v2Runs = 0, poisonRuns = 0;
    int recovered = 0, haltsWithLag = 0;
    int recoveredGap = 0, recoveredSchema = 0, recoveredEpoch = 0, recoveredRefused = 0;
    int fills = 0, lanes = 0, regeneses = 0;
    // RC2 S10' 5.3: in-place successors (FILL / RESTART / above e / SOURCE_SCHEMA valid copies) and their recoveries
    int successorFills = 0, successorRestarts = 0, successorsAbove = 0, schemaSuccessors = 0;
    int successorAttaches = 0, stepBacks = 0, schemaRescanRecoveries = 0, successorRecoveries = 0;
};
Stats g_stats;
// S10' lane A (2026-11-01, carried finding): the S9b row-3 shape where a BEHIND verdict on a source the domain no
// longer follows goes stale (its successor recording appears during hole_grace) and the grace-expiry decision halts
// although a memo-free read now delivers e. Set only by staleBehindHaltRecoversByTheReScan: the halt-time "delivers"
// check is recorded instead of failed there, and the run must still end complete (every record once, in order).
bool g_staleBehindHaltTolerated = false;
int g_staleBehindHalts = 0;

// The duty loop of section 3 with a checking core port in front of the stub.
struct Property : CorePort
{
    const Scenario &sc;
    std::mt19937_64 &rng;
    ArchiveMetrics archiveMetrics;
    std::unique_ptr<ResumeMemo> memo;
    FakeEncoder encoder;
    StubCore core;
    BridgeMetrics metrics;
    BridgeSettings settings;
    std::unique_ptr<BridgeDomain> domain;
    SimSources sources;
    TimePoint now{};
    std::string failure;

    // (a)
    std::uint64_t offerSync = ~0ULL;
    std::int64_t nextInSync = 0;
    // (b)
    std::size_t knownBoundary = 0;          // losses at offer index < this are known to the domain
    std::vector<std::size_t> losses;        // offer indices of frames that will never apply
    std::map<std::int64_t, std::size_t> queryIndex; // correlation id -> offer index of its first send
    // (i)
    int offersOfRefused = 0;
    // (g)
    TimePoint lastBehindFailover{};
    bool haveBehindFailover = false;

    Property(const Scenario &s, std::mt19937_64 &r) : sc(s), rng(r)
    {
        settings = testSettings(sc.sources);
        settings.ringCapacity = sc.smallRing ? 96 : 4096; // 96: full above 32 in flight
        settings.keyTableCapacity = 64;
        settings.reportInterval = sc.slow ? 100ms : 1'000ms;
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
        memo = std::make_unique<ResumeMemo>(sc.sources, &archiveMetrics);
        domain = std::make_unique<BridgeDomain>(settings, *memo, archiveMetrics, encoder, *this, metrics);
        FollowSettings fs;
        fs.backoff = 100ms;
        fs.probeInterval = 1'000ms;
        fs.neverConnectedTimeout = 1'500ms;
        fs.endpointResolveTimeout = 1'000ms;
        sources.build(sc.sources, *memo, archiveMetrics, domain.get(), fs);
        for (std::size_t s = 0; s < sc.sources; ++s)
        {
            build(sc, s, *sources.archives[s]);
        }
        core.commitImmediately = false;
        core.v3 = sc.v2Until < 0;
        if (sc.foreignPrefix || 0 != sc.startCursor)
        {
            core.bound = EPOCH;
            core.cursor = std::max<std::int64_t>(1, sc.startCursor);
        }
        if (sc.coreRefuseSeq)
        {
            core.refuseSeqs = { sc.coreRefuseSeq };
        }
        core.lossHook = [this](std::size_t index) { losses.push_back(index); };
        core.gapHook = [this](std::size_t index, std::int64_t seq) {
            // (b): an entry above cursor + 1 is legal only behind a loss the domain had not been told about when the
            // frame was offered
            bool excused = false;
            for (std::size_t l : losses)
            {
                if (l < index && l >= knownAt[index])
                {
                    excused = true;
                    break;
                }
            }
            if (!excused && failure.empty())
            {
                failure = "the core received seq " + std::to_string(seq) + " above cursor + 1 (" +
                    std::to_string(core.cursor + 1) + ") with no unknown loss before it";
            }
        };
    }

    std::vector<std::size_t> knownAt; // per offer index: knownBoundary when it was offered

    OfferOutcome tryOffer(const std::uint8_t *frame, std::size_t length) noexcept override
    {
        if (sc.retryProbability > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < sc.retryProbability)
        {
            return OfferOutcome::RETRY; // back-pressure: nothing advanced
        }
        const FakeFrame f = decodeFake(frame, length);
        knownAt.push_back(knownBoundary);
        if (T_QUERY_FEED_POSITION == f.moneyTemplate && !queryIndex.count(f.correlationId))
        {
            queryIndex[f.correlationId] = core.offered.size();
        }
        if (!f.records.empty())
        {
            // (a): per sync the offers are C + 1, C + 2, ... (staged ones included, in ring order)
            if (domain->syncs() != offerSync)
            {
                offerSync = domain->syncs();
                nextInSync = domain->syncCursor() + 1;
            }
            for (const FakeRecord &r : f.records)
            {
                if (r.seq != nextInSync && failure.empty())
                {
                    failure = "sync from " + std::to_string(domain->syncCursor()) + " offered " + std::to_string(r.seq) +
                        " where " + std::to_string(nextInSync) + " was due";
                }
                nextInSync = r.seq + 1;
                if (r.seq == (sc.poisonSeq ? sc.poisonSeq : sc.coreRefuseSeq) && core.v3 && 0 != core.bound)
                {
                    ++offersOfRefused; // an offer a behaviour 3, bound core judges (the v1/v2 phase drops them all)
                }
            }
        }
        if (debug)
        {
            std::printf("  offer t%u n%zu first=%lld sync=%llu cur=%lld\n", f.moneyTemplate, f.records.size(),
                f.records.empty() ? (long long)f.correlationId : (long long)f.records.front().seq,
                (unsigned long long)domain->syncs(), (long long)core.cursor);
        }
        return core.tryOffer(frame, length);
    }
    bool debug = std::getenv("CT_BRIDGE_PROPERTY_DEBUG") != nullptr;

    // FIX-1: what the core emitted reaches the domain sc.lag duty iterations later, in order
    std::deque<std::pair<int, CoreEgress>> egressQueue;

    void deliver(int it)
    {
        while (!core.egress.empty())
        {
            egressQueue.emplace_back(it, core.egress.front());
            core.egress.pop_front();
        }
        // committed egress reaches the domain in order; what it learns moves the known-loss boundary
        while (!egressQueue.empty() && egressQueue.front().first + sc.lag <= it)
        {
            const CoreEgress e = egressQueue.front().second;
            egressQueue.pop_front();
            switch (e.kind)
            {
                case CoreEgress::REPORT:
                {
                    auto it = queryIndex.find(e.report.correlationId);
                    if (it != queryIndex.end())
                    {
                        knownBoundary = std::max(knownBoundary, it->second);
                    }
                    domain->onReport(e.report, now);
                    break;
                }
                case CoreEgress::REJECT:
                    knownBoundary = std::max(knownBoundary, core.offered.size());
                    domain->onSettleRejected(e.reject, now);
                    break;
                case CoreEgress::ACK:
                    domain->onAdminAck(e.ack, now);
                    break;
            }
        }
    }

    // FIX-3: an operator refills source s with a complete journal of `epoch` (one active recording, nothing bad)
    // RC2 S10' 5.3: an in-place successor recording on source s (the ME re-journals from `from` in place: the active
    // recording stops, a new one holds from..to with valid copies); the old recordings stay in the chain
    void successor(std::size_t s, std::int64_t from, std::int64_t to, std::int64_t epoch)
    {
        SimArchive &a = *sources.archives[s];
        if (!a.recordings.empty())
        {
            a.stop(a.recordings.back());
        }
        SimRecording &r = a.record();
        for (std::int64_t seq = from; seq <= to; ++seq)
        {
            const std::uint16_t t = sc.templates[static_cast<std::size_t>(seq)];
            Bytes b = TEMPLATE_RELEASE == t ? makeRelease(seq, epoch, static_cast<std::uint16_t>(seq % 3))
                                            : makeFrame(t, seq, epoch, 64 * seq);
            putLe64(b, FRAME_OFFSET_EGRESS_SEQ, 64 * seq);
            a.append(r, b);
        }
    }

    void fill(std::size_t s, std::int64_t epoch)
    {
        SimArchive &a = *sources.archives[s];
        a.recordings.clear();
        SimRecording &r = a.record();
        for (std::int64_t seq = 1; seq <= sc.n; ++seq)
        {
            const std::uint16_t t = sc.templates[static_cast<std::size_t>(seq)];
            Bytes b = TEMPLATE_RELEASE == t ? makeRelease(seq, epoch, static_cast<std::uint16_t>(seq % 3))
                                            : makeFrame(t, seq, epoch, 64 * seq);
            putLe64(b, FRAME_OFFSET_EGRESS_SEQ, 64 * seq);
            a.append(r, b);
        }
    }
};

std::string runScenario(std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    const Scenario sc = generate(rng);
    Property p(sc, rng);
    auto fail = [&](const std::string &why) { return "seed " + std::to_string(seed) + ": " + why + " :: " + sc.describe(); };
    ++g_stats.runs;
    g_stats.v2Runs += sc.v2Until >= 0 ? 1 : 0;
    g_stats.poisonRuns += (sc.poisonSeq || sc.coreRefuseSeq) ? 1 : 0;

    p.sources.start(p.now, 0);
    p.domain->onSessionUp(p.now);
    int sessionUpAt = -1;
    bool haltSeen = false, haltChecked = false;
    std::vector<Truth> snapshot;
    std::int64_t snapshotE = 0;
    bool v2Halted = false;
    bool v2HaltClean = false;
    std::int64_t currentEpoch = EPOCH;  // FIX-3: a re-genesis moves the run to epoch 9
    int eventAt = -1;                    // FIX-3: the post-halt event of this halt
    bool eventDone = false;              // at most one post-halt event per run
    BridgeHalt pendingKind = BridgeHalt::NONE;
    std::int64_t pendingE = 0;           // a halt waiting to be recovered: its kind and e
    // S10' 5.3: an in-place successor holding a valid e was recorded for the halt at successorE (in time for a re-scan)
    std::int64_t successorE = 0;
    bool successorRecovered = false;
    bool successorSchema = false;        // the successor carried valid copies for a SOURCE_SCHEMA halt (R0)
    std::int64_t fillE = 0;              // a FILL successor (only e) on fillSource: e + 1 lies in an earlier recording
    std::size_t fillSource = 0;
    std::uint64_t attachesAtHalt = 0;
    for (int it = 0; it < 12'000; ++it)
    {
        p.now += 20ms;
        if (!p.failure.empty())
        {
            return fail(p.failure);
        }
        if (sc.v2Until >= 0 && it == sc.v2Until)
        {
            p.core.v3 = true;
        }
        // the ME writer goes away, the successor is recorded later
        for (std::size_t s = 0; s < sc.sources; ++s)
        {
            if (sc.deferredRun[s] < 0)
            {
                continue;
            }
            SimArchive &a = *p.sources.archives[s];
            if (it == sc.stopAt[s] && !a.recordings.empty())
            {
                a.stop(a.recordings.back());
            }
            if (it == sc.stopAt[s] + sc.successorDelay[s])
            {
                buildRun(sc, s, a, static_cast<std::size_t>(sc.deferredRun[s]));
            }
        }
        // the core commits in order, now and then; an election loses the uncommitted tail, then the fence
        if (sc.elections && std::uniform_int_distribution<int>(0, 59)(rng) == 0)
        {
            ++g_stats.elections;
            p.core.elect();
            // egress committed but not yet delivered may be lost too
            if (std::uniform_int_distribution<int>(0, 1)(rng) == 0)
            {
                p.core.egress.clear();
                p.egressQueue.clear();
            }
            p.knownBoundary = p.core.offered.size();
            p.domain->onFence(FenceTrigger::NEW_LEADER, p.now);
            sessionUpAt = it + static_cast<int>(std::uniform_int_distribution<int>(1, 20)(rng));
        }
        if (it == sessionUpAt)
        {
            p.domain->onSessionUp(p.now); // the new term's registration was acknowledged
        }
        if (std::uniform_int_distribution<int>(0, 99)(rng) < (sc.slow ? 15 : 70))
        {
            p.core.commit(); // a slow core lets the cursor linger while the follower re-reads (the K8 proofs)
        }
        // 1. egress
        p.deliver(it);
        const std::uint64_t droppedBefore = BridgeMetrics::get(p.metrics.stagedDropped);
        const std::uint64_t fencesBefore = p.metrics.totalFences();
        const DecisionRow waitBefore = p.domain->waiting();
        const std::uint64_t failoversBefore = BridgeMetrics::get(p.metrics.sourceFailovers);
        // 3-6. the duty iteration
        const int domainLimit = p.domain->fragmentLimit();
        // the harness may poll with a smaller budget than the domain allows (never a larger one)
        const int limit = sc.slow && domainLimit > 0 ? std::min(domainLimit, static_cast<int>(std::uniform_int_distribution<int>(1, 4)(rng)))
                                                     : domainLimit;
        const int n = p.sources.doWork(*p.domain, limit, p.now);
        p.domain->onPollDone(n, p.now);
        const SourceCommand cmd = p.domain->takeSourceCommand();
        if (p.debug)
        {
            const BridgeView dv = p.domain->view();
            std::printf("it %d state %s last %lld C %lld halt %s wait %s cmd %d->%zu limit %d n %d core %lld latch %u cur %zu fo %llu park %llu v %d%d%d\n", it,
                stateName(dv.state), (long long)dv.last, (long long)dv.cursor, bridgeHaltName(dv.halt),
                decisionRowName(p.domain->waiting()), static_cast<int>(cmd.kind), cmd.index, limit, n, (long long)p.core.cursor,
                p.core.latch, p.domain->current(), (unsigned long long)BridgeMetrics::get(p.metrics.sourceFailovers),
                (unsigned long long)BridgeMetrics::get(p.metrics.sourceParks),
                static_cast<int>(p.domain->verdicts().at(0).kind), sc.sources > 1 ? static_cast<int>(p.domain->verdicts().at(1).kind) : 0,
                sc.sources > 2 ? static_cast<int>(p.domain->verdicts().at(2).kind) : 0);
            std::printf("   q %d stale %llu reg %d eq %zu pend %zu\n", p.core.queries, (unsigned long long)BridgeMetrics::get(p.metrics.reportStale), p.domain->registeredThisTerm() ? 1 : 0, p.egressQueue.size(), p.core.pending.size());
        }
        if (SourceCommandKind::SWITCH == cmd.kind)
        {
            p.sources.switchTo(cmd.index, p.now);
        }
        else if (SourceCommandKind::REATTACH == cmd.kind)
        {
            p.sources.reattach(p.now);
        }
        p.domain->tick(p.now);
        if (!p.failure.empty())
        {
            return fail(p.failure);
        }
        // (e) staging dropped only at fences and session losses
        if (BridgeMetrics::get(p.metrics.stagedDropped) != droppedBefore && p.metrics.totalFences() == fencesBefore)
        {
            return fail("staged trades dropped without a fence");
        }
        // (f) the ring never exceeds its capacity
        if (p.domain->ring().size() > p.domain->ring().capacity() || p.domain->ring().dropped() > 0)
        {
            return fail("the in-flight ring overflowed");
        }
        // (g) with every source behind at most one failover per behindRescan
        if (DecisionRow::ALL_BEHIND == waitBefore && BridgeMetrics::get(p.metrics.sourceFailovers) > failoversBefore)
        {
            ++g_stats.behindFailovers;
            if (p.haveBehindFailover && p.now - p.lastBehindFailover < p.settings.behindRescan - 100ms)
            {
                return fail("two all-behind failovers within one behindRescan");
            }
            p.haveBehindFailover = true;
            p.lastBehindFailover = p.now;
        }
        // (c) snapshot the memo-free truth when the decision is taken (the confirming report follows)
        const DecisionRow w = p.domain->waiting();
        const bool deciding = DecisionRow::GAP == w || DecisionRow::EPOCH == w || DecisionRow::SCHEMA == w;
        if (deciding && snapshot.empty())
        {
            snapshotE = p.domain->view().expected;
            for (std::size_t s = 0; s < sc.sources; ++s)
            {
                snapshot.push_back(memoFree(*p.sources.archives[s], snapshotE, p.domain->view().syncEpoch));
            }
        }
        const BridgeView v = p.domain->view();
        const bool haltedNow = (State::HALTED == v.state || State::HALTING == v.state) && BridgeHalt::NONE != v.halt;
        if (haltedNow && sc.v2Until >= 0 && it < sc.v2Until && !v2Halted)
        {
            // S10' W4: whether the journal was clean is judged when the v1/v2 halt happens, not at the end of the run (a
            // post-halt fill can add seq 1 later, and with 5.1 R1 the halted bridge now reaches it)
            v2Halted = true;
            v2HaltClean = false;
            for (std::size_t s = 0; s < sc.sources && !v2HaltClean; ++s)
            {
                if (sc.poisonSeq && sc.poisoned[s])
                {
                    continue;
                }
                bool all = true;
                for (std::int64_t e = 1; e <= sc.n && all; ++e)
                {
                    all = Truth::DELIVERS == memoFree(*p.sources.archives[s], e, currentEpoch);
                }
                v2HaltClean = all;
            }
        }
        if (haltedNow && !haltChecked && BridgeHalt::CORE_REFUSED != v.halt)
        {
            haltChecked = true;
            haltSeen = true;
            const std::int64_t e = p.domain->halt().e;
            if (snapshot.empty() || snapshotE != e)
            {
                snapshot.clear();
                for (std::size_t s = 0; s < sc.sources; ++s)
                {
                    snapshot.push_back(memoFree(*p.sources.archives[s], e, p.domain->halt().syncEpoch));
                }
            }
            bool anyBadOrForeign = false, allForeign = true, anyHole = false;
            std::string truths;
            for (std::size_t s = 0; s < sc.sources; ++s)
            {
                const Truth t = snapshot[s];
                truths += std::string(" src") + std::to_string(s) + "=" + truthName(t);
                if (Truth::DELIVERS == t && g_staleBehindHaltTolerated)
                {
                    ++g_staleBehindHalts; // recorded; the class checks below judge the other sources
                    continue;
                }
                if (Truth::DELIVERS == t)
                {
                    return fail(std::string("halt ") + bridgeHaltName(v.halt) + " at " + std::to_string(e) +
                        " although a memo-free read delivers it:" + truths);
                }
                anyBadOrForeign = anyBadOrForeign || Truth::BAD == t || Truth::FOREIGN == t;
                allForeign = allForeign && Truth::FOREIGN == t;
                anyHole = anyHole || Truth::HOLE == t;
            }
            for (std::size_t s = 0; s < sc.sources; ++s)
            {
                truths += std::string(" src") + std::to_string(s) + "=" + truthName(snapshot[s]);
            }
            // S10' 5.3 R2: after a FILL successor (only e), e + 1 sits in the earlier recording: one memo-free re-read
            // reaches it, so a halt at e + 1 while that recording holds a valid e + 1 is a missing step back
            if (0 != fillE && e == fillE + 1 && !sc.elections && holdsValid(*p.sources.archives[fillSource], e, currentEpoch))
            {
                return fail("halted again at e + 1 with the old recording holding it (FILL successor at " +
                    std::to_string(fillE) + ", source " + std::to_string(fillSource) + ")");
            }
            if (BridgeHalt::GAP == v.halt && anyBadOrForeign)
            {
                return fail("GAP at " + std::to_string(e) + " with a bad or foreign source:" + truths);
            }
            if (BridgeHalt::EPOCH_MISMATCH == v.halt && !allForeign)
            {
                return fail("EPOCH_MISMATCH at " + std::to_string(e) + " without every source foreign:" + truths);
            }
            if (BridgeHalt::SOURCE_SCHEMA == v.halt && !anyBadOrForeign)
            {
                return fail("SOURCE_SCHEMA at " + std::to_string(e) + " without a bad or foreign source:" + truths);
            }
            if (BridgeHalt::GAP == v.halt)
            {
                ++g_stats.gaps;
            }
            else if (BridgeHalt::EPOCH_MISMATCH == v.halt)
            {
                ++g_stats.epochs;
            }
            else
            {
                ++(anyHole ? g_stats.mixed : g_stats.schemas);
            }
        }
        if (!haltedNow)
        {
            if (haltChecked)
            {
                ++g_stats.recovered;
            }
            haltChecked = false;
            if (!deciding)
            {
                snapshot.clear();
            }
        }
        // FIX-3: halts are recovered from: count each kind once the core passed its e with the halt gone
        if (haltedNow && BridgeHalt::NONE == pendingKind)
        {
            pendingKind = v.halt;
            pendingE = p.domain->halt().e;
            attachesAtHalt = BridgeMetrics::get(p.metrics.haltSuccessorAttaches);
            ++g_stats.haltsWithLag; // every run has an egress lag >= 1 (FIX-1)
            if (sc.postHaltEvents && !eventDone)
            {
                eventAt = it + static_cast<int>(std::uniform_int_distribution<int>(50, 400)(rng));
            }
        }
        if (BridgeHalt::NONE != pendingKind && !haltedNow && BridgeHalt::NONE == v.halt && p.core.cursor >= pendingE)
        {
            if (0 != successorE && pendingE == successorE)
            {
                successorRecovered = true;
            }
            if (BridgeHalt::SOURCE_SCHEMA == pendingKind && BridgeMetrics::get(p.metrics.haltSuccessorAttaches) > attachesAtHalt)
            {
                ++g_stats.schemaRescanRecoveries; // R0: the re-scan attached a successor's valid copy
            }
            switch (pendingKind)
            {
                case BridgeHalt::GAP: ++g_stats.recoveredGap; break;
                case BridgeHalt::SOURCE_SCHEMA: ++g_stats.recoveredSchema; break;
                case BridgeHalt::EPOCH_MISMATCH: ++g_stats.recoveredEpoch; break;
                case BridgeHalt::CORE_REFUSED: ++g_stats.recoveredRefused; break;
                case BridgeHalt::NONE: break;
            }
            pendingKind = BridgeHalt::NONE;
        }
        if (it == eventAt && !eventDone && haltedNow)
        {
            eventDone = true;
            const std::int64_t e = p.domain->halt().e;
            if (BridgeHalt::EPOCH_MISMATCH == v.halt)
            {
                // the operator runs the dual genesis: the archives are wiped, the ME journals epoch 9 from seq 1, the
                // core is re-genesised to it (unbound, genesisId 9)
                ++g_stats.regeneses;
                currentEpoch = FOREIGN;
                for (std::size_t s = 0; s < sc.sources; ++s)
                {
                    p.fill(s, FOREIGN);
                }
                p.core.bound = 0;
                p.core.cursor = 0;
                p.core.latch = 0;
                p.core.latchSkip = 0;
                p.core.consumePosition = 0;
                p.core.digests.clear();
                p.core.genesisId = FOREIGN;
            }
            else if ((BridgeHalt::GAP == v.halt || BridgeHalt::SOURCE_SCHEMA == v.halt) &&
                std::uniform_int_distribution<int>(0, 2)(rng) == 0)
            {
                // S10' 5.3: an in-place successor (the old recordings stay): FILL (only e), RESTART (e .. n), above e
                // (e + k .. n: the halt stays, seen = the min), or for SOURCE_SCHEMA valid copies from e on
                const std::size_t s = std::uniform_int_distribution<std::size_t>(0, sc.sources - 1)(rng);
                const int kind = BridgeHalt::SOURCE_SCHEMA == v.halt ? 1 : std::uniform_int_distribution<int>(0, 2)(rng);
                const bool inTime = it < 12'000 - 3'000 - 600; // a re-scan (60 s = 3000 iterations) still comes
                const bool foreignBefore = sc.foreignTail && sc.foreignTail <= sc.n;
                if (0 == kind)
                {
                    ++g_stats.successorFills;
                    p.successor(s, e, e, currentEpoch);
                    fillE = e;
                    fillSource = s;
                }
                else if (1 == kind)
                {
                    ++(BridgeHalt::SOURCE_SCHEMA == v.halt ? g_stats.schemaSuccessors : g_stats.successorRestarts);
                    p.successor(s, e, sc.n, currentEpoch);
                }
                else
                {
                    ++g_stats.successorsAbove;
                    p.successor(s, std::min<std::int64_t>(sc.n, e + 1 + std::uniform_int_distribution<int>(0, 3)(rng)), sc.n,
                        currentEpoch);
                }
                // elections fence a re-scan every ~1.2 s here (a fence does not restart the 6 s cadence, N-5): the walk
                // may never finish, so those runs check safety only
                if (kind != 2 && inTime && !foreignBefore && currentEpoch == EPOCH && !sc.poisonSeq && !sc.coreRefuseSeq &&
                    !sc.elections)
                {
                    successorE = e; // the halt at e must be recovered (the re-scan walks to the successor)
                    successorSchema = BridgeHalt::SOURCE_SCHEMA == v.halt;
                }
            }
            else if (BridgeHalt::GAP == v.halt && std::uniform_int_distribution<int>(0, 1)(rng) == 0)
            {
                ++g_stats.fills; // an operator refills one node's archive with the missing records
                p.fill(std::uniform_int_distribution<std::size_t>(0, sc.sources - 1)(rng), currentEpoch);
            }
            else
            {
                // a second lane delivered e (and, for GAP, up to seen - 1): the core's cursor moves past the halt
                ++g_stats.lanes;
                const std::int64_t to = BridgeHalt::GAP == v.halt ? std::max(e, p.domain->halt().seen - 1) : e;
                if (p.core.cursor < to && 0 != p.core.bound)
                {
                    p.core.cursor = to;
                    p.core.consumePosition = std::max(p.core.consumePosition, 64 * to);
                    p.core.reachClear();
                }
            }
        }
        if (p.core.cursor == sc.n && v.last == sc.n && v.staged == 0 && !haltedNow && v.halt == BridgeHalt::NONE)
        {
            break;
        }
    }
    if (std::getenv("CT_BRIDGE_PROPERTY_SUMMARY"))
    {
        std::printf("seed %llu sources %zu verifications %llu halt %s cursor %lld/%lld :: %s\n", (unsigned long long)seed,
            sc.sources, (unsigned long long)BridgeMetrics::get(p.metrics.holeVerifications),
            bridgeHaltName(p.domain->view().halt), (long long)p.core.cursor, (long long)sc.n, sc.describe().c_str());
    }
    g_stats.verifications += static_cast<int>(BridgeMetrics::get(p.metrics.holeVerifications));
    g_stats.fences += static_cast<int>(p.metrics.totalFences());
    g_stats.ringFullPauses += static_cast<int>(BridgeMetrics::get(p.metrics.ringFullPauses));
    g_stats.storms += static_cast<int>(BridgeMetrics::get(p.metrics.stormRotations) + BridgeMetrics::get(p.metrics.coreRefused));
    const BridgeView v = p.domain->view();
    // which sources carry every seq memo-free (and no poisoned copy)?
    bool cleanSource = false;
    for (std::size_t s = 0; s < sc.sources && !cleanSource; ++s)
    {
        if (sc.poisonSeq && sc.poisoned[s])
        {
            continue;
        }
        bool all = true;
        for (std::int64_t e = 1; e <= sc.n && all; ++e)
        {
            all = Truth::DELIVERS == memoFree(*p.sources.archives[s], e, currentEpoch);
        }
        cleanSource = all;
    }
    // (j) a v1/v2 phase never ends in a halt (the halt (c) checks against the journal itself is the only exception:
    // a journal without seq 1 halts GAP at 1 whatever the core's behaviour), and the run binds once the core switches
    if (v2Halted && v2HaltClean)
    {
        return fail("a halt during the core's behaviour 1/2 phase");
    }
    // (i) a stub-core refusal: CORE_REFUSED unless a source carries an accepted copy, bounded re-offers
    if (sc.poisonSeq || sc.coreRefuseSeq)
    {
        const int bound = p.settings.stormFences * static_cast<int>(sc.sources);
        if (p.offersOfRefused > bound + 1)
        {
            return fail("the refused seq was offered " + std::to_string(p.offersOfRefused) + " times (bound " +
                std::to_string(bound) + ")");
        }
        if (BridgeHalt::CORE_REFUSED == v.halt)
        {
            ++g_stats.coreRefused;
            bool acceptedCopy = false;
            for (std::size_t s = 0; s < sc.sources; ++s)
            {
                if (sc.coreRefuseSeq == 0 && !sc.poisoned[s] &&
                    Truth::DELIVERS == memoFree(*p.sources.archives[s], sc.poisonSeq, EPOCH))
                {
                    acceptedCopy = true;
                }
            }
            // 9.6 as amended 2026-10-31 (review FIX-2): a source that never offered C' + 1 stays untried and is re-tried on
            // the haltRescan cadence, so an accepted copy that exists at the end of the run must have been offered
            if (acceptedCopy && p.domain->halt().e == sc.poisonSeq)
            {
                return fail("CORE_REFUSED although a source carries an accepted copy of the refused seq");
            }
            return {};
        }
    }
    g_stats.successorAttaches += static_cast<int>(BridgeMetrics::get(p.metrics.haltSuccessorAttaches));
    g_stats.stepBacks += static_cast<int>(BridgeMetrics::get(p.metrics.haltStepbacks));
    // S10' 5.3 (d): an in-place successor holding a valid e recovers the halt at e by the re-scan (R0 / R1)
    if (0 != successorE)
    {
        if (!successorRecovered)
        {
            return fail(std::string("no recovery although a source carries every seq (") +
                (successorSchema ? "SOURCE_SCHEMA halt never recovered with valid copies present" : "GAP") +
                "; an in-place successor holds e = " + std::to_string(successorE) + "; halt " + bridgeHaltName(v.halt) +
                ", core " + std::to_string(p.core.cursor) + ")");
        }
        ++g_stats.successorRecoveries;
    }
    // (d) liveness
    const bool refusalExpected = sc.coreRefuseSeq != 0 || (sc.poisonSeq != 0 && !cleanSource);
    if (cleanSource && !refusalExpected)
    {
        if (p.core.cursor != sc.n || v.halt != BridgeHalt::NONE)
        {
            return fail("a clean source exists but the core stopped at " + std::to_string(p.core.cursor) + " of " +
                std::to_string(sc.n) + " (halt " + bridgeHaltName(v.halt) + ", last " + std::to_string(v.last) +
                ", state " + stateName(v.state) + ", waiting " + decisionRowName(p.domain->waiting()) + ")");
        }
        ++g_stats.completed;
    }
    if (sc.v2Until >= 0 && cleanSource && !refusalExpected && p.core.bound != currentEpoch)
    {
        return fail("the core never bound after the switch to behaviour 3");
    }
    (void)haltSeen;
    return {};
}

}

TEST_CASE("BridgeDomainPropertyTest.everyRecordOnceInOrderOrALoudHaltAcrossRandomScenarios")
{
    int runs = 2000;
    if (const char *env = std::getenv("CT_BRIDGE_PROPERTY_RUNS"))
    {
        runs = std::atoi(env);
    }
    std::uint64_t base = 0x5eedb41dULL;
    if (const char *env = std::getenv("CT_BRIDGE_PROPERTY_SEED"))
    {
        base = std::strtoull(env, nullptr, 0);
    }
    g_stats = Stats{};
    int failures = 0;
    std::string first;
    for (int i = 0; i < runs; ++i)
    {
        const std::string result = runScenario(base + static_cast<std::uint64_t>(i));
        if (!result.empty())
        {
            if (failures++ == 0)
            {
                first = result;
            }
            if (failures >= 5)
            {
                break;
            }
        }
    }
    MESSAGE("runs " << g_stats.runs << " (seed base " << base << "): completed " << g_stats.completed << ", GAP "
            << g_stats.gaps << ", SOURCE_SCHEMA " << g_stats.schemas << ", mixed " << g_stats.mixed << ", EPOCH_MISMATCH "
            << g_stats.epochs << ", CORE_REFUSED " << g_stats.coreRefused << ", recovered halts " << g_stats.recovered
            << "; behind failovers " << g_stats.behindFailovers << ", hole verifications " << g_stats.verifications
            << ", storm actions " << g_stats.storms << ", fences " << g_stats.fences << ", ring-full pauses "
            << g_stats.ringFullPauses << ", elections " << g_stats.elections << ", v2 runs " << g_stats.v2Runs
            << ", refusal runs " << g_stats.poisonRuns << "; halts with lag " << g_stats.haltsWithLag
            << ", recovered GAP " << g_stats.recoveredGap << ", SOURCE_SCHEMA " << g_stats.recoveredSchema
            << ", EPOCH_MISMATCH " << g_stats.recoveredEpoch << ", CORE_REFUSED " << g_stats.recoveredRefused
            << " (fills " << g_stats.fills << ", lanes " << g_stats.lanes << ", re-geneses " << g_stats.regeneses << ")"
            << "; S10' successors: FILL " << g_stats.successorFills << ", RESTART " << g_stats.successorRestarts << ", above e "
            << g_stats.successorsAbove << ", SOURCE_SCHEMA " << g_stats.schemaSuccessors << "; successor attaches "
            << g_stats.successorAttaches << ", step-backs " << g_stats.stepBacks << ", schema re-scan recoveries "
            << g_stats.schemaRescanRecoveries << ", successor recoveries " << g_stats.successorRecoveries);
    INFO(first);
    CHECK(failures == 0);
    if (failures == 0 && runs >= 400)
    {
        // FIX-1 (2026-10-31): with an egress lag in every run, halts are still reached (BLOCKER-1 would stop them)
        CHECK(g_stats.haltsWithLag > 0);
        CHECK(g_stats.gaps > 0);
    }
    if (failures == 0 && runs >= 1000)
    {
        // (h) non-vacuous: every mechanism the property is about ran
        CHECK(g_stats.gaps > 0);
        CHECK(g_stats.schemas > 0);
        CHECK(g_stats.mixed > 0);
        CHECK(g_stats.epochs > 0);
        CHECK(g_stats.coreRefused > 0);
        CHECK(g_stats.behindFailovers > 0);
        CHECK(g_stats.verifications > 0);
        CHECK(g_stats.storms > 0);
        CHECK(g_stats.fences > 0);
        CHECK(g_stats.ringFullPauses > 0);
        CHECK(g_stats.completed > runs / 3);
        // FIX-1 and FIX-3 of the 2026-10-31 review: halts are reached with an egress lag, and recovered from
        CHECK(g_stats.haltsWithLag > 0);
        CHECK(g_stats.recoveredGap > 0);
        CHECK(g_stats.recoveredSchema > 0);
        CHECK(g_stats.recoveredEpoch > 0);
        CHECK(g_stats.recoveredRefused > 0);
        // S10' 5.3 (C5): recoveries by an in-place successor, by the R2 step back and by the R0 schema re-scan ran
        CHECK(g_stats.successorAttaches > 0);
        CHECK(g_stats.stepBacks > 0);
        CHECK(g_stats.schemaRescanRecoveries > 0);
        CHECK(g_stats.successorRecoveries > 0);
    }
}

TEST_CASE("BridgeDomainPropertyTest.staleBehindHaltRecoversByTheReScan")
{
    // S10' lane A (W9 item 3): the four S9b seeds (bases 31337, 99999999, 555000, 8675309) that fail the halt-time check
    // "halt ... although a memo-free read delivers it" on 2039454 and on the S10' tree. Cause (pre-halt, S9b 8.3 row 3,
    // not R0-R2): during hole_grace the domain parks on the first BEHIND source; another BEHIND source's successor
    // recording appears meanwhile and is never read, so the grace-expiry decision counts it HOLED-without-seen and
    // halts SOURCE_SCHEMA / GAP. Carried (README lane A). Pinned here: with F-1 (R0 schema re-scan, R1 re-list) the halt
    // is transient, the run completes with every record once and in order, and the halt is counted recovered.
    g_staleBehindHaltTolerated = true;
    g_staleBehindHalts = 0;
    g_stats = Stats{};
    std::string failure;
    for (const std::uint64_t seed : { 31441ULL, 100001135ULL, 556400ULL, 8676632ULL })
    {
        const std::string r = runScenario(seed);
        if (!r.empty() && failure.empty())
        {
            failure = r;
        }
    }
    g_staleBehindHaltTolerated = false;
    INFO(failure);
    CHECK(failure.empty());
    CHECK(g_stats.completed == 4);
    CHECK(g_stats.recovered >= 4);
    CHECK(g_staleBehindHalts == 4); // the shape is still reached: when it drops to 0 the carry is fixed upstream
}
