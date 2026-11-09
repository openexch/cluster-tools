// RealCoreOracleTest (the E3 foundation): the real BridgeDomain + BridgeCodec, reading
// SimArchive sources built from an `story` journal through real follower, feed the REAL behaviour-3 core
// (`serve`, assets b05b8e8 production code inside the pinned harness jar). The records of round r are appended to
// the simulated archives only after `ROUND r` was applied (the ME journals an answer only after it saw the command),
// so the log order equals the story's. Faults: cold restarts of the domain at random points (the one BRIDGE session
// of the story reused: its second RegisterSession answers DUPLICATE), elections ("drop the uncommitted tail, deliver
// onFence(NEW_LEADER), only then the next FRAME"), holes on a subset of sources filled from another, padding,
// re-journalled duplicate recordings, back-pressure.
//
// Class 1 (clean): the final snapshotSha256 equals the story's clean hash; lastAppliedJournalSeq == N; no latch
// (the `L` line of DUMP asserted); feedDuplicateCount == 0 (N = 1); invariantsHold; per sync ascending
// offers. Class 2 (a hole on every source): GAP at e; DUMP(e - 1) equals the clean model prefix with `L true 1`;
// after an operator refill the re-scan recovers, ADMIN OPEN, and the final hash equals the clean run's with `L false 0`.
// Class 3: an outcome with ledgerCapacity 0 (or a release entry with commandIdHigh 0) on every copy: the
// core refuses it, the bridge rotates once per source and parks in CORE_REFUSED, the seq offered at most
// stormFences x sources times.
#include "doctest/doctest.h"

#include "BridgeFeedFiles.h"
#include "ServeClient.h"
#include "SimArchive.h"

#include "cluster_tools/adapters/sbe/BridgeCodec.h"
#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/archive/FollowerLoop.h"
#include "cluster_tools/archive/ResumeMemo.h"
#include "cluster_tools/domain/bridge/BridgeDomain.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <sys/wait.h>

using namespace cluster_tools::archive;
using namespace cluster_tools::domain::bridge;
using cluster_tools::sbe_adapter::bridge_codec::BridgeCodec;
using cluster_tools::sbe_adapter::bridge_codec::EgressEvent;
using cluster_tools::sbe_adapter::bridge_codec::EgressKind;
using ct_archive_test::SimArchive;
using ct_archive_test::SimRecording;
using namespace std::chrono_literals;

namespace {

enum class Kind { CLEAN, HOLE, CORE_ONLY };

std::int64_t rd64(const std::uint8_t *p)
{
    std::uint64_t u = 0;
    for (int i = 7; i >= 0; --i)
    {
        u = (u << 8) | p[i];
    }
    return static_cast<std::int64_t>(u);
}

// the journalSeqs a money feed frame carries
std::vector<std::int64_t> seqsOf(const std::uint8_t *f, std::size_t n)
{
    std::vector<std::int64_t> out;
    if (n < 8)
    {
        return out;
    }
    const std::uint16_t t = static_cast<std::uint16_t>(f[2] | (f[3] << 8));
    switch (t)
    {
        case T_ME_JOURNAL_TRADE: out.push_back(rd64(f + 8 + 101)); break;
        case T_ME_JOURNAL_TRADE_BATCH:
        {
            const std::size_t count = static_cast<std::size_t>(f[10] | (f[11] << 8));
            for (std::size_t i = 0; i < count; ++i)
            {
                out.push_back(rd64(f + 12 + 109 * i + 101));
            }
            break;
        }
        case T_ME_JOURNAL_TERMINAL: out.push_back(rd64(f + 8 + 53)); break;
        case T_ME_COMMAND_OUTCOME: out.push_back(rd64(f + 8 + 124)); break;
        case T_ME_COMMAND_RELEASE:
        case T_ME_ENGINE_CONFIG: out.push_back(rd64(f + 8 + 8)); break;
        default: break;
    }
    return out;
}

std::string run(const std::string &cmd)
{
    const int rc = std::system(cmd.c_str());
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0 ? "" : "command failed: " + cmd;
}

// the core port: frames are in flight (uncommitted) until the "cluster" commits them to serve in order
struct OracleCore : CorePort
{
    ct_oracle::ServeClient &serve;
    std::int64_t session;
    std::mt19937_64 &rng;
    BridgeDomain *domain = nullptr;
    std::deque<ct_oracle::Bytes> inflight;
    std::deque<std::pair<long, ct_oracle::Bytes>> egress; // (the iteration it was emitted, the frame)
    long iteration = 0;
    int lag = 1; // egress reaches the domain lag duty iterations after the commit
    double retry = 0;
    std::string failure;
    std::uint64_t offerSync = ~0ULL;
    std::int64_t nextInSync = 0;
    std::int64_t watchSeq = 0;
    int watchOffers = 0;
    int frames = 0, queries = 0, feedHalts = 0;
    std::int64_t lastHaltExpected = 0, lastHaltSeen = 0;
    std::uint8_t lastHaltReason = 0;

