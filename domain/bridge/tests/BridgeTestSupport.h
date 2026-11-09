#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_TESTS_BRIDGE_TEST_SUPPORT_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_TESTS_BRIDGE_TEST_SUPPORT_H

// Test doubles for the bridge domain (preamble):
//   FakeEncoder  encodes {template, seq, epoch, egressSeq, flag} per record (flag = the record's LAST byte, so a
//                truncated staging copy and a poisoned copy are both visible to the stub core);
//   StubCore     a scriptable CorePort and an in-memory core with a dense cursor modelled on the real core's
//                admit (AE 1396-1460): binds on a 68 at seq 1 with meEpoch = genesisId, SettleRejected FEED_GAP /
//                FEED_UNBOUND / ME_EPOCH_MISMATCH like the core, decode poison latches SOURCE_SCHEMA with NO SettleRejected,
//                a 4.2 refusal latches SETTLE_REFUSED with a SettleRejected, a 62 applies entry by entry and stops
//                at the first refusal, FeedHalt answered per D3a, a v1/v2 phase that drops 68 and 66 and reports
//                0/0/NONE, an uncommitted tail lost at an election, and queries answered in log order;
//   SimSources SourceSet semantics (switchTo / reattach) over SimArchive followers, pure;
//   Rig          the frozen duty iteration of over all of the above, on a manual clock.

#include "SimArchive.h"
#include "TestSupport.h"

#include "cluster_tools/archive/FollowPolicy.h"
#include "cluster_tools/archive/FollowerLoop.h"
#include "cluster_tools/archive/ResumeMemo.h"
#include "cluster_tools/domain/bridge/BridgeDomain.h"
#include "cluster_tools/domain/bridge/BridgeMetrics.h"
#include "cluster_tools/domain/bridge/BridgeSettings.h"
#include "cluster_tools/domain/bridge/Ports.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ct_bridge_test {

using namespace cluster_tools::archive;
using namespace cluster_tools::domain::bridge;
using namespace ct_archive_test;
using namespace std::chrono_literals;

inline constexpr std::int64_t EPOCH = 5;
inline constexpr std::uint8_t FLAG_POISON = 0xEE; // the stub core decode-poisons this copy (SOURCE_SCHEMA, no reject)
inline constexpr std::uint8_t FLAG_REFUSE = 0xDD; // the stub core refuses this copy (SETTLE_REFUSED + SettleRejected)

// ------------------------------------------------------------------ the fake encoder

struct FakeRecord
{
    std::int64_t seq = 0;
    std::int64_t epoch = 0;
    std::int64_t egress = 0;
    std::uint8_t flag = 0;
    std::uint16_t journalTemplate = 0;
};

struct FakeFrame
{
    std::uint16_t moneyTemplate = 0;
    std::vector<FakeRecord> records;
    // 66
    std::int64_t correlationId = 0;
    HaltReason reason = HaltReason::NONE;
    std::int64_t meEpochSeen = 0, expected = 0, seen = 0, identity = 0;
};

inline void le64(std::uint8_t *at, std::int64_t v)
{
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
    {
        at[i] = static_cast<std::uint8_t>(u >> (8 * i));
    }
}

inline std::int64_t rd64(const std::uint8_t *at)
{
    std::uint64_t u = 0;
    for (int i = 7; i >= 0; --i)
    {
        u = (u << 8) | at[i];
    }
    return static_cast<std::int64_t>(u);
}

inline FakeFrame decodeFake(const std::uint8_t *f, std::size_t n)
{
    FakeFrame out;
    out.moneyTemplate = static_cast<std::uint16_t>(f[0] | (f[1] << 8));
    const std::size_t count = static_cast<std::size_t>(f[2] | (f[3] << 8));
    if (T_FEED_HALT == out.moneyTemplate)
    {
        out.correlationId = rd64(f + 8);
        out.reason = static_cast<HaltReason>(f[16]);
        out.meEpochSeen = rd64(f + 17);
        out.expected = rd64(f + 25);
        out.seen = rd64(f + 33);
        out.identity = rd64(f + 41);
        return out;
    }
    if (T_QUERY_FEED_POSITION == out.moneyTemplate)
    {
        out.correlationId = rd64(f + 8);
        return out;
    }
    for (std::size_t i = 0; i < count && 8 + 32 * (i + 1) <= n; ++i)
    {
        const std::uint8_t *e = f + 8 + 32 * i;
        FakeRecord r;
        r.seq = rd64(e);
        r.epoch = rd64(e + 8);
        r.egress = rd64(e + 16);
        r.flag = e[24];
        r.journalTemplate = static_cast<std::uint16_t>(e[25] | (e[26] << 8));
        out.records.push_back(r);
    }
    return out;
}

