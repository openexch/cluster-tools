// Bridge.e3_app: the E3 (RealCoreOracleTest, the real behaviour-3 core in `serve`) driven THROUGH the
// app wiring: BridgeAgent<ClusterClient<FakeTransport>, E3Sources> with the real BridgeCodec, serve behind the fake
// transport. Every app frame the agent offers (RegisterSession 80 included: serve answers it on the story's BRIDGE
// session 701, DUPLICATE; OPEN_BRIDGE is NOT used) is in flight until the "cluster" commits it to serve
// in order; egress reaches the agent 1..8 iterations later, stamped with the term; an election (only while a FRAME is
// uncommitted) drops the uncommitted tail and the undelivered egress and announces a NewLeaderEvent; a cold restart is
// a new agent and a new client over the SAME serve session (the archives outlive it), its registration DUPLICATE.
// Asserts per story: the final snapshotSha256 equals the story's clean hash, lastAppliedJournalSeq == N, the
// latch line `L false 0`, invariantsHold, feedDuplicateCount == 0 (one session), per-sync ascending offers.
// Non-vacuity over the run: lossy elections > 0, restarts > 0. RED: FENCE_OFF ("CORE STATE:"; REGISTRATION_GATE_OFF cannot
// fail here, see negative-controls-e3.txt).
// CT_BRIDGE_E3_APP_STORIES stories (ctest 100; 10^3 once on the build box).
//
// The SECOND-SESSION variant. A cold restart opens a NEW BRIDGE session on
// serve (OPEN_BRIDGE: the cluster's session open; the agent's own RegisterSession then answers DUPLICATE) under a new
// identity (serve has no session close, so the dead session stays registered: under the 4R rule a second live
// session of one identity is REJECTED DUPLICATE_IDENTITY, the deployment closes the old one first; a new identity is the
// Replace shape). The restart happens while a feed frame is in flight, and the dead session's uncommitted tail
// is NOT dropped: it is sequenced on the old session (ingress the leader had not yet read) 0..50 iterations AFTER the
// new session's first feed frame was sequenced (at the latest 400 iterations after the restart, or before the next
// round), so the core gets the same records from both sessions: duplicates across two sessions. Asserts per story: feedDuplicateMismatchCount == 0, the CanonicalDump sha equal to the clean model's (review
// Not snapshotSha256, whose session section differs with two sessions), cursor N, the latch line `L false 0`,
// invariantsHold. Non-vacuity: reopenings > 0 and duplicates > 0. RED: FENCE_OFF ("CORE STATE:"). The same-session
// variant above is unchanged (feedDuplicateCount == 0, the snapshot hash).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <sys/wait.h>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <filesystem>
#include <memory>
#include <random>

#include "AppRig.h"
#include "BridgeFeedFiles.h"
#include "ServeClient.h"

#include "cluster_tools/adapters/sbe/BridgeCodec.h"

using namespace ct_bridge_app_test;
using cluster_tools::sbe_adapter::bridge_codec::BridgeCodec;
namespace fs = std::filesystem;

