// RC2 S12 contract 11.1 ResyncPropertyTest (critic S12 RED "F3 property test including backlog entries").
//
// A seeded core model mints CREATE, CANCEL, AMEND, RELEASE_ID (UP_TO and ORDER) and orphan CANCEL
// dispatches, a few anomalous ones of every class (I, C, RA, RX, unroutable), resolves random pending ones,
// and answers RequestPendingDispatch per S8 3.8 (backlog entries included, the minted bytes replayed) with
// the reply placed in the per-term FIFO egress queue of the forwarder's session, interleaved with live
// broadcasts by apply. The forwarder side is the real ResyncMachine + SendRing + AdmissionWatcher +
// classify(), wired the way ForwarderAgent wires them (rule S, ORIGIN = the AdmissionState after the
// AdminAck, C3 entry = T1, exits = D0); the ME is a sink that refuses CAPACITY-prone entries while the
// core's CAPACITY bit is set.
//
// Faults: a core leader change (the old image's undelivered suffix lost, the surviving prefix delivered
// interleaved with the new term; the dispatch mask lost until re-registration; old-term ingress dropped);
// stale old-term frames between new-term reply entries (FIX-1); a session close per A-2 (a partial reply,
// a lost AdmissionState, a discard: the queue's suffix lost); a 108 delayed past T9 so both replies arrive;
// a lost request (no 108 at all); CAPACITY'd entries kept pending until the gate opens (T3); ring stalls
// (ME back-pressure); bursts that overflow hold or ring; forwarder restarts (a fresh machine).
//
// Properties at quiescence: (P1) every pending, routable seq was pushed after the last RESYNC(0) of the
// current instance; (P2) pushes strictly ascending between ring clears and triggers; (P3) no pushed seq was
// never minted; (P4) every push is the 5.5 form of the minted dispatch; (P5) hold and ring within their
// fixed buffers; (P6) liveness: quiescence is reached without any resolved entry being re-delivered by the
// core; (P7) no stale-term entry reaches the machine; (P8) every pending entry was accepted by the ME after
// its last CAPACITY refusal. A failing seed prints the seed and the event trace.
#include "doctest/doctest.h"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "cluster_tools/domain/forwarder/AdmissionWatcher.h"
#include "cluster_tools/domain/forwarder/ResyncMachine.h"
#include "cluster_tools/domain/forwarder/TestSeams.h"

using namespace cluster_tools::domain::forwarder;

namespace {

constexpr std::int64_t STEP_NS = 50'000'000LL; // 50 ms of model time per step
constexpr std::int64_t STALE_TAG = 1LL << 50;    // P7: marks a stale-term copy (review FIX-4)

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
};

enum class FrameKind : std::uint8_t
{
    DISPATCH,
    END,
    ADMIN_ACK,
    ADMISSION,
    NEW_LEADER,
    CLOSED,
};

struct Frame
{
    FrameKind kind = FrameKind::DISPATCH;
    std::int64_t term = 0;
    std::uint64_t session = 0;
    Dispatch dispatch;
    std::int64_t correlationId = 0;
    std::int32_t count = 0;
    std::int64_t head = 0;
    std::uint8_t gateState = 0;
    std::uint32_t gateReasons = 0;
    std::int64_t releaseAt = 0; // old-image frames: not before (the deposed leader's image drains late)
};

struct Ingress
{
    bool registerSession = false;
    std::int64_t term = 0;
    std::uint64_t session = 0;
    PendingRequest request;
};

constexpr std::uint32_t BIT_CAPACITY = 1u << 4;
constexpr std::uint32_t BIT_ADMIN = 1u << 0;

// What the forwarder pushed and the ME saw, per instance.
struct PushLog final : SendRingObserver
{
    std::map<std::int64_t, std::uint64_t> lastPushGeneration; // seq -> generation of its last push
    std::uint64_t generation = 0;
    std::int64_t lastPushed = 0;
    bool ascendingBroken = false;
    std::int64_t brokenAt = 0;
    std::uint64_t pushes = 0;
    bool pushedDuringStaleDelivery = false;
    bool inStaleDelivery = false;
    std::vector<RingSlot> pushed; // only for P3/P4 (checked as they arrive by the model)