struct FakeEncoder : FrameEncoder
{
    std::vector<Bytes> recordsSeen; // every record the domain handed over, byte for byte
    bool failNext = false;
    bool keepRecords = true; // the zero-allocation window turns the copying off

    static void putRecord(std::uint8_t *at, const std::uint8_t *record, std::size_t length)
    {
        std::memset(at, 0, 32);
        le64(at, readLe64(record + FRAME_OFFSET_JOURNAL_SEQ));
        le64(at + 8, readLe64(record + FRAME_OFFSET_ME_EPOCH));
        le64(at + 16, readLe64(record + FRAME_OFFSET_EGRESS_SEQ));
        at[24] = record[length - 1];
        const std::uint16_t t = readLe16(record + 2);
        at[25] = static_cast<std::uint8_t>(t & 0xff);
        at[26] = static_cast<std::uint8_t>(t >> 8);
    }

    std::size_t encodeRecord(const std::uint8_t *record, std::size_t length, std::uint8_t *out,
        std::size_t capacity) noexcept override
    {
        if (failNext || capacity < 40 || length < 32)
        {
            failNext = false;
            return 0;
        }
        if (keepRecords)
        {
            recordsSeen.emplace_back(record, record + length);
        }
        const std::uint16_t t = readLe16(record + 2);
        const std::uint16_t money = frameTemplateFor(t);
        std::memset(out, 0, 8);
        out[0] = static_cast<std::uint8_t>(money & 0xff);
        out[1] = static_cast<std::uint8_t>(money >> 8);
        out[2] = 1;
        putRecord(out + 8, record, length);
        return 40;
    }

    std::size_t encodeTradeBatch(const StagedTrade *trades, std::size_t n, std::uint8_t *out,
        std::size_t capacity) noexcept override
    {
        if (failNext || capacity < 8 + 32 * n)
        {
            failNext = false;
            return 0;
        }
        std::memset(out, 0, 8);
        out[0] = T_ME_JOURNAL_TRADE_BATCH;
        out[2] = static_cast<std::uint8_t>(n & 0xff);
        out[3] = static_cast<std::uint8_t>(n >> 8);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (keepRecords)
            {
                recordsSeen.emplace_back(trades[i].record, trades[i].record + TRADE_RECORD_LENGTH);
            }
            putRecord(out + 8 + 32 * i, trades[i].record, TRADE_RECORD_LENGTH);
        }
        return 8 + 32 * n;
    }

    std::size_t encodeFeedHalt(std::int64_t correlationId, HaltReason reason, std::int64_t meEpochSeen,
        std::int64_t expected, std::int64_t seen, std::int64_t identity, std::uint8_t *out,
        std::size_t capacity) noexcept override
    {
        if (capacity < 49)
        {
            return 0;
        }
        std::memset(out, 0, 49);
        out[0] = T_FEED_HALT;
        le64(out + 8, correlationId);
        out[16] = static_cast<std::uint8_t>(reason);
        le64(out + 17, meEpochSeen);
        le64(out + 25, expected);
        le64(out + 33, seen);
        le64(out + 41, identity);
        return 49;
    }

    std::size_t encodeQuery(std::int64_t correlationId, std::uint8_t *out, std::size_t capacity) noexcept override
    {
        if (capacity < 16)
        {
            return 0;
        }
        std::memset(out, 0, 16);
        out[0] = T_QUERY_FEED_POSITION;
        le64(out + 8, correlationId);
        return 16;
    }
};

// ------------------------------------------------------------------ the stub core

