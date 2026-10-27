// ChainWalkPropertyTest (RC2 S9a contract 5.5, 9.1, review F20): seeded random multi-source chains
// (re-journal step-backs, per-source holes at recording boundaries, a whole-box hole, head and
// active purges, padding that shifts frame boundaries, bad frames on a subset of sources, empty
// recordings), driven through the REAL FollowPolicy + FollowerLoop + ResumeMemo on simulated
// archives, with a stub owner (the S9b forward rule seq == last + 1, failover in configured order,
// hole verification, D-1 bad-frame rotation) and a stub core (applies offers after a delay, a fence
// loses the in-flight tail and starts a new sync from the core cursor).
//
// Asserted per run:
//   - per sync, the offered seqs are C + 1, C + 2, ... once each and in order;
//   - the core never receives a seq other than cursor + 1 (no duplicate, no gap reaches the core);
//   - a GAP halt at e only when every source's MEMO-FREE read holes at e (ground truth from the
//     generated chains), so a memo artefact never becomes a FeedHalt GAP;
//   - SOURCE_SCHEMA only when every source was found bad at e in one rotation;
//   - liveness: without a halt the core reaches N.
// RED (negative controls): the forward rule replaced by seq > last (a hole is forwarded), and
// separately hole verification off (a false GAP).
#include "doctest/doctest.h"

#include "SimArchive.h"

#include "cluster_tools/archive/TestSeams.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <sstream>

using namespace cluster_tools::archive;
using namespace ct_archive_test;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t EPOCH = 5;

struct Run
{
    std::int64_t from;
    std::int64_t to; // to = from - 1: an empty recording
};