namespace {

std::int64_t rd64le(const std::uint8_t *p)
{
    std::int64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

std::vector<std::int64_t> seqsOf(const std::uint8_t *f, std::size_t n)
{
    std::vector<std::int64_t> out;
    if (n < 8)
    {
        return out;
    }
    switch (rd16(f + 2))
    {
        case T_ME_JOURNAL_TRADE: out.push_back(rd64le(f + 8 + 101)); break;
        case T_ME_JOURNAL_TRADE_BATCH:
            for (std::size_t i = 0, c = rd16(f + 10); i < c; ++i)
            {
                out.push_back(rd64le(f + 12 + 109 * i + 101));
            }
            break;
        case T_ME_JOURNAL_TERMINAL: out.push_back(rd64le(f + 8 + 53)); break;
        case T_ME_COMMAND_OUTCOME: out.push_back(rd64le(f + 8 + 124)); break;
        case T_ME_COMMAND_RELEASE:
        case T_ME_ENGINE_CONFIG: out.push_back(rd64le(f + 8 + 8)); break;
        default: break;
    }
    return out;
}

// SourceSet semantics over archives that outlive one bridge process (a restart rebuilds the followers)
struct E3Sources
{
    std::vector<std::unique_ptr<SimArchive>> &archives;
    std::vector<std::unique_ptr<FollowPolicy>> policies;
    std::vector<std::unique_ptr<FollowerLoop<SimArchive>>> loops;
    std::size_t cur = 0;
    std::optional<std::size_t> pending;
    std::mt19937_64 &rng;

    E3Sources(std::vector<std::unique_ptr<SimArchive>> &a, ResumeMemo &memo, ArchiveMetrics &metrics, FollowObserver *observer,
        std::mt19937_64 &r) :
        archives(a), rng(r)
    {
        FollowSettings fs;
        fs.backoff = 100ms;
        fs.probeInterval = 1'000ms;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            policies.push_back(std::make_unique<FollowPolicy>(i, fs, memo, &metrics, observer));
            loops.push_back(std::make_unique<FollowerLoop<SimArchive>>(*a[i], *policies[i], &metrics));
        }
    }
    void start(TimePoint now, std::size_t first = 0)
    {
        cur = first;
        policies[first]->attach(now);
    }
    int doWork(FrameSink &sink, int limit, TimePoint now)
    {
        const int l = limit > 0 ? std::min(limit, static_cast<int>(1 + rng() % 8)) : 0; // a slow follower now and then
        const int w = loops[cur]->doWork(sink, l, now);
        if (pending && policies[cur]->idle() && !policies[cur]->wantsDisconnect())
        {
            cur = *pending;
            pending.reset();
            policies[cur]->attach(now);
        }
        return w;
    }
    void switchTo(std::size_t index, TimePoint now)
    {
        if (index == cur && !pending)
        {
            return;
        }
        pending = index;
        policies[cur]->detach(now);
    }
    void reattach(TimePoint now) { policies[cur]->reattach(now); }
    void stop(TimePoint now)
    {
        pending.reset();
        policies[cur]->detach(now);
    }
    std::size_t current() const { return cur; }
    bool currentIdle() const { return policies[cur]->idle(); }
};

using Client = cl::ClusterClient<FakeTransport, app::LinkObserver>;
using Agent = app::BridgeAgent<Client, E3Sources>;

// one bridge process: its client, memo, followers, tap and agent
struct Process
{
    app::LinkObserver observer;
    std::unique_ptr<Client> client;
    ArchiveMetrics metrics;
    std::unique_ptr<ResumeMemo> memo;
    app::ObserverTap tap;
    std::unique_ptr<E3Sources> sources;
    BridgeCodec codec;
    std::unique_ptr<Agent> agent;