struct CoreEgress
{
    enum Kind { REPORT, REJECT, ACK } kind = REPORT;
    FeedPositionReport report;
    SettleRejected reject;
    AdminAck ack;
};

struct StubCore : CorePort
{
    // configuration
    std::int64_t genesisId = EPOCH;
    bool v3 = true;               // false: behaviour 1/2 (68 and 66 unhandled, reports 0/0/NONE)
    bool commitImmediately = true;
    bool silentDrop = false;      // a bound core that drops feed frames without a trace
    std::deque<OfferOutcome> script; // the outcome of the next calls (OK when empty)
    std::function<OfferOutcome(const FakeFrame &)> outcomeFor; // overrides the script when set
    std::set<std::int64_t> refuseSeqs;  // decode-poison these seqs whatever the copy (a core-only range rule)
    int rejectFeedHalts = 0; // answer the next n valid FeedHalts REJECTED
    std::function<void(StubCore &)> beforeReport; // e.g. another lane applies entries before this query is answered

    // state
    std::int64_t bound = 0;
    std::int64_t cursor = 0;
    std::int64_t consumePosition = 0; // P: the highest egressSeq applied (row 6)
    int rc1Applied = 0;               // behaviour 1/2: 61-64 settle on the path, the feed cursor stays 0
    std::uint8_t latch = 0;
    std::int64_t latchExpected = 0, latchSeen = 0, latchClearAt = 0;
    std::map<std::int64_t, std::uint64_t> digests;

    // logs
    std::vector<FakeFrame> offered;     // every frame whose offer returned OK, in offer order
    std::vector<FakeFrame> received;    // every frame the core applied (committed), in log order
    std::vector<std::int64_t> applied;  // every seq applied, in order
    std::vector<FakeFrame> feedHalts;
    int queries = 0, duplicates = 0, gapRejects = 0, unboundRejects = 0, epochRejects = 0, refusals = 0;
    int calls = 0, retries = 0, aboveCursorPlusOne = 0;
    std::deque<std::pair<std::size_t, FakeFrame>> pending; // uncommitted: (index into `offered`, frame)
    std::size_t applying = 0; // the offer index of the frame being applied
    std::function<void(std::size_t)> lossHook; // a feed frame (or its tail entries) that will never apply
    std::function<void(std::size_t, std::int64_t)> gapHook; // an entry above cursor + 1 reached the core
    std::deque<CoreEgress> egress;

    OfferOutcome tryOffer(const std::uint8_t *frame, std::size_t length) noexcept override
    {
        ++calls;
        FakeFrame f = decodeFake(frame, length);
        OfferOutcome o = OfferOutcome::OK;
        if (outcomeFor)
        {
            o = outcomeFor(f);
        }
        else if (!script.empty())
        {
            o = script.front();
            script.pop_front();
        }
        if (OfferOutcome::OK != o)
        {
            ++retries;
            return o;
        }
        offered.push_back(f);
        pending.emplace_back(offered.size() - 1, f);
        if (commitImmediately)
        {
            commit();
        }
        return o;
    }

    void commit()
    {
        while (!pending.empty())
        {
            const auto [index, f] = pending.front();
            pending.pop_front();
            applying = index;
            apply(f);
        }
    }
    void commitFirst(std::size_t n)
    {
        while (n-- > 0 && !pending.empty())
        {
            const auto [index, f] = pending.front();
            pending.pop_front();
            applying = index;
            apply(f);
        }
    }
    void elect() // the uncommitted tail is lost
    {
        for (const auto &[index, f] : pending)
        {
            if (lossHook && !f.records.empty())
            {
                lossHook(index);
            }
        }
        pending.clear();
    }
    void lost()
    {
        if (lossHook)
        {
            lossHook(applying);
        }
    }

    FeedPositionReport reportFor(std::int64_t corr) const
    {
        FeedPositionReport r;
        r.correlationId = corr;
        if (v3)
        {
            r.meEpoch = bound;
            r.lastAppliedJournalSeq = cursor;
            r.haltReasonRaw = latch;
            r.feedHaltedRaw = latch != 0 ? 1 : 0;
            r.feedHalted = latch != 0;
        }
        return r;
    }

