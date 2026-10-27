// The 4.5 follow decision table, the pause rule and the frame path (RC2 S9a contract 9.1), driven
// through FollowerLoop over the simulated archive on a manual clock.
#include "doctest/doctest.h"

#include "SimArchive.h"

using namespace cluster_tools::archive;
using namespace ct_archive_test;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t EPOCH = 9;

bool following(Harness &h)
{
    return h.policy->phase() == Phase::FOLLOWING;
}

// Attach and reach FOLLOWING without reading a fragment (paused steps).
void attachPaused(Harness &h)
{
    h.attach();
    REQUIRE(h.runUntil([&] { return following(h); }, 50, 1ms, 0));
}

// Attach unpaused (the attach's control calls run), stopping the moment the replay is FOLLOWING:
// the poll of that iteration ran before the startReplay, so nothing has been read yet.
void attachThenPause(Harness &h)
{
    h.attach();
    REQUIRE(h.runUntil([&] { return following(h); }, 50, 1ms, 32));
    REQUIRE(h.sink.seqs.empty());
}

}

TEST_CASE("FollowPolicyTest.imageEndOnLiveFollowProbesAtOnce")
{
    SUBCASE("stopped and fully consumed -> RECORDING_ENDED, no source fault")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        const int probesBefore = h.archive.probeCalls;
        h.archive.stop(r);
        const TimePoint stoppedAt = h.now;
        REQUIRE(h.runUntil([&] { return h.observer.ended == 1; }, 100));
        CHECK(h.now - stoppedAt < 50ms); // within an iteration or two of the image ending, not at the 10 s probe
        CHECK(h.archive.probeCalls == probesBefore + 1);
        CHECK(h.observer.faults.empty());
        CHECK(h.metrics.totalStalls() == 0);
        CHECK(ArchiveMetrics::get(h.metrics.recordingSuccessions) == 1);
    }
    SUBCASE("consumed short of the stop -> EARLY_EOF")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 5, EPOCH);
        attachPaused(h);
        h.step(2); // two of five frames
        REQUIRE(h.sink.seqs.size() == 2);
        h.archive.stop(r);
        h.archive.dropImageNow = true;
        h.run(5, 1ms, 0);
        CHECK(h.observer.sawFault(FaultKind::EARLY_EOF));
        CHECK(h.metrics.stalls(StallReason::EARLY_EOF) == 1);
    }
    SUBCASE("still active -> IMAGE_LOST")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.archive.dropImageNow = true;
        h.run(5);
        CHECK(h.observer.sawFault(FaultKind::IMAGE_LOST));
        CHECK_FALSE(h.observer.sawFault(FaultKind::ARCHIVE_GONE));
    }
    SUBCASE("probe throws -> ARCHIVE_GONE")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.archive.dropImageNow = true;
        h.archive.probeFails = true;
        h.run(5);
        CHECK(h.observer.sawFault(FaultKind::ARCHIVE_GONE));
        CHECK(h.archive.stopCalls == 0); // ARCHIVE_GONE: no stopReplay that would only wait out the timeout
        CHECK(h.archive.disconnects >= 1);
    }
}

TEST_CASE("FollowPolicyTest.neverConnected15s")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.archive.imageNeverConnects = true;
    h.attach();
    REQUIRE(h.runUntil([&] { return following(h); }, 50));
    const TimePoint t0 = h.now;
    h.runUntil([&] { return h.observer.sawFault(FaultKind::NEVER_CONNECTED); }, 400, 50ms);
    REQUIRE(h.observer.sawFault(FaultKind::NEVER_CONNECTED));
    CHECK(h.now - t0 >= 14'900ms);
    CHECK(h.now - t0 <= 15'200ms);
    CHECK(h.metrics.stalls(StallReason::NEVER_CONNECTED) == 1);
    CHECK(h.archive.stopCalls == 1); // the leaked replay is stopped, so MAX_REPLAYS is never exhausted
}

TEST_CASE("FollowPolicyTest.probeThrowsWhenArchiveGone")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.probeFails = true;
    const TimePoint t0 = h.now;
    h.runUntil([&] { return h.observer.sawFault(FaultKind::ARCHIVE_GONE); }, 300, 50ms);
    REQUIRE(h.observer.sawFault(FaultKind::ARCHIVE_GONE));
    CHECK(h.now - t0 <= 10'100ms);
    CHECK(h.metrics.stalls(StallReason::ARCHIVE_GONE) == 1);
}

TEST_CASE("FollowPolicyTest.grewWhileStuck")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.deliverNothing = true;
    h.archive.appendTrades(r, 4, 2, EPOCH); // recorded grows, the replay stays put (the 07-10 shape)
    h.runUntil([&] { return h.observer.sawFault(FaultKind::STALL); }, 600, 50ms);
    CHECK(h.observer.sawFault(FaultKind::STALL));
    CHECK(h.metrics.stalls(StallReason::STALL) == 1);
}