    void onPush(const RingSlot &slot) override
    {
        pushes++;
        if (slot.seq >= 1)
        {
            if (slot.seq <= lastPushed)
            {
                ascendingBroken = true;
                brokenAt = slot.seq;
            }
            lastPushed = slot.seq;
        }
        lastPushGeneration[slot.seq] = generation;
        if (inStaleDelivery)
        {
            pushedDuringStaleDelivery = true;
        }
        pushed.push_back(slot);
    }
    void onClear(std::size_t) override
    {
        generation++;
        lastPushed = 0;
    }
    void onTriggerBoundary(std::int64_t cursor) { lastPushed = cursor; }
};

struct Run
{
    explicit Run(std::uint64_t seed) : seed(seed), rng(seed) {}

    std::uint64_t seed;
    Rng rng;
    std::int64_t now = 1'000'000'000'000LL;
    std::vector<std::string> trace;

    // ---- core model
    std::int64_t dispatchSeq = 0;
    std::map<std::int64_t, Dispatch> minted;
    std::map<std::int64_t, Dispatch> pending;
    std::set<std::int64_t> capacityProne;
    std::uint32_t gateReasons = BIT_ADMIN; // genesis: SUSPENDED/ADMIN until opened below
    std::int64_t coreTerm = 1;
    std::uint64_t coreSession = 1;
    bool registered = false; // the dispatch mask of this session on this leader
    std::deque<Frame> egress;   // the current leader's FIFO queue for the forwarder's session
    std::deque<Frame> oldEgress; // the surviving prefix of the previous leader's image
    std::deque<Ingress> ingress;
    bool ingressStalled = false;
    bool dropNextBroadcast = false;
    bool loseNextRequest = false;
    std::int64_t lastMintNs = 0;
    std::int64_t ingressStallUntil = 0;
    std::int64_t nextOrderId = 1;

    // ---- forwarder stand-in (what ForwarderAgent does around the domain)
    std::unique_ptr<SendRing> ring;
    std::unique_ptr<ResyncMachine> machine;
    std::unique_ptr<PushLog> log;
    AdmissionWatcher watcher;
    std::int64_t clientTerm = 1;
    std::uint64_t clientSession = 1;
    bool c3 = false;
    bool expectOrigin = false;
    bool sessionClosedSeen = false;
    std::uint64_t staleDropped = 0;
    std::uint64_t staleReachedMachine = 0;
    bool ringStalled = false;
    std::int64_t ringStallUntil = 0;
    std::uint64_t instances = 0;

    // ---- the ME (review FIX-4): offers land in the current ME term's uncommitted tail; a commit makes them accepted
    // (or CAPACITY-refused); an ME leader change truncates the tail and the agent raises T2; an ME session close loses
    // the tail too, nothing is offered until the next session reaches M3, which raises T2.
    std::map<std::int64_t, std::int64_t> lastAccepted; // seq -> commit time
    std::map<std::int64_t, std::int64_t> lastRefused;
    std::vector<std::int64_t> meTail;
    std::int64_t lastMeLossNs = 0;
    bool meDown = false;
    std::int64_t meDownUntil = 0;

    // ---- statistics
    std::uint64_t stats[18] = {};

    std::string where() const
    {
        std::ostringstream o;
        o << "seed " << seed << " trace (last " << trace.size() << "):\n";
        for (const std::string &t : trace)
        {
            o << "  " << t << "\n";
        }
        return o.str();
    }
    void note(const std::string &event)
    {
        if (trace.size() >= 400)
        {
            trace.erase(trace.begin(), trace.begin() + 100);
        }
        std::ostringstream o;
        o << "t=" << (now / STEP_NS) << " " << event;
        trace.push_back(o.str());
    }