    Process(FakeTransport &t, std::vector<std::unique_ptr<SimArchive>> &archives, std::int64_t identity, std::mt19937_64 &rng)
    {
        client = std::make_unique<Client>(t, AppRig::clientSettings(), &observer);
        memo = std::make_unique<ResumeMemo>(archives.size(), &metrics);
        sources = std::make_unique<E3Sources>(archives, *memo, metrics, &tap, rng);
        app::AgentSettings s;
        s.identity = identity;
        s.sourceCount = archives.size();
        s.probeInterval = 1'000ms;
        s.registrarResendNs = 2 * SECOND;
        s.tweakDomain = [](BridgeSettings &d) {
            d.ringCapacity = 4096;
            d.reportInterval = 200ms;
            d.reportIntervalHalted = 200ms;
            d.queryResend = 50ms;
            d.reportTimeout = 500ms;
            d.invalidReportResend = 500ms;
            d.feedHaltResend = 500ms;
            d.haltRescan = 3'000ms;
            d.behindDwell = 1'000ms;
            d.behindRescan = 6'000ms;
            d.holeGrace = 6'000ms;
            d.unreachableRetry = 1'000ms;
            d.probeInterval = 1'000ms;
        };
        agent = std::make_unique<Agent>(*client, *sources, tap, *memo, metrics, codec, s);
        observer.target = agent.get();
        agent->start();
    }
    ~Process()
    {
        observer.target = nullptr;
        tap.bind(nullptr);
    }
};

struct Outcome
{
    std::string failure;
    int restarts = 0, elections = 0, lostFrames = 0, registrations = 0;
    int reopenings = 0, lateTailFrames = 0; // the second-session variant
    std::int64_t duplicates = 0;
};

Outcome story(std::uint64_t seed, const std::string &work, bool secondSession = false)
{
    Outcome out;
    auto fail = [&](const std::string &why) {
        out.failure = "seed " + std::to_string(seed) + ": " + why;
        return out;
    };
    auto coreFail = [&](const std::string &why) { return fail("CORE STATE: " + why); };
    std::mt19937_64 rng(seed);
    const std::string dir = work + "/story-" + std::to_string(seed);
    fs::remove_all(dir);
    const std::string java = CT_JAVA, jar = CT_FEED_HARNESS_JAR;
    const std::string cmd = "'" + java + "' -jar '" + jar + "' story " + std::to_string(seed) + " 10 '" + dir + "' 2> '" + dir +
        ".story.log'";
    const int rc = std::system(cmd.c_str());
    if (!(WIFEXITED(rc) && 0 == WEXITSTATUS(rc)))
    {
        return fail("story generation failed");
    }
    std::vector<std::vector<ct_oracle::Bytes>> rounds;
    {
        using namespace ct_bridge_codec_test;
        std::vector<ct_oracle::Bytes> cur;
        for (const FjrEntry &e : decodeFjr(ct_bridge_codec_test::readFile(dir + "/journal.fjr")))
        {
            if (e.flush)
            {
                rounds.push_back(cur);
                cur.clear();
            }
            else
            {
                cur.push_back(e.record);
            }
        }
    }
    ct_oracle::ServeClient serve(java, jar, dir + ".serve.log");
    const ct_oracle::Ready ready = serve.boot(dir);
    const std::size_t nSources = 1 + static_cast<std::size_t>(rng() % 3);
    std::vector<std::unique_ptr<SimArchive>> archives;
    std::vector<SimRecording *> active;
    for (std::size_t i = 0; i < nSources; ++i)
    {
        archives.push_back(std::make_unique<SimArchive>());
        active.push_back(&archives.back()->record());
    }
    FakeTransport t;
    std::int64_t term = 3, clientSession = 900;
    std::int32_t image = 900, leader = 0;
    std::set<std::int64_t> answered;
    std::int64_t session = ready.bridgeSession, identity = ready.identity; // the core session the agent's frames land on
    std::int64_t nextSession = ready.bridgeSession + 1000;
    auto process = std::make_unique<Process>(t, archives, identity, rng);
    std::deque<ct_oracle::Bytes> inflight;           // offered, not committed
    std::deque<ct_oracle::Bytes> oldTail;            // second-session variant: the dead session's unsequenced tail
    std::int64_t oldTailSession = 0;
    int oldTailAt = -1;
    bool tailWaitsForNewFeed = false; // the tail is sequenced after the new session's first feed frame
    bool restartPending = false;
    std::deque<std::pair<long, ct_oracle::Bytes>> egress; // committed answers, by iteration
    std::map<std::size_t, std::size_t> scanned;
    const int lag = 1 + static_cast<int>(rng() % 8);
    std::uint64_t offerSync = ~0ULL;
    std::int64_t nextInSync = 0;
    std::string offerFailure;
    bool electionPending = false;
    std::size_t nextRound = 0;
    auto commit = [&](long it) {
        while (!inflight.empty())
        {
            const ct_oracle::Bytes f = inflight.front();
            inflight.pop_front();
            const std::uint16_t tid = rd16(f.data() + 2);
            if (isRegisterSession(f.data(), f.size()))
            {
                out.registrations++;
                for (auto &e : serve.frame(session, f.data(), f.size()))
                {
                    egress.emplace_back(it, std::move(e));
                }
            }
            else if (T_QUERY_FEED_POSITION == tid)
            {
                egress.emplace_back(it, serve.query(session, rd64le(f.data() + 8)));
            }
            else
            {
                for (auto &e : serve.frame(session, f.data(), f.size()))
                {
                    egress.emplace_back(it, std::move(e));
                }
                if (tailWaitsForNewFeed && !oldTail.empty())
                {
                    tailWaitsForNewFeed = false;
                    oldTailAt = static_cast<int>(it) + static_cast<int>(rng() % 51);
                }
            }
        }
    };
    // the dead session's tail, sequenced late on its own session; its egress goes to a session nobody reads
    auto flushOldTail = [&]() {
        while (!oldTail.empty())
        {
            const ct_oracle::Bytes f = oldTail.front();
            oldTail.pop_front();
            if (T_QUERY_FEED_POSITION == rd16(f.data() + 2))
            {
                (void)serve.query(oldTailSession, rd64le(f.data() + 8));
            }
            else
            {
                out.lateTailFrames += isRegisterSession(f.data(), f.size()) ? 0 : 1;
                (void)serve.frame(oldTailSession, f.data(), f.size());
            }
        }
        oldTailAt = -1;
        tailWaitsForNewFeed = false;
    };
    auto deliver = [&](long it, bool all) {
        while (!egress.empty() && (all || egress.front().first + lag <= it))
        {
            t.pushAppMessage(clientSession, egress.front().second, image, term);
            egress.pop_front();
        }
    };
    const int maxIterations = 80'000;
    for (int it = 0; it < maxIterations; ++it)
    {
        t.now += 10 * MS;
        const std::int64_t c = t.lastConnectCorrelation();
        if (c > 0 && answered.insert(c).second)
        {
            ++clientSession;
            t.pushSessionEvent(c, clientSession, term, leader, cc::EventCode::OK, "", image);
        }
        // an election, only while a FRAME is uncommitted: the tail and the undelivered egress are lost
        if (!electionPending && rng() % 150 == 0)
        {
            electionPending = true;
        }
        const bool frameInFlight = std::any_of(inflight.begin(), inflight.end(), [](const ct_oracle::Bytes &f) {
            return !isRegisterSession(f.data(), f.size()) && T_QUERY_FEED_POSITION != rd16(f.data() + 2);
        });
        if (electionPending && frameInFlight)
        {
            electionPending = false;
            ++out.elections;
            for (const auto &f : inflight)
            {
                out.lostFrames += (!isRegisterSession(f.data(), f.size()) && T_QUERY_FEED_POSITION != rd16(f.data() + 2)) ? 1 : 0;
            }
            inflight.clear();
            egress.clear();
            for (const auto &f : oldTail)
            {
                out.lostFrames += (!isRegisterSession(f.data(), f.size()) && T_QUERY_FEED_POSITION != rd16(f.data() + 2)) ? 1 : 0;
            }
            oldTail.clear(); // still unsequenced: lost with the term
            oldTailAt = -1;
            ++term;
            leader = (leader + 1) % 3;
            t.pushNewLeader(clientSession, term, leader, CORE_ENDPOINTS, image);
        }
        if (it == oldTailAt)
        {
            flushOldTail();
        }
        if (rng() % 10 < 6)
        {
            commit(it);
        }
        // a cold restart: a new client and agent over the same serve session (registration DUPLICATE); in the
        // second-session variant over a NEW serve session, the old session's tail sequenced later
        bool restartNow = rng() % 1500 == 0;
        if (secondSession)
        {
            // the variant restarts only while a feed frame is in flight, so the dead session leaves a tail
            restartPending = restartPending || restartNow;
            restartNow = restartPending && std::any_of(inflight.begin(), inflight.end(), [](const ct_oracle::Bytes &f) {
                return !isRegisterSession(f.data(), f.size()) && T_QUERY_FEED_POSITION != rd16(f.data() + 2);
            }); // after this iteration's commit: what is still in flight now
        }
        if (restartNow)
        {
            restartPending = false;
            ++out.restarts;
            if (!secondSession)
            {
                commit(it);
                deliver(it, true);
            }
            process->agent->requestStop();
            process->agent->doWork();
            if (secondSession)
            {
                flushOldTail(); // an earlier dead session's tail is older than this one's
                oldTail.clear();
                for (const auto &f : inflight)
                {
                    oldTail.push_back(f);
                }
                oldTailSession = session;
                oldTailAt = it + 400; // the bound; the first new feed frame brings it forward
                tailWaitsForNewFeed = true;
                inflight.clear();
                egress.clear(); // addressed to the dead session
                session = nextSession++;
                identity = ready.identity + out.restarts;
                (void)serve.openBridge(session, identity);
                ++out.reopenings;
            }
            else
            {
                inflight.clear(); // the old session's close and keepalives carry nothing for serve
            }
            process.reset();
            process = std::make_unique<Process>(t, archives, identity, rng);
            offerSync = ~0ULL; // a new domain counts its syncs from 0 again: the next offer opens a new sync
        }
        deliver(it, false);
        process->agent->doWork();
        // what the agent offered: in flight until the commit; per-sync ascending offers
        for (std::size_t i = 0; i < t.publications.size(); ++i)
        {
            auto &offered = t.publications[i].offered;
            for (std::size_t &seen = scanned[i]; seen < offered.size(); ++seen)
            {
                const auto &fr = offered[seen];
                if (fr.size() < 40 || cc::SessionMessageHeader::sbeTemplateId() != rd16(fr.data() + 2))
                {
                    continue;
                }
                std::int64_t frameTerm;
                std::memcpy(&frameTerm, fr.data() + 8, 8);
                if (frameTerm != term)
                {
                    continue; // dropped by the consensus module
                }
                ct_oracle::Bytes payload(fr.begin() + 32, fr.end());
                const std::vector<std::int64_t> seqs = seqsOf(payload.data(), payload.size());
                const auto &dom = process->agent->domain();
                if (!seqs.empty())
                {
                    if (dom.syncs() != offerSync)
                    {
                        offerSync = dom.syncs();
                        nextInSync = dom.syncCursor() + 1;
                    }
                    for (std::int64_t s : seqs)
                    {
                        if (s != nextInSync && offerFailure.empty())
                        {
                            offerFailure = "sync from " + std::to_string(dom.syncCursor()) + " offered " + std::to_string(s) +
                                " where " + std::to_string(nextInSync) + " was due";
                        }
                        nextInSync = s + 1;
                    }
                }
                inflight.push_back(std::move(payload));
            }
        }
        if (process->agent->stopped())
        {
            return fail(std::string("the agent stopped: ") + process->agent->stopReason().code + " " +
                process->agent->stopReason().detail);
        }
        const BridgeView v = process->agent->domain().view();
        if (nextRound < rounds.size())
        {
            const bool barrier = 0 == nextRound || v.cursor >= ready.barriers[nextRound - 1];
            if (barrier)
            {
                flushOldTail(); // a round is written only once everything offered before it is sequenced
                commit(it);
                deliver(it, true);
                (void)serve.round(static_cast<std::uint32_t>(nextRound));
                for (const ct_oracle::Bytes &rec : rounds[nextRound])
                {
                    for (std::size_t s = 0; s < nSources; ++s)
                    {
                        archives[s]->append(*active[s], rec);
                    }
                }
                ++nextRound;
            }
        }
        if (nextRound == rounds.size() && v.cursor == ready.n && v.last == ready.n && v.staged == 0 &&
            v.halt == BridgeHalt::NONE)
        {
            break;
        }
    }
    flushOldTail();
    commit(maxIterations);
    const std::string result = serve.finish();
    const ct_oracle::ServeClient::Dump dump = secondSession ? serve.dump(ready.n) : serve.dump();
    if (ct_oracle::jsonField(result, "lastAppliedJournalSeq") != std::to_string(ready.n))
    {
        return coreFail("lastAppliedJournalSeq " + ct_oracle::jsonField(result, "lastAppliedJournalSeq") + " of " +
            std::to_string(ready.n));
    }
    if (secondSession)
    {
        // Two registered sessions change the snapshot's session section; the money state is the dump
        if (dump.liveSha != dump.modelSha)
        {
            return coreFail("the CanonicalDump sha " + dump.liveSha.substr(0, 12) + " differs from the clean model's " +
                dump.modelSha.substr(0, 12));
        }
        if (ct_oracle::jsonField(result, "feedDuplicateMismatchCount") != "0")
        {
            return coreFail("feedDuplicateMismatchCount " + ct_oracle::jsonField(result, "feedDuplicateMismatchCount"));
        }
    }
    else if (ct_oracle::jsonField(result, "snapshotSha256") != ready.cleanSha)
    {
        return coreFail("snapshotSha256 differs from the clean run's");
    }
    if (dump.latchLine() != "L false 0")
    {
        return coreFail("the latch line is '" + dump.latchLine() + "'");
    }
    if (ct_oracle::jsonField(result, "invariantsHold") != "true")
    {
        return coreFail("invariantsHold false");
    }
    out.duplicates = ct_oracle::duplicateCount(result);
    if (!secondSession && out.duplicates != 0)
    {
        return coreFail("feedDuplicateCount " + std::to_string(out.duplicates));
    }
    if (!offerFailure.empty())
    {
        return fail(offerFailure);
    }
    fs::remove_all(dir);
    return out;
}

}