struct Scenario
{
    std::int64_t n = 0;
    std::size_t sources = 0;
    std::int64_t wholeBoxHole = 0;                 // 0 = none
    std::vector<std::vector<Run>> runs;            // per source
    std::vector<std::int64_t> badSeq;              // per source, 0 = none
    std::vector<std::vector<std::int64_t>> pads;   // per source: pad before these seqs
    std::vector<std::int64_t> headPurgeRecordings; // per source: leading recordings purged later
    int coreDelay = 3;                             // iterations from an offer to its apply
    std::vector<int> badFirstCopy;                 // per source: the bad frame is a superseded copy before good data
    std::vector<int> deferredRun;                  // per source: index of the run recorded late (-1 = none)
    std::vector<int> stopAt;                       // per source: iteration the preceding recording stops
    std::vector<int> successorDelay;               // per source: iterations until the successor is recorded
    std::string describe() const
    {
        std::ostringstream o;
        o << "n=" << n << " hole=" << wholeBoxHole;
        for (std::size_t s = 0; s < sources; ++s)
        {
            o << " | src" << s << (badSeq[s] ? " bad=" + std::to_string(badSeq[s]) : "") << ":";
            for (const auto &r : runs[s])
            {
                o << " [" << r.from << "," << r.to << "]";
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
    sc.n = pick(8, 80);
    sc.sources = static_cast<std::size_t>(pick(1, 3));
    sc.wholeBoxHole = coin(0.15) ? pick(2, sc.n - 1) : 0; // seq n always exists, so a hole is visible
    sc.runs.resize(sc.sources);
    sc.badSeq.assign(sc.sources, 0);
    sc.pads.resize(sc.sources);
    sc.headPurgeRecordings.assign(sc.sources, 0);
    sc.badFirstCopy.assign(sc.sources, 0);
    sc.deferredRun.assign(sc.sources, -1);
    sc.stopAt.assign(sc.sources, 0);
    sc.successorDelay.assign(sc.sources, 0);
    for (std::size_t s = 0; s < sc.sources; ++s)
    {
        sc.coreDelay = static_cast<int>(pick(1, 30));
        auto &runs = sc.runs[s];
        if (!sc.wholeBoxHole && sc.n >= 16 && coin(0.35))
        {
            // The K8 crash shape (S3 contract 9): a long run, a short re-journal that crashed again, then
            // a restart from a snapshot above the short run's end. A memo-free read finds every seq
            // (the long run holds them); a resume at the short run's proof sees a hole that is not real.
            const std::int64_t a = pick(4, sc.n - 8);
            runs.push_back({ 1, a + 5 });
            runs.push_back({ a - 2, a });
            runs.push_back({ a + 3, sc.n });
            continue;
        }
        std::int64_t from = 1;
        for (int guard = 0; guard < 12; ++guard)
        {
            const bool last = runs.size() >= 5 || coin(0.3);
            std::int64_t to = last ? sc.n : pick(from - 1, sc.n);
            if (sc.wholeBoxHole && from <= sc.wholeBoxHole && sc.wholeBoxHole <= to)
            {
                // the whole-box hole sits at a recording boundary on every source (the K8 shape)
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
                from = to + 1; // continue
            }
            else if (r < 0.85)
            {
                from = pick(std::max<std::int64_t>(1, runs.back().from), to + 1); // re-journal step back
            }
            else
            {
                from = to + 1 + pick(1, 3); // a per-source hole across the boundary
            }
            from = std::min(from, sc.n); // every source records seq n: a source that never reaches e is S9b's case
            if (sc.wholeBoxHole && from == sc.wholeBoxHole)
            {
                from = sc.wholeBoxHole + 1;
            }
        }
        if (runs.empty() || runs.back().to < sc.n)
        {
            runs.push_back({ from, sc.n }); // the active recording reaches n
            if (sc.wholeBoxHole && runs.back().from <= sc.wholeBoxHole && sc.wholeBoxHole <= sc.n)
            {
                runs.back() = { sc.wholeBoxHole + 1, sc.n };
            }
        }
        if (coin(0.2))
        {
            sc.badSeq[s] = pick(1, sc.n);
        }
        for (int p = 0; p < 2; ++p)
        {
            if (coin(0.3))
            {
                sc.pads[s].push_back(pick(1, sc.n));
            }
        }
        if (runs.size() > 2 && coin(0.2))
        {
            sc.headPurgeRecordings[s] = 1;
        }
        sc.badFirstCopy[s] = coin(0.4) ? 1 : 0;
        // Mid-run: the active recording stops (the ME writer goes away) and its successor is recorded
        // 0 to 3 s later (review F-5 b): the successor path (rows 7a, 10) and F-1's late listing.
        const Run lastRun = runs.back();
        if (lastRun.to - lastRun.from >= 3 && coin(0.35))
        {
            const std::int64_t mid = pick(lastRun.from + 1, lastRun.to - 1);
            const std::int64_t next = pick(std::max<std::int64_t>(lastRun.from, mid - 2), mid + 1);
            runs.back() = { lastRun.from, mid };
            runs.push_back({ next, lastRun.to });
            sc.deferredRun[s] = static_cast<int>(runs.size() - 1);
            sc.stopAt[s] = static_cast<int>(pick(10, 150));
            sc.successorDelay[s] = static_cast<int>(pick(0, 300));
        }
    }
    return sc;
}

void buildRun(const Scenario &sc, std::size_t s, SimArchive &archive, std::size_t i)
{
    const auto &runs = sc.runs[s];
    SimRecording &rec = archive.record();
    // the bad frame replaces the first copy (a superseded corrupt copy before good data) or the last
    // copy of badSeq on this source
    std::size_t badRun = runs.size();
    for (std::size_t j = 0; j < runs.size(); ++j)
    {
        if (runs[j].from <= sc.badSeq[s] && sc.badSeq[s] <= runs[j].to && sc.badSeq[s] != sc.wholeBoxHole)
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
        if (seq == sc.wholeBoxHole)
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
        Bytes frame = makeFrame(TEMPLATE_TRADE, seq, EPOCH, 64 * seq);
        if (seq == sc.badSeq[s] && i == badRun)
        {
            putLe16(frame, 4, 3); // a schema 3 frame
        }
        archive.append(rec, frame);
    }
}

// Every run except a deferred one; all but the last recorded run are stopped.
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

// Ground truth: does a memo-free read of source s from last = e - 1 hole at e (a frame > e before e)?
// A bad frame met first counts as "bad", not "holed".
enum class Truth { DELIVERS, HOLE, BAD, NOTHING };
Truth memoFree(const SimArchive &archive, std::int64_t e)
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
            if (!a.accepted())
            {
                return Truth::BAD;
            }
            if (a.peek.journalSeq < e)
            {
                continue;
            }
            return a.peek.journalSeq == e ? Truth::DELIVERS : Truth::HOLE;
        }
    }
    return Truth::NOTHING;
}