    OracleCore(ct_oracle::ServeClient &s, std::int64_t sess, std::mt19937_64 &r) : serve(s), session(sess), rng(r) {}

    OfferOutcome tryOffer(const std::uint8_t *f, std::size_t n) noexcept override
    {
        if (retry > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < retry)
        {
            return OfferOutcome::RETRY;
        }
        const std::vector<std::int64_t> seqs = seqsOf(f, n);
        if (!seqs.empty() && nullptr != domain)
        {
            if (domain->syncs() != offerSync)
            {
                offerSync = domain->syncs();
                nextInSync = domain->syncCursor() + 1;
            }
            for (std::int64_t s : seqs)
            {
                if (s != nextInSync && failure.empty())
                {
                    failure = "sync from " + std::to_string(domain->syncCursor()) + " offered " + std::to_string(s) +
                        " where " + std::to_string(nextInSync) + " was due";
                }
                nextInSync = s + 1;
                watchOffers += s == watchSeq ? 1 : 0;
            }
        }
        const std::uint16_t t = static_cast<std::uint16_t>(f[2] | (f[3] << 8));
        if (T_FEED_HALT == t)
        {
            ++feedHalts;
            lastHaltReason = f[8 + 8];
            lastHaltExpected = rd64(f + 8 + 17);
            lastHaltSeen = rd64(f + 8 + 25);
        }
        inflight.emplace_back(f, f + n);
        return OfferOutcome::OK;
    }

    void commit(std::size_t max = ~std::size_t(0))
    {
        while (!inflight.empty() && max-- > 0)
        {
            const ct_oracle::Bytes f = inflight.front();
            inflight.pop_front();
            const std::uint16_t t = static_cast<std::uint16_t>(f[2] | (f[3] << 8));
            if (T_QUERY_FEED_POSITION == t)
            {
                ++queries;
                egress.emplace_back(iteration, serve.query(session, rd64(f.data() + 8)));
            }
            else
            {
                ++frames;
                for (auto &e : serve.frame(session, f.data(), f.size()))
                {
                    egress.emplace_back(iteration, std::move(e));
                }
            }
        }
    }

    void deliver(TimePoint now, bool all = false)
    {
        while (!egress.empty() && nullptr != domain && (all || egress.front().first + lag <= iteration))
        {
            const ct_oracle::Bytes f = egress.front().second;
            egress.pop_front();
            EgressEvent ev;
            switch (BridgeCodec::decodeEgress(f.data(), f.size(), ev))
            {
                case EgressKind::REPORT: domain->onReport(ev.report, now); break;
                case EgressKind::ADMIN_ACK: domain->onAdminAck(ev.ack, now); break;
                case EgressKind::SETTLE_REJECTED: domain->onSettleRejected(ev.reject, now); break;
                case EgressKind::MALFORMED: domain->onMalformedEgress(ev.templateId, now); break;
                case EgressKind::IGNORED: break;
            }
        }
    }
};

// one bridge process: its memo, followers and domain (the archives outlive it)
struct Bridge
{
    BridgeSettings settings;
    ArchiveMetrics archiveMetrics;
    std::unique_ptr<ResumeMemo> memo;
    BridgeCodec codec;
    BridgeMetrics metrics;
    std::unique_ptr<BridgeDomain> domain;
    std::vector<std::unique_ptr<FollowPolicy>> policies;
    std::vector<std::unique_ptr<FollowerLoop<SimArchive>>> loops;
    std::size_t current = 0;
    std::optional<std::size_t> pending;

