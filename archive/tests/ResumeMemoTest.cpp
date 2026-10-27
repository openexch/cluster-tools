// The per-source resume memo (RC2 S9a contract section 5, 9.1): every row of the 5.3 table, the
// 5.2 rules, and the ChainResumeMemoTest cases carried or retired (archive/tests/PARITY.md).
#include "doctest/doctest.h"

#include "SimArchive.h"
#include "TestSupport.h"

#include "cluster_tools/archive/ResumeMemo.h"

using namespace cluster_tools::archive;
using namespace ct_archive_test;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t E = 7;
constexpr std::size_t A = 0;
constexpr std::size_t B = 1;

std::vector<Recording> chain4()
{
    // ChainResumeMemoTest.chain(): stopped(0,0,64), stopped(1,0,64), stopped(2,0,320), active(3,0)
    return { stopped(0, 0, 64), stopped(1, 0, 64), stopped(2, 0, 320), active(3, 0) };
}

Peek peek(std::int64_t seq, std::int64_t epoch = E)
{
    return Peek{ TEMPLATE_TRADE, seq, epoch, 64 };
}

}

// ---- carried ChainResumeMemoTest cases

TEST_CASE("ResumeMemoTest.coldWalkFromChainStart") // walksTheWholeChainWhenNothingIsRemembered
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    const ResumePoint p = memo.resume(A);
    CHECK(p.index == 0);
    CHECK(p.position == 0);
    CHECK(p.memoFree);
    CHECK_FALSE(p.fromProof);
}

TEST_CASE("ResumeMemoTest.prefixResumePastStoppedProvenRecordings") // resumesPastTheDrainedPrefixAtTheSameSyncPoint
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    CHECK(memo.proveFromSkip(A, chain4()[0].key, peek(2), 64));
    CHECK(memo.proveFromSkip(A, chain4()[1].key, peek(4), 64));
    const ResumePoint p = memo.resume(A);
    CHECK(p.index == 2); // the last proof sits at its recording's stop: start at the next recording
    CHECK(p.position == 0);
    CHECK_FALSE(p.fromProof);
    CHECK_FALSE(p.memoFree);
}

TEST_CASE("ResumeMemoTest.retainedAcrossAReportAdvance") // retainsProvenPrefixWhenBothWatermarksAdvance + 14
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[2].key, peek(9), 256);
    memo.onReport(E, 112);
    memo.onListing(A, chain4());
    const ResumePoint p = memo.resume(A);
    CHECK(p.index == 2);
    CHECK(p.position == 256);
    CHECK(p.fromProof);
    CHECK(p.proofSeq == 9);
}

TEST_CASE("ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor") // keepsThePrefixProvenBeforeAForward..., forwardSeals...
{
    ResumeMemo memo(1);
    memo.onReport(E, 5);
    memo.onListing(A, { active(0, 0) });
    CHECK(memo.proveFromSkip(A, key(0), peek(5), 160));
    // seq 6 is above the core's cursor (a forward, perhaps unacknowledged): it never proves.
    CHECK_FALSE(memo.proveFromSkip(A, key(0), peek(6), 192));
    CHECK(memo.proof(A, key(0))->endPosition == 160);
    // The core never applied 6: the next report keeps C = 5 and the resume is at the seq-5 proof.
    memo.onReport(E, 5);
    CHECK(memo.resume(A).position == 160);
    // another epoch's frame never proves either
    CHECK_FALSE(memo.proveFromSkip(A, key(0), peek(3, E + 1), 96));
}

TEST_CASE("ResumeMemoTest.unboundCoreProvesNothing")
{
    ResumeMemo memo(1);
    memo.onReport(0, 0);
    memo.onListing(A, { active(0, 0) });
    CHECK_FALSE(memo.proveFromSkip(A, key(0), peek(1, 0), 32));
    CHECK_FALSE(memo.prove(A, key(0), 1, 0, 32));
    CHECK(memo.proofCount(A) == 0);
}

TEST_CASE("ResumeMemoTest.lastRecordingWithAProofWins") // CHANGE of doesNotRememberAGapInTheDrainedPrefix
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[2].key, peek(30), 160);
    const ResumePoint p = memo.resume(A);
    CHECK(p.index == 2);
    CHECK(p.position == 160);
    CHECK_FALSE(p.memoFree); // a hole seen on this read is verified from the chain start (5.2 rule 5)
}