TEST_CASE("FollowPolicyTest.stoppedButStuckBeforeStop")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.deliverNothing = true;
    h.archive.appendTrades(r, 4, 2, EPOCH);
    h.archive.stop(r);
    h.runUntil([&] { return h.observer.sawFault(FaultKind::STALL_AFTER_STOP); }, 600, 50ms);
    CHECK(h.observer.sawFault(FaultKind::STALL_AFTER_STOP));
    CHECK(h.metrics.stalls(StallReason::STALL_AFTER_STOP) == 1);
}

TEST_CASE("FollowPolicyTest.recordingStopsThenSuccessor")
{
    Harness h;
    auto &first = h.archive.record();
    h.archive.appendTrades(first, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.stop(first);
    auto &second = h.archive.record(); // the ME writer reconnected: a new recording
    h.archive.appendTrades(second, 4, 3, EPOCH);
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 6; }, 200));
    CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6 });
    CHECK(h.observer.ended == 1);
    CHECK(h.observer.faults.empty());
    CHECK(ArchiveMetrics::get(h.metrics.recordingSuccessions) == 1);
    CHECK(h.archive.stopCalls == 0); // the archive itself ended the live replay
}

TEST_CASE("FollowPolicyTest.stoppedReplayBeforeEofIsFailure")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 5, EPOCH);
    h.archive.stop(r);
    h.archive.record(); // an active successor, never reached
    attachPaused(h);
    h.step(2);
    REQUIRE(h.sink.seqs.size() == 2);
    h.archive.dropImageNow = true;
    h.run(20, 1ms, 0);
    CHECK(h.observer.sawFault(FaultKind::EARLY_EOF)); // never "drained" (cpp: S10, BridgeEndToEndTest.java:696)
    CHECK(h.policy->chainIndex() == 0);
}

TEST_CASE("FollowPolicyTest.boundedReplayStuck")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 5, EPOCH);
    h.archive.stop(r);
    h.archive.deliverNothing = true; // the image stays live and delivers nothing
    h.attach();
    REQUIRE(h.runUntil([&] { return following(h); }, 50));
    const TimePoint t0 = h.now;
    h.runUntil([&] { return h.observer.sawFault(FaultKind::STALL_BOUNDED_STUCK); }, 600, 50ms);
    REQUIRE(h.observer.sawFault(FaultKind::STALL_BOUNDED_STUCK));
    CHECK(h.now - t0 >= 19'900ms); // two consecutive 10 s checks
    CHECK(h.metrics.stalls(StallReason::BOUNDED_STUCK) == 1);
    CHECK(h.archive.probeCalls == 0); // no archive call
}

TEST_CASE("FollowPolicyTest.chainExhaustedBacksOffOneSecond")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 2, EPOCH);
    h.archive.stop(r);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 2; }, 100));
    const int listsBefore = h.archive.listCalls;
    h.run(5000, 1ms); // 5 s with no active recording
    const int lists = h.archive.listCalls - listsBefore;
    CHECK(lists >= 4);
    CHECK(lists <= 6);
    CHECK(h.observer.exhausted >= 4);
}

TEST_CASE("FollowPolicyTest.asyncArchiveErrorFaults")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 2, EPOCH);
    h.archive.imageNeverConnects = true;
    h.attach();
    REQUIRE(h.runUntil([&] { return following(h); }, 50));
    h.run(50, 100ms); // 5 s: the archive's replay connect timeout
    h.archive.pendingAsyncError = "no connection established for replayChannel";
    h.run(2);
    CHECK(h.observer.sawFault(FaultKind::ARCHIVE_ERROR));
    CHECK(h.metrics.stalls(StallReason::ARCHIVE_ERROR) == 1);
    CHECK_FALSE(h.observer.sawFault(FaultKind::NEVER_CONNECTED));
}