    // ------------------------------------------------------------------ core
    Dispatch mint()
    {
        Dispatch d;
        d.dispatchSeq = ++dispatchSeq;
        const std::uint64_t r = rng.below(100);
        const std::int64_t orderId = nextOrderId++;
        const std::int64_t attempt = static_cast<std::int64_t>(rng.below(5));
        if (r < 30)
        {
            d.kind = KIND_CREATE;
            d.timeInForce = static_cast<std::uint8_t>(rng.below(2));
        }
        else if (r < 50)
        {
            d.kind = KIND_CANCEL;
            d.oldOrderId = 900 + orderId;
        }
        else if (r < 62)
        {
            d.kind = KIND_AMEND;
            d.oldOrderId = 900 + orderId;
            d.expectedOldRemaining = 1 + static_cast<std::int64_t>(rng.below(9));
        }
        else if (r < 80)
        {
            d.kind = KIND_RELEASE_ID; // backlog entry (S8 3.1): UP_TO or ORDER
            d.releaseScope = static_cast<std::uint8_t>(rng.below(2));
        }
        else if (r < 88)
        {
            d.kind = KIND_CANCEL; // orphan cancel (S8 C4): price/qty/budget/tif/eor 0
            d.oldOrderId = 900 + orderId;
        }
        else
        {
            d.kind = static_cast<std::uint8_t>(rng.below(3));
            if (d.kind != KIND_CREATE)
            {
                d.oldOrderId = 900 + orderId;
            }
            if (KIND_AMEND == d.kind)
            {
                d.expectedOldRemaining = 2;
            }
        }
        d.commandIdHigh = orderId;
        d.commandIdLow = (attempt << 2) | (KIND_RELEASE_ID == d.kind && SCOPE_ORDER == d.releaseScope ? 3 : d.kind % 3);
        if (KIND_RELEASE_ID != d.kind)
        {
            d.userId = 1 + static_cast<std::int64_t>(rng.below(50));
            d.omsOrderId = orderId;
            d.marketId = 1 + static_cast<std::int32_t>(rng.below(3));
            d.side = static_cast<std::uint8_t>(rng.below(2));
            if (r < 62)
            {
                d.price = 100 + static_cast<std::int64_t>(rng.below(50));
                d.quantity = 1 + static_cast<std::int64_t>(rng.below(20));
            }
        }
        if (r >= 88 || rng.chance(2))
        {
            // anomalous on purpose: one of every class (I, C, RA, RX, unroutable, V12)
            const bool release = KIND_RELEASE_ID == d.kind;
            switch (rng.below(6))
            {
                case 0: // C (R2) / RA (A1)
                    if (release) d.price = 9; else d.userId = 0;
                    break;
                case 1: // I (B4: LIMIT_MAKER IOC; tif on CANCEL/AMEND) / RX (X2)
                    if (release) d.releaseScope = 2;
                    else
                    {
                        d.orderType = 2;
                        d.timeInForce = 1;
                    }
                    break;
                case 2: // C (R4) / RX (X1)
                    if (release) d.commandIdHigh = -orderId; else d.timeInForce = 5;
                    break;
                case 3: // I (B3) / RA (A1)
                    if (release) d.userId = 3; else d.omsOrderId = orderId + 1;
                    break;
                case 4: // unroutable
                    if (!release)
                    {
                        d.commandIdHigh = 0;
                        d.commandIdLow = 0;
                    }
                    break;
                default: // V12 carrier
                    d.kind = static_cast<std::uint8_t>(4 + rng.below(3));
                    break;
            }
        }
        minted[d.dispatchSeq] = d;
        pending[d.dispatchSeq] = d;
        lastMintNs = now;
        if (d.kind <= KIND_AMEND && rng.chance(20))
        {
            capacityProne.insert(d.dispatchSeq);
        }
        if (registered && dropNextBroadcast)
        {
            // beyond A-2 (a correct core closes the session instead): one broadcast silently lost; the
            // forwarder must still recover it through T5 (the next entry) or T7 (the head probe)
            dropNextBroadcast = false;
            note("FAULT broadcast " + std::to_string(d.dispatchSeq) + " silently lost");
        }
        else if (registered)
        {
            Frame f;
            f.kind = FrameKind::DISPATCH;
            f.term = coreTerm;
            f.session = coreSession;
            f.dispatch = d;
            egress.push_back(f);
        }
        stats[0]++;
        return d;
    }

    void resolveSome()
    {
        if (pending.empty())
        {
            return;
        }
        // prefer an entry the ME accepted (an outcome came back), sometimes any (another forwarder's copy)
        auto it = pending.begin();
        if (rng.chance(50) && pending.size() > 16)
        {
            it = pending.end();
            std::advance(it, -static_cast<long>(1 + rng.below(16))); // a recent one, often still in a reply window
        }
        else
        {
            std::advance(it, static_cast<long>(rng.below(pending.size())));
        }
        const bool accepted = lastAccepted.count(it->first) > 0 &&
            (lastRefused.count(it->first) == 0 || lastAccepted[it->first] > lastRefused[it->first]);
        if (accepted || rng.chance(15))
        {
            const std::int64_t seq = it->first;
            pending.erase(it);
            capacityProne.erase(seq);
            stats[1]++;
        }
    }

