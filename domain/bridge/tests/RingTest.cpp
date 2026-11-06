// RingTest (RC2 S9b 13.3, R-B4-4): the in-flight ring, its proofs (S9a 5.2 rule 2) and the key table.
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

using namespace ct_bridge_test;

namespace {

void bindAt(Direct &d, std::int64_t c)
{
    d.core.bound = EPOCH;
    d.core.cursor = c;
    d.start();
    REQUIRE(d.domain->view().state == State::STREAMING);
}

void list(Direct &d, std::size_t source, std::int64_t recordings)
{
    std::vector<Recording> chain;
    for (std::int64_t id = 0; id < recordings; ++id)
    {
        chain.push_back(active(id, 0));
    }
    d.memo->onListing(source, chain);
}

// the periodic report after 10 s
void periodicReport(Direct &d)
{
    d.tick(10'001ms);
    d.pump();
}

std::int64_t proofSeq(const Direct &d, std::size_t source, std::int64_t recording)
{
    const auto p = d.memo->proof(source, key(recording));
    return p ? p->seq : 0;
}

}

TEST_CASE("RingTest.memoProvesAllKinds")
{
    Direct d;
    list(d, 0, 6);
    bindAt(d, 10);
    // one recording per kind: 61 (rec 0), a 62 of two (rec 1), 63 (rec 2), 64 (rec 3), 67 (rec 4), 68 (rec 5)
    REQUIRE(d.frame(makeFrame(TEMPLATE_TRADE, 11, EPOCH, 64 * 11), 0, 0) == SinkAction::CONTINUE);
    d.poll(0);
    REQUIRE(d.frame(makeFrame(TEMPLATE_TRADE, 12, EPOCH, 64 * 12), 0, 1) == SinkAction::CONTINUE);
    REQUIRE(d.frame(makeFrame(TEMPLATE_TRADE, 13, EPOCH, 64 * 13), 0, 1) == SinkAction::CONTINUE);
    REQUIRE(d.frame(makeFrame(TEMPLATE_TERMINAL, 14, EPOCH, 64 * 14), 0, 2) == SinkAction::CONTINUE);
    REQUIRE(d.frame(makeFrame(TEMPLATE_OUTCOME, 15, EPOCH, 64 * 15), 0, 3) == SinkAction::CONTINUE);
    Bytes release = makeRelease(16, EPOCH, 3);
    putLe64(release, FRAME_OFFSET_EGRESS_SEQ, 64 * 16); // egressSeq ascends with the journal (D1 row 6)
    REQUIRE(d.frame(release, 0, 4) == SinkAction::CONTINUE);
    REQUIRE(d.frame(makeFrame(TEMPLATE_CONFIG, 17, EPOCH, 64 * 17), 0, 5) == SinkAction::CONTINUE);
    REQUIRE(d.domain->ring().size() == 7);
    periodicReport(d);
    CHECK(d.domain->ring().size() == 0);
    std::vector<std::int64_t> proved;
    for (std::int64_t r = 0; r < 6; ++r)
    {
        proved.push_back(proofSeq(d, 0, r));
    }
    CHECK(proved == std::vector<std::int64_t>{ 11, 13, 14, 15, 16, 17 });
}

TEST_CASE("RingTest.ringProofLandsOnItsOwnSourceOnly")
{
    Direct d(2);
    list(d, 0, 1);
    list(d, 1, 1);
    bindAt(d, 10);
    d.attach(1, 0); // a failover: B attached
    for (std::int64_t s = 11; s <= 14; ++s)
    {
        REQUIRE(d.frame(makeFrame(TEMPLATE_TERMINAL, s, EPOCH, 64 * s), 1, 0) == SinkAction::CONTINUE);
    }
    d.attach(0, 0); // a fail-back before the report pops the run read from B
    periodicReport(d);
    const std::int64_t onA = proofSeq(d, 0, 0);
    CHECK(onA == 0);
    CHECK(proofSeq(d, 1, 0) == 14);
}

TEST_CASE("RingTest.ringOverflowStopsReading")
{
    Direct d(1, [](BridgeSettings &s) { s.ringCapacity = 128; });
    bindAt(d, 10);
    const int queriesBefore = d.core.queries;
    int offered = 0;
    for (std::int64_t s = 11; s < 11 + 200; ++s)
    {
        if (d.rec(TEMPLATE_TERMINAL, s) != SinkAction::CONTINUE)
        {
            break;
        }
        ++offered;
    }
    CHECK(offered <= 65); // full = fewer than 64 free slots
    CHECK(d.domain->fragmentLimit() == 0);
    CHECK(d.core.queries == queriesBefore + 1); // an immediate query
    CHECK(BridgeMetrics::get(d.metrics.ringFullPauses) >= 1);
    d.pump(); // the report pops the ring
    CHECK(d.domain->ring().size() == 0);
    CHECK(d.domain->fragmentLimit() == 32);
}

TEST_CASE("RingTest.syncStartKeepsProofsBelowCursorDropsTheRest")
{
    Direct d;
    list(d, 0, 1);
    bindAt(d, 10);
    d.core.commitImmediately = false;
    for (std::int64_t s = 11; s <= 20; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TERMINAL, s) == SinkAction::CONTINUE);
    }
    d.core.commitFirst(5); // 11..15 committed
    d.core.elect();        // 16..20 lost with the old leader
    d.core.commitImmediately = true;
    d.domain->onFence(FenceTrigger::NEW_LEADER, d.now);
    d.domain->onSessionUp(d.now);
    d.pump();
    REQUIRE(d.domain->view().state == State::STREAMING);
    CHECK(d.domain->view().last == 15);
    const std::size_t ring = d.domain->ring().size();
    CHECK(ring == 0);
    CHECK(proofSeq(d, 0, 0) == 15);
}

