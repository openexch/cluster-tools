// The journal-neutral follow library on the core order journal profile: the frame gate in every read mode,
// the chain walk property, the memo seed, the archive id in the key, the expected-epoch filter, the timed
// re-list and the reader checkpoint after a source switch, driven through the REAL FollowPolicy + FollowerLoop
// + ResumeMemo over the simulated archive on a manual clock; plus the prefixed archive settings.
//
// The bridge's own suites (FramePeekTest, ChainModelTest, ResumeMemoTest, FollowPolicyTest,
// ChainWalkPropertyTest, ArchiveSettingsTest) are not touched by this file: they pin the ME journal profile.
#include "doctest/doctest.h"

#include "SimArchive.h"

#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/archive/TestSeams.h"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>

using namespace cluster_tools::archive;
using namespace ct_archive_test;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t E = 9;              // the expected epoch (the core genesis id)
constexpr std::int32_t CORE_STREAM = 4102; // the core order journal stream

Bytes coreFrame(std::int64_t seq, std::int64_t epoch, std::int64_t logPosition = 4096)
{
    Bytes b(CORE_ORDER_FRAME_LENGTH, 0);
    putLe16(b, 0, CORE_ORDER_BLOCK_EVENT);
    putLe16(b, 2, CORE_ORDER_TEMPLATE_EVENT);
    putLe16(b, 4, CORE_ORDER_JOURNAL_SCHEMA_ID);
    putLe16(b, 6, CORE_ORDER_JOURNAL_SCHEMA_VERSION);
    putLe64(b, FRAME_OFFSET_JOURNAL_SEQ, seq);
    putLe64(b, FRAME_OFFSET_ME_EPOCH, epoch);
    putLe64(b, FRAME_OFFSET_EGRESS_SEQ, logPosition);
    for (std::size_t i = 32; i < b.size(); ++i)
    {
        b[i] = static_cast<std::uint8_t>(seq + i); // a body that differs per record
    }
    return b;
}

// The simulated archive with an archive id. Like the adapter (ArchiveSource::list), it stamps that id on every
// listed key only when the owner keys by archive id (FollowSettings::keyByArchiveId); otherwise every key
// carries ARCHIVE_ID_UNKNOWN.
struct StampedArchive : SimArchive
{
    std::int64_t archiveId = ARCHIVE_ID_UNKNOWN;
    bool keyByArchiveId = false;

    ListResult list()
    {
        ListResult r = SimArchive::list();
        for (auto &rec : r.recordings)
        {
            rec.key.archiveId = keyByArchiveId ? archiveId : ARCHIVE_ID_UNKNOWN;
        }
        return r;
    }
};

void appendCore(SimArchive &a, SimRecording &r, std::int64_t from, std::int64_t count, std::int64_t epoch = E)
{
    for (std::int64_t s = from; s < from + count; ++s)
    {
        a.append(r, coreFrame(s, epoch, 64 * s));
    }
}

struct CoreSink : FrameSink
{
    std::vector<std::int64_t> seqs, epochs, recordings, ends;
    std::vector<Refusal> refused;

    SinkAction onFrame(const JournalFrame &f) noexcept override
    {
        seqs.push_back(f.peek.journalSeq);
        epochs.push_back(f.peek.epoch());
        recordings.push_back(f.key.recordingId);
        ends.push_back(f.endPosition);
        return SinkAction::CONTINUE;
    }

    RefusalAction onRefused(const RefusedFrame &f) noexcept override
    {
        refused.push_back(f.reason);
        return RefusalAction::CLOSE_REPLAY;
    }

    bool has(std::int64_t seq) const { return std::find(seqs.begin(), seqs.end(), seq) != seqs.end(); }
};

struct CoreObserver : RecordingObserver
{
    int foreign = 0;
    void onForeignEpochRecording(std::size_t, const Recording &, std::int64_t) noexcept override { ++foreign; }
};

FollowSettings coreSettings()
{
    FollowSettings s;
    s.profile = CORE_ORDER_JOURNAL;
    s.journalStreamId = CORE_STREAM;
    s.journalMtu = 1408;
    s.probeInterval = 50ms;
    s.backoff = 10ms;
    return s;
}

FollowSettings keyedSettings()
{
    FollowSettings s = coreSettings();
    s.keyByArchiveId = true;
    return s;
}

// One source over a stamped simulated archive: the memo, the policy and the loop on a manual clock.
template<typename Sink = CoreSink>
struct Rig
{
    StampedArchive archive;
    ArchiveMetrics metrics;
    ResumeMemo memo{ 1, &metrics };
    CoreObserver observer;
    std::unique_ptr<FollowPolicy> policy;
    std::unique_ptr<FollowerLoop<StampedArchive>> loop;
    Sink sink;
    TimePoint now{};

    explicit Rig(FollowSettings settings = coreSettings())
    {
        archive.streamId = CORE_STREAM;
        archive.keyByArchiveId = settings.keyByArchiveId;
        policy = std::make_unique<FollowPolicy>(0, settings, memo, &metrics, &observer);
        loop = std::make_unique<FollowerLoop<StampedArchive>>(archive, *policy, &metrics);
    }

    void step(int limit = 32)
    {
        now += 1ms;
        loop->doWork(sink, limit, now);
    }

    template<typename Pred>
    bool runUntil(Pred pred, int maxIterations = 3000)
    {
        for (int i = 0; i < maxIterations; ++i)
        {
            if (pred())
            {
                return true;
            }
            step();
        }
        return pred();
    }
};