    void flipGate(bool capacity)
    {
        const std::uint32_t before = gateReasons;
        gateReasons = capacity ? (gateReasons | BIT_CAPACITY) : (gateReasons & ~BIT_CAPACITY);
        if (before == gateReasons)
        {
            return;
        }
        stats[2]++;
        if (registered)
        {
            Frame f;
            f.kind = FrameKind::ADMISSION;
            f.term = coreTerm;
            f.session = coreSession;
            f.gateState = 0 == gateReasons ? 0 : 1;
            f.gateReasons = gateReasons;
            egress.push_back(f);
        }
    }

    void applyIngress()
    {
        Ingress in = ingress.front();
        ingress.pop_front();
        if (in.term != coreTerm || in.session != coreSession)
        {
            stats[3]++; // the consensus module drops an ended term's or a closed session's ingress
            return;
        }
        if (in.registerSession)
        {
            registered = true;
            Frame ack;
            ack.kind = FrameKind::ADMIN_ACK;
            ack.term = coreTerm;
            ack.session = coreSession;
            egress.push_back(ack);
            Frame state;
            state.kind = FrameKind::ADMISSION;
            state.term = coreTerm;
            state.session = coreSession;
            state.gateState = 0 == gateReasons ? 0 : 1;
            state.gateReasons = gateReasons;
            egress.push_back(state);
            return;
        }
        // 65 (S8 3.8): pending above from, ascending, up to the clamped page; the minted bytes replayed.
        std::int32_t count = 0;
        if (in.request.maxEntries > 0)
        {
            const std::int32_t max = std::min(in.request.maxEntries, 4096);
            for (auto it = pending.upper_bound(in.request.fromDispatchSeq); it != pending.end() && count < max; ++it)
            {
                Frame f;
                f.kind = FrameKind::DISPATCH;
                f.term = coreTerm;
                f.session = coreSession;
                f.dispatch = minted.at(it->first);
                egress.push_back(f);
                count++;
            }
        }
        Frame end;
        end.kind = FrameKind::END;
        end.term = coreTerm;
        end.session = coreSession;
        end.correlationId = in.request.correlationId;
        end.count = count;
        end.head = dispatchSeq;
        egress.push_back(end);
        stats[4]++;
    }

    // ------------------------------------------------------------------ faults
    void leaderChange()
    {
        note("FAULT leader change");
        stats[5]++;
        // the old image: a prefix survives (delivered, possibly interleaved), the suffix is lost
        const std::size_t keep = static_cast<std::size_t>(rng.below(egress.size() + 1));
        std::int64_t at = oldEgress.empty() ? now : std::max(now, oldEgress.back().releaseAt);
        for (std::size_t i = 0; i < egress.size(); i++)
        {
            // a session close is learned whatever happens to the image (timeout, image loss): keep it
            if (i < keep || FrameKind::CLOSED == egress[i].kind)
            {
                at += static_cast<std::int64_t>(rng.below(4)) * STEP_NS; // drains up to seconds late
                Frame f = egress[i];
                f.releaseAt = at;
                oldEgress.push_back(f);
            }
        }
        egress.clear();
        // outcomes the new leader applies early resolve some of the entries still in the old image
        for (const Frame &f : oldEgress)
        {
            if (FrameKind::DISPATCH == f.kind && pending.count(f.dispatch.dispatchSeq) > 0 && rng.chance(30))
            {
                pending.erase(f.dispatch.dispatchSeq);
                capacityProne.erase(f.dispatch.dispatchSeq);
                stats[1]++;
            }
        }
        coreTerm++;
        registered = false; // the new leader's queue carries CH_ALL: no dispatch until re-registration
        Frame nle;
        nle.kind = FrameKind::NEW_LEADER;
        nle.term = coreTerm;
        nle.session = coreSession;
        egress.push_back(nle);
    }

    void sessionClose()
    {
        note("FAULT session close (A-2)");
        stats[6]++;
        const std::size_t keep = static_cast<std::size_t>(rng.below(egress.size() + 1));
        std::deque<Frame> kept;
        for (std::size_t i = 0; i < egress.size(); i++)
        {
            if (i < keep || FrameKind::CLOSED == egress[i].kind)
            {
                kept.push_back(egress[i]);
            }
        }
        egress.swap(kept);
        Frame closed;
        closed.kind = FrameKind::CLOSED;
        closed.term = coreTerm;
        closed.session = coreSession;
        egress.push_back(closed);
        registered = false;
        coreSession++; // the client's next connect opens this one
    }