TEST_CASE("RingTest.keyTableOverflowPauses")
{
    Direct d; // 64 keys per source in the test settings (4096 in production)
    bindAt(d, 10);
    int accepted = 0;
    for (std::int64_t r = 0; r < 65; ++r)
    {
        if (d.frame(makeFrame(TEMPLATE_TERMINAL, 11 + r, EPOCH, 64 * (11 + r)), 0, r) == SinkAction::CONTINUE)
        {
            ++accepted;
        }
    }
    CHECK(accepted == 64);
    const int limit = d.domain->fragmentLimit();
    CHECK(limit == 0);
    CHECK(d.metrics.deferred(DeferReason::KEYTABLE_FULL) == 1);
    d.pump(); // the immediate query's report empties the ring and clears the table
    CHECK(d.domain->fragmentLimit() == 32);
    CHECK(d.frame(makeFrame(TEMPLATE_TERMINAL, 75, EPOCH, 64 * 75), 0, 64) == SinkAction::CONTINUE);
}

TEST_CASE("RingTest.keyTableSurvivesStagingAcrossAPop")
{
    Direct d;
    list(d, 0, 2);
    bindAt(d, 10);
    REQUIRE(d.frame(makeFrame(TEMPLATE_TERMINAL, 11, EPOCH, 64 * 11), 0, 0) == SinkAction::CONTINUE);
    for (std::int64_t s = 12; s <= 51; ++s)
    {
        REQUIRE(d.frame(makeFrame(TEMPLATE_TRADE, s, EPOCH, 64 * s), 0, 0) == SinkAction::CONTINUE);
    }
    d.poll(40);
    periodicReport(d); // pops 11: the ring is empty, 40 trades of K1 are still staged
    REQUIRE(d.domain->ring().size() == 0);
    REQUIRE(d.domain->view().staged == 40);
    REQUIRE(d.frame(makeFrame(TEMPLATE_TERMINAL, 52, EPOCH, 64 * 52), 0, 1) == SinkAction::CONTINUE);
    periodicReport(d);
    const std::int64_t k1 = proofSeq(d, 0, 0);
    CHECK(k1 == 51);
    CHECK(proofSeq(d, 0, 1) == 52);
}

TEST_CASE("RingTest.ringFullMeansNoRoomForACappedFlush")
{
    Direct d(1, [](BridgeSettings &s) { s.ringCapacity = 128; });
    bindAt(d, 10);
    for (std::int64_t s = 11; s < 11 + 65; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TERMINAL, s) == SinkAction::CONTINUE);
    }
    REQUIRE(d.domain->ring().freeSlots() == 63);
    const int limit = d.domain->fragmentLimit();
    CHECK(limit == 0); // 63 free slots cannot take a capped 62: paused before the flush
}