// ---- the frame gate in every read mode

enum class Fault { SCHEMA_ID, VERSION, TEMPLATE, BLOCK, FRAGMENTED, SEQ_ZERO, EPOCH_ZERO };
enum class Mode { MEMO_READ, MEMO_FREE, AT_OR_BELOW_CURSOR, LIVE_AT_HEAD, BOUNDED_STOPPED };

const char *modeName(Mode m)
{
    switch (m)
    {
        case Mode::MEMO_READ: return "memo read";
        case Mode::MEMO_FREE: return "memo-free read";
        case Mode::AT_OR_BELOW_CURSOR: return "a record at or below the cursor";
        case Mode::LIVE_AT_HEAD: return "live follow at the head";
        case Mode::BOUNDED_STOPPED: return "bounded replay of a stopped recording";
    }
    return "?";
}

Refusal expectedRefusal(Fault f)
{
    switch (f)
    {
        case Fault::SCHEMA_ID:
        case Fault::VERSION: return Refusal::SCHEMA;
        case Fault::TEMPLATE: return Refusal::TEMPLATE;
        case Fault::BLOCK: return Refusal::BLOCK;
        case Fault::FRAGMENTED: return Refusal::FRAGMENTED;
        case Fault::SEQ_ZERO:
        case Fault::EPOCH_ZERO: return Refusal::HEAD;
    }
    return Refusal::NONE;
}

struct BadFrame
{
    Bytes bytes;
    std::uint8_t flags = FRAME_FLAGS_UNFRAGMENTED;
    std::int64_t seq = 0; // the seq a delivery of it would carry
};

BadFrame badFrame(Fault f, std::int64_t seq)
{
    BadFrame b{ coreFrame(seq, E), FRAME_FLAGS_UNFRAGMENTED, seq };
    switch (f)
    {
        case Fault::SCHEMA_ID: putLe16(b.bytes, 4, 4); break;  // an ME journal frame on the core stream
        case Fault::VERSION: putLe16(b.bytes, 6, 2); break;
        case Fault::TEMPLATE: putLe16(b.bytes, 2, 2); break;
        case Fault::BLOCK: putLe16(b.bytes, 0, CORE_ORDER_BLOCK_EVENT - 1); break;
        case Fault::FRAGMENTED: b.flags = 0x80; break;
        case Fault::SEQ_ZERO:
            putLe64(b.bytes, FRAME_OFFSET_JOURNAL_SEQ, 0);
            b.seq = 0;
            break;
        case Fault::EPOCH_ZERO: putLe64(b.bytes, FRAME_OFFSET_ME_EPOCH, 0); break;
    }
    return b;
}

// What the follower did with the bad frame: the refusal's label, "accepted", or the fault that stopped it.
std::string gateVerdict(Fault fault, Mode mode)
{
    Rig<> r;
    StampedArchive &a = r.archive;
    const std::int64_t badSeq = Mode::AT_OR_BELOW_CURSOR == mode ? 50 : 777;
    const BadFrame bad = badFrame(fault, badSeq);
    bool expectMemoRead = false;
    switch (mode)
    {
        case Mode::MEMO_FREE:
        {
            SimRecording &r0 = a.record();
            appendCore(a, r0, 1, 3);
            a.append(r0, bad.bytes, bad.flags);
            r.policy->attach(r.now);
            break;
        }
        case Mode::AT_OR_BELOW_CURSOR:
        {
            r.memo.onReport(E, 100);
            SimRecording &r0 = a.record();
            appendCore(a, r0, 1, 5);
            a.append(r0, bad.bytes, bad.flags);
            r.policy->attach(r.now);
            break;
        }
        case Mode::LIVE_AT_HEAD:
        {
            SimRecording &r0 = a.record();
            appendCore(a, r0, 1, 3);
            r.policy->attach(r.now);
            REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 3; }));
            for (int i = 0; i < 5; ++i)
            {
                r.step(); // at the head: nothing more to read
            }
            a.append(*a.find(0), bad.bytes, bad.flags);
            break;
        }
        case Mode::BOUNDED_STOPPED:
        {
            SimRecording &r0 = a.record();
            appendCore(a, r0, 1, 3);
            a.append(r0, bad.bytes, bad.flags);
            a.stop(r0);
            a.record(); // an active successor, still empty
            r.policy->attach(r.now);
            break;
        }
        case Mode::MEMO_READ:
        {
            r.memo.onReport(E, 6);
            SimRecording &r0 = a.record();
            appendCore(a, r0, 1, 4);
            a.stop(r0);
            SimRecording &r1 = a.record();
            appendCore(a, r1, 5, 2);
            r.policy->attach(r.now);
            REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 6; }));
            REQUIRE(r.memo.proofCount(0) > 0);
            a.append(*a.find(1), bad.bytes, bad.flags); // the first frame after the proof
            r.policy->reattach(r.now);                // the rewind resumes at the proof
            r.sink.seqs.clear();
            expectMemoRead = true;
            break;
        }
    }
    const std::size_t faultsBefore = r.observer.faults.size();
    const bool done = r.runUntil([&] {
        return !r.sink.refused.empty() || r.sink.has(bad.seq) || r.observer.faults.size() > faultsBefore;
    });
    if (expectMemoRead)
    {
        REQUIRE(!r.observer.attaches.empty());
        REQUIRE_MESSAGE(!r.observer.attaches.back().memoFree, "the rewind did not resume from the memo");
    }
    if (!r.sink.refused.empty())
    {
        return refusalLabel(r.sink.refused.front());
    }
    if (r.sink.has(bad.seq))
    {
        return "accepted";
    }
    if (r.observer.faults.size() > faultsBefore)
    {
        return std::string("not delivered (") + faultName(r.observer.faults.back().kind) + ")";
    }
    return done ? "?" : "nothing happened";
}