TEST_CASE("ResumeMemoTest.historyChange") // wiped archive / id reuse / changed incarnation (cases 7, 16, 18)
{
    SUBCASE("a stopped recording's stop changed (re-genesised archive, same ids)")
    {
        ResumeMemo memo(1);
        memo.onReport(E, 100);
        memo.onListing(A, chain4());
        memo.proveFromSkip(A, chain4()[1].key, peek(4), 64);
        const auto out = memo.onListing(A, { stopped(0, 0, 999), stopped(1, 0, 12), active(2, 0) });
        CHECK(out.historyChanged);
        CHECK(memo.resume(A).memoFree);
    }
    SUBCASE("a recording id reappears with another incarnation")
    {
        for (int variant = 0; variant < 3; ++variant)
        {
            CAPTURE(variant);
            ResumeMemo memo(1);
            memo.onReport(E, 100);
            memo.onListing(A, chain4());
            memo.proveFromSkip(A, chain4()[3].key, peek(20), 160);
            auto reborn = chain4();
            if (variant == 0) reborn[3].key.startTimestamp += 1;
            if (variant == 1) reborn[3].key.initialTermId += 1;
            if (variant == 2) reborn[3].key.sessionId += 1;
            CHECK(memo.onListing(A, reborn).historyChanged);
            CHECK(memo.proofCount(A) == 0);
            // the old descriptors reappearing never restore the old marks
            CHECK(memo.onListing(A, chain4()).historyChanged);
            CHECK(memo.resume(A).memoFree);
        }
    }
    SUBCASE("a stopped recording is active again")
    {
        ResumeMemo memo(1);
        memo.onReport(E, 100);
        memo.onListing(A, chain4());
        memo.proveFromSkip(A, chain4()[2].key, peek(9), 256);
        auto again = chain4();
        again[2].stopPosition = NULL_POSITION;
        CHECK(memo.onListing(A, again).historyChanged);
    }
}

TEST_CASE("ResumeMemoTest.middleRecordingGoneIsHistoryChange") // and the shrunk chain (case 9)
{
    ResumeMemo memo(1);
    ArchiveMetrics metrics;
    ResumeMemo counted(1, &metrics);
    for (ResumeMemo *m : { &memo, &counted })
    {
        m->onReport(E, 100);
        m->onListing(A, chain4());
        m->proveFromSkip(A, chain4()[2].key, peek(9), 256);
    }
    auto gap = chain4();
    gap.erase(gap.begin() + 1); // recording 1 gone, 0 still there: not retention
    CHECK(memo.onListing(A, gap).historyChanged);
    CHECK(memo.proofCount(A) == 0);
    CHECK(counted.onListing(A, { stopped(0, 0, 64) }).historyChanged); // the tail is gone
    CHECK(metrics.invalidations(MemoReason::HISTORY_CHANGED) == 1);
}

TEST_CASE("ResumeMemoTest.headRetentionTrimsProvenRecordings") // CHANGE of forgetsEverythingWhenTheDrainedPrefixWasPurged
{
    ArchiveMetrics metrics;
    ResumeMemo memo(1, &metrics);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[0].key, peek(2), 64);
    memo.proveFromSkip(A, chain4()[2].key, peek(9), 256);
    // Retention deleted recordings 0-1: their proofs go, the proof on 2 stays.
    const auto out = memo.onListing(A, { chain4()[2], chain4()[3] });
    CHECK_FALSE(out.historyChanged);
    CHECK(out.trimmedProofs == 1);
    CHECK(ArchiveMetrics::get(metrics.memoTrims) == 1);
    const ResumePoint p = memo.resume(A);
    CHECK(p.index == 0);
    CHECK(p.position == 256);
    CHECK(p.fromProof);
    // a proven recording leaving the head (case 15): back to the next recording's start
    memo.onListing(A, { chain4()[3] });
    CHECK(memo.resume(A).memoFree);
}