    void restartForwarder()
    {
        note("FAULT forwarder restart");
        stats[7]++;
        egress.clear();
        oldEgress.clear();
        registered = false;
        coreSession++;
        startInstance();
        clientSession = coreSession;
        clientTerm = coreTerm;
        sendRegister();
    }

    // ------------------------------------------------------------------ forwarder stand-in
    void startInstance()
    {
        machine.reset();
        ring = std::make_unique<SendRing>();
        log = std::make_unique<PushLog>();
        ring->setObserverForTest(log.get());
        machine = std::make_unique<ResyncMachine>(*ring);
        watcher.reset();
        c3 = false;
        expectOrigin = false;
        instances++;
    }

    void sendRegister()
    {
        Ingress in;
        in.registerSession = true;
        in.term = clientTerm;
        in.session = clientSession;
        ingress.push_back(in);
    }

    void leaveC3()
    {
        if (c3)
        {
            c3 = false;
            machine->onUnregistered();
        }
        expectOrigin = false;
    }

    void deliver(const Frame &f, bool fromOld)
    {
        if (FrameKind::NEW_LEADER == f.kind)
        {
            note("NLE term " + std::to_string(f.term));
            clientTerm = f.term;
            leaveC3();
            watcher.reset();
            sendRegister();
            return;
        }
        if (FrameKind::CLOSED == f.kind)
        {
            note("CLOSED");
            leaveC3();
            watcher.reset();
            clientSession = coreSession; // reconnect: a new session
            clientTerm = coreTerm;
            sendRegister();
            return;
        }
        const bool filter = !test_seams::disabled(test_seams::Rule::AGENT_STALE_TERM_FILTER);
        const bool stale = f.term != clientTerm || f.session != clientSession;
        if (stale && filter)
        {
            staleDropped++;
            return;
        }
        if (stale)
        {
            staleReachedMachine++;
        }
        log->inStaleDelivery = stale;
        Frame tagged = f;
        if (stale && FrameKind::DISPATCH == f.kind && f.dispatch.kind <= KIND_AMEND)
        {
            // P7 provenance: a stale-term copy carries a marker in a field the forwarder copies to the wire unchecked
            // (price), so a push of it is recognised in any later call (held, then pushed at a final page).
            tagged.dispatch.price ^= STALE_TAG;
        }
        machine->onCoreFrame(now);
        switch (f.kind)
        {
            case FrameKind::ADMIN_ACK:
                expectOrigin = true; // the AdmissionState right after a registration ack is its ORIGIN reply
                if (!c3)
                {
                    c3 = true;
                    note("C3");
                    machine->onRegistered(now);
                }
                break;
            case FrameKind::ADMISSION:
                if (expectOrigin)
                {
                    expectOrigin = false;
                    if (watcher.onOrigin(0, f.gateState, f.gateReasons))
                    {
                        machine->resyncFromZero(Trigger::T3, now);
                    }
                }
                else if (watcher.onBroadcast(0, f.gateState, f.gateReasons))
                {
                    note("T3");
                    machine->resyncFromZero(Trigger::T3, now);
                }
                break;
            case FrameKind::DISPATCH:
                machine->onDispatch(tagged.dispatch, classify(tagged.dispatch), now);
                break;
            case FrameKind::END:
                note("END corr " + std::to_string(f.correlationId) + " count " + std::to_string(f.count) + " head " +
                    std::to_string(f.head) + (fromOld ? " (old image)" : ""));
                machine->onPendingEnd(f.correlationId, f.count, f.head, now);
                break;
            default:
                break;
        }
        log->inStaleDelivery = false;
    }

    void pollEgress(int limit)
    {
        for (int i = 0; i < limit; i++)
        {
            if (!oldEgress.empty() && oldEgress.front().releaseAt <= now && (egress.empty() || rng.chance(50)))
            {
                const Frame f = oldEgress.front();
                oldEgress.pop_front();
                deliver(f, true);
                continue;
            }
            if (egress.empty())
            {
                if (oldEgress.empty())
                {
                    return;
                }
                continue; // the old image's next frame is not due yet
            }
            const Frame f = egress.front();
            egress.pop_front();
            deliver(f, false);
        }
    }

    void offerRequest(bool loseIt)
    {
        const PendingRequest *r = machine->requestDue();
        if (nullptr == r || !c3)
        {
            return;
        }
        if (loseNextRequest)
        {
            loseNextRequest = false;
            loseIt = true;
        }
        if (!loseIt)
        {
            Ingress in;
            in.term = clientTerm;
            in.session = clientSession;
            in.request = *r;
            ingress.push_back(in);
        }
        else
        {
            note("FAULT request lost");
            stats[8]++;
        }
        machine->onRequestSent(now);
    }