struct Core
{
    std::int64_t cursor = 0;
    std::deque<std::pair<int, std::int64_t>> inFlight; // (apply at iteration, seq)
    int rejects = 0;

    void offer(std::int64_t seq, int now, int delay) { inFlight.emplace_back(now + delay, seq); }
    void tick(int now)
    {
        while (!inFlight.empty() && inFlight.front().first <= now)
        {
            const std::int64_t seq = inFlight.front().second;
            inFlight.pop_front();
            if (seq == cursor + 1)
            {
                cursor = seq;
            }
            else
            {
                ++rejects; // a duplicate or a gap reached the core
            }
        }
    }
    void loseInFlight() { inFlight.clear(); }
};

enum class Halt { NONE, GAP, SOURCE_SCHEMA, MIXED };

// What the runs exercised, so a green run is shown not to be a vacuous one.
struct Stats
{
    int completed = 0, gaps = 0, schemas = 0, mixed = 0, fences = 0, switches = 0, verifications = 0;
    int faults = 0, resumesFromProof = 0, trims = 0, successions = 0;
};
Stats g_stats;

struct Owner : FrameSink
{
    std::size_t sources;
    ResumeMemo &memo;
    Core &core;
    std::mt19937_64 &rng;
    int now = 0;
    std::int64_t syncCursor = 0; // C of the sync
    std::int64_t last = 0;
    std::vector<std::int64_t> offeredThisSync;
    std::size_t current = 0;
    std::int64_t markedAt = 0;
    std::set<std::size_t> holed, bad;
    Halt halt = Halt::NONE;
    std::int64_t haltSeq = 0;
    bool block = false;         // a switch or a rewind is pending: everything aborts
    bool wantReattach = false;
    std::optional<std::size_t> wantSwitch;
    int coreDelay = 3;
    std::string failure;
    std::function<void(std::size_t, std::int64_t)> verifyProbe;

    Owner(std::size_t n, ResumeMemo &m, Core &c, std::mt19937_64 &r) : sources(n), memo(m), core(c), rng(r) {}

    bool forwardRule(std::int64_t seq) const
    {
        if (test_seams::disabled(test_seams::Rule::PROPERTY_FORWARD_GREATER))
        {
            return seq > last; // RED shape: a hole is forwarded
        }
        return seq == last + 1;
    }

    void newSync(std::int64_t cursor)
    {
        syncCursor = cursor;
        last = cursor;
        offeredThisSync.clear();
        markedAt = 0;
        holed.clear();
        bad.clear();
    }

    void mark(bool isBad)
    {
        const std::int64_t e = last + 1;
        if (markedAt != e)
        {
            markedAt = e;
            holed.clear();
            bad.clear();
        }
        (isBad ? bad : holed).insert(current);
        if (holed.size() + bad.size() >= sources)
        {
            halt = bad.empty() ? Halt::GAP : holed.empty() ? Halt::SOURCE_SCHEMA : Halt::MIXED;
            haltSeq = e;
            block = true;
            return;
        }
        // the next source in configured order not yet marked at e
        for (std::size_t k = 1; k <= sources; ++k)
        {
            const std::size_t next = (current + k) % sources;
            if (!holed.count(next) && !bad.count(next))
            {
                wantSwitch = next;
                block = true;
                return;
            }
        }
    }

    SinkAction onFrame(const JournalFrame &frame) noexcept override
    {
        if (block || halt != Halt::NONE)
        {
            return SinkAction::ABORT;
        }
        if (std::uniform_int_distribution<int>(0, 19)(rng) == 0)
        {
            return SinkAction::ABORT; // back-pressure: redelivered
        }
        const std::int64_t seq = frame.peek.journalSeq;
        if (seq <= last)
        {
            return SinkAction::CONTINUE; // SKIP
        }
        if (forwardRule(seq))
        {
            last = seq;
            offeredThisSync.push_back(seq);
            core.offer(seq, now, coreDelay);
            return SinkAction::CONTINUE;
        }
        // a hole at e = last + 1 (5.2 rule 5, then failover in configured order)
        if (!frame.readWasMemoFree && verifyProbe)
        {
            verifyProbe(current, last + 1);
        }
        if (memo.onHole(current, last + 1, frame.readWasMemoFree) == HoleDecision::VERIFY_FROM_CHAIN_START)
        {
            wantReattach = true;
            block = true;
            return SinkAction::ABORT;
        }
        mark(false);
        return SinkAction::ABORT;
    }