TEST_CASE("FollowPolicyTest.ownerPauseIsNotAStall")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.appendTrades(r, 4, 20, EPOCH); // backlog while the owner is back-pressured
    const int probesBefore = h.archive.probeCalls;
    h.run(1200, 50ms, 0); // 60 s paused
    CHECK(h.observer.faults.empty());
    CHECK(h.archive.probeCalls - probesBefore >= 5); // the paused follower still probes
    // Resume: the baselines restart, reading catches up, still no stall.
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 23; }, 100));
    h.run(400, 50ms);
    CHECK(h.observer.faults.empty());
}

TEST_CASE("FollowPolicyTest.pausedFollowerStillProbes")
{
    SUBCASE("row 9 while paused")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        attachThenPause(h);
        h.archive.probeFails = true;
        h.runUntil([&] { return h.observer.sawFault(FaultKind::ARCHIVE_GONE); }, 400, 50ms, 0);
        CHECK(h.observer.sawFault(FaultKind::ARCHIVE_GONE));
    }
    SUBCASE("row 7a while paused")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        attachThenPause(h);
        h.step(0);
        h.archive.dropImageNow = true;
        h.run(5, 1ms, 0);
        CHECK(h.observer.sawFault(FaultKind::IMAGE_LOST));
    }
    SUBCASE("row 13 while paused")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        attachThenPause(h);
        h.archive.pendingAsyncError = "replay error";
        h.run(3, 1ms, 0);
        CHECK(h.observer.sawFault(FaultKind::ARCHIVE_ERROR));
    }
}

TEST_CASE("FollowPolicyTest.abortedFrameIsNoRegression")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 40, EPOCH);
    h.sink.abortEvery = 3; // back-pressure: every third delivery attempt is aborted and redelivered
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 40; }, 400));
    CHECK(h.observer.faults.empty());
    for (std::size_t i = 0; i < h.sink.seqs.size(); ++i)
    {
        CHECK(h.sink.seqs[i] == static_cast<std::int64_t>(i + 1)); // once each, in order
    }
}

TEST_CASE("FollowPolicyTest.seqRegressionInRecordingFaults")
{
    Harness h;
    h.memo->onReport(EPOCH, 2);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.archive.append(r, makeFrame(TEMPLATE_TRADE, 2, EPOCH)); // seq 2 again inside one recording
    h.archive.appendTrades(r, 4, 1, EPOCH);
    h.attach();
    h.runUntil([&] { return h.observer.sawFault(FaultKind::SEQ_REGRESSION_IN_RECORDING); }, 100);
    REQUIRE(h.observer.sawFault(FaultKind::SEQ_REGRESSION_IN_RECORDING));
    CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3 }); // the regressed frame is never delivered
    CHECK(h.metrics.invalidations(MemoReason::SEQ_REGRESSION) == 1);
    CHECK(h.memo->proofCount(0) == 0); // memo of the source invalidated
}

TEST_CASE("FollowPolicyTest.resumeBeltTrips")
{
    Harness h;
    h.memo->onReport(EPOCH, 5);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 5, EPOCH);
    h.archive.appendTrades(r, 7, 2, EPOCH); // 6 is not where the proof says it is
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 7; }, 100));
    REQUIRE(h.memo->proof(0, r.rec.key));
    CHECK(h.memo->proof(0, r.rec.key)->seq == 5);
    h.policy->reattach(h.now); // a rewind: resume at (K, proof.p)
    h.runUntil([&] { return h.observer.sawFault(FaultKind::RESUME_BELT); }, 100);
    REQUIRE(h.observer.sawFault(FaultKind::RESUME_BELT));
    CHECK(h.metrics.invalidations(MemoReason::RESUME_BELT) == 1);
    // Re-read from the chain start (memo-free) after the back-off.
    const std::size_t attaches = h.observer.attaches.size();
    REQUIRE(h.runUntil([&] { return h.observer.attaches.size() > attaches; }, 2000));
    CHECK(h.observer.attaches.back().memoFree);
    CHECK(h.observer.attaches.back().from == 0);
}