TEST_CASE("ResumeMemoTest.activeStartPurgeTrimsProofsOnly")
{
    ArchiveMetrics metrics;
    ResumeMemo memo(1, &metrics);
    memo.onReport(E, 100);
    memo.onListing(A, { active(0, 0) });
    memo.proveFromSkip(A, key(0), peek(40), 1 << 20);
    SUBCASE("purge below the proof keeps it (the Java key lost it)")
    {
        CHECK_FALSE(memo.onListing(A, { active(0, 1 << 19) }).historyChanged);
        CHECK(memo.resume(A).position == (1 << 20));
        CHECK(ArchiveMetrics::get(metrics.memoTrims) == 0);
    }
    SUBCASE("purge above the proof trims it")
    {
        const auto out = memo.onListing(A, { active(0, 1 << 21) });
        CHECK_FALSE(out.historyChanged);
        CHECK(out.trimmedProofs == 1);
        const ResumePoint p = memo.resume(A);
        CHECK(p.position == (1 << 21));
        CHECK(p.memoFree); // index 0 at its (new) start
    }
    SUBCASE("start moving backwards is a history change")
    {
        memo.onListing(A, { active(0, 1 << 19) });
        CHECK(memo.onListing(A, { active(0, 0) }).historyChanged);
    }
}

TEST_CASE("ResumeMemoTest.highWaterOnlyAdvances") // theSkippedHeadHighWaterOnlyEverAdvances + 11, 12
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, { stopped(5, 128, 640) });
    CHECK(memo.resume(A).position == 128);
    memo.proveFromSkip(A, key(5), peek(8), 256);
    CHECK(memo.resume(A).position == 256);
    memo.proveFromSkip(A, key(5), peek(4), 160); // a later read that got less far never rewinds the mark
    CHECK(memo.proof(A, key(5))->endPosition == 256);
    CHECK(memo.proof(A, key(5))->seq == 8);
}

TEST_CASE("ResumeMemoTest.activeToStoppedPreservesTheMark") // activeToStoppedPreservesByteMark...
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, { active(0, 0) });
    memo.proveFromSkip(A, key(0), peek(3), 160);
    ResumePoint p = memo.resume(A);
    CHECK(p.fromProof);
    CHECK(p.position == 160);
    CHECK_FALSE(memo.onListing(A, { stopped(0, 0, 160) }).historyChanged);
    p = memo.resume(A);
    CHECK(p.index == 1); // stopped exactly at the proof: drained, next recording
    CHECK_FALSE(memo.onListing(A, { stopped(0, 0, 160), active(1, 0) }).historyChanged);
    CHECK(memo.resume(A).index == 1);
}

TEST_CASE("ResumeMemoTest.stopBeforeTheRememberedFrontierIsHistoryChange") // stopBeforeRememberedActiveFrontier...
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[3].key, peek(20), 160);
    auto truncated = chain4();
    truncated[3].stopPosition = 96;
    CHECK(memo.onListing(A, truncated).historyChanged);
    CHECK(memo.resume(A).memoFree);
}

TEST_CASE("ResumeMemoTest.emptyThenExhaustedChainGainsSuccessor") // emptyThenFullySkippedStoppedChain...
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    const std::vector<Recording> stoppedChain{ stopped(0, 0, 0), stopped(1, 0, 160) };
    memo.onListing(A, stoppedChain);
    memo.proveFromSkip(A, key(1), peek(4), 160);
    for (int epoch = 0; epoch < 5; ++epoch)
    {
        memo.onListing(A, stoppedChain);
        CHECK(memo.resume(A).index == 2); // exhausted: the walk backs off (4.5 row 15)
    }
    memo.onListing(A, { stoppedChain[0], stoppedChain[1], active(2, 0) });
    const ResumePoint p = memo.resume(A);
    CHECK(p.index == 2);
    CHECK(p.position == 0);
}

TEST_CASE("ResumeMemoTest.repeatedFailuresKeepResumingForward") // keepsResumingAcrossRepeatedFailuresAtTheSameSyncPoint
{
    ResumeMemo memo(1);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[1].key, peek(4), 64);
    CHECK(memo.resume(A).index == 2);
    memo.proveFromSkip(A, chain4()[2].key, peek(9), 320);
    CHECK(memo.resume(A).index == 3); // straight to the active recording
}

// ---- 5.3 rows 1, 2 (either cursor regression; another epoch)