    Bridge(std::vector<std::unique_ptr<SimArchive>> &archives, OracleCore &core, std::int64_t identity)
    {
        const std::size_t n = archives.size();
        settings.sourceCount = n;
        settings.identity = identity;
        settings.ringCapacity = 4096;
        settings.reportInterval = 200ms;
        settings.reportIntervalHalted = 200ms;
        settings.queryResend = 50ms;
        settings.reportTimeout = 500ms;
        settings.invalidReportResend = 500ms;
        settings.feedHaltResend = 500ms;
        settings.haltRescan = 3'000ms;
        settings.behindDwell = 1'000ms;
        settings.behindRescan = 6'000ms;
        settings.holeGrace = 6'000ms;
        settings.unreachableRetry = 1'000ms;
        settings.probeInterval = 1'000ms;
        memo = std::make_unique<ResumeMemo>(n, &archiveMetrics);
        domain = std::make_unique<BridgeDomain>(settings, *memo, archiveMetrics, codec, core, metrics);
        FollowSettings fs;
        fs.backoff = 100ms;
        fs.probeInterval = 1'000ms;
        for (std::size_t i = 0; i < n; ++i)
        {
            policies.push_back(std::make_unique<FollowPolicy>(i, fs, *memo, &archiveMetrics, domain.get()));
            loops.push_back(std::make_unique<FollowerLoop<SimArchive>>(*archives[i], *policies[i], &archiveMetrics));
        }
        core.domain = domain.get();
    }

    void start(TimePoint now)
    {
        policies[0]->attach(now);
        domain->onSessionUp(now);
    }

    void stop(TimePoint now)
    {
        policies[current]->detach(now);
        for (int i = 0; i < 20; ++i)
        {
            loops[current]->doWork(*domain, 0, now);
        }
    }

