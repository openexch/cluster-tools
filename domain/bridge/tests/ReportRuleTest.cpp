// ReportRuleTest: the periodic report rules of 7.2 and the query cadence.
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

void periodicReport(Direct &d)
{
    d.tick(10'001ms);
    d.pump();
}

}

TEST_CASE("ReportRuleTest.lossBelowTheQueryFences")
{
    Direct d;
    bindAt(d, 10);
    d.core.commitImmediately = false;
    for (std::int64_t s = 11; s <= 15; ++s)
    {
        REQUIRE(d.rec(TEMPLATE_TERMINAL, s) == SinkAction::CONTINUE);
    }
    d.core.commitFirst(2); // 11, 12 applied
    d.core.elect();        // 13..15 lost on the way (lost ingress)
    d.core.commitImmediately = true;
    d.tick(10'001ms); // the periodic query: ordered after every committed offer of the session
    d.pump();
    const std::uint64_t losses = d.metrics.fenceCount(FenceTrigger::REPORT_LOSS);
    CHECK(losses == 1);
    d.pump(); // the fence's query answers: a sync from C' = 12
    CHECK(d.domain->view().state == State::STREAMING);
    CHECK(d.domain->view().last == 12);
    CHECK(d.domain->takeSourceCommand().kind == SourceCommandKind::REATTACH);
}

TEST_CASE("ReportRuleTest.epochChangeFences")
{
    Direct d;
    bindAt(d, 10);
    d.core.bound = 9; // the core was re-genesised or replaced (rule 1 does not fire: C' >= coversSeq)
    periodicReport(d);
    CHECK(d.metrics.fenceCount(FenceTrigger::REPORT_EPOCH_CHANGED) == 1);
    CHECK(d.metrics.fenceCount(FenceTrigger::REPORT_LOSS) == 0);
    CHECK(d.domain->view().syncEpoch == 9);
    CHECK(d.domain->view().last == 10);
}

TEST_CASE("ReportRuleTest.cursorRegressionFences")
{
    Direct d;
    bindAt(d, 10);
    d.core.cursor = 20; // another lane is ahead (accepted, last stays 10)
    periodicReport(d);
    REQUIRE(d.domain->view().cursor == 20);
    d.core.cursor = 15; // the core was restored: below the previous report, above this lane's coversSeq
    periodicReport(d);
    CHECK(d.metrics.fenceCount(FenceTrigger::REPORT_CURSOR_REGRESSED) == 1);
    CHECK(d.metrics.fenceCount(FenceTrigger::REPORT_LOSS) == 0);
    CHECK(d.domain->view().last == 15);
}

TEST_CASE("ReportRuleTest.cursorAboveLastDoesNotMoveLast")
{
    // N = 2: another lane is ahead; this lane's forwards up to C' become core duplicates
    Direct d;
    bindAt(d, 10);
    d.core.cursor = 20;
    periodicReport(d);
    const std::int64_t last = d.domain->view().last;
    CHECK(last == 10);
    CHECK(d.domain->view().cursor == 20);
    CHECK(d.metrics.totalFences() == 0);
    CHECK(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::CONTINUE); // the forward rule is still last + 1
    CHECK(d.core.offeredSeqs() == std::vector<std::int64_t>{ 11 });
}

TEST_CASE("ReportRuleTest.staleCorrelationIgnored")
{
    Direct d;
    d.domain->onSessionUp(d.now);
    d.core.egress.clear();
    FeedPositionReport r = d.core.reportFor(d.core.offered.back().correlationId + 77);
    d.domain->onReport(r, d.now);
    CHECK(BridgeMetrics::get(d.metrics.reportStale) == 1);
    CHECK(d.domain->view().state == State::SYNCING);
    r.correlationId = d.core.offered.back().correlationId;
    d.domain->onReport(r, d.now);
    CHECK(d.domain->view().state == State::STREAMING);
}

TEST_CASE("ReportRuleTest.queryResentEvery500msUntilAnswered")
{
    Direct d;
    d.domain->onSessionUp(d.now);
    d.core.egress.clear(); // nothing answers
    const std::int64_t corr = d.core.offered.back().correlationId;
    for (int i = 0; i < 200; ++i) // 2 s
    {
        d.now += 10ms;
        d.domain->tick(d.now);
    }
    int sends = 0;
    for (const auto &f : d.core.offered)
    {
        if (T_QUERY_FEED_POSITION == f.moneyTemplate)
        {
            CHECK(f.correlationId == corr); // the same query: its coversSeq stays exact
            ++sends;
        }
    }
    CHECK(sends == 5); // 0, 0.5, 1.0, 1.5, 2.0 s
    d.core.egress.clear();
    d.domain->onReport(d.core.reportFor(corr), d.now);
    CHECK(d.domain->view().state == State::STREAMING);
}

TEST_CASE("ReportRuleTest.reportTimeoutIsDegradedNotFatal")
{
    Direct d;
    d.domain->onSessionUp(d.now);
    d.core.egress.clear();
    const std::int64_t corr = d.core.offered.back().correlationId;
    for (int i = 0; i < 600; ++i) // 6 s
    {
        d.now += 10ms;
        d.domain->tick(d.now);
        d.core.egress.clear();
    }
    CHECK((d.domain->view().degraded & bit(Degraded::REPORT_TIMEOUT)) != 0);
    CHECK(BridgeMetrics::get(d.metrics.reportTimeouts) == 1);
    CHECK(d.domain->fatal() == Fatal::NONE);
    d.domain->onReport(d.core.reportFor(corr), d.now);
    CHECK((d.domain->view().degraded & bit(Degraded::REPORT_TIMEOUT)) == 0);
    CHECK(d.domain->view().state == State::STREAMING);
}