// ---- the chain walk property (an owner stub with the forward rule)

struct Run
{
    std::int64_t from;
    std::int64_t to; // to < from: an empty recording
};

struct WalkScenario
{
    std::int64_t n = 0;
    std::vector<Run> runs;      // in recordingId order; the last one is recorded late
    int stopAt = 1;             // iteration the second-to-last (active) recording stops
    int successorDelay = 1;     // iterations after that until the last recording appears
    std::vector<int> rewinds;   // iterations of an owner rewind (resume from the memo)
    int abortPct = 0;           // owner back-pressure per frame, percent

    std::string describe() const
    {
        std::ostringstream o;
        o << "n=" << n << " stopAt=" << stopAt << " delay=" << successorDelay << " runs:";
        for (const auto &r : runs)
        {
            o << " [" << r.from << "," << r.to << "]";
        }
        return o.str();
    }
};

WalkScenario generate(std::mt19937_64 &rng)
{
    auto pick = [&](std::int64_t lo, std::int64_t hi) {
        return lo >= hi ? lo : std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    auto coin = [&](double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; };
    WalkScenario sc;
    sc.n = pick(10, 60);
    const std::int64_t first = coin(0.15) ? pick(2, 4) : 1; // a source whose chain starts above 1
    bool holed = false;
    std::int64_t from = first;
    while (from <= sc.n && sc.runs.size() < 8)
    {
        if (coin(0.12))
        {
            sc.runs.push_back({ from, from - 1 }); // an empty recording
        }
        const std::int64_t to = sc.runs.size() >= 6 ? sc.n : std::min(sc.n, pick(from, from + 14));
        sc.runs.push_back({ from, to });
        if (to >= sc.n)
        {
            break;
        }
        const double r = std::uniform_real_distribution<double>(0, 1)(rng);
        if (r < 0.45)
        {
            from = to + 1; // continue
        }
        else if (r < 0.88 || holed)
        {
            from = pick(std::max(first, to - 6), to + 1); // a restart: the log replay re-records an overlap
        }
        else
        {
            from = to + 2; // an interior hole at to + 1
            holed = true;
        }
    }
    if (sc.runs.back().to < sc.n)
    {
        sc.runs.push_back({ sc.runs.back().to + 1, sc.n });
    }
    // At least two non-empty recordings: one that stops while followed, and its late successor.
    std::size_t nonEmpty = 0;
    for (const auto &r : sc.runs)
    {
        nonEmpty += r.to >= r.from ? 1 : 0;
    }
    if (nonEmpty < 2)
    {
        const Run last = sc.runs.back();
        const std::int64_t mid = (last.from + last.to) / 2;
        sc.runs.back() = { last.from, mid };
        sc.runs.push_back({ mid + 1, last.to });
    }
    while (sc.runs.back().to < sc.runs.back().from)
    {
        sc.runs.pop_back();
    }
    sc.stopAt = static_cast<int>(pick(1, 4 * sc.n));
    sc.successorDelay = static_cast<int>(pick(1, sc.n));
    if (coin(0.3))
    {
        for (int i = 0, k = static_cast<int>(pick(1, 3)); i < k; ++i)
        {
            sc.rewinds.push_back(static_cast<int>(pick(1, 6 * sc.n)));
        }
    }
    sc.abortPct = coin(0.3) ? static_cast<int>(pick(5, 30)) : 0;
    return sc;
}

struct Truth
{
    bool halts = false;
    std::string verdict;
    std::int64_t applied = 0;
};

// The forward rule over the final chain, recordings in recordingId order.
Truth groundTruth(const WalkScenario &sc)
{
    Truth t;
    bool first = true;
    for (const auto &r : sc.runs)
    {
        for (std::int64_t s = r.from; s <= r.to; ++s)
        {
            if (s <= t.applied)
            {
                first = false;
                continue;
            }
            if (s == t.applied + 1)
            {
                t.applied = s;
                first = false;
                continue;
            }
            t.halts = true;
            t.verdict = first && 0 == t.applied ? "BELOW_RANGE(" + std::to_string(s) + ")"
                                                 : "HOLE(" + std::to_string(t.applied + 1) + ".." + std::to_string(s - 1) + ")";
            return t;
        }
    }
    return t;
}

// The owner: SKIP at or below the cursor, APPLY the next, and a jump is BELOW_RANGE or HOLE only on a
// memo-free read (a jump on a memo read is verified by a rewind from the chain start).
struct ForwardOwner : FrameSink
{
    ResumeMemo *memo = nullptr;
    std::mt19937_64 *rng = nullptr;
    int abortPct = 0;
    std::int64_t applied = 0;
    std::vector<std::int64_t> appliedSeqs;
    std::optional<std::string> verdict;
    bool firstOfRead = true;
    bool rewindDue = false;

    SinkAction onFrame(const JournalFrame &f) noexcept override
    {
        if (verdict || rewindDue)
        {
            return SinkAction::ABORT;
        }
        if (abortPct > 0 && std::uniform_int_distribution<int>(0, 99)(*rng) < abortPct)
        {
            return SinkAction::ABORT; // back-pressure: redelivered on a later iteration
        }
        const std::int64_t s = f.peek.journalSeq;
        if (s <= applied)
        {
            firstOfRead = false;
            return SinkAction::CONTINUE;
        }
        if (s == applied + 1)
        {
            applied = s;
            appliedSeqs.push_back(s);
            memo->onReport(E, applied);
            memo->prove(0, f.key, s, E, f.endPosition);
            firstOfRead = false;
            return SinkAction::CONTINUE;
        }
        if (f.readWasMemoFree)
        {
            verdict = firstOfRead && 0 == applied ? "BELOW_RANGE(" + std::to_string(s) + ")"
                                                  : "HOLE(" + std::to_string(applied + 1) + ".." + std::to_string(s - 1) + ")";
            return SinkAction::ABORT;
        }
        if (HoleDecision::VERIFY_FROM_CHAIN_START == memo->onHole(0, applied + 1, false))
        {
            rewindDue = true;
            return SinkAction::ABORT;
        }
        verdict = "HOLE(" + std::to_string(applied + 1) + ".." + std::to_string(s - 1) + ")";
        return SinkAction::ABORT;
    }

    RefusalAction onRefused(const RefusedFrame &) noexcept override { return RefusalAction::CLOSE_REPLAY; }
};

struct ReadStartObserver : CoreObserver
{
    ForwardOwner *owner = nullptr;
    const ResumeMemo *memo = nullptr;

    void onAttached(const AttachInfo &a) noexcept override
    {
        CoreObserver::onAttached(a);
        if (!a.memoFree || a.fromProof)
        {
            return;
        }
        // a memo-free read begins at the first recording with bytes
        const auto &chain = memo->chain(0);
        std::size_t firstWithBytes = 0;
        while (firstWithBytes < chain.size() && !chain[firstWithBytes].active() &&
            chain[firstWithBytes].stopPosition == chain[firstWithBytes].startPosition)
        {
            ++firstWithBytes;
        }
        if (a.chainIndex == firstWithBytes && a.from == a.recording.startPosition)
        {
            owner->firstOfRead = true;
        }
    }
};

struct WalkOutcome
{
    std::string failure; // empty = held
    int recoveries = 0;
};

WalkOutcome walkOnce(std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    const WalkScenario sc = generate(rng);
    const Truth truth = groundTruth(sc);
    WalkOutcome out;
    auto fail = [&](const std::string &why) {
        out.failure = "seed " + std::to_string(seed) + ": " + why + " :: " + sc.describe();
        return out;
    };

    StampedArchive a;
    a.streamId = CORE_STREAM;
    ArchiveMetrics metrics;
    ResumeMemo memo(1, &metrics);
    ForwardOwner owner;
    owner.memo = &memo;
    owner.rng = &rng;
    owner.abortPct = sc.abortPct;
    ReadStartObserver observer;
    observer.owner = &owner;
    observer.memo = &memo;
    FollowPolicy policy(0, coreSettings(), memo, &metrics, &observer);
    FollowerLoop<StampedArchive> loop(a, policy, &metrics);

    // Every recording but the last is there at the start; the second-to-last is ACTIVE and stops at stopAt;
    // the last one appears successorDelay iterations later.
    const std::size_t last = sc.runs.size() - 1;
    std::size_t old = last - 1;
    for (std::size_t i = 0; i < last; ++i)
    {
        SimRecording &rec = a.record();
        if (sc.runs[i].to >= sc.runs[i].from)
        {
            appendCore(a, rec, sc.runs[i].from, sc.runs[i].to - sc.runs[i].from + 1);
        }
        if (i != old)
        {
            a.stop(rec);
        }
    }
    const std::int64_t oldId = static_cast<std::int64_t>(old);
    TimePoint now{};
    policy.attach(now);
    bool successorAdded = false;
    int rewindsHonoured = 0;
    std::int64_t appliedAtRewind = 0;
    const int cap = 30'000;
    for (int it = 1; it <= cap; ++it)
    {
        if (it == sc.stopAt)
        {
            a.stop(*a.find(oldId));
        }
        if (!successorAdded && it >= sc.stopAt + sc.successorDelay)
        {
            SimRecording &rec = a.record();
            appendCore(a, rec, sc.runs[last].from, sc.runs[last].to - sc.runs[last].from + 1);
            successorAdded = true;
        }
        if (std::find(sc.rewinds.begin(), sc.rewinds.end(), it) != sc.rewinds.end() && !owner.verdict)
        {
            policy.reattach(now);
            ++rewindsHonoured;
            appliedAtRewind = owner.applied;
        }
        if (owner.rewindDue)
        {
            owner.rewindDue = false;
            policy.reattach(now);
        }
        now += 1ms;
        loop.doWork(owner, 32, now);
        if (owner.verdict || (successorAdded && !truth.halts && owner.applied >= truth.applied))
        {
            break;
        }
    }
    out.recoveries = observer.ended + (rewindsHonoured > 0 && owner.applied > appliedAtRewind ? 1 : 0);
    for (std::size_t i = 0; i < owner.appliedSeqs.size(); ++i)
    {
        if (owner.appliedSeqs[i] != static_cast<std::int64_t>(i) + 1)
        {
            return fail("applied out of order or twice at index " + std::to_string(i));
        }
    }
    if (truth.halts)
    {
        if (!owner.verdict)
        {
            return fail("follower stuck on a stopped recording or short of the head: expected " + truth.verdict +
                ", applied " + std::to_string(owner.applied));
        }
        if (*owner.verdict != truth.verdict)
        {
            return fail("verdict " + *owner.verdict + ", the chain says " + truth.verdict);
        }
        if (owner.applied != truth.applied)
        {
            return fail("applied " + std::to_string(owner.applied) + " before the verdict, the chain says " +
                std::to_string(truth.applied));
        }
        return out;
    }
    if (owner.verdict)
    {
        return fail("verdict " + *owner.verdict + " on a chain without one");
    }
    if (owner.applied != truth.applied)
    {
        return fail("follower stuck on a stopped recording or short of the head: applied " +
            std::to_string(owner.applied) + " of " + std::to_string(truth.applied));
    }
    return out;
}

// ---- the memo seed

struct SeedCase
{
    const char *name;
    std::int64_t recordingId;
    std::int64_t archiveId;
    std::int64_t seq;
    std::int64_t endPosition;
};

}