    void reject(std::int64_t s, std::uint8_t reason, std::int64_t detail)
    {
        CoreEgress e;
        e.kind = CoreEgress::REJECT;
        e.reject.tradeId = s;
        e.reject.marketId = static_cast<std::int32_t>(s);
        e.reject.journalPosition = s;
        e.reject.reasonRaw = reason;
        e.reject.detail = detail;
        egress.push_back(e);
    }

    static int exitClass(std::uint8_t r)
    {
        return 1 == r ? 1 : (3 == r || 5 == r) ? 2 : (2 == r || 4 == r) ? 3 : 0;
    }

    std::int64_t latchSkip = 0;

    // AE latchDecision (5.1): a higher exit class replaces the slot; a skippable latch replaces a skip-0 one of its
    // class; otherwise the new latch is suppressed
    void setLatch(std::uint8_t reason, std::int64_t expected, std::int64_t seen, std::int64_t clearAt, std::int64_t skip = 0)
    {
        if (0 != latch)
        {
            const int next = exitClass(reason), current = exitClass(latch);
            const bool write = next > current || (next == current && skip != 0 && latchSkip == 0);
            if (!write)
            {
                return;
            }
        }
        latchSkip = skip;
        latch = reason;
        latchExpected = expected;
        latchSeen = seen;
        latchClearAt = clearAt;
    }

    void reachClear()
    {
        if (0 != latch && latchClearAt > 0 && cursor >= latchClearAt)
        {
            latch = 0;
            latchSkip = 0;
        }
    }

    static std::uint64_t digestOf(const FakeRecord &r, std::uint16_t moneyTemplate)
    {
        const std::uint16_t t = T_ME_JOURNAL_TRADE_BATCH == moneyTemplate ? T_ME_JOURNAL_TRADE : moneyTemplate;
        return (static_cast<std::uint64_t>(r.egress) * 1000003ULL) ^ (static_cast<std::uint64_t>(r.flag) << 56) ^ t;
    }

    // one feed entry; returns false when the entry stopped the frame (a refusal)
    bool admit(const FakeRecord &r, std::uint16_t moneyTemplate)
    {
        const std::int64_t s = r.seq, m = r.epoch;
        if (s < 1 || m <= 0)
        {
            setLatch(3, cursor + 1, s, cursor + 1);
            return false;
        }
        if (0 == bound)
        {
            if (1 == s && T_ME_ENGINE_CONFIG == moneyTemplate && m == genesisId && FLAG_POISON != r.flag)
            {
                bound = m;
                cursor = 1;
                consumePosition = std::max(consumePosition, r.egress);
                digests[1] = digestOf(r, moneyTemplate);
                applied.push_back(1);
                reachClear(); // AE commit(): the bind is a commit, and every commit reach-clears (S7 3.3)
                return true;
            }
            ++unboundRejects;
            reject(s, 8, 1);
            if (1 == s)
            {
                setLatch(4, 1, 1, 0);
            }
            else
            {
                setLatch(1, 1, s, s - 1); // Row 3: a hole at 1, reach-cleared at c >= seen - 1 like any GAP
            }
            return false;
        }
        if (m != bound)
        {
            ++epochRejects;
            reject(s, 7, bound);
            setLatch(2, cursor + 1, s, 0);
            return false;
        }
        if (s <= cursor)
        {
            auto it = digests.find(s);
            if (it != digests.end() && it->second != digestOf(r, moneyTemplate))
            {
                if (1 == s && T_ME_ENGINE_CONFIG == moneyTemplate)
                {
                    setLatch(2, cursor + 1, s, 0); // another FRESH under the bound epoch: genesis only (AE 1427-1431)
                }
                else
                {
                    setLatch(3, cursor + 1, s, cursor + 1);
                }
                return false;
            }
            ++duplicates;
            return true;
        }
        if (s > cursor + 1)
        {
            if (gapHook)
            {
                gapHook(applying, s);
            }
            ++gapRejects;
            ++aboveCursorPlusOne;
            reject(s, 6, cursor + 1);
            setLatch(1, cursor + 1, s, s - 1);
            return false;
        }
        if (r.egress < consumePosition)
        {
            setLatch(3, cursor + 1, s, cursor + 1); // Row 6: the ME rewound inside its epoch, skip 0, no reject
            return false;
        }
        if (FLAG_POISON == r.flag || refuseSeqs.count(s))
        {
            setLatch(3, s, s, s, s); // decode poison: no SettleRejected, skip s, reach-cleared by a valid copy
            return false;
        }
        if (FLAG_REFUSE == r.flag)
        {
            ++refusals;
            reject(s, 5, 0); // a 4.2 refusal of a trade: the real core answers TRADE_GAP 5 (OrderEngine 1158-1161)
            setLatch(5, s, s, s, s);
            return false;
        }
        cursor = s;
        consumePosition = std::max(consumePosition, r.egress);
        digests[s] = digestOf(r, moneyTemplate);
        applied.push_back(s);
        reachClear();
        return true;
    }