    RefusalAction onRefused(const RefusedFrame &) noexcept override
    {
        if (!block && halt == Halt::NONE)
        {
            mark(true); // D-1: bad at the owner's e = last + 1, failover first
        }
        return RefusalAction::CLOSE_REPLAY;
    }
};

struct Source
{
    SimArchive archive;
    std::unique_ptr<FollowPolicy> policy;
    std::unique_ptr<FollowerLoop<SimArchive>> loop;
};

// One run; returns an empty string when every property held.
std::string runScenario(std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    const Scenario sc = generate(rng);
    ArchiveMetrics metrics;
    ResumeMemo memo(sc.sources, &metrics);
    Core core;
    Owner owner(sc.sources, memo, core, rng);
    owner.coreDelay = sc.coreDelay;
    std::vector<std::unique_ptr<Source>> *sourcesRef = nullptr;
    if (std::getenv("CT_PROPERTY_DEBUG"))
    {
        owner.verifyProbe = [&](std::size_t s, std::int64_t e) {
            const Truth t = memoFree((*sourcesRef)[s]->archive, e);
            std::printf("seed %llu memo-read hole src%zu e=%lld truth=%d sources=%zu :: %s\n", (unsigned long long)seed, s,
                (long long)e, static_cast<int>(t), sc.sources, sc.describe().c_str());
        };
    }
    FollowSettings settings;
    settings.backoff = 100ms;
    std::vector<std::unique_ptr<Source>> sources;
    for (std::size_t s = 0; s < sc.sources; ++s)
    {
        auto src = std::make_unique<Source>();
        build(sc, s, src->archive);
        src->policy = std::make_unique<FollowPolicy>(s, settings, memo, &metrics, nullptr);
        src->loop = std::make_unique<FollowerLoop<SimArchive>>(src->archive, *src->policy, &metrics);
        sources.push_back(std::move(src));
    }
    sourcesRef = &sources;
    auto fail = [&](const std::string &why) { return "seed " + std::to_string(seed) + ": " + why + " :: " + sc.describe(); };

    // Ground truth: a memo-free read of the archives as they are at the moment of the halt.
    auto everySourceHolesAt = [&](std::int64_t e) {
        for (const auto &src : sources)
        {
            if (memoFree(src->archive, e) != Truth::HOLE)
            {
                return false;
            }
        }
        return true;
    };

    TimePoint now{};
    memo.onReport(EPOCH, 0);
    memo.beginSync();
    owner.newSync(0);
    sources[0]->policy->attach(now);
    std::optional<std::size_t> switching;
    int pausedFor = 0;
    for (int it = 0; it < 60'000; ++it)
    {
        now += 10ms;
        owner.now = it;
        core.tick(it);
        if (core.rejects > 0)
        {
            return fail("the core received a seq other than cursor + 1");
        }
        if (owner.halt != Halt::NONE)
        {
            break;
        }
        if (core.cursor == sc.n && owner.last == sc.n)
        {
            break;
        }
        // the ME writer goes away mid-run, the successor is recorded later (review F-5 b)
        for (std::size_t s = 0; s < sc.sources; ++s)
        {
            if (sc.deferredRun[s] < 0)
            {
                continue;
            }
            if (it == sc.stopAt[s])
            {
                sources[s]->archive.stop(sources[s]->archive.recordings.back());
            }
            if (it == sc.stopAt[s] + sc.successorDelay[s])
            {
                buildRun(sc, s, sources[s]->archive, static_cast<std::size_t>(sc.deferredRun[s]));
            }
        }
        // retention: once, a stopped head recording whose every record the core applied is purged;
        // later, the active recording's applied head is purged (purgeSegments). Never under a replay.
        if (it == 80 || it == 160)
        {
            for (std::size_t s = 0; s < sc.sources; ++s)
            {
                SimArchive &archive = sources[s]->archive;
                if (archive.recordings.size() < 2)
                {
                    continue;
                }
                SimRecording &target = it == 80 ? archive.recordings.front() : archive.recordings.back();
                if ((it == 80 && (!sc.headPurgeRecordings[s] || target.rec.active())) ||
                    (archive.replayRunning() && archive.replayRecordingId() == target.rec.key.recordingId))
                {
                    continue;
                }
                std::size_t applied = 0;
                while (applied < target.frames.size())
                {
                    const auto &f = target.frames[applied];
                    if (!f.bytes.empty())
                    {
                        const Admission a = admitFrame(f.flags, f.bytes.data(), f.bytes.size());
                        if (!a.accepted() || a.peek.journalSeq > core.cursor)
                        {
                            break;
                        }
                    }
                    ++applied;
                }
                if (it == 80 && applied == target.frames.size())
                {
                    archive.recordings.pop_front();
                }
                else if (it == 160 && applied >= 2 && applied < target.frames.size())
                {
                    target.rec.startPosition = target.frames[applied].start;
                    target.frames.erase(target.frames.begin(), target.frames.begin() + static_cast<long>(applied));
                }
            }
        }
        // a FeedPositionReport every 10 iterations; a fence (a new leader, losing the in-flight tail)
        // now and then: the next sync rewinds to the memo proof for C'
        if (it % 5 == 4)
        {
            const bool fence = std::uniform_int_distribution<int>(0, 9)(rng) == 0;
            if (fence)
            {
                ++g_stats.fences;
                core.loseInFlight();
                memo.onReport(EPOCH, core.cursor);
                memo.beginSync();
                owner.newSync(core.cursor);
                owner.block = false;
                owner.wantSwitch.reset();
                owner.wantReattach = false;
                if (!switching)
                {
                    sources[owner.current]->policy->reattach(now); // rewind to the memo proof for C'
                }
            }
            else
            {
                memo.onReport(EPOCH, core.cursor);
            }
        }
        // the per-sync property
        for (std::size_t i = 0; i < owner.offeredThisSync.size(); ++i)
        {
            if (owner.offeredThisSync[i] != owner.syncCursor + 1 + static_cast<std::int64_t>(i))
            {
                return fail("sync from " + std::to_string(owner.syncCursor) + " offered " +
                    std::to_string(owner.offeredThisSync[i]) + " at index " + std::to_string(i));
            }
        }
        // owner actions, applied outside the follower callbacks
        if (owner.wantReattach && !switching)
        {
            owner.wantReattach = false;
            owner.block = false;
            sources[owner.current]->policy->reattach(now);
        }
        if (owner.wantSwitch && !switching)
        {
            switching = *owner.wantSwitch;
            ++g_stats.switches;
            owner.wantSwitch.reset();
            sources[owner.current]->policy->detach(now);
        }
        if (switching && sources[owner.current]->policy->idle())
        {
            owner.current = *switching;
            switching.reset();
            owner.block = false;
            sources[owner.current]->policy->attach(now);
        }
        // pauses: the owner stops reading now and then (core back-pressure, a fence)
        if (pausedFor == 0 && std::uniform_int_distribution<int>(0, 99)(rng) == 0)
        {
            pausedFor = std::uniform_int_distribution<int>(1, 30)(rng);
        }
        // a varying fragment budget, so the walk and the core cursor interleave at every speed
        int limit = std::uniform_int_distribution<int>(0, 3)(rng) == 0 ? 32 : std::uniform_int_distribution<int>(1, 4)(rng);
        if (pausedFor > 0)
        {
            --pausedFor;
            limit = 0;
        }
        sources[owner.current]->loop->doWork(owner, limit, now);
        if (owner.halt == Halt::GAP && !everySourceHolesAt(owner.haltSeq)) // the archives as they are now
        {
            return fail("FeedHalt GAP at " + std::to_string(owner.haltSeq) +
                " although a memo-free read of some source delivers it");
        }
    }
    g_stats.verifications += static_cast<int>(metrics.invalidations(MemoReason::HOLE_VERIFY));
    g_stats.resumesFromProof += static_cast<int>(metrics.resumes(ResumeKind::PROOF));
    g_stats.trims += static_cast<int>(ArchiveMetrics::get(metrics.memoTrims));
    g_stats.successions += static_cast<int>(ArchiveMetrics::get(metrics.recordingSuccessions));
    for (const auto &src : sources)
    {
        g_stats.faults += static_cast<int>(src->policy->faultCount());
    }
    if (owner.halt == Halt::GAP)
    {
        ++g_stats.gaps;
        return {};
    }
    if (owner.halt == Halt::SOURCE_SCHEMA || owner.halt == Halt::MIXED)
    {
        // review F-5 a: each source's verdict against its archive, read memo-free from e - 1
        for (std::size_t s = 0; s < sc.sources; ++s)
        {
            const Truth t = memoFree(sources[s]->archive, owner.haltSeq);
            if (owner.bad.count(s) && t != Truth::BAD)
            {
                return fail("source " + std::to_string(s) + " marked bad at " + std::to_string(owner.haltSeq) +
                    " but its memo-free read " + (t == Truth::DELIVERS ? "delivers it" : t == Truth::HOLE ? "holes" : "ends"));
            }
            if (owner.holed.count(s) && t != Truth::HOLE)
            {
                return fail("source " + std::to_string(s) + " marked holed at " + std::to_string(owner.haltSeq) +
                    " but its memo-free read does not hole there");
            }
            if (!owner.bad.count(s) && !owner.holed.count(s))
            {
                return fail("a halt with source " + std::to_string(s) + " unmarked at " + std::to_string(owner.haltSeq));
            }
        }
        if (owner.halt == Halt::SOURCE_SCHEMA && (owner.bad.size() != sc.sources || !owner.holed.empty()))
        {
            return fail("SOURCE_SCHEMA without every source bad at " + std::to_string(owner.haltSeq));
        }
        if (owner.halt == Halt::MIXED && (owner.bad.empty() || owner.holed.empty()))
        {
            return fail("a mixed halt without both a bad and a holed source");
        }
        ++(owner.halt == Halt::SOURCE_SCHEMA ? g_stats.schemas : g_stats.mixed);
        return {}; // every source unusable at e, some holed and some bad: S9b open question (README)
    }
    if (core.cursor != sc.n)
    {
        // liveness: only a whole-box hole may stop the core short, and then a GAP must have fired
        return fail("no halt and the core stopped at " + std::to_string(core.cursor) + " of " + std::to_string(sc.n) +
            " (owner last " + std::to_string(owner.last) + ")");
    }
    ++g_stats.completed;
    return {};
}

}