TEST_CASE("JournalProfileTest.coreProfileAdmitsItsFramesAndOnlyThem")
{
    const Bytes core = coreFrame(42, E, 123456);
    const Admission ok = admitFrame(CORE_ORDER_JOURNAL, FRAME_FLAGS_UNFRAGMENTED, core.data(), core.size());
    REQUIRE(ok.accepted());
    CHECK(ok.peek.journalSeq == 42);
    CHECK(ok.peek.epoch() == E);
    CHECK(ok.peek.position() == 123456);
    // The ME gate refuses a core frame, and the core gate refuses an ME frame: one profile per stream.
    CHECK(admitFrame(FRAME_FLAGS_UNFRAGMENTED, core.data(), core.size()).refusal == Refusal::SCHEMA);
    const Bytes me = makeFrame(TEMPLATE_TRADE, 1, E);
    CHECK(admitFrame(CORE_ORDER_JOURNAL, FRAME_FLAGS_UNFRAGMENTED, me.data(), me.size()).refusal == Refusal::SCHEMA);
    // A frame longer than the profile's largest is refused before the header is read.
    Bytes longer = core;
    longer.push_back(0);
    CHECK(admitFrame(CORE_ORDER_JOURNAL, FRAME_FLAGS_UNFRAGMENTED, longer.data(), longer.size()).refusal == Refusal::LENGTH);
    // The ME profile is the default gate, value for value.
    for (const Bytes &b : { makeFrame(TEMPLATE_TRADE, 3, E), makeRelease(4, E, 2), makeFrame(TEMPLATE_CONFIG, 0, E) })
    {
        const Admission d = admitFrame(FRAME_FLAGS_UNFRAGMENTED, b.data(), b.size());
        const Admission p = admitFrame(ME_JOURNAL, FRAME_FLAGS_UNFRAGMENTED, b.data(), b.size());
        CHECK(d.refusal == p.refusal);
        CHECK(d.peek.journalSeq == p.peek.journalSeq);
    }
}