TEST_CASE("FollowPolicyTest.fatalMonitorShortCircuits")
{
    Harness h;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.fatal = true;
    h.archive.appendTrades(r, 4, 3, EPOCH);
    const int calls = h.archive.controlCalls;
    h.run(3000, 10ms);
    CHECK(h.observer.sawFault(FaultKind::FATAL));
    CHECK(h.archive.controlCalls == calls); // no archive call after the monitor fired
    CHECK(h.sink.seqs.size() == 3);
    CHECK(h.policy->phase() == Phase::FATAL);
}

TEST_CASE("FollowPolicyTest.atMostOneControlCallPerIteration")
{
    Harness h;
    auto &a = h.archive.record();
    h.archive.appendTrades(a, 1, 4, EPOCH);
    h.archive.stop(a);
    h.archive.stop(h.archive.record()); // empty
    auto &c = h.archive.record();
    h.archive.appendTrades(c, 5, 4, EPOCH);
    h.attach();
    int maxPerStep = 0;
    for (int i = 0; i < 4000; ++i)
    {
        if (i == 50)
        {
            h.archive.stop(c);
            h.archive.appendTrades(h.archive.record(), 9, 2, EPOCH);
        }
        if (i == 400)
        {
            h.archive.probeFails = true;
        }
        if (i == 1000)
        {
            h.archive.probeFails = false;
        }
        const int before = h.archive.controlCalls;
        h.step(32, 10ms);
        maxPerStep = std::max(maxPerStep, h.archive.controlCalls - before);
    }
    CHECK(maxPerStep == 1);
}

TEST_CASE("FollowPolicyTest.storageRefusalSendsNoRequestAndKeepsProofs")
{
    Harness h;
    h.memo->onReport(EPOCH, 3);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 5, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 5; }, 100));
    REQUIRE(h.memo->proofCount(0) == 1);
    const int starts = h.archive.startCalls;
    h.archive.storageRefused = true;
    h.policy->reattach(h.now);
    h.runUntil([&] { return h.observer.sawFault(FaultKind::REPLAY_STORAGE); }, 100);
    CHECK(h.observer.sawFault(FaultKind::REPLAY_STORAGE));
    CHECK(h.archive.startCalls == starts); // no request sent
    CHECK(h.memo->proofCount(0) == 1);     // proofs kept (R-K4: no rescan on retry)
    CHECK(h.metrics.openFailures(OpenFailure::STORAGE) >= 1);
    h.archive.storageRefused = false;
    const std::size_t attaches = h.observer.attaches.size();
    REQUIRE(h.runUntil([&] { return h.observer.attaches.size() > attaches; }, 3000));
    CHECK(h.observer.attaches.back().fromProof);
}

TEST_CASE("FollowPolicyTest.unresolvedEndpointFaultsAfterTenSeconds")
{
    Harness h;
    h.archive.appendTrades(h.archive.record(), 1, 2, EPOCH);
    h.archive.endpointNeverResolves = true;
    h.attach();
    const TimePoint t0 = h.now;
    h.runUntil([&] { return h.observer.sawFault(FaultKind::REPLAY_ENDPOINT_UNRESOLVED); }, 400, 50ms);
    REQUIRE(h.observer.sawFault(FaultKind::REPLAY_ENDPOINT_UNRESOLVED));
    CHECK(h.now - t0 >= 9'900ms);
    CHECK(h.archive.startCalls == 0); // a port-0 channel is never sent to the archive
    CHECK(h.metrics.openFailures(OpenFailure::ENDPOINT) == 1);
}

TEST_CASE("FollowPolicyTest.detachStopsReplayFreeingTheSlot")
{
    Harness h;
    h.archive.maxReplays = 1;
    h.archive.leakReplayOnClose = true; // the archive keeps a replay until stopReplay
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, EPOCH);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    h.archive.dropImageNow = true; // IMAGE_LOST: a source fault, the replay is detached
    REQUIRE(h.runUntil([&] { return h.observer.sawFault(FaultKind::IMAGE_LOST); }, 50));
    const std::size_t attaches = h.observer.attaches.size();
    REQUIRE(h.runUntil([&] { return h.observer.attaches.size() > attaches; }, 3000));
    CHECK(h.archive.stopCalls >= 1);
    CHECK(h.metrics.openFailures(OpenFailure::MAX_REPLAYS) == 0);
}

TEST_CASE("FollowPolicyTest.connectFailureBacksOffOneSecond")
{
    Harness h;
    h.archive.reachable = false;
    h.attach();
    h.run(3000, 1ms);
    CHECK(h.archive.connectCalls >= 2);
    CHECK(h.archive.connectCalls <= 4);
    CHECK(h.observer.sawFault(FaultKind::CONNECT_FAILED));
}