    void drainRing(int frames)
    {
        if (ringStalled || meDown)
        {
            return;
        }
        for (int i = 0; i < frames && !ring->empty(); i++)
        {
            const FramePlan p = ring->nextFrame();
            for (std::uint32_t k = 0; k < p.count; k++)
            {
                meTail.push_back(ring->at(k).seq);
            }
            ring->pop(p.count);
        }
    }

    void meCommit()
    {
        for (const std::int64_t seq : meTail)
        {
            const bool refused = 0 != (gateReasons & BIT_CAPACITY) && capacityProne.count(seq) > 0;
            (refused ? lastRefused : lastAccepted)[seq] = now;
        }
        meTail.clear();
    }

    void meLoss(bool sessionClose)
    {
        note(std::string("FAULT ME ") + (sessionClose ? "session close" : "leader change") + ": " +
            std::to_string(meTail.size()) + " uncommitted offers lost");
        stats[sessionClose ? 17 : 16]++;
        meTail.clear();
        lastMeLossNs = now;
        if (sessionClose)
        {
            meDown = true; // M0 until the next session reaches M3
            meDownUntil = now + static_cast<std::int64_t>(2 + rng.below(20)) * STEP_NS;
        }
        else if (!test_seams::disabled(test_seams::Rule::AGENT_T2))
        {
            machine->resyncFromZero(Trigger::T2, now); // ME NewLeaderEvent in M3
        }
    }

    // ------------------------------------------------------------------ the loop
    void step(bool faults)
    {
        now += STEP_NS;
        if (ingressStalled && now >= ingressStallUntil)
        {
            ingressStalled = false;
        }
        if (ringStalled && now >= ringStallUntil)
        {
            ringStalled = false;
        }
        if (meDown && now >= meDownUntil)
        {
            meDown = false; // the new ME session reaches M3
            if (!test_seams::disabled(test_seams::Rule::AGENT_T2))
            {
                machine->resyncFromZero(Trigger::T2, now);
            }
        }
        if (faults)
        {
            const std::uint64_t r = rng.below(1000);
            if (r >= 990) meLoss(false);
            else if (r >= 984) meLoss(true);
            else if (r < 6) leaderChange();
            else if (r < 11) sessionClose();
            else if (r < 13) restartForwarder();
            else if (r < 20)
            {
                ingressStalled = true; // a 108 delayed past T9 (egress silent meanwhile)
                ingressStallUntil = now + static_cast<std::int64_t>(4 + rng.below(5)) * 1'000'000'000LL;
                note("FAULT ingress stall");
                stats[9]++;
            }
            else if (r < 33)
            {
                dropNextBroadcast = true;
            }
            else if (r < 43)
            {
                ringStalled = true;
                ringStallUntil = now + static_cast<std::int64_t>(1 + rng.below(40)) * STEP_NS;
            }
        }
        // the core: one apply per step, ingress or a user event
        if (!ingress.empty() && !ingressStalled && rng.chance(60))
        {
            applyIngress();
        }
        else if (faults && !ingressStalled)
        {
            const std::uint64_t r = rng.below(100);
            if (r < 42) mint();
            else if (r < 75) resolveSome();
            else if (r < 80) flipGate(rng.chance(50));
        }
        pollEgress(static_cast<int>(1 + rng.below(6)));
        offerRequest(faults && rng.chance(2));
        machine->tick(now);
        offerRequest(false);
        drainRing(faults ? static_cast<int>(1 + rng.below(4)) : 64); // the ME catches up once faults stop
        if (!faults || rng.chance(60))
        {
            meCommit();
        }
        if (!faults && std::getenv("CT_FORWARDER_PROPERTY_DEBUG") && (now / STEP_NS) % 500 == 0)
        {
            note("debug ring " + std::to_string(ring->depth()) + " head seq " +
                (ring->empty() ? std::string("-") : std::to_string(ring->at(0).seq)) + " cursor " +
                std::to_string(machine->cursor()) + " pushes " + std::to_string(log->pushes));
        }

        // P5 and the invariants the machine itself reports
        REQUIRE_MESSAGE(machine->holdSize() <= ResyncMachine::HOLD_CAPACITY, where());
        REQUIRE_MESSAGE(ring->depth() <= SendRing::CAPACITY, where());
        REQUIRE_MESSAGE(machine->verdict() == ResyncVerdict::NONE, where());
        // P2: strictly ascending between ring clears (a RESYNC(cursor) restarts above the cursor, which is at or
        // above every earlier push, so it keeps the order; a RESYNC(0) clears the ring)
        REQUIRE_MESSAGE(!log->ascendingBroken, "P2 broken at seq " << log->brokenAt << "\n" << where());
        // P3/P4: every push is a minted seq in its 5.5 form
        for (const RingSlot &slot : log->pushed)
        {
            REQUIRE_MESSAGE(!(SlotForm::COMMAND == slot.form && 0 != (slot.command.price & STALE_TAG)),
                "P7: a stale-term entry was pushed (seq " << slot.seq << ", provenance tag)\n" << where());
            const auto it = minted.find(slot.seq);
            REQUIRE_MESSAGE(it != minted.end(), "P3: pushed seq " << slot.seq << " never minted\n" << where());
            RingSlot want;
            REQUIRE(toRingSlot(it->second, classify(it->second), want));
            REQUIRE_MESSAGE(want.form == slot.form, "P4 form, seq " << slot.seq << "\n" << where());
            if (SlotForm::COMMAND == slot.form)
            {
                REQUIRE_MESSAGE(want.command == slot.command, "P4 command, seq " << slot.seq << "\n" << where());
            }
            else
            {
                REQUIRE_MESSAGE(want.release == slot.release, "P4 release, seq " << slot.seq << "\n" << where());
            }
        }
        log->pushed.clear();
        // P7
        REQUIRE_MESSAGE(!log->pushedDuringStaleDelivery, "P7: a stale-term entry was pushed\n" << where());
    }