TEST_CASE("JournalProfileTest.coreGateOneVerdictInEveryReadMode")
{
    const Fault faults[] = { Fault::SCHEMA_ID, Fault::VERSION, Fault::TEMPLATE, Fault::BLOCK, Fault::FRAGMENTED,
        Fault::SEQ_ZERO, Fault::EPOCH_ZERO };
    const Mode modes[] = { Mode::MEMO_READ, Mode::MEMO_FREE, Mode::AT_OR_BELOW_CURSOR, Mode::LIVE_AT_HEAD,
        Mode::BOUNDED_STOPPED };
    int verdicts = 0;
    for (const Fault fault : faults)
    {
        const std::string want = refusalLabel(expectedRefusal(fault));
        for (const Mode mode : modes)
        {
            const std::string got = gateVerdict(fault, mode);
            ++verdicts;
            const bool same = got == want;
            const std::string text = "frame refused: " + want + " expected, " + (got == "accepted" ? "" : "got ") + got +
                " (" + modeName(mode) + ")";
            CHECK_MESSAGE(same, text);
        }
    }
    CHECK(verdicts == 35);
}

TEST_CASE("JournalProfileTest.coreChainWalkProperty")
{
    int recoveries = 0;
    int halts = 0;
    int complete = 0;
    for (std::uint64_t seed = 1; seed <= 300; ++seed)
    {
        const WalkOutcome o = walkOnce(seed * 7919);
        CHECK_MESSAGE(o.failure.empty(), o.failure);
        if (!o.failure.empty())
        {
            break;
        }
        recoveries += o.recoveries;
        std::mt19937_64 rng(seed * 7919);
        (groundTruth(generate(rng)).halts ? halts : complete) += 1;
    }
    MESSAGE("recoveries " << recoveries << ", verdicts " << halts << ", complete " << complete);
    CHECK_MESSAGE(recoveries > 0, "no recovery in the seed set");
    CHECK(halts > 0);
    CHECK(complete > 0);
}

