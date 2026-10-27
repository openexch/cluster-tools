// The 4.2 recording validity table and the chain listing (RC2 S9a contract 9.1). Ports of
// BridgeAgentEmptyRecordingTest (rows 1-3) plus the new MTU row and its N4 ordering.
#include "doctest/doctest.h"

#include "SimArchive.h"
#include "TestSupport.h"

#include "cluster_tools/archive/Chain.h"

using namespace cluster_tools::archive;
using namespace ct_archive_test;

namespace {
constexpr std::int32_t MTU = 1408;
}

TEST_CASE("ChainModelTest.stoppedEmptyRecordingIsSkipped")
{
    CHECK(classifyRecording(stopped(0, 0, 0), 0, MTU) == RecordingVerdict::EMPTY);
    CHECK(classifyRecording(stopped(1, 128, 128), 128, MTU) == RecordingVerdict::EMPTY);

    // The 09-02 bug C shape end to end: an empty stopped genesis recording, then data. No [0, 0)
    // replay is ever requested and the data of the successor is delivered.
    Harness h;
    h.archive.stop(h.archive.record());
    auto &data = h.archive.record();
    h.archive.appendTrades(data, 1, 6, 9);
    h.attach();
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 6; }, 200));
    for (const auto &r : h.archive.requests)
    {
        CHECK(r.recordingId == 1);
        CHECK(r.length != 0);
    }
    CHECK(ArchiveMetrics::get(h.metrics.emptyRecordingsSkipped) == 1);
}

TEST_CASE("ChainModelTest.stoppedRecordingWithBytesIsFollowed")
{
    CHECK(classifyRecording(stopped(0, 0, 160), 0, MTU) == RecordingVerdict::REPLAY_BOUNDED);
    CHECK(classifyRecording(stopped(0, 64, 320), 64, MTU) == RecordingVerdict::REPLAY_BOUNDED);
    CHECK(classifyRecording(stopped(0, 0, 320), 160, MTU) == RecordingVerdict::REPLAY_BOUNDED);
}

TEST_CASE("ChainModelTest.activeRecordingIsNeverSkipped")
{
    CHECK(classifyRecording(active(0, 0), 0, MTU) == RecordingVerdict::FOLLOW_LIVE);
    CHECK(classifyRecording(active(0, 640), 640, MTU) == RecordingVerdict::FOLLOW_LIVE);
    CHECK(classifyRecording(active(0, 0), 4096, MTU) == RecordingVerdict::FOLLOW_LIVE);
}

TEST_CASE("ChainModelTest.malformedStoppedExtentIsNotSilentlySkipped")
{
    // ChainResumeMemoTest.malformedBoundsRemainVisibleEvenIfTheRecordingLooksEmpty
    CHECK(classifyRecording(stopped(0, -2, -2), -2, MTU) == RecordingVerdict::MALFORMED_EXTENT);
    CHECK(classifyRecording(stopped(0, 160, 96), 160, MTU) == RecordingVerdict::MALFORMED_EXTENT);
    Recording zeroTerm = stopped(0, 0, 0);
    zeroTerm.key.termBufferLength = 0;
    CHECK(classifyRecording(zeroTerm, 0, MTU) == RecordingVerdict::MALFORMED_EXTENT);
    Recording zeroMtu = active(0, 0);
    zeroMtu.key.mtuLength = 0;
    CHECK(classifyRecording(zeroMtu, 0, MTU) == RecordingVerdict::MALFORMED_EXTENT);

    // Loud at the walk: a fault, never a skip.
    Harness h;
    auto &bad = h.archive.record(-2);
    bad.rec.stopPosition = -2;
    h.attach();
    h.run(20);
    CHECK(h.observer.sawFault(FaultKind::MALFORMED_EXTENT));
    CHECK(h.archive.startCalls == 0);
}

TEST_CASE("ChainModelTest.mtuMismatchRefusesTheSource")
{
    CHECK(classifyRecording(stopped(0, 0, 160, 4096), 0, MTU) == RecordingVerdict::MTU_MISMATCH);
    CHECK(classifyRecording(active(0, 0, 4096), 0, MTU) == RecordingVerdict::MTU_MISMATCH);

    // The source stays refused (gauge 1, no replay) until its chain changes.
    Harness h;
    h.archive.mtu = 4096;
    auto &r = h.archive.record();
    h.archive.appendTrades(r, 1, 3, 9);
    h.attach();
    h.run(3000);
    CHECK(h.observer.refusedSources == 1);
    CHECK(h.archive.startCalls == 0);
    CHECK(ArchiveMetrics::get(h.metrics.sourceRefusedMtu[0]) == 1);
    CHECK(h.policy->phase() == Phase::REFUSED);
    // Still refused while the chain is unchanged, re-listed once per back-off.
    CHECK(h.archive.listCalls <= 5);
    // The chain changes (the 4096 recording retired, a new one at the journal MTU): the refusal lifts.
    h.archive.recordings.pop_front();
    h.archive.mtu = 1408;
    h.archive.appendTrades(h.archive.record(), 1, 2, 9);
    REQUIRE(h.runUntil([&] { return h.sink.seqs.size() == 2; }, 2000));
    CHECK(ArchiveMetrics::get(h.metrics.sourceRefusedMtu[0]) == 0);
}

TEST_CASE("ChainModelTest.emptyOrDrainedNeverRefusesForMtu")
{
    // Review N4: a recording that needs no replay can never refuse a source.
    CHECK(classifyRecording(stopped(0, 0, 0, 4096), 0, MTU) == RecordingVerdict::EMPTY);
    CHECK(classifyRecording(stopped(0, 0, 160, 4096), 160, MTU) == RecordingVerdict::DRAINED);
}

TEST_CASE("ChainModelTest.drainedStoppedRecordingOpensNoZeroLengthReplay")
{
    CHECK(classifyRecording(stopped(0, 0, 320), 320, MTU) == RecordingVerdict::DRAINED);
    CHECK(classifyRecording(stopped(0, 64, 64 + 160), 64 + 160, MTU) == RecordingVerdict::DRAINED);
}

TEST_CASE("ChainModelTest.foreignStreamDescriptorDropped")
{
    Recording foreign = stopped(1, 0, 160);
    foreign.key.streamId = 4002;
    const ChainListing listing = buildChain({ active(3, 0), foreign, stopped(0, 0, 64), stopped(2, 0, 96) }, 4001);
    REQUIRE(listing.recordings.size() == 3);
    CHECK(listing.droppedForeignStream == 1);
    CHECK(listing.recordings[0].key.recordingId == 0);
    CHECK(listing.recordings[1].key.recordingId == 2);
    CHECK(listing.recordings[2].key.recordingId == 3);
}

TEST_CASE("ChainModelTest.resumePositionOutsideTheExtentIsRejectedNotSkipped")
{
    CHECK(classifyRecording(stopped(0, 0, 320), 352, MTU) == RecordingVerdict::RESUME_OUTSIDE);
    CHECK(classifyRecording(active(0, 640), 320, MTU) == RecordingVerdict::RESUME_OUTSIDE);
}

TEST_CASE("ChainModelTest.storageRequirementIsThreeTermsPlusHeadroom")
{
    CHECK(replayStorageRequirement(64 * 1024 * 1024) == 3ULL * 64 * 1024 * 1024 + 16ULL * 1024 * 1024);
    CHECK(replayStorageRequirement(65536) == 3ULL * 65536 + 16ULL * 1024 * 1024);
}