TEST_CASE("BridgeE3AppTest.theRealCoreEndsInTheCleanStateThroughTheAppWiring")
{
    const char *env = std::getenv("CT_BRIDGE_E3_APP_STORIES");
    const int n = nullptr == env ? 100 : std::atoi(env);
    const fs::path work = fs::temp_directory_path() / "ct-bridge-e3-app";
    fs::create_directories(work);
    int failures = 0, restarts = 0, elections = 0, lost = 0, registrations = 0;
    std::string first;
    for (int i = 0; i < n; ++i)
    {
        const Outcome o = story(0xe3a9ULL + static_cast<std::uint64_t>(i), work.string());
        restarts += o.restarts;
        elections += o.elections;
        lost += o.lostFrames;
        registrations += o.registrations;
        if (!o.failure.empty() && failures++ < 3)
        {
            MESSAGE(o.failure);
            if (first.empty())
            {
                first = o.failure;
            }
        }
    }
    MESSAGE("E3 through the app: " << n << " stories, restarts " << restarts << ", elections " << elections << " (lost frames "
        << lost << "), RegisterSession frames through serve " << registrations << ", failures " << failures);
    INFO(first);
    const bool clean = 0 == failures;
    CHECK(clean); // FENCE_OFF / REGISTRATION_GATE_OFF RED: "CORE STATE:"
    CHECK(lost > 0);
    CHECK(restarts > 0);
    CHECK(registrations > n); // the app's registrar registered through FRAME (one per session, plus re-registrations)
}