TEST_CASE("ResumeMemoTest.epochChangeInvalidatesEverySource")
{
    ArchiveMetrics metrics;
    ResumeMemo memo(2, &metrics);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.onListing(B, chain4());
    memo.proveFromSkip(A, chain4()[2].key, peek(9), 256);
    memo.proveFromSkip(B, chain4()[2].key, peek(9), 288);
    memo.onReport(E + 1, 100);
    CHECK(memo.proofCount(A) == 0);
    CHECK(memo.proofCount(B) == 0);
    CHECK(metrics.invalidations(MemoReason::EPOCH_CHANGED) == 2);
    // a report with epoch 0 (the core lost its binding) is an epoch change too
    memo.proveFromSkip(A, chain4()[2].key, peek(9, E + 1), 256);
    memo.onReport(0, 0);
    CHECK(memo.proofCount(A) == 0);
}

TEST_CASE("ResumeMemoTest.cursorRegressionInvalidatesEverySource")
{
    ArchiveMetrics metrics;
    ResumeMemo memo(2, &metrics);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.onListing(B, chain4());
    memo.proveFromSkip(A, chain4()[2].key, peek(90), 256);
    memo.proveFromSkip(B, chain4()[2].key, peek(30), 288);
    memo.onReport(E, 85); // C' >= every proof of B but < maxProven
    CHECK(memo.proofCount(A) == 0);
    CHECK(memo.proofCount(B) == 0);
    CHECK(metrics.invalidations(MemoReason::CURSOR_REGRESSED) == 2);
    // a report at exactly maxProven keeps the memo
    CHECK(memo.proveFromSkip(A, chain4()[2].key, peek(80), 256));
    memo.onReport(E, 80);
    CHECK(memo.proofCount(A) == 1);
}

// ---- 5.2 rules 2 and the per-source memo (F9, M3)

TEST_CASE("ResumeMemoTest.ringProofLandsOnItsOwnSourceOnly")
{
    ResumeMemo memo(2);
    memo.onReport(E, 0);
    memo.onListing(A, { active(0, 0) });
    memo.onListing(B, { active(0, 0) }); // identical descriptors on another archive
    memo.onReport(E, 10);
    CHECK(memo.prove(B, key(0), 10, E, 416));
    CHECK(memo.proofCount(A) == 0);
    CHECK(memo.resume(A).position == 0);
    CHECK(memo.resume(B).position == 416);
    // a ring proof above the cursor, of another epoch, or unaligned is refused
    CHECK_FALSE(memo.prove(A, key(0), 11, E, 448));
    CHECK_FALSE(memo.prove(A, key(0), 9, E + 1, 384));
    CHECK_FALSE(memo.prove(A, key(0), 9, E, 390));
}

TEST_CASE("ResumeMemoTest.perSourceMemoSurvivesFailBack")
{
    ResumeMemo memo(2);
    memo.onReport(E, 2);
    // A and B share recording ids and starts but not byte histories (PAD on B).
    memo.onListing(A, { active(0, 0) });
    memo.onListing(B, { active(0, 0) });
    memo.proveFromSkip(A, key(0), peek(2), 320);
    CHECK(memo.resume(A).position == 320);
    // failover to B: B resumes at its own (empty) memo, A's position is never used on B
    CHECK(memo.resume(B).position == 0);
    memo.proveFromSkip(B, key(0), peek(2), 416);
    memo.onListing(B, {}); // an empty listing on B does not touch A (retired "emptyChainOnAnotherSource")
    // fail back to A: A's proof is still there
    memo.onListing(A, { active(0, 0) });
    CHECK(memo.resume(A).position == 320);
}

TEST_CASE("ResumeMemoTest.sourceFaultsInvalidateTheSourceOnly")
{
    ArchiveMetrics metrics;
    ResumeMemo memo(2, &metrics);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.onListing(B, chain4());
    memo.proveFromSkip(A, chain4()[2].key, peek(9), 256);
    memo.proveFromSkip(B, chain4()[2].key, peek(9), 256);
    memo.invalidate(A, MemoReason::SEQ_REGRESSION);
    CHECK(memo.proofCount(A) == 0);
    CHECK(memo.proofCount(B) == 1);
    memo.invalidate(B, MemoReason::RESUME_BELT);
    CHECK(memo.proofCount(B) == 0);
    CHECK(metrics.invalidations(MemoReason::SEQ_REGRESSION) == 1);
    CHECK(metrics.invalidations(MemoReason::RESUME_BELT) == 1);
}