    void step(TimePoint now, std::mt19937_64 &rng)
    {
        const int domainLimit = domain->fragmentLimit();
        const int limit = domainLimit > 0 ? std::min(domainLimit, static_cast<int>(1 + rng() % 8)) : 0;
        const int work = loops[current]->doWork(*domain, limit, now);
        if (pending && policies[current]->idle() && !policies[current]->wantsDisconnect())
        {
            current = *pending;
            pending.reset();
            policies[current]->attach(now);
        }
        domain->onPollDone(work, now);
        const SourceCommand cmd = domain->takeSourceCommand();
        if (SourceCommandKind::SWITCH == cmd.kind && (cmd.index != current || pending))
        {
            pending = cmd.index;
            policies[current]->detach(now);
        }
        else if (SourceCommandKind::REATTACH == cmd.kind)
        {
            policies[current]->reattach(now);
        }
        domain->tick(now);
    }
};

struct Story
{
    std::vector<std::vector<ct_oracle::Bytes>> rounds; // the schema 4 records of each round's ME answer
};

Story readStory(const std::string &dir)
{
    using namespace ct_bridge_codec_test;
    Story s;
    std::vector<ct_oracle::Bytes> cur;
    for (const FjrEntry &e : decodeFjr(readFile(dir + "/journal.fjr")))
    {
        if (e.flush)
        {
            s.rounds.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(e.record);
        }
    }
    return s;
}

struct Outcome
{
    std::string failure;
    int restarts = 0, elections = 0, holes = 0, rejournals = 0;
    int lostFrames = 0; // FRAME entries an election dropped uncommitted (FENCE_OFF needs at least one)
};

Outcome scenario(std::uint64_t seed, Kind kind, const std::string &work)
{
    namespace fs = std::filesystem;
    Outcome out;
    auto fail = [&](const std::string &why) {
        out.failure = "seed " + std::to_string(seed) + " class " + std::to_string(static_cast<int>(kind)) + ": " + why;
        return out;
    };
    // a failure the real core's state shows (the oracle's verdict), named first; the bridge-side ascending-offer
    // check is reported only when the core state is clean
    auto coreFail = [&](const std::string &why) { return fail("CORE STATE: " + why); };
    std::mt19937_64 rng(seed);
    const std::string dir = work + "/story-" + std::to_string(seed);
    fs::remove_all(dir);
    const std::string java = CT_JAVA, jar = CT_FEED_HARNESS_JAR;
    if (std::string e = run("'" + java + "' -jar '" + jar + "' story " + std::to_string(seed) + " 10 '" + dir + "' 2> '" + dir +
            ".story.log'");
        !e.empty())
    {
        return fail(e);
    }
    Story story = readStory(dir);
    ct_oracle::ServeClient serve(java, jar, dir + ".serve.log");
    const ct_oracle::Ready ready = serve.boot(dir);
    const std::size_t sources = 1 + static_cast<std::size_t>(rng() % 3);
    std::vector<std::unique_ptr<SimArchive>> archives;
    std::vector<SimRecording *> active;
    for (std::size_t i = 0; i < sources; ++i)
    {
        archives.push_back(std::make_unique<SimArchive>());
        active.push_back(&archives.back()->record());
    }
    // the faulted seq of classes 2 and 3
    std::int64_t target = 0;
    std::size_t targetRound = 0;
    {
        std::int64_t seq = 0;
        std::vector<std::pair<std::int64_t, std::size_t>> candidates;
        for (std::size_t r = 0; r < story.rounds.size(); ++r)
        {
            for (const auto &rec : story.rounds[r])
            {
                ++seq;
                const std::uint16_t t = static_cast<std::uint16_t>(rec[2] | (rec[3] << 8));
                const bool fits = Kind::HOLE == kind ? (seq > 2 && r >= 2 && r + 2 < story.rounds.size() &&
                                                           rec != story.rounds[r].back())
                    : Kind::CORE_ONLY == kind ? (r >= 2 && (TEMPLATE_OUTCOME == t ||
                                                               (TEMPLATE_RELEASE == t && rec.size() > RELEASE_MIN_FRAME)))
                                              : false;
                if (fits)
                {
                    candidates.emplace_back(seq, r);
                }
            }
        }
        if (Kind::CLEAN != kind)
        {
            if (candidates.empty())
            {
                return fail("no candidate record for the class");
            }
            const auto pick = candidates[rng() % candidates.size()];
            target = pick.first;
            targetRound = pick.second;
        }
    }
    // the per-source fault plan: holes filled from another source, padding, a re-journalled duplicate recording
    std::vector<std::int64_t> holeOn(sources, 0);
    if (sources >= 2 && Kind::CLEAN == kind && rng() % 2 == 0)
    {
        holeOn[rng() % sources] = 2 + static_cast<std::int64_t>(rng() % std::max<std::int64_t>(1, ready.n - 2));
        ++out.holes;
    }
    OracleCore core(serve, ready.bridgeSession, rng);
    core.retry = rng() % 3 == 0 ? 0.05 : 0.0;
    core.lag = 1 + static_cast<int>(rng() % 8);
    core.watchSeq = target;
    auto bridge = std::make_unique<Bridge>(archives, core, ready.identity);
    TimePoint now{};
    bridge->start(now);
    std::int64_t appended = 0;
    std::vector<ct_oracle::Bytes> journal; // every record appended so far (the operator refill)
    bool sawGap = false, gapChecked = false, adminOpened = false;
    int sessionUpAt = -1;
    std::size_t nextRound = 0;
    const int maxIterations = 60'000;
    int stopAt = maxIterations;
    bool electionPending = false;
    for (int it = 0; it < stopAt; ++it)
    {
        now += 10ms;
        core.iteration = it;
        if (!core.failure.empty() && stopAt == maxIterations)
        {
            stopAt = std::min(maxIterations, it + 3'000); // let the core show what the defect did, then judge it
        }
        // an election: the uncommitted tail and the undelivered egress are lost; the fence comes before the next FRAME.
        // It is drawn 1 in 150 iterations and happens at the first iteration after the draw that has a FRAME in flight,
        // before the commit: an election that loses no frame cannot show a missing fence (the 2026-10-31 rerun found
        // every election of the 10 stories losing nothing once the lag draw shifted the random stream).
        if (Kind::CLEAN == kind && !electionPending && rng() % 150 == 0)
        {
            electionPending = true;
        }
        const bool frameInFlight = std::any_of(core.inflight.begin(), core.inflight.end(), [](const ct_oracle::Bytes &f) {
            return T_QUERY_FEED_POSITION != static_cast<std::uint16_t>(f[2] | (f[3] << 8));
        });
        if (electionPending && frameInFlight)
        {
            electionPending = false;
            ++out.elections;
            for (const ct_oracle::Bytes &f : core.inflight)
            {
                const std::uint16_t t = static_cast<std::uint16_t>(f[2] | (f[3] << 8));
                out.lostFrames += T_QUERY_FEED_POSITION == t ? 0 : 1;
            }
            core.inflight.clear();
            core.egress.clear();
            bridge->domain->onFence(FenceTrigger::NEW_LEADER, now);
            sessionUpAt = it + 1 + static_cast<int>(rng() % 10);
        }
        // the cluster commits the in-flight entries now and then (always before a round: the barrier)
        if (rng() % 10 < 6)
        {
            core.commit();
        }
        if (it == sessionUpAt)
        {
            bridge->domain->onSessionUp(now);
        }
        // a cold restart of the bridge process (the session of the story reused: RegisterSession DUPLICATE)
        if (Kind::CLEAN == kind && rng() % 1500 == 0)
        {
            ++out.restarts;
            core.commit(); // what the old process offered reaches the log before the new one queries
            core.deliver(now, true);
            bridge->stop(now);
            core.domain = nullptr;
            core.egress.clear();
            bridge.reset();
            bridge = std::make_unique<Bridge>(archives, core, ready.identity);
            (void)serve.openBridge(ready.bridgeSession, ready.identity);
            bridge->start(now);
            sessionUpAt = -1;
        }
        core.deliver(now);
        bridge->step(now, rng);
        const BridgeView v = bridge->domain->view();
        // rounds: ROUND r needs cursor >= b(r - 1); its records reach the archives only after it was applied
        if (nextRound < story.rounds.size())
        {
            const bool barrier = 0 == nextRound || v.cursor >= ready.barriers[nextRound - 1];
            if (barrier && (Kind::HOLE != kind || !sawGap || adminOpened || nextRound <= targetRound))
            {
                core.commit();
                core.deliver(now, true);
                (void)serve.round(static_cast<std::uint32_t>(nextRound));
                for (ct_oracle::Bytes rec : story.rounds[nextRound])
                {
                    ++appended;
                    if (Kind::CORE_ONLY == kind && appended == target)
                    {
                        const std::uint16_t t = static_cast<std::uint16_t>(rec[2] | (rec[3] << 8));
                        if (TEMPLATE_OUTCOME == t)
                        {
                            std::memset(rec.data() + 8 + 145, 0, 4); // ledgerCapacity 0 (S8 3.9 a, a core-only rule)
                        }
                        else
                        {
                            std::memset(rec.data() + 8 + 44 + 4, 0, 8); // the first entry's commandIdHigh 0 (S8 3.9 c)
                        }
                    }
                    journal.push_back(rec);
                    for (std::size_t s = 0; s < sources; ++s)
                    {
                        if ((Kind::HOLE == kind && appended == target) || appended == holeOn[s])
                        {
                            continue; // a hole on this source (every source for class 2)
                        }
                        if (rng() % 40 == 0)
                        {
                            archives[s]->pad(*active[s], 64);
                        }
                        archives[s]->append(*active[s], rec);
                    }
                }
                // now and then a source re-journals: its recording stops and a new one starts a few records back
                for (std::size_t s = 0; s < sources; ++s)
                {
                    if (Kind::CLEAN == kind && rng() % 6 == 0 && journal.size() > 3)
                    {
                        ++out.rejournals;
                        archives[s]->stop(*active[s]);
                        active[s] = &archives[s]->record();
                        const std::size_t back = 1 + rng() % 3;
                        for (std::size_t j = journal.size() - back; j < journal.size(); ++j)
                        {
                            archives[s]->append(*active[s], journal[j]);
                        }
                    }
                }
                ++nextRound;
            }
        }
        // class 2: the GAP halt, its DUMP against the clean prefix, the operator refill, the recovery, ADMIN OPEN
        if (Kind::HOLE == kind)
        {
            if (!sawGap && v.halt == BridgeHalt::GAP && v.state == State::HALTED)
            {
                sawGap = true;
                core.commit();
                core.deliver(now, true);
                if (bridge->domain->halt().e != target || core.lastHaltReason != 1 || core.lastHaltExpected != target)
                {
                    return fail("expected FeedHalt GAP at " + std::to_string(target) + ", halt e " +
                        std::to_string(bridge->domain->halt().e) + " reason " + std::to_string(core.lastHaltReason));
                }
                const ct_oracle::ServeClient::Dump d = serve.dump(target - 1);
                if (d.liveSha != d.modelSha)
                {
                    return fail("the money state at the GAP differs from the clean prefix up to e - 1");
                }
                if (d.latchLine() != "L true 1")
                {
                    return fail("the latch line at the GAP is '" + d.latchLine() + "', expected 'L true 1'");
                }
                gapChecked = true;
                // the operator refills source 0's archive with the complete journal (a new recording)
                archives[0]->recordings.clear();
                active[0] = &archives[0]->record();
                for (const auto &rec : journal)
                {
                    archives[0]->append(*active[0], rec);
                }
            }
            if (sawGap && !adminOpened && v.cursor >= target && v.halt == BridgeHalt::NONE)
            {
                core.commit();
                core.deliver(now, true);
                (void)serve.adminOpen(); // the gate stays SUSPENDED (BRIDGE_HALT) until a human opens it
                adminOpened = true;
            }
        }
        if (Kind::CORE_ONLY == kind && v.halt == BridgeHalt::CORE_REFUSED)
        {
            break;
        }
        if (nextRound == story.rounds.size() && v.cursor == ready.n && v.last == ready.n && v.staged == 0 &&
            v.halt == BridgeHalt::NONE)
        {
            core.commit();
            core.deliver(now, true);
            break;
        }
    }
    core.commit();
    const std::string result = serve.finish();
    const ct_oracle::ServeClient::Dump dump = serve.dump();
    const BridgeView v = bridge->domain->view();
    std::printf("oracle seed %llu class %d: lag %d, sources %zu, N %lld, core cursor %s, frames %d, queries %d, FeedHalts %d, "
                "elections %d (lost frames %d), syncs %llu, halt %s, latch '%s', sha %s\n",
        (unsigned long long)seed, static_cast<int>(kind), core.lag, sources, (long long)ready.n,
        ct_oracle::jsonField(result, "lastAppliedJournalSeq").c_str(), core.frames, core.queries, core.feedHalts,
        out.elections, out.lostFrames, (unsigned long long)bridge->domain->syncs(), bridgeHaltName(v.halt), dump.latchLine().c_str(),
        ct_oracle::jsonField(result, "snapshotSha256") == ready.cleanSha ? "== clean" : "!= clean");
    if (Kind::CORE_ONLY == kind)
    {
        if (v.halt != BridgeHalt::CORE_REFUSED)
        {
            return coreFail(std::string("expected CORE_REFUSED, halt ") + bridgeHaltName(v.halt) + " cursor " +
                std::to_string(v.cursor));
        }
        if (core.watchOffers > bridge->settings.stormFences * static_cast<int>(sources))
        {
            return coreFail("the refused seq was offered " + std::to_string(core.watchOffers) + " times");
        }
        if (ct_oracle::jsonField(result, "lastAppliedJournalSeq") != std::to_string(target - 1))
        {
            return coreFail("the core cursor is " + ct_oracle::jsonField(result, "lastAppliedJournalSeq") + ", expected " +
                std::to_string(target - 1));
        }
        if (dump.latchLine() != "L true 3")
        {
            return coreFail("the latch line is '" + dump.latchLine() + "', expected 'L true 3' (SOURCE_SCHEMA)");
        }
        return out;
    }
    if (Kind::HOLE == kind && !gapChecked)
    {
        return coreFail("no GAP halt at the hole " + std::to_string(target));
    }
    if (ct_oracle::jsonField(result, "lastAppliedJournalSeq") != std::to_string(ready.n))
    {
        return coreFail("lastAppliedJournalSeq " + ct_oracle::jsonField(result, "lastAppliedJournalSeq") + " of " + std::to_string(ready.n) +
            " (halt " + bridgeHaltName(v.halt) + ", state " + stateName(v.state) + ", last " + std::to_string(v.last) + ")");
    }
    if (ct_oracle::jsonField(result, "snapshotSha256") != ready.cleanSha)
    {
        return coreFail("snapshotSha256 " + ct_oracle::jsonField(result, "snapshotSha256") + " differs from the clean run's " + ready.cleanSha);
    }
    if (dump.latchLine() != "L false 0")
    {
        return coreFail("the latch line is '" + dump.latchLine() + "', expected 'L false 0'");
    }
    if (ct_oracle::jsonField(result, "invariantsHold") != "true")
    {
        return coreFail("invariantsHold is " + ct_oracle::jsonField(result, "invariantsHold"));
    }
    if (ct_oracle::duplicateCount(result) != 0)
    {
        return coreFail("feedDuplicateCount " + std::to_string(ct_oracle::duplicateCount(result)) + " (N = 1: every re-offer follows a fence)");
    }
    if (!core.failure.empty())
    {
        return fail(core.failure);
    }
    fs::remove_all(dir);
    return out;
}

std::string workDir()
{
    namespace fs = std::filesystem;
    const fs::path d = fs::temp_directory_path() / "ct-bridge-oracle";
    fs::create_directories(d);
    return d.string();
}

int runs(const char *env, int dflt)
{
    const char *v = std::getenv(env);
    return v ? std::atoi(v) : dflt;
}

std::uint64_t base()
{
    const char *v = std::getenv("CT_ORACLE_SEED");
    return v ? std::strtoull(v, nullptr, 0) : 0x0ac1eULL;
}

}

TEST_CASE("RealCoreOracleTest.theRealCoreEndsInTheCleanStateAcrossRestartsElectionsAndPartialHoles")
{
    const int n = runs("CT_ORACLE_RUNS", 10);
    int failures = 0, restarts = 0, elections = 0, holes = 0, rejournals = 0, lostFrames = 0;
    std::string first;
    for (int i = 0; i < n; ++i)
    {
        const Outcome o = scenario(base() + static_cast<std::uint64_t>(i), Kind::CLEAN, workDir());
        restarts += o.restarts;
        elections += o.elections;
        lostFrames += o.lostFrames;
        holes += o.holes;
        rejournals += o.rejournals;
        if (!o.failure.empty() && failures++ == 0)
        {
            first = o.failure;
        }
    }
    MESSAGE("E3 oracle (clean): " << n << " stories, restarts " << restarts << ", elections " << elections
            << " (lost frames " << lostFrames << "), partial holes " << holes << ", re-journals " << rejournals);
    INFO(first);
    CHECK(failures == 0);
    CHECK(lostFrames > 0); // non-vacuity: an election dropped an uncommitted frame, so a missing fence would show
}

TEST_CASE("RealCoreOracleTest.aHoleOnEverySourceHaltsGapAtTheCleanPrefixAndRecoversAfterTheFill")
{
    const int n = std::max(1, runs("CT_ORACLE_RUNS", 10) / 3);
    int failures = 0;
    std::string first;
    for (int i = 0; i < n; ++i)
    {
        const Outcome o = scenario(base() + 1000 + static_cast<std::uint64_t>(i), Kind::HOLE, workDir());
        if (!o.failure.empty() && failures++ == 0)
        {
            first = o.failure;
        }
    }
    MESSAGE("E3 oracle (hole on every source): " << n << " stories");
    INFO(first);
    CHECK(failures == 0);
}

TEST_CASE("RealCoreOracleTest.aCoreOnlyRangeRecordEndsInCoreRefusedWithoutAReofferLoop")
{
    const int n = std::max(1, runs("CT_ORACLE_RUNS", 10) / 3);
    int failures = 0;
    std::string first;
    for (int i = 0; i < n; ++i)
    {
        const Outcome o = scenario(base() + 2000 + static_cast<std::uint64_t>(i), Kind::CORE_ONLY, workDir());
        if (!o.failure.empty() && failures++ == 0)
        {
            first = o.failure;
        }
    }
    MESSAGE("E3 oracle (core-only range record): " << n << " stories");
    INFO(first);
    CHECK(failures == 0);
}