TEST_CASE("BridgeE3AppTest.aSecondSessionAfterARestartEndsInTheCleanDump")
{
    // The restarted bridge on a NEW core session, the dead session's tail sequenced late
    const char *env = std::getenv("CT_BRIDGE_E3_APP_STORIES");
    const int n = nullptr == env ? 100 : std::atoi(env);
    const fs::path work = fs::temp_directory_path() / "ct-bridge-e3-app-2s";
    fs::create_directories(work);
    int failures = 0, restarts = 0, reopenings = 0, elections = 0, lost = 0, late = 0;
    std::int64_t duplicates = 0;
    std::string first;
    for (int i = 0; i < n; ++i)
    {
        const Outcome o = story(0xe3a9ULL + 0x10000ULL + static_cast<std::uint64_t>(i), work.string(), true);
        restarts += o.restarts;
        reopenings += o.reopenings;
        elections += o.elections;
        lost += o.lostFrames;
        late += o.lateTailFrames;
        duplicates += o.duplicates;
        if (!o.failure.empty() && failures++ < 3)
        {
            MESSAGE(o.failure);
            if (first.empty())
            {
                first = o.failure;
            }
        }
    }
    MESSAGE("E3 second session: " << n << " stories, restarts " << restarts << " = new sessions " << reopenings
        << ", late dead-session tail frames " << late << ", feed duplicates " << duplicates << ", elections " << elections
        << " (lost frames " << lost << "), failures " << failures);
    INFO(first);
    const bool clean = 0 == failures;
    CHECK(clean); // FENCE_OFF RED: "CORE STATE:"
    CHECK(reopenings > 0);
    CHECK(duplicates > 0); // two sessions over one story must meet each other's records (0 = the tail never overlapped)
    CHECK(lost > 0);
}