TEST_CASE("JournalProfileTest.memoSeedIsUsedOnlyThroughTheBelt")
{
    for (int round = 0; round < 2; ++round) // twice: two process lives, each handed its stored proof
    {
        const std::int64_t archiveId = 4242 + round;
        const SeedCase cases[] = {
            { "a correct stored proof", 1, archiveId, 6, 512 },
            { "a proof whose next frame is not sequence + 1", 1, archiveId, 5, 512 },
            { "a proof with another archive id", 1, 999, 6, 512 },
            { "a proof on a recording that no longer exists", 17, archiveId, 6, 512 },
        };
        for (const SeedCase &c : cases)
        {
            CAPTURE(round);
            CAPTURE(c.name);
            Rig<> r(keyedSettings());
            r.archive.archiveId = archiveId;
            SimRecording &r0 = r.archive.record();
            appendCore(r.archive, r0, 1, 4);
            r.archive.stop(r0);
            SimRecording &r1 = r.archive.record();
            appendCore(r.archive, r1, 5, 4); // ends 256, 512, 768, 1024
            r.memo.onReport(E, 8);
            RecordingKey key = r1.rec.key;
            key.recordingId = c.recordingId;
            key.archiveId = c.archiveId;
            REQUIRE(r.memo.seed(0, key, c.seq, E, c.endPosition));
            r.policy->attach(r.now);
            REQUIRE(r.runUntil([&] { return !r.archive.requests.empty(); }));
            const std::string name = c.name;
            if (name == "a correct stored proof")
            {
                REQUIRE(r.runUntil([&] { return r.sink.seqs.size() >= 2; }));
                CHECK(r.archive.requests.front().recordingId == 1);
                CHECK_MESSAGE(r.archive.requests.front().from == 512, "the read did not start at the proof");
                CHECK(r.archive.requests.size() == 1); // no rescan
                CHECK((r.sink.seqs == std::vector<std::int64_t>{ 7, 8 }));
                CHECK_FALSE(r.observer.sawFault(FaultKind::RESUME_BELT));
                CHECK(r.observer.attaches.front().fromProof);
                CHECK_FALSE(r.observer.attaches.front().memoFree);
                CHECK(ArchiveMetrics::get(r.metrics.memoSeedsAdopted) == 1);
            }
            else if (name == "a proof whose next frame is not sequence + 1")
            {
                r.runUntil([&] { return r.sink.seqs.size() >= 8 || r.sink.has(7); }, 500);
                CHECK_MESSAGE(r.observer.sawFault(FaultKind::RESUME_BELT), "seeded proof used without the belt");
                const bool sevenAfterOne = !r.sink.has(7) || r.sink.has(1);
                CHECK_MESSAGE(sevenAfterOne, "seeded proof used without the belt: 7 read without 1..6");
                REQUIRE(r.runUntil([&] { return r.sink.seqs.size() >= 8; }));
                CHECK((r.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6, 7, 8 })); // the memo-free read
                CHECK(r.archive.requests.back().recordingId == 1);
                CHECK(r.archive.requests[1].recordingId == 0);
                CHECK(r.archive.requests[1].from == 0);
            }
            else
            {
                REQUIRE(r.runUntil([&] { return r.sink.seqs.size() >= 8; }));
                CHECK(r.memo.pendingSeeds(0) == 0);
                CHECK(ArchiveMetrics::get(r.metrics.memoSeedsDropped) == 1);
                CHECK(ArchiveMetrics::get(r.metrics.memoSeedsAdopted) == 0);
                CHECK(r.archive.requests.front().recordingId == 0); // memo-free, from the chain start
                CHECK(r.archive.requests.front().from == 0);
                CHECK(r.observer.attaches.front().memoFree);
                CHECK((r.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6, 7, 8 })); // nothing skipped
            }
        }
    }
    // Refused at the door: no report yet, another epoch, above the cursor, below 1, not a frame end.
    ResumeMemo memo(1);
    CHECK_FALSE(memo.seed(0, key(1), 3, E, 512));
    memo.onReport(E, 8);
    CHECK_FALSE(memo.seed(0, key(1), 3, E + 1, 512));
    CHECK_FALSE(memo.seed(0, key(1), 9, E, 512));
    CHECK_FALSE(memo.seed(0, key(1), 0, E, 512));
    CHECK_FALSE(memo.seed(0, key(1), 3, E, 500));
    CHECK(memo.seed(0, key(1), 3, E, 512));
}

TEST_CASE("JournalProfileTest.wipedArchiveRestartingItsIdsNeverMatchesAnOldProof")
{
    // The same recording id and the same start timestamp on an archive with another id: the key differs.
    Rig<> r(keyedSettings());
    r.archive.archiveId = 11;
    SimRecording &r0 = r.archive.record();
    appendCore(r.archive, r0, 1, 3);
    r.memo.onReport(E, 3);
    r.policy->attach(r.now);
    REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 3; }));
    REQUIRE(r.memo.proofCount(0) == 1);
    r.archive.archiveId = 12; // the member's archive was wiped and started again
    r.policy->reattach(r.now);
    REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 6; }));
    CHECK(r.observer.attaches.back().memoFree); // the old proof was not used
    CHECK(r.archive.requests.back().from == 0);
    CHECK(r.metrics.invalidations(MemoReason::HISTORY_CHANGED) == 1);
}

TEST_CASE("JournalProfileTest.archiveIdLeftOutOfTheKeyByDefault")
{
    // The default: the archive id is not in the key, so a stored proof is matched on the recording id, the
    // start timestamp and the rest of the descriptor, and every resume goes through the belt.
    CHECK_FALSE(FollowSettings{}.keyByArchiveId);
    Rig<> r;
    r.archive.archiveId = 11;
    SimRecording &r0 = r.archive.record();
    appendCore(r.archive, r0, 1, 3);
    r.memo.onReport(E, 3);
    r.policy->attach(r.now);
    REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 3; }));
    REQUIRE_FALSE(r.observer.attaches.empty());
    CHECK(r.observer.attaches.front().recording.key.archiveId == ARCHIVE_ID_UNKNOWN); // the listed key
    REQUIRE(r.memo.proofCount(0) == 1);
    // A restart that keeps the descriptor but changes the archive id is not a history change here.
    r.archive.archiveId = 12;
    appendCore(r.archive, r0, 4, 2);
    r.policy->reattach(r.now);
    REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 5; }));
    CHECK_FALSE(r.observer.attaches.back().memoFree);
    CHECK(r.metrics.invalidations(MemoReason::HISTORY_CHANGED) == 0);
    CHECK((r.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5 }));
}

