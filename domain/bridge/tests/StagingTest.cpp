// StagingTest (RC2 S9b 13.2, R-B3-1..3, K3): the staging decision of 6.1 as the live domain runs it.
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

}

TEST_CASE("StagingTest.recordBytesReachTheEncoderUnchanged")
{
    Direct d;
    bindAt(d, 10);
    std::vector<Bytes> sent;
    for (std::int64_t s = 11; s <= 14; ++s)
    {
        Bytes b = makeFrame(s == 13 ? TEMPLATE_TERMINAL : TEMPLATE_TRADE, s, EPOCH, 64 * s);
        for (std::size_t i = 32; i < b.size(); ++i)
        {
            b[i] = static_cast<std::uint8_t>(i * 7 + s); // every body byte distinct, the last one non-zero
        }
        sent.push_back(b);
        REQUIRE(d.frame(b) == SinkAction::CONTINUE);
    }
    d.poll(0);
    const std::vector<Bytes> seen = d.encoder.recordsSeen;
    CHECK(seen == sent);
}

TEST_CASE("StagingTest.outcomeIsForwardedAndFlushesStagedTrades")
{
    Direct d;
    bindAt(d, 10);
    CHECK(d.rec(TEMPLATE_TRADE, 11) == SinkAction::CONTINUE);
    CHECK(d.rec(TEMPLATE_TRADE, 12) == SinkAction::CONTINUE);
    CHECK(d.rec(TEMPLATE_TRADE, 13) == SinkAction::CONTINUE);
    CHECK(d.rec(TEMPLATE_OUTCOME, 14) == SinkAction::CONTINUE);
    std::vector<std::uint16_t> templates;
    for (const auto &f : d.core.offered)
    {
        if (!f.records.empty())
        {
            templates.push_back(f.moneyTemplate);
        }
    }
    CHECK(templates == std::vector<std::uint16_t>{ 62, 64 });
    CHECK(d.core.offeredSeqs() == std::vector<std::int64_t>{ 11, 12, 13, 14 });
}

TEST_CASE("StagingTest.capSixtyFourSplits")
{
    Direct d;
    bindAt(d, 10);
    for (std::int64_t s = 11; s < 11 + 65; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    d.poll(0);
    std::vector<std::uint16_t> templates;
    std::vector<std::size_t> sizes;
    for (const auto &f : d.core.offered)
    {
        if (!f.records.empty())
        {
            templates.push_back(f.moneyTemplate);
            sizes.push_back(f.records.size());
        }
    }
    CHECK(templates == std::vector<std::uint16_t>{ 62, 61 });
    CHECK(sizes == std::vector<std::size_t>{ 64, 1 });
}

TEST_CASE("StagingTest.emptyPollFlushes")
{
    Direct d;
    bindAt(d, 10);
    for (std::int64_t s = 11; s <= 15; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    d.poll(5); // a non-empty poll keeps the partial batch
    CHECK(d.core.feedOffers() == 0);
    d.poll(0);
    const int offers = d.core.feedOffers();
    CHECK(offers == 1);
    CHECK(d.domain->view().staged == 0);
}

TEST_CASE("StagingTest.flushBeforeHaltAndBeforeSwitch")
{
    Direct d(2);
    bindAt(d, 10);
    for (std::int64_t s = 11; s <= 13; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    // the source dies (connect level) with three trades staged: the rotation's SWITCH waits for the flush
    d.core.script = { OfferOutcome::RETRY };
    SourceFault fault;
    fault.source = 0;
    fault.kind = FaultKind::ARCHIVE_GONE;
    d.domain->onFault(fault);
    REQUIRE(d.domain->view().staged == 3);
    const SourceCommand held = d.domain->takeSourceCommand();
    CHECK(held.kind == SourceCommandKind::NONE);
    d.tick(); // the pre-switch flush is retried
    CHECK(d.domain->view().staged == 0);
    const SourceCommand cmd = d.domain->takeSourceCommand();
    CHECK(cmd.kind == SourceCommandKind::SWITCH);
    CHECK(cmd.index == 1);
    CHECK(d.core.offeredSeqs() == std::vector<std::int64_t>{ 11, 12, 13 });
}

TEST_CASE("StagingTest.backPressureKeepsEverythingAndAborts")
{
    Direct d;
    bindAt(d, 10);
    REQUIRE(d.rec(TEMPLATE_TRADE, 11) == SinkAction::CONTINUE);
    REQUIRE(d.rec(TEMPLATE_TRADE, 12) == SinkAction::CONTINUE);
    d.core.script = { OfferOutcome::RETRY };
    CHECK(d.rec(TEMPLATE_OUTCOME, 13) == SinkAction::ABORT); // the pre-outcome flush is back-pressured
    const std::size_t staged = d.domain->view().staged;
    CHECK(staged == 2);
    CHECK(d.domain->view().last == 12);
    CHECK(d.core.feedOffers() == 0);
    // the redelivered frame then goes: 62(11, 12), 64(13)
    CHECK(d.rec(TEMPLATE_OUTCOME, 13) == SinkAction::CONTINUE);
    CHECK(d.core.offeredSeqs() == std::vector<std::int64_t>{ 11, 12, 13 });
    // a single back-pressured: ABORT, last unchanged
    d.core.script = { OfferOutcome::RETRY };
    CHECK(d.rec(TEMPLATE_TERMINAL, 14) == SinkAction::ABORT);
    const std::int64_t last = d.domain->view().last;
    CHECK(last == 13);
    CHECK(d.rec(TEMPLATE_TERMINAL, 14) == SinkAction::CONTINUE);
    CHECK(d.domain->view().last == 14);
    CHECK(BridgeMetrics::get(d.metrics.backpressureAborts) == 2);
}

TEST_CASE("StagingTest.fenceDropsStagingAndNothingElse")
{
    Direct d;
    bindAt(d, 10);
    for (std::int64_t s = 11; s <= 13; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    d.domain->onFence(FenceTrigger::NEW_LEADER, d.now);
    CHECK(d.domain->view().staged == 0);
    CHECK(BridgeMetrics::get(d.metrics.stagedDropped) == 3);
    CHECK(d.core.feedOffers() == 0);
    // the new term: registration, then the report: a sync from C' = 10 with a rewind of the source
    d.domain->onSessionUp(d.now);
    d.pump();
    CHECK(d.domain->view().state == State::STREAMING);
    CHECK(d.domain->view().last == 10);
    const SourceCommand cmd = d.domain->takeSourceCommand();
    CHECK(cmd.kind == SourceCommandKind::REATTACH);
}

TEST_CASE("StagingTest.tickRetriesOnlyABackPressuredFlush")
{
    Direct d;
    bindAt(d, 10);
    for (std::int64_t s = 11; s < 11 + 40; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    d.poll(40); // a full poll: no empty-poll flush
    const int before = d.core.calls;
    d.tick();
    const int offersAfterTick = d.core.calls - before;
    CHECK(offersAfterTick == 0);
    CHECK(d.domain->view().staged == 40);
    // a back-pressured flush (the empty poll) is retried by tick
    d.core.script = { OfferOutcome::RETRY };
    d.poll(0);
    CHECK(d.domain->view().staged == 40);
    d.tick();
    CHECK(d.domain->view().staged == 0);
    REQUIRE(d.core.feedOffers() == 1);
    CHECK(d.core.offered.back().records.size() == 40);
}