TEST_CASE("FollowPolicyTest.chainEmptyWaitsOneSecondAndRelists")
{
    Harness h;
    h.attach();
    h.run(3000, 1ms);
    CHECK(h.archive.listCalls >= 2);
    CHECK(h.archive.listCalls <= 4);
    CHECK(h.observer.empty >= 2);
    h.archive.appendTrades(h.archive.record(), 1, 2, EPOCH);
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 2; }, 2000));
}

TEST_CASE("FollowPolicyTest.refusedFrameClosesTheReplayOrContinuesForTheTail")
{
    SUBCASE("the bridge shape: SCHEMA_REFUSED closes the replay")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 2, EPOCH);
        Bytes schema3 = makeFrame(TEMPLATE_TRADE, 3, EPOCH);
        putLe16(schema3, 4, 3);
        h.archive.append(r, schema3);
        h.archive.appendTrades(r, 4, 2, EPOCH);
        h.attach();
        h.runUntil([&] { return h.observer.sawFault(FaultKind::SCHEMA_REFUSED); }, 100);
        REQUIRE(h.observer.sawFault(FaultKind::SCHEMA_REFUSED));
        CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2 });
        CHECK(h.observer.faults.back().refusal == Refusal::SCHEMA);
        CHECK(h.metrics.refusals(Refusal::SCHEMA) == 1);
    }
    SUBCASE("the journal-tail shape: printed and the poll goes on")
    {
        Harness h;
        h.sink.onRefusal = RefusalAction::SKIP_AND_CONTINUE;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 2, EPOCH);
        h.archive.append(r, makeFrame(TEMPLATE_TRADE, 3, EPOCH), 0x80);
        h.archive.appendTrades(r, 4, 2, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 4; }, 100));
        CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 4, 5 });
        CHECK(h.sink.refused == std::vector<Refusal>{ Refusal::FRAGMENTED });
        CHECK(h.observer.faults.empty());
    }
}

// ---- review fixes (S9a review.md, 2026-10-27)

TEST_CASE("FollowPolicyTest.successorListedLateContinuesTheRead") // F-1, probe P1
{
    SUBCASE("unbound: no proof exists, the read continues after the ended recording")
    {
        Harness h;
        auto &first = h.archive.record();
        h.archive.appendTrades(first, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.archive.stop(first);
        REQUIRE(h.runUntil([&] { return h.observer.ended == 1; }, 100));
        h.run(1500, 1ms); // the ME is down: at least one chain-exhausted back-off and re-list
        CHECK(h.policy->hasContinuation());
        auto &second = h.archive.record();
        h.archive.appendTrades(second, 4, 3, EPOCH);
        h.runUntil([&] { return h.sink.seqs.size() >= 6 && h.sink.seqs.back() == 6; }, 5000);
        CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6 }); // each seq exactly once
        CHECK(h.archive.requests.size() == 2);                               // one replay per recording
        CHECK(h.observer.faults.empty());
    }
    SUBCASE("bound, core cursor behind the reader: no memo re-read, the read stays memo-free")
    {
        Harness h;
        h.memo->onReport(EPOCH, 1);
        auto &first = h.archive.record();
        h.archive.appendTrades(first, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.archive.stop(first);
        REQUIRE(h.runUntil([&] { return h.observer.ended == 1; }, 100));
        h.run(2500, 1ms);
        auto &second = h.archive.record();
        h.archive.appendTrades(second, 4, 3, EPOCH);
        h.runUntil([&] { return h.sink.seqs.size() >= 6 && h.sink.seqs.back() == 6; }, 5000);
        CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6 });
        CHECK(h.archive.requests.size() == 2);
        CHECK(h.observer.attaches.back().memoFree);
    }
    SUBCASE("the same after DRAINED reached the chain end")
    {
        Harness h;
        auto &first = h.archive.record();
        h.archive.appendTrades(first, 1, 3, EPOCH);
        h.archive.stop(first);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.run(2500, 1ms);
        auto &second = h.archive.record();
        h.archive.appendTrades(second, 4, 3, EPOCH);
        h.runUntil([&] { return h.sink.seqs.size() >= 6 && h.sink.seqs.back() == 6; }, 5000);
        CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5, 6 });
        CHECK(h.archive.requests.size() == 2);
    }
    SUBCASE("an owner rewind clears the continuation point")
    {
        Harness h;
        auto &first = h.archive.record();
        h.archive.appendTrades(first, 1, 3, EPOCH);
        h.archive.stop(first);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.run(1200, 1ms);
        REQUIRE(h.policy->hasContinuation());
        h.policy->reattach(h.now); // an owner rewind clears the continuation point
        CHECK_FALSE(h.policy->hasContinuation());
    }
}