TEST_CASE("JournalProfileTest.expectedEpochFilter")
{
    FollowSettings s = coreSettings();
    s.expectedEpoch = E;
    Rig<> r(s);
    StampedArchive &a = r.archive;
    SimRecording &old0 = a.record(); // a previous genesis, first by recordingId
    appendCore(a, old0, 1, 5, 3);
    a.stop(old0);
    SimRecording &old1 = a.record();
    appendCore(a, old1, 6, 2, 3);
    a.stop(old1);
    SimRecording &now0 = a.record();
    appendCore(a, now0, 1, 6, E);
    r.policy->attach(r.now);
    r.runUntil([&] { return r.sink.seqs.size() >= 6; });
    const bool foreignWalked = std::any_of(r.sink.epochs.begin(), r.sink.epochs.end(), [](std::int64_t e) { return e != E; });
    CHECK_MESSAGE(!foreignWalked, "recording of another epoch walked");
    CHECK((r.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6 }));
    CHECK(ArchiveMetrics::get(r.metrics.foreignEpochRecordings) == 2);
    CHECK(r.observer.foreign == 2);
    // A second walk of the same chain does not replay the foreign recordings again, and counts nothing new.
    r.policy->reattach(r.now);
    const std::size_t attaches = r.observer.attaches.size();
    REQUIRE(r.runUntil([&] { return r.observer.attaches.size() > attaches; }));
    std::map<std::int64_t, int> requests;
    for (const auto &q : a.requests)
    {
        ++requests[q.recordingId];
    }
    CHECK(requests[0] == 1);
    CHECK(requests[1] == 1);
    CHECK(requests[2] == 2);
    CHECK(ArchiveMetrics::get(r.metrics.foreignEpochRecordings) == 2);
    // The library never derives the epoch: below 1 is refused, in the settings and at run time.
    ResumeMemo memo(1);
    FollowSettings zero = coreSettings();
    zero.expectedEpoch = 0;
    CHECK_THROWS_AS(FollowPolicy(0, zero, memo, nullptr, nullptr), std::invalid_argument);
    CHECK_THROWS_AS(r.policy->expectEpoch(0, r.now), std::invalid_argument);
    CHECK_THROWS_AS(r.policy->expectEpoch(-1, r.now), std::invalid_argument);
    CHECK(*r.policy->expectedEpoch() == E);
}

TEST_CASE("JournalProfileTest.successorWhileTheOldRecordingStillReadsActive")
{
    FollowSettings s = coreSettings();
    s.relistInterval = 20ms;
    s.probeInterval = 1000ms; // only the timed re-list can find the successor in time
    Rig<> r(s);
    StampedArchive &a = r.archive;
    SimRecording &r0 = a.record();
    appendCore(a, r0, 1, 5);
    r.policy->attach(r.now);
    REQUIRE(r.runUntil([&] { return r.sink.seqs.size() == 5; }));
    for (int i = 0; i < 30; ++i)
    {
        r.step(); // following at the head of an ACTIVE recording
    }
    // The member restarted: its old recording kept no stop position and still receives its last frames; the
    // new recording starts below them (the log replay re-records an overlap).
    appendCore(a, *a.find(0), 6, 2);
    SimRecording &r1 = a.record();
    appendCore(a, r1, 4, 6); // 4..9
    const int listsBefore = a.listCalls;
    int iterationsToList = -1;
    for (int i = 1; i <= 40 && iterationsToList < 0; ++i)
    {
        r.step();
        if (a.listCalls > listsBefore)
        {
            iterationsToList = i;
        }
    }
    r.runUntil([&] { return r.sink.has(9); }, 400);
    CHECK_MESSAGE(r.sink.has(9), "successor not found while the old recording is active");
    REQUIRE(iterationsToList > 0);
    CHECK(iterationsToList <= 21); // inside one re-list interval
    // The old recording is drained to its end first: 6 and 7 come from it, before any frame of the successor.
    const auto firstOfR1 = std::find(r.sink.recordings.begin(), r.sink.recordings.end(), 1) - r.sink.recordings.begin();
    std::vector<std::int64_t> fromR0(r.sink.seqs.begin(), r.sink.seqs.begin() + firstOfR1);
    CHECK((fromR0 == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6, 7 }));
    CHECK(ArchiveMetrics::get(r.metrics.relistsWhileAttached) >= 1);
    CHECK(r.observer.faults.empty());
}