    void burst()
    {
        note("FAULT burst");
        stats[10]++;
        ringStalled = true;
        ringStallUntil = now + 200 * STEP_NS;
        const int n = 70'000;
        for (int i = 0; i < n; i++)
        {
            mint();
            if (i % 64 == 0)
            {
                pollEgress(64);
                machine->tick(now);
            }
        }
    }

    // Quiescent = nothing in flight anywhere AND a head probe or a resync end whose query the core applied
    // after the last mint (lastHead >= the last minted seq): a broadcast lost beyond A-2 with no later traffic
    // is found by the next 10 s head probe (T7), not before.
    bool quiescent() const
    {
        return machine->lastHead() >= dispatchSeq && egress.empty() && oldEgress.empty() && ingress.empty() && ring->empty() && meTail.empty() && !meDown && c3 &&
            Mode::LIVE == machine->mode() && !machine->requestInFlight() && nullptr == machine->requestDue() &&
            !ringStalled && !ingressStalled;
    }

    void run(int faultSteps)
    {
        startInstance();
        sendRegister();
        flipGate(false);
        gateReasons = 0; // ADMIN OPEN after genesis
        for (int i = 0; i < faultSteps; i++)
        {
            step(true);
            if (seed % 997 == 3 && i == faultSteps / 2)
            {
                burst();
            }
        }
        // quiescence: faults off, the gate opens (a BROADCAST the forwarder sees, T3)
        note("QUIESCE");
        flipGate(false);
        loseNextRequest = 0 == seed % 4; // only T9 can recover a request lost now
        int steps = 0;
        while (!quiescent())
        {
            step(false);
            REQUIRE_MESSAGE(++steps < 100'000, "P6 liveness: no quiescence (lastHead " << machine->lastHead()
                << " minted " << dispatchSeq << " egress " << egress.size() << " old " << oldEgress.size()
                << " ingress " << ingress.size() << " ring " << ring->depth() << " c3 " << c3 << " mode "
                << nameOf(machine->mode()) << " inFlight " << machine->requestInFlight() << " ringStalled " << ringStalled << " until " << (ringStallUntil - now) / STEP_NS << " ingressStalled " << ingressStalled << ")\n" << where());
        }
        if (0 == seed % 2)
        {
            // Late loss (review FIX-4 kept T7 observable): after quiescence, with faults off and no ME loss left
            // to trigger T2, one more dispatch whose broadcast is lost and no traffic after it. Only the 10 s head
            // probe (T7) can find it; with T7 off the probe reply is ignored and P1 fails.
            note("LATE LOSS");
            dropNextBroadcast = true;
            mint();
            dropNextBroadcast = false;
            int late = 0;
            while (!quiescent())
            {
                step(false);
                REQUIRE_MESSAGE(++late < 100'000, "P6 liveness: a broadcast lost after quiescence was never found (lastHead "
                    << machine->lastHead() << " minted " << dispatchSeq << ")\n" << where());
            }
        }
        stats[11] += machine->counters().resyncs[5] + machine->counters().resyncs[7];
        stats[12] += machine->counters().resyncs[9];
        stats[13] += machine->counters().resyncs[3];
        stats[14] += machine->counters().shapeViolations[0] + machine->counters().shapeViolations[1] +
            machine->counters().shapeViolations[2];
        stats[15] += staleDropped;
        check();
    }