    void apply(const FakeFrame &f)
    {
        received.push_back(f);
        if (T_QUERY_FEED_POSITION == f.moneyTemplate)
        {
            ++queries;
            if (beforeReport)
            {
                beforeReport(*this);
            }
            CoreEgress e;
            e.kind = CoreEgress::REPORT;
            e.report = reportFor(f.correlationId);
            egress.push_back(e);
            return;
        }
        if (T_FEED_HALT == f.moneyTemplate)
        {
            feedHalts.push_back(f);
            if (!v3)
            {
                return; // unhandled: no AdminAck ever comes
            }
            CoreEgress e;
            e.kind = CoreEgress::ACK;
            e.ack.correlationId = f.correlationId;
            e.ack.refTemplateId = T_FEED_HALT;
            bool invalid = false;
            switch (f.reason)
            {
                case HaltReason::GAP: invalid = !(f.expected >= 1 && f.expected < f.seen); break;
                case HaltReason::EPOCH_MISMATCH: invalid = f.meEpochSeen == 0; break;
                case HaltReason::SOURCE_SCHEMA: invalid = f.expected < 1; break;
                case HaltReason::SOURCE_UNBOUND:
                case HaltReason::SETTLE_REFUSED: invalid = false; break;
                default: invalid = true; break;
            }
            if (invalid || rejectFeedHalts > 0)
            {
                if (!invalid)
                {
                    --rejectFeedHalts; // scripted: a core that refuses a valid FeedHalt (the 9.3 bug path)
                }
                e.ack.resultRaw = ADMIN_REJECTED;
            }
            else
            {
                const bool stale = HaltReason::GAP == f.reason ? cursor >= f.seen - 1
                    : HaltReason::EPOCH_MISMATCH == f.reason ? (bound == 0 || f.meEpochSeen == bound)
                    : cursor >= f.expected;
                const bool writes = 0 == latch || exitClass(static_cast<std::uint8_t>(f.reason)) > exitClass(latch);
                if (stale || latch == static_cast<std::uint8_t>(f.reason) || !writes ||
                    HaltReason::SOURCE_UNBOUND == f.reason || HaltReason::SETTLE_REFUSED == f.reason)
                {
                    e.ack.resultRaw = ADMIN_DUPLICATE;
                }
                else
                {
                    e.ack.resultRaw = ADMIN_ACCEPTED;
                    setLatch(static_cast<std::uint8_t>(f.reason), f.expected, f.seen,
                        HaltReason::GAP == f.reason ? f.seen - 1 : HaltReason::SOURCE_SCHEMA == f.reason ? f.expected : 0);
                }
            }
            egress.push_back(e);
            return;
        }
        if (!v3 || silentDrop)
        {
            if (!v3 && T_ME_ENGINE_CONFIG != f.moneyTemplate && !silentDrop)
            {
                ++rc1Applied; // behaviour 1/2: 61-64 apply on the path (no feed cursor, reports stay 0/0/NONE)
            }
            lost();
            return; // behaviour 1/2: 68 unhandled; a silent drop leaves no trace
        }
        for (const FakeRecord &r : f.records)
        {
            if (!admit(r, f.moneyTemplate))
            {
                lost();
                break; // a 62 applies entry by entry and stops at the first refusal
            }
        }
    }