TEST_CASE("JournalProfileTest.checkpointAfterASourceSwitch")
{
    const std::size_t A = 0, B = 1;
    ResumeMemo memo(2);
    RecordingKey ka = key(3);
    ka.archiveId = 11;
    RecordingKey kb = key(1);
    kb.archiveId = 22;
    memo.onListing(A, { Recording{ ka, 0, NULL_POSITION } });
    memo.onListing(B, { Recording{ kb, 0, NULL_POSITION } });
    memo.onReport(E, 20);
    REQUIRE(memo.prove(A, ka, 20, E, 640));
    const ReaderCheckpoint onA = memo.readerCheckpoint(A, 20);
    CHECK((onA == ReaderCheckpoint{ E, 11, 3, 640, 20 }));
    // Switched to B, which has read nothing yet: the logical sequence holds, no field of A survives.
    const ReaderCheckpoint onB = memo.readerCheckpoint(B, 20);
    const bool noOldField = onB.position == NULL_POSITION && onB.recordingId == -1 && onB.archiveId == ARCHIVE_ID_UNKNOWN;
    CHECK_MESSAGE(noOldField, "checkpoint position from another source");
    CHECK(onB.logicalSeq == 20);
    CHECK(onB.sourceEpoch == E);
    // B reads on: its own recording and position.
    memo.onReport(E, 21);
    REQUIRE(memo.prove(B, kb, 21, E, 320));
    const ReaderCheckpoint later = memo.readerCheckpoint(B, 21);
    const bool own = later == ReaderCheckpoint{ E, 22, 1, 320, 21 };
    CHECK_MESSAGE(own, "checkpoint position from another source");
}

TEST_CASE("JournalProfileTest.settingsUnderAnotherPrefix")
{
    std::map<std::string, std::string> env = {
        { "HISTORY_PROJECTOR_JOURNAL_ARCHIVES", "10.0.0.1:8010,10.0.0.2:8010" },
        { "HISTORY_PROJECTOR_JOURNAL_CONTROL_STREAM", "10" },
        { "HISTORY_PROJECTOR_JOURNAL_STREAM", "4102" },
        { "HISTORY_PROJECTOR_JOURNAL_CHANNEL", "aeron:ipc" },
        { "HISTORY_PROJECTOR_REPLAY_STREAM", "4103" },
        { "HISTORY_PROJECTOR_LOCAL_HOST", "10.0.0.9" },
        { "HISTORY_PROJECTOR_JOURNAL_MTU", "256" },
        { "BRIDGE_HALT_ON_GAP", "true" }, // the bridge's key means nothing here
    };
    const ArchiveSettingsKeys keys = ArchiveSettingsKeys::withPrefix("HISTORY_PROJECTOR_", CORE_ORDER_JOURNAL);
    auto parse = [&](const std::map<std::string, std::string> &e, SettingsScope scope) {
        return parseArchiveSettings(
            [&](const std::string &k) -> std::optional<std::string> {
                auto it = e.find(k);
                return it == e.end() ? std::nullopt : std::optional<std::string>(it->second);
            },
            scope, keys);
    };
    auto mentions = [](const ArchiveSettingsResult &r, const std::string &needle) {
        return std::any_of(r.errors.begin(), r.errors.end(), [&](const std::string &e) { return e.find(needle) != std::string::npos; });
    };
    const auto ok = parse(env, SettingsScope::EXTERNAL);
    const bool parsed = ok.ok();
    REQUIRE_MESSAGE(parsed, (ok.errors.empty() ? std::string() : ok.errors.front()));
    CHECK(ok.settings->archives.size() == 2);
    CHECK(ok.settings->journalStreamId == 4102);
    CHECK(ok.settings->journalMtu == 256); // the core record's frame bound, not the ME one
    // The ME journal's bound under the bridge's keys refuses 256.
    std::map<std::string, std::string> bridgeEnv;
    for (const auto &[k, v] : env)
    {
        if (k.rfind("HISTORY_PROJECTOR_", 0) == 0)
        {
            bridgeEnv["BRIDGE_" + (k == "HISTORY_PROJECTOR_JOURNAL_ARCHIVES" ? std::string("ME_JOURNAL_ARCHIVES")
                                                                            : k.substr(18))] = v;
        }
    }
    const auto bridge = parseArchiveSettings(
        [&](const std::string &k) -> std::optional<std::string> {
            auto it = bridgeEnv.find(k);
            return it == bridgeEnv.end() ? std::nullopt : std::optional<std::string>(it->second);
        },
        SettingsScope::BRIDGE_EXTERNAL);
    CHECK(mentions(bridge, "BRIDGE_JOURNAL_MTU must be a multiple of 32 in [1144"));
    // Errors name the owner's keys and the owner.
    auto missing = env;
    missing.erase("HISTORY_PROJECTOR_JOURNAL_ARCHIVES");
    CHECK(mentions(parse(missing, SettingsScope::EXTERNAL), "HISTORY_PROJECTOR_JOURNAL_ARCHIVES is required"));
    auto withDir = env;
    withDir["HISTORY_PROJECTOR_DRIVER_DIR"] = "/dev/shm";
    CHECK(mentions(parse(withDir, SettingsScope::EXTERNAL),
        "HISTORY_PROJECTOR_DRIVER_DIR is set but the process runs on an external driver (HISTORY_PROJECTOR_DRIVER_MODE=external)"));
    CHECK(parse(withDir, SettingsScope::EMBEDDED).ok());
    CHECK(mentions(parse(env, SettingsScope::EMBEDDED), "HISTORY_PROJECTOR_DRIVER_DIR is required"));
    auto small = env;
    small["HISTORY_PROJECTOR_JOURNAL_MTU"] = "224";
    CHECK(mentions(parse(small, SettingsScope::EXTERNAL), "HISTORY_PROJECTOR_JOURNAL_MTU must be a multiple of 32 in [248"));
}