// ---- 5.2 rule 5 hole verification

TEST_CASE("ResumeMemoTest.holeAfterMemoResumeIsVerifiedFromChainStart")
{
    ArchiveMetrics metrics;
    ResumeMemo memo(1, &metrics);
    memo.onReport(E, 10);
    memo.beginSync();
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[3].key, peek(10), 480);
    const ResumePoint p = memo.resume(A);
    REQUIRE_FALSE(p.memoFree);
    // a hole at e = 11 seen on this memo read is not believed: the memo is invalidated and the
    // next read starts at the chain start
    CHECK(memo.onHole(A, 11, p.memoFree) == HoleDecision::VERIFY_FROM_CHAIN_START);
    CHECK(memo.proofCount(A) == 0);
    CHECK(metrics.invalidations(MemoReason::HOLE_VERIFY) == 1);
    const ResumePoint again = memo.resume(A);
    CHECK(again.memoFree);
    CHECK(again.index == 0);
    // the memo-free read sees the same hole: believed
    CHECK(memo.onHole(A, 11, again.memoFree) == HoleDecision::BELIEVE);
}

TEST_CASE("ResumeMemoTest.holeVerifiedOncePerSourceAndSeq")
{
    ArchiveMetrics metrics;
    ResumeMemo memo(2, &metrics);
    memo.onReport(E, 10);
    memo.beginSync();
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[3].key, peek(10), 480);
    CHECK(memo.onHole(A, 11, false) == HoleDecision::VERIFY_FROM_CHAIN_START);
    // while the verification is pending the source's reads are memo-free, even with new proofs
    memo.proveFromSkip(A, chain4()[3].key, peek(10), 480);
    CHECK(memo.holeVerificationPending(A));
    CHECK(memo.resume(A).memoFree);
    CHECK(memo.onHole(A, 11, false) == HoleDecision::VERIFY_FROM_CHAIN_START);
    CHECK(metrics.invalidations(MemoReason::HOLE_VERIFY) == 1); // once per (source, e) per sync
    // another source, or another e, is its own verification
    memo.onListing(B, chain4());
    memo.proveFromSkip(B, chain4()[3].key, peek(10), 480);
    CHECK(memo.onHole(B, 11, false) == HoleDecision::VERIFY_FROM_CHAIN_START);
    CHECK(metrics.invalidations(MemoReason::HOLE_VERIFY) == 2);
    // a memo-free pass that reaches e clears the marker: the memo is usable again
    memo.onFrameRead(A, 11, true);
    CHECK_FALSE(memo.holeVerificationPending(A));
    CHECK_FALSE(memo.resume(A).memoFree);
    // a new sync clears every marker
    memo.beginSync();
    CHECK_FALSE(memo.holeVerificationPending(B));
}

// ---- 4.3 rules, through the follow policy on the simulated archive

TEST_CASE("ResumeMemoTest.rejectedRememberedPositionClearsProofs")
{
    // 96 is 32-aligned but sits inside the first trade frame (BridgeEndToEndTest.java:741 shape).
    Harness h;
    h.memo->onReport(9, 2);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 2, 9);
    h.memo->onListing(0, { r.rec });
    h.memo->injectProofForTest(0, r.rec.key, Proof{ 1, 9, 96 });
    h.attach();
    h.runUntil([&] { return h.observer.sawFault(FaultKind::POSITION_REJECTED); }, 100);
    REQUIRE(h.observer.sawFault(FaultKind::POSITION_REJECTED));
    CHECK(h.metrics.invalidations(MemoReason::POSITION_REJECTED) == 1);
    CHECK(h.metrics.openFailures(OpenFailure::INVALID_POSITION) >= 1);
    // the next attach reads both intact frames from the catalog start: no data lost
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 2; }, 3000));
    CHECK(h.sink.seqs == std::vector<std::int64_t>{ 1, 2 });
    CHECK(h.archive.requests.back().from == 0);
}