    std::vector<std::int64_t> offeredSeqs() const
    {
        std::vector<std::int64_t> s;
        for (const auto &f : offered)
        {
            for (const auto &r : f.records)
            {
                s.push_back(r.seq);
            }
        }
        return s;
    }
    std::vector<std::uint16_t> offeredTemplates() const
    {
        std::vector<std::uint16_t> s;
        for (const auto &f : offered)
        {
            s.push_back(f.moneyTemplate);
        }
        return s;
    }
    int feedOffers() const
    {
        int n = 0;
        for (const auto &f : offered)
        {
            n += f.records.empty() ? 0 : 1;
        }
        return n;
    }
};

// ------------------------------------------------------------------ SourceSet semantics, pure

struct SimSources
{
    std::vector<std::unique_ptr<SimArchive>> archives;
    std::vector<std::unique_ptr<FollowPolicy>> policies;
    std::vector<std::unique_ptr<FollowerLoop<SimArchive>>> loops;
    std::size_t current = 0;
    std::optional<std::size_t> pending;
    int switches = 0, reattaches = 0;

    void build(std::size_t n, ResumeMemo &memo, ArchiveMetrics &metrics, FollowObserver *observer, FollowSettings fs)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            archives.push_back(std::make_unique<SimArchive>());
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            policies.push_back(std::make_unique<FollowPolicy>(i, fs, memo, &metrics, observer));
            loops.push_back(std::make_unique<FollowerLoop<SimArchive>>(*archives[i], *policies[i], &metrics));
        }
    }
    void start(TimePoint now, std::size_t first = 0)
    {
        current = first;
        policies[first]->attach(now);
    }
    int doWork(FrameSink &sink, int limit, TimePoint now)
    {
        const int w = loops[current]->doWork(sink, limit, now);
        if (pending && policies[current]->idle() && !policies[current]->wantsDisconnect())
        {
            current = *pending;
            pending.reset();
            policies[current]->attach(now);
        }
        return w;
    }
    void switchTo(std::size_t index, TimePoint now)
    {
        if (index == current && !pending)
        {
            return;
        }
        ++switches;
        pending = index;
        policies[current]->detach(now);
    }
    void reattach(TimePoint now)
    {
        ++reattaches;
        policies[current]->reattach(now);
    }
};

// ------------------------------------------------------------------ the rig: the frozen duty iteration

inline BridgeSettings testSettings(std::size_t sources)
{
    BridgeSettings s;
    s.sourceCount = sources;
    s.identity = 0xB1;
    s.correlationBase = 1000;
    s.ringCapacity = 4096;
    s.keyTableCapacity = 64;
    return s;
}

struct Rig
{
    BridgeSettings settings;
    ArchiveMetrics archiveMetrics;
    std::unique_ptr<ResumeMemo> memo;
    FakeEncoder encoder;
    StubCore core;
    BridgeMetrics metrics;
    std::unique_ptr<BridgeDomain> domain;
    SimSources sources;
    TimePoint now{};
    bool deliverEgress = true;
    std::function<void(const CoreEgress &)> egressTap;
    std::function<bool(const CoreEgress &)> egressFilter; // false: the event is lost (a leader change)
    std::vector<SourceCommand> commands;

    explicit Rig(std::size_t n, BridgeSettings s = BridgeSettings{}, bool useDefaults = true,
        FollowSettings fs = FollowSettings{})
    {
        settings = useDefaults ? testSettings(n) : s;
        settings.sourceCount = n;
        memo = std::make_unique<ResumeMemo>(n, &archiveMetrics);
        domain = std::make_unique<BridgeDomain>(settings, *memo, archiveMetrics, encoder, core, metrics);
        fs.backoff = 100ms;
        sources.build(n, *memo, archiveMetrics, domain.get(), fs);
    }

    SimArchive &archive(std::size_t i) { return *sources.archives[i]; }

    // An egress latency (2026-10-31 review). With lag > 0 every event the core emitted reaches the domain
    // `lag` duty iterations later (a Raft commit plus the egress hop), never before the next poll
    int lag = 0;
    long stepNo = 0;
    std::deque<std::pair<long, CoreEgress>> lagged;

