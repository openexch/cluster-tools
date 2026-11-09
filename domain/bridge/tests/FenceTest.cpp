// FenceTest and OfferOutcomeTest: the fence (the domain half)
// and the offer outcomes.
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

TEST_CASE("FenceTest.everyTriggerStopsOffersDropsStagingRequeriesAndRewinds")
{
    for (unsigned t = 0; t < static_cast<unsigned>(FenceTrigger::COUNT); ++t)
    {
        const FenceTrigger trigger = static_cast<FenceTrigger>(t);
        CAPTURE(fenceTriggerName(trigger));
        Direct d;
        bindAt(d, 10);
        for (std::int64_t s = 11; s <= 13; ++s)
        {
            REQUIRE(d.rec(TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
        }
        const std::int64_t lastQuery = d.core.offered.back().correlationId;
        d.core.egress.clear();
        d.domain->onFence(trigger, d.now);
        const int limit = d.domain->fragmentLimit();
        CHECK(limit == 0);
        CHECK(d.domain->view().state == State::SYNCING);
        CHECK(d.domain->view().staged == 0);
        CHECK(BridgeMetrics::get(d.metrics.stagedDropped) == 3);
        REQUIRE(T_QUERY_FEED_POSITION == d.core.offered.back().moneyTemplate);
        CHECK(d.core.offered.back().correlationId != lastQuery); // a fresh correlation id
        CHECK(d.rec(TEMPLATE_TRADE, 14) == SinkAction::ABORT);   // no feed offer while syncing
        if (FenceTrigger::NEW_LEADER == trigger || FenceTrigger::SESSION_REESTABLISHED == trigger)
        {
            CHECK_FALSE(d.domain->registeredThisTerm());
            d.domain->onSessionUp(d.now);
        }
        d.pump();
        CHECK(d.domain->view().state == State::STREAMING);
        CHECK(d.domain->view().last == 10);
        CHECK(d.domain->takeSourceCommand().kind == SourceCommandKind::REATTACH);
        CHECK(d.metrics.fenceCount(trigger) == 1);
        CHECK(d.core.feedOffers() == 0);
    }
}

TEST_CASE("FenceTest.feedUnboundRejectFences")
{
    Direct d;
    bindAt(d, 10);
    SettleRejected r;
    r.reasonRaw = static_cast<std::uint8_t>(RejectReason::FEED_UNBOUND);
    d.domain->onSettleRejected(r, d.now);
    CHECK(d.metrics.fenceCount(FenceTrigger::SETTLE_REJECTED_UNBOUND) == 1);
    CHECK(d.domain->view().state == State::SYNCING);
    r.reasonRaw = static_cast<std::uint8_t>(RejectReason::ME_EPOCH_MISMATCH);
    d.pump();
    d.domain->onSettleRejected(r, d.now);
    CHECK(d.metrics.fenceCount(FenceTrigger::SETTLE_REJECTED_EPOCH) == 1);
}

TEST_CASE("FenceTest.otherRejectReasonsAreCountedOnly")
{
    Direct d;
    bindAt(d, 10);
    for (std::uint8_t reason = 0; reason <= 4; ++reason)
    {
        SettleRejected r;
        r.reasonRaw = reason;
        d.domain->onSettleRejected(r, d.now);
        CHECK(BridgeMetrics::get(d.metrics.settleRejected[reason]) == 1);
    }
    CHECK(d.metrics.totalFences() == 0);
    CHECK(d.domain->view().state == State::STREAMING);
}

TEST_CASE("FenceTest.settleRejectedBurstIsOneFence")
{
    // One refusal is followed by a FEED_GAP reject per in-flight frame (AE 1447-1451): one query, one storm count
    Direct d;
    bindAt(d, 10);
    d.core.silentDrop = true; // the core refused 11 (the refusal itself left only a latch)
    for (std::int64_t s = 11; s <= 13; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TERMINAL, s) == SinkAction::CONTINUE);
    }
    d.core.silentDrop = false;
    const int queriesBefore = d.core.queries;
    for (int i = 0; i < 2000; ++i)
    {
        SettleRejected r;
        r.reasonRaw = static_cast<std::uint8_t>(RejectReason::FEED_GAP);
        r.journalPosition = 12 + i;
        r.detail = 11;
        d.domain->onSettleRejected(r, d.now);
    }
    const int queries = d.core.queries - queriesBefore;
    CHECK(queries == 1);
    CHECK(d.metrics.fenceCount(FenceTrigger::SETTLE_REJECTED_GAP) == 2000);
    d.pump();
    CHECK(d.domain->view().state == State::STREAMING);
    CHECK(d.domain->stormCount() == 1);
    CHECK(d.domain->stormCursor() == 10);
}

TEST_CASE("FenceTest.haltedFenceBelowEMinusOneRefeeds")
{
    // A fence while HALTED whose report shows C' < e - 1 (pre-halt records lost with the old leader)
    Direct d;
    bindAt(d, 10);
    REQUIRE(d.rec(TEMPLATE_TRADE, 12) == SinkAction::ABORT); // a hole at 11
    d.pump();
    REQUIRE(d.domain->view().state == State::HALTED);
    d.core.cursor = 7; // the new leader lost 8..10
    d.domain->onFence(FenceTrigger::NEW_LEADER, d.now);
    d.domain->onSessionUp(d.now);
    d.pump();
    const State state = d.domain->view().state;
    CHECK(state == State::STREAMING);
    CHECK(d.domain->view().halt == BridgeHalt::NONE);
    CHECK(d.domain->view().last == 7);
}

TEST_CASE("FenceTest.noFeedOfferBeforeRegistration")
{
    Direct d;
    bindAt(d, 10);
    d.domain->onFence(FenceTrigger::NEW_LEADER, d.now);
    d.pump(); // the new term's report is in, the registration is not
    const bool streamingBeforeRegistration = d.domain->view().state == State::STREAMING;
    CHECK_FALSE(streamingBeforeRegistration);
    CHECK(d.domain->fragmentLimit() == 0);
    d.domain->onSessionUp(d.now); // the registration of the new term is acknowledged
    d.pump();
    CHECK(d.domain->view().state == State::STREAMING);
}

TEST_CASE("OfferOutcomeTest.closedStopsReadsUntilSessionUp")
{
    Direct d;
    bindAt(d, 10);
    d.core.script = { OfferOutcome::CLOSED };
    CHECK(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::ABORT);
    CHECK(d.domain->fragmentLimit() == 0);
    CHECK(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::ABORT);
    CHECK(d.domain->fatal() == Fatal::NONE);
    d.domain->onSessionDown(d.now);
    d.domain->onSessionUp(d.now);
    d.pump();
    CHECK(d.domain->fragmentLimit() == 32);
}

TEST_CASE("OfferOutcomeTest.transportErrorIsFatal")
{
    Direct d;
    bindAt(d, 10);
    d.core.script = { OfferOutcome::REFUSED };
    CHECK(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::ABORT);
    CHECK(d.domain->fatal() == Fatal::OFFER_REFUSED);
    CHECK(d.domain->fragmentLimit() == 0);
}

TEST_CASE("OfferOutcomeTest.encoderOverflowIsFatal")
{
    Direct d;
    bindAt(d, 10);
    d.encoder.failNext = true;
    CHECK(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::ABORT);
    CHECK(d.domain->fatal() == Fatal::ENCODER_OVERFLOW);
    CHECK(d.domain->view().last == 10);
}