TEST_CASE("FollowPolicyTest.sinkAbortBackPressureIsNotAStall") // F-4, probe P4
{
    SUBCASE("live follow")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.archive.appendTrades(r, 4, 20, EPOCH);
        h.sink.abortNext = 1'000'000; // every offer back-pressured; the owner keeps polling with 32
        const int probes = h.archive.probeCalls;
        h.run(1200, 50ms);           // 60 s
        CHECK(h.observer.faults.empty());
        CHECK(h.archive.probeCalls - probes >= 5); // still probing (rows 7a, 9 run)
        h.sink.abortNext = 0;
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 23; }, 200));
        CHECK(h.observer.faults.empty());
    }
    SUBCASE("bounded replay")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 20, EPOCH);
        h.archive.stop(r);
        h.archive.record();
        h.sink.abortNext = 1'000'000;
        h.attach();
        h.run(1200, 50ms);
        CHECK(h.observer.faults.empty());
        h.sink.abortNext = 0;
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 20; }, 200));
    }
    SUBCASE("a genuinely stuck replay with an unpaused owner still stalls")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 20, EPOCH);
        h.archive.stop(r);
        h.archive.deliverNothing = true;
        h.attach();
        h.runUntil([&] { return h.observer.sawFault(FaultKind::STALL_BOUNDED_STUCK); }, 600, 50ms);
        CHECK(h.observer.sawFault(FaultKind::STALL_BOUNDED_STUCK));
    }
}

TEST_CASE("FollowPolicyTest.stopAfterPaddingIsDrainedNotEarlyEof") // N-1, probe P5
{
    SUBCASE("bounded replay")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        h.archive.pad(r, 256);
        h.archive.stop(r);
        auto &next = h.archive.record();
        h.archive.appendTrades(next, 4, 2, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() >= 5; }, 3000));
        CHECK_FALSE(h.observer.sawFault(FaultKind::EARLY_EOF));
        CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2, 3, 4, 5 });
    }
    SUBCASE("live follow: the recording stops right after a padding frame")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 3, EPOCH);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.archive.pad(r, 256);
        h.archive.stop(r);
        REQUIRE(h.runUntil([&] { return h.observer.ended == 1 || !h.observer.faults.empty(); }, 200));
        CHECK(h.observer.ended == 1);
        CHECK(h.observer.faults.empty());
    }
}

TEST_CASE("FollowPolicyTest.asyncErrorPollRunsEveryIteration") // N-4
{
    SUBCASE("an error that arrives before the replay is not charged to it")
    {
        Harness h;
        h.archive.appendTrades(h.archive.record(), 1, 3, EPOCH);
        h.attach();
        h.step(); // connected
        h.archive.pendingAsyncError = "a late answer to an earlier request";
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
        h.run(20);
        CHECK(h.observer.faults.empty());
    }
    SUBCASE("a lost control session while backing off ends the connection")
    {
        Harness h;
        auto &r = h.archive.record();
        h.archive.appendTrades(r, 1, 2, EPOCH);
        h.archive.stop(r);
        h.attach();
        REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 2; }, 100));
        h.run(5); // the chain is exhausted: backing off
        const int disconnects = h.archive.disconnects;
        h.archive.controlSessionLost = true;
        h.run(5);
        CHECK(h.observer.sawFault(FaultKind::ARCHIVE_GONE));
        CHECK(h.archive.disconnects > disconnects);
    }
}

TEST_CASE("FollowPolicyTest.detachWhileConnectingCancelsTheAsyncConnect") // N-8
{
    Harness h;
    h.archive.connectPendingSteps = 1'000'000;
    h.attach();
    h.run(5);
    REQUIRE(h.policy->phase() == Phase::CONNECTING);
    h.policy->detach(h.now);
    h.run(2);
    CHECK(h.policy->idle());
    CHECK(h.archive.disconnects >= 1);
}