    void pumpEgress()
    {
        if (lag > 0)
        {
            while (!core.egress.empty())
            {
                lagged.emplace_back(stepNo, core.egress.front());
                core.egress.pop_front();
            }
            while (deliverEgress && !lagged.empty() && lagged.front().first + lag <= stepNo)
            {
                const CoreEgress e = lagged.front().second;
                lagged.pop_front();
                deliver(e);
            }
            return;
        }
        while (deliverEgress && !core.egress.empty())
        {
            const CoreEgress e = core.egress.front();
            core.egress.pop_front();
            if (egressTap)
            {
                egressTap(e);
            }
            if (egressFilter && !egressFilter(e))
            {
                continue;
            }
            switch (e.kind)
            {
                case CoreEgress::REPORT: domain->onReport(e.report, now); break;
                case CoreEgress::REJECT: domain->onSettleRejected(e.reject, now); break;
                case CoreEgress::ACK: domain->onAdminAck(e.ack, now); break;
            }
        }
    }

    void deliver(const CoreEgress &e)
    {
        if (egressTap)
        {
            egressTap(e);
        }
        if (egressFilter && !egressFilter(e))
        {
            return;
        }
        switch (e.kind)
        {
            case CoreEgress::REPORT: domain->onReport(e.report, now); break;
            case CoreEgress::REJECT: domain->onSettleRejected(e.reject, now); break;
            case CoreEgress::ACK: domain->onAdminAck(e.ack, now); break;
        }
    }

    void step(std::chrono::milliseconds advance = 10ms)
    {
        ++stepNo;
        now += advance;
        pumpEgress();
        const int limit = domain->fragmentLimit();
        const int n = sources.doWork(*domain, limit, now);
        domain->onPollDone(n, now);
        const SourceCommand cmd = domain->takeSourceCommand();
        if (SourceCommandKind::SWITCH == cmd.kind)
        {
            commands.push_back(cmd);
            sources.switchTo(cmd.index, now);
        }
        else if (SourceCommandKind::REATTACH == cmd.kind)
        {
            commands.push_back(cmd);
            sources.reattach(now);
        }
        domain->tick(now);
    }

    void up()
    {
        sources.start(now, 0);
        domain->onSessionUp(now);
    }

    template<typename Pred>
    bool runUntil(Pred pred, std::chrono::milliseconds limit = 120'000ms, std::chrono::milliseconds advance = 10ms)
    {
        const TimePoint end = now + limit;
        while (now < end)
        {
            if (pred())
            {
                return true;
            }
            step(advance);
        }
        return pred();
    }

    void runFor(std::chrono::milliseconds d, std::chrono::milliseconds advance = 10ms)
    {
        const TimePoint end = now + d;
        while (now < end)
        {
            step(advance);
        }
    }

    int feedHaltOffers() const
    {
        int n = 0;
        for (const auto &f : core.offered)
        {
            n += T_FEED_HALT == f.moneyTemplate ? 1 : 0;
        }
        return n;
    }

    int switchesTo(std::size_t i) const
    {
        int n = 0;
        for (const auto &c : commands)
        {
            n += (SourceCommandKind::SWITCH == c.kind && c.index == i) ? 1 : 0;
        }
        return n;
    }
};

// A journal of `n` records on one recording: seq 1 is a 68, then trades (or the given template).
inline void appendJournal(SimArchive &a, SimRecording &rec, std::int64_t from, std::int64_t to, std::int64_t epoch = EPOCH,
    std::uint16_t otherTemplate = TEMPLATE_TRADE)
{
    for (std::int64_t s = from; s <= to; ++s)
    {
        a.append(rec, makeFrame(1 == s ? TEMPLATE_CONFIG : otherTemplate, s, epoch, 64 * s));
    }
}

// A record whose LAST byte carries the stub core's flag.
inline Bytes flagged(std::uint16_t templateId, std::int64_t seq, std::int64_t epoch, std::uint8_t flag)
{
    Bytes b = makeFrame(templateId, seq, epoch, 64 * seq);
    b.back() = flag;
    return b;
}

// ------------------------------------------------------------------ direct driving (no follower)

struct Direct
{
    BridgeSettings settings;
    ArchiveMetrics archiveMetrics;
    std::unique_ptr<ResumeMemo> memo;
    FakeEncoder encoder;
    StubCore core;
    BridgeMetrics metrics;
    std::unique_ptr<BridgeDomain> domain;
    TimePoint now{};
    std::vector<Bytes> keep; // frame bytes must stay valid during onFrame only, kept for simplicity