TEST_CASE("ChainWalkPropertyTest.forwardedSeqsAreDenseAndHolesAreRealAcrossRandomChains")
{
    int runs = 10'000;
    if (const char *env = std::getenv("CT_PROPERTY_RUNS"))
    {
        runs = std::atoi(env);
    }
    std::uint64_t base = 0x5eed5a9aULL;
    if (const char *env = std::getenv("CT_PROPERTY_SEED"))
    {
        base = std::strtoull(env, nullptr, 0);
    }
    int gaps = 0;
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
    (void)gaps;
    MESSAGE("runs " << runs << ": completed " << g_stats.completed << ", GAP " << g_stats.gaps << ", SOURCE_SCHEMA "
            << g_stats.schemas << ", mixed " << g_stats.mixed << "; fences " << g_stats.fences << ", failovers "
            << g_stats.switches << ", hole verifications " << g_stats.verifications << ", proof resumes "
            << g_stats.resumesFromProof << ", trims " << g_stats.trims << ", successions " << g_stats.successions
            << ", source faults " << g_stats.faults);
    INFO(first);
    CHECK(failures == 0);
    if (failures == 0 && runs >= 1000)
    {
        // non-vacuous: every mechanism the property is about actually ran
        CHECK(g_stats.gaps > 0);
        CHECK(g_stats.fences > 0);
        CHECK(g_stats.switches > 0);
        CHECK(g_stats.verifications > 0);
        CHECK(g_stats.resumesFromProof > 0);
        CHECK(g_stats.successions > 0);
        CHECK(g_stats.schemas > 0);
        CHECK(g_stats.mixed > 0);
        CHECK(g_stats.completed > runs / 4);
    }
}