    void check()
    {
        for (const auto &[seq, d] : pending)
        {
            const Classification c = classify(d);
            if (DispatchClass::UNROUTABLE == c.cls)
            {
                continue; // the only dispatch never put on the wire (5.5)
            }
            const auto pushed = log->lastPushGeneration.find(seq);
            REQUIRE_MESSAGE(pushed != log->lastPushGeneration.end(),
                "P1: pending seq " << seq << " (" << nameOf(c.cls) << ") never pushed by this instance\n" << where());
            REQUIRE_MESSAGE(pushed->second == log->generation,
                "P1: pending seq " << seq << " not pushed since the last RESYNC(0)\n" << where());
            const auto accepted = lastAccepted.find(seq);
            REQUIRE_MESSAGE(accepted != lastAccepted.end(), "P8: pending seq " << seq << " never accepted\n" << where());
            const auto refused = lastRefused.find(seq);
            REQUIRE_MESSAGE((refused == lastRefused.end() || accepted->second > refused->second),
                "P8: pending seq " << seq << " not re-offered after its CAPACITY refusal\n" << where());
            REQUIRE_MESSAGE(accepted->second > lastMeLossNs,
                "P8: pending seq " << seq << " not accepted by the ME after the last ME loss\n" << where());
        }
    }
};

int seedCount()
{
    const char *env = std::getenv("CT_FORWARDER_PROPERTY_SEEDS");
    return nullptr != env ? std::atoi(env) : 10'000;
}

}

TEST_CASE("ResyncPropertyTest.everyPendingDispatchReachesTheMeAtQuiescence")
{
#if defined(__GLIBC__)
    // Each fresh machine allocates two 64 Ki-slot buffers; keep them on the heap free list across seeds.
    mallopt(M_MMAP_THRESHOLD, 64 * 1024 * 1024);
#endif
    const int seeds = seedCount();
    std::uint64_t totals[18] = {};
    std::uint64_t instances = 0;
    const char *first = std::getenv("CT_FORWARDER_PROPERTY_FIRST_SEED"); // reproduce one failing seed
    const int firstSeed = nullptr != first ? std::atoi(first) : 1;
    for (int seed = firstSeed; seed < firstSeed + seeds; seed++)
    {
        Run run(static_cast<std::uint64_t>(seed));
        run.run(600);
        for (int i = 0; i < 18; i++)
        {
            totals[i] += run.stats[i];
        }
        instances += run.instances;
    }
    MESSAGE("seeds=" << seeds << " minted=" << totals[0] << " resolved=" << totals[1] << " gateFlips=" << totals[2]
        << " droppedIngress=" << totals[3] << " replies=" << totals[4] << " leaderChanges=" << totals[5]
        << " closes=" << totals[6] << " restarts=" << totals[7] << " lostRequests=" << totals[8]
        << " ingressStalls=" << totals[9] << " bursts=" << totals[10] << " gapOrHeadResyncs=" << totals[11]
        << " t9=" << totals[12] << " t3=" << totals[13] << " shapeViolations=" << totals[14]
        << " staleDropped=" << totals[15] << " meLeaderChanges=" << totals[16] << " meSessionCloses=" << totals[17]
        << " instances=" << instances);
    // non-vacuity: every fault and every recovery path ran
    if (seeds >= 1000)
    {
        CHECK(totals[5] > 0);
        CHECK(totals[6] > 0);
        CHECK(totals[7] > 0);
        CHECK(totals[8] > 0);
        CHECK(totals[9] > 0);
        CHECK(totals[10] > 0);
        CHECK(totals[11] > 0);
        CHECK(totals[12] > 0);
        CHECK(totals[13] > 0);
        CHECK(totals[15] > 0);
        CHECK(totals[16] > 0);
        CHECK(totals[17] > 0);
    }
}