TEST_CASE("ResumeMemoTest.capacityRejectionKeepsProofs")
{
    Harness h;
    h.memo->onReport(9, 1);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, 9);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 3; }, 100));
    REQUIRE(h.memo->proof(0, r.rec.key));
    const Proof proven = *h.memo->proof(0, r.rec.key);
    CHECK(proven.endPosition == 160); // the real boundary after the first trade
    h.archive.startError = ArchiveErrorKind::MAX_REPLAYS;
    h.archive.startErrorCount = 1;
    h.policy->reattach(h.now);
    h.runUntil([&] { return h.observer.sawFault(FaultKind::REPLAY_OPEN); }, 100);
    REQUIRE(h.observer.sawFault(FaultKind::REPLAY_OPEN));
    CHECK(h.metrics.openFailures(OpenFailure::MAX_REPLAYS) == 1);
    REQUIRE(h.memo->proof(0, r.rec.key)); // resource pressure does not disprove valid byte history
    CHECK(*h.memo->proof(0, r.rec.key) == proven);
    const std::size_t attaches = h.observer.attaches.size();
    REQUIRE(h.runUntil([&] { return h.observer.attaches.size() > attaches; }, 3000));
    CHECK(h.observer.attaches.back().from == 160); // resume at the proof once the slot frees
}

TEST_CASE("ResumeMemoTest.unalignedPositionRejectedBeforeRequest")
{
    Harness h;
    h.memo->onReport(9, 2);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 2, 9);
    h.memo->onListing(0, { r.rec });
    h.memo->injectProofForTest(0, r.rec.key, Proof{ 1, 9, 100 });
    h.attach();
    h.runUntil([&] { return h.observer.sawFault(FaultKind::POSITION_REJECTED); }, 100);
    REQUIRE(h.observer.sawFault(FaultKind::POSITION_REJECTED));
    CHECK(h.archive.startCalls == 0); // no request sent: the archive would answer GENERIC forever
    CHECK(h.memo->proofCount(0) == 0);
}

TEST_CASE("ResumeMemoTest.purgeRaceOnInvalidPositionTrimsNotInvalidates")
{
    // The listing says start 0, the proof is at 160, a purge to 192 lands between the listing and
    // startReplay: INVALID_POSITION, re-list once, the start moved past `from`: trim, resume at the
    // new start, the memo is not invalidated.
    Harness h;
    h.memo->onReport(9, 1);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 6, 9);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 6; }, 100));
    REQUIRE(h.memo->proof(0, r.rec.key));
    CHECK(h.memo->proof(0, r.rec.key)->endPosition == 160);
    bool purged = false;
    h.archive.onStartReplay = [&] {
        if (!purged)
        {
            purged = true;
            SimRecording *rec = h.archive.find(0);
            rec->rec.startPosition = 480; // retention purged past the proof (3 frames)
            rec->frames.erase(rec->frames.begin(), rec->frames.begin() + 3);
        }
    };
    h.policy->reattach(h.now);
    const std::size_t attaches = h.observer.attaches.size();
    REQUIRE(h.runUntil([&] { return h.observer.attaches.size() > attaches; }, 3000));
    CHECK(h.metrics.invalidations(MemoReason::POSITION_REJECTED) == 0);
    CHECK(ArchiveMetrics::get(h.metrics.memoTrims) == 1);
    CHECK(h.observer.attaches.back().from == 480);
    CHECK_FALSE(h.observer.sawFault(FaultKind::POSITION_REJECTED));
}

// ---- review fixes (S9a review.md, 2026-10-27)