    explicit Direct(std::size_t n = 1, std::function<void(BridgeSettings &)> tweak = {})
    {
        settings = testSettings(n);
        settings.behindDwell = std::chrono::hours(1); // no follower drives a Direct domain: no BEHIND unless asked
        if (tweak)
        {
            tweak(settings);
        }
        memo = std::make_unique<ResumeMemo>(n, &archiveMetrics);
        domain = std::make_unique<BridgeDomain>(settings, *memo, archiveMetrics, encoder, core, metrics);
    }

    void pump()
    {
        while (!core.egress.empty())
        {
            const CoreEgress e = core.egress.front();
            core.egress.pop_front();
            switch (e.kind)
            {
                case CoreEgress::REPORT: domain->onReport(e.report, now); break;
                case CoreEgress::REJECT: domain->onSettleRejected(e.reject, now); break;
                case CoreEgress::ACK: domain->onAdminAck(e.ack, now); break;
            }
        }
    }

    // session up + the first report, attached to `source` on recording `id`
    void start(std::size_t source = 0, std::int64_t recordingId = 0)
    {
        domain->onSessionUp(now);
        pump();
        attach(source, recordingId);
        domain->tick(now);
    }

    void attach(std::size_t source, std::int64_t recordingId = 0, bool memoFree = true)
    {
        AttachInfo info;
        info.source = source;
        info.recording = active(recordingId, 0);
        info.memoFree = memoFree;
        domain->onAttached(info);
    }

    SinkAction frame(const Bytes &bytes, std::size_t source = 0, std::int64_t recordingId = 0, bool memoFree = true,
        std::int64_t endPosition = 0)
    {
        keep.push_back(bytes);
        const Bytes &b = keep.back();
        const Admission a = admitFrame(FRAME_FLAGS_UNFRAGMENTED, b.data(), b.size());
        JournalFrame f;
        f.sourceIndex = source;
        f.key = key(recordingId);
        f.endPosition = 0 != endPosition ? endPosition : 64 * a.peek.journalSeq;
        f.bytes = b.data();
        f.length = b.size();
        f.peek = a.peek;
        f.readWasMemoFree = memoFree;
        return domain->onFrame(f);
    }

    SinkAction rec(std::uint16_t templateId, std::int64_t seq, std::int64_t epoch = EPOCH, std::size_t source = 0,
        std::int64_t recordingId = 0)
    {
        return frame(makeFrame(templateId, seq, epoch, 64 * seq), source, recordingId);
    }

    void poll(int fragments)
    {
        (void)domain->fragmentLimit();
        domain->onPollDone(fragments, now);
    }

    void tick(std::chrono::milliseconds advance = 10ms)
    {
        now += advance;
        pump();
        (void)domain->fragmentLimit();
        domain->tick(now);
        pump();
    }

    // deliver exactly one egress event; false when there is none
    bool pumpOne()
    {
        if (core.egress.empty())
        {
            return false;
        }
        const CoreEgress e = core.egress.front();
        core.egress.pop_front();
        switch (e.kind)
        {
            case CoreEgress::REPORT: domain->onReport(e.report, now); break;
            case CoreEgress::REJECT: domain->onSettleRejected(e.reject, now); break;
            case CoreEgress::ACK: domain->onAdminAck(e.ack, now); break;
        }
        return true;
    }

    int feedHaltOffers() const
    {
        int n = 0;
        for (const auto &f : core.offered)
        {
            n += T_FEED_HALT == f.moneyTemplate ? 1 : 0;
        }
        return n;
    }
};

}

#endif