TEST_CASE("ResumeMemoTest.secondPurgeRaceAfterAFaultStillTrims") // F-2, probe P2
{
    Harness h;
    h.memo->onReport(9, 6);
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 6, 9);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 6; }, 100));
    int purges = 0;
    std::int64_t purgeTo = 0;
    h.archive.onStartReplay = [&] {
        if (purgeTo > 0)
        {
            SimRecording *rec = h.archive.find(0);
            std::size_t drop = 0;
            while (drop < rec->frames.size() && rec->frames[drop].end <= purgeTo)
            {
                ++drop;
            }
            rec->rec.startPosition = purgeTo;
            rec->frames.erase(rec->frames.begin(), rec->frames.begin() + static_cast<long>(drop));
            purgeTo = 0;
            ++purges;
        }
    };
    // first race, through an owner rewind
    purgeTo = h.memo->proof(0, r.rec.key)->endPosition + 160;
    h.archive.appendTrades(r, 7, 4, 9);
    h.policy->reattach(h.now);
    REQUIRE(h.runUntil([&] { return purges == 1 && h.policy->phase() == Phase::FOLLOWING; }, 3000));
    CHECK(h.metrics.invalidations(MemoReason::POSITION_REJECTED) == 0);
    // the core advances, the read proves further
    h.memo->onReport(9, 10);
    h.archive.appendTrades(r, 11, 4, 9);
    REQUIRE(h.runUntil([&] { return h.sink.seqs.back() == 14; }, 3000));
    REQUIRE(h.memo->proof(0, r.rec.key));
    // a source fault (no owner rewind), then a second purge races the re-attach
    purgeTo = h.memo->proof(0, r.rec.key)->endPosition + 160;
    h.archive.dropImageNow = true;
    REQUIRE(h.runUntil([&] { return purges == 2; }, 5000));
    h.run(3000, 1ms);
    CHECK(h.metrics.invalidations(MemoReason::POSITION_REJECTED) == 0); // trimmed again, memo kept
    CHECK_FALSE(h.observer.sawFault(FaultKind::POSITION_REJECTED));
    CHECK(ArchiveMetrics::get(h.metrics.memoTrims) == 2);
}

TEST_CASE("ResumeMemoTest.believedHoleIsVerifiedAndStopsForcingColdWalks") // F-3, probe P3; amended 5.2 rule 5
{
    ArchiveMetrics metrics;
    ResumeMemo memo(2, &metrics);
    memo.onReport(9, 10);
    memo.beginSync();
    const std::vector<Recording> chain{ stopped(0, 0, 640), active(1, 640) };
    memo.onListing(0, chain);
    memo.proveFromSkip(0, chain[1].key, Peek{ 1, 10, 9, 0 }, 960);
    REQUIRE_FALSE(memo.resume(0).memoFree);
    CHECK(memo.onHole(0, 11, false) == HoleDecision::VERIFY_FROM_CHAIN_START);
    CHECK(memo.holeVerificationPending(0));
    CHECK(memo.resume(0).memoFree); // pending: the verification read is memo-free
    CHECK(memo.onHole(0, 11, true) == HoleDecision::BELIEVE); // the memo-free read confirms it
    CHECK_FALSE(memo.holeVerificationPending(0));
    CHECK(memo.holeVerified(0, 11));
    // the owner failed over; source 0 re-proves on a later read of the same sync
    memo.onReport(9, 20);
    memo.proveFromSkip(0, chain[1].key, Peek{ 1, 20, 9, 0 }, 1280);
    const ResumePoint p = memo.resume(0);
    CHECK_FALSE(p.memoFree); // no forced cold walk any more
    CHECK(p.position == 1280);
    // a later memo-read hole at the same (source, e) is believed, without a second invalidation
    CHECK(memo.onHole(0, 11, false) == HoleDecision::BELIEVE);
    CHECK(metrics.invalidations(MemoReason::HOLE_VERIFY) == 1);
    // another e is its own verification; a new sync clears the verified set
    CHECK(memo.onHole(0, 30, false) == HoleDecision::VERIFY_FROM_CHAIN_START);
    CHECK(metrics.invalidations(MemoReason::HOLE_VERIFY) == 2);
    memo.beginSync();
    CHECK_FALSE(memo.holeVerified(0, 11));
}

TEST_CASE("ResumeMemoTest.wipedToAnEmptyChainIsHistoryChange") // N-5
{
    ArchiveMetrics metrics;
    ResumeMemo memo(1, &metrics);
    memo.onReport(E, 100);
    memo.onListing(A, chain4());
    memo.proveFromSkip(A, chain4()[2].key, peek(9), 256);
    const auto out = memo.onListing(A, {});
    CHECK(out.historyChanged);
    CHECK(metrics.invalidations(MemoReason::HISTORY_CHANGED) == 1);
    CHECK(ArchiveMetrics::get(metrics.memoTrims) == 0);
}
