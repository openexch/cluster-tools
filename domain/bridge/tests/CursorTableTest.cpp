// CursorTableTest (RC2 S9b 13.1): the per-frame table of 7.3 (G1, the epoch rows, the head rule) and the report
// validity of 7.2.
#include "doctest/doctest.h"

#include "BridgeTestSupport.h"

using namespace ct_bridge_test;

namespace {

// A core already bound to EPOCH with cursor c (its earlier digests unknown: duplicates are not byte-checked).
void boundAt(StubCore &core, std::int64_t c)
{
    core.bound = EPOCH;
    core.cursor = c;
}

bool anyOfferAtOrBelow(const StubCore &core, std::int64_t c)
{
    for (std::int64_t s : core.offeredSeqs())
    {
        if (s <= c)
        {
            return true;
        }
    }
    return false;
}

}

TEST_CASE("CursorTableTest.skipBelowLastNeverOffers")
{
    Direct d;
    boundAt(d.core, 10);
    d.start();
    REQUIRE(d.domain->view().state == State::STREAMING);
    REQUIRE(d.domain->view().last == 10);
    for (std::int64_t s = 1; s <= 10; ++s)
    {
        CHECK(d.rec(1 == s ? TEMPLATE_CONFIG : TEMPLATE_TRADE, s) == SinkAction::CONTINUE);
    }
    // the record after a missing 11 is a hole, never a forward
    CHECK(d.rec(TEMPLATE_TRADE, 12) == SinkAction::ABORT);
    d.poll(0);
    const int feed = d.core.feedOffers();
    CHECK(feed == 0);
    CHECK(BridgeMetrics::get(d.metrics.skipped[0]) == 9);
}

TEST_CASE("CursorTableTest.terminalBelowCursorNeverForwarded")
{
    Direct d;
    boundAt(d.core, 10);
    d.start();
    // the Java bridge's inclusive terminal boundary (cpp F3) re-offered a 63 at the cursor
    CHECK(d.rec(TEMPLATE_TERMINAL, 10) == SinkAction::CONTINUE);
    CHECK(d.rec(TEMPLATE_TERMINAL, 9) == SinkAction::CONTINUE);
    const int feed = d.core.feedOffers();
    CHECK(feed == 0);
    CHECK(d.rec(TEMPLATE_TERMINAL, 11) == SinkAction::CONTINUE);
    CHECK(d.core.offeredSeqs() == std::vector<std::int64_t>{ 11 });
}

TEST_CASE("CursorTableTest.bridgeRestart3xSendsNothingBelowCursor")
{
    // R-B4-1 / H2: three cold restarts against a core at C = 1000 over a 2000-record journal
    for (int restart = 0; restart < 3; ++restart)
    {
        Rig rig(1);
        boundAt(rig.core, 1000);
        SimRecording &rec = rig.archive(0).record();
        for (std::int64_t s = 1; s <= 2000; ++s)
        {
            const std::uint16_t t = 1 == s ? TEMPLATE_CONFIG : (s % 7 == 0 ? TEMPLATE_TERMINAL : TEMPLATE_TRADE);
            rig.archive(0).append(rec, makeFrame(t, s, EPOCH, 64 * s));
        }
        rig.up();
        REQUIRE(rig.runUntil([&] { return rig.core.cursor == 2000; }, 600'000ms));
        const bool below = anyOfferAtOrBelow(rig.core, 1000);
        CHECK_FALSE(below);
        const std::vector<std::int64_t> offered = rig.core.offeredSeqs();
        REQUIRE(offered.size() == 1000);
        for (std::size_t i = 0; i < offered.size(); ++i)
        {
            CHECK(offered[i] == 1001 + static_cast<std::int64_t>(i));
        }
        CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    }
}

TEST_CASE("CursorTableTest.holeIsNeverForwarded")
{
    Direct d;
    d.start(); // an unbound core: C = 0
    CHECK(d.rec(TEMPLATE_CONFIG, 1) == SinkAction::CONTINUE);
    CHECK(d.rec(TEMPLATE_TRADE, 2) == SinkAction::CONTINUE);
    CHECK(d.rec(TEMPLATE_TRADE, 4) == SinkAction::ABORT);
    d.poll(0); // the empty poll flushes the staged 2
    const std::vector<std::int64_t> offered = d.core.offeredSeqs();
    CHECK(offered == std::vector<std::int64_t>{ 1, 2 });
}

TEST_CASE("CursorTableTest.epochMismatchHalts")
{
    // R-B4-2: every source foreign at e (an ME re-genesis): FeedHalt EPOCH_MISMATCH, nothing of the new epoch offered
    Rig rig(2);
    boundAt(rig.core, 3);
    for (std::size_t i = 0; i < 2; ++i)
    {
        SimRecording &rec = rig.archive(i).record();
        appendJournal(rig.archive(i), rec, 1, 3, EPOCH);
        for (std::int64_t s = 4; s <= 10; ++s)
        {
            rig.archive(i).append(rec, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
        }
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->view().state == State::HALTED; }));
    bool foreignOffered = false;
    for (const auto &f : rig.core.offered)
    {
        for (const auto &r : f.records)
        {
            foreignOffered = foreignOffered || r.epoch == 9;
        }
    }
    CHECK_FALSE(foreignOffered);
    REQUIRE(rig.core.feedHalts.size() >= 1);
    CHECK(rig.core.feedHalts.front().reason == HaltReason::EPOCH_MISMATCH);
    CHECK(rig.core.feedHalts.front().expected == 4);
    CHECK(rig.core.feedHalts.front().meEpochSeen == 9);
    CHECK(rig.domain->view().halt == BridgeHalt::EPOCH_MISMATCH);
}

TEST_CASE("CursorTableTest.foreignEpochOnOneSourceFailsOver")
{
    // D-S9b-1: one node with an unwiped old-epoch tail is a per-source verdict, not a genesis-only halt
    Rig rig(2);
    boundAt(rig.core, 3);
    SimRecording &a = rig.archive(0).record();
    appendJournal(rig.archive(0), a, 1, 3, EPOCH);
    for (std::int64_t s = 4; s <= 10; ++s)
    {
        rig.archive(0).append(a, makeFrame(TEMPLATE_TRADE, s, 9, 64 * s));
    }
    SimRecording &b = rig.archive(1).record();
    appendJournal(rig.archive(1), b, 1, 20, EPOCH);
    rig.up();
    rig.runUntil([&] { return rig.core.cursor == 20 || rig.domain->view().halt != BridgeHalt::NONE; });
    const BridgeHalt halt = rig.domain->view().halt;
    CHECK(halt == BridgeHalt::NONE);
    REQUIRE(rig.core.cursor == 20);
    CHECK(rig.core.feedHalts.empty());
    CHECK(rig.switchesTo(1) >= 1);
    CHECK(rig.metrics.badCount(BadReason::FOREIGN_EPOCH) >= 1);
}

TEST_CASE("CursorTableTest.unboundSyncAdoptsTheEpochOfSeqOne")
{
    Rig rig(1);
    SimRecording &rec = rig.archive(0).record();
    appendJournal(rig.archive(0), rec, 1, 5, EPOCH);
    rig.archive(0).append(rec, makeFrame(TEMPLATE_TRADE, 6, 7, 64 * 6));
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->view().state == State::HALTED; }));
    CHECK(rig.domain->view().syncEpoch == EPOCH);
    bool sixOffered = false;
    for (std::int64_t s : rig.core.offeredSeqs())
    {
        sixOffered = sixOffered || s == 6;
    }
    CHECK_FALSE(sixOffered);
    CHECK(rig.core.cursor == 5);
    CHECK(rig.domain->view().halt == BridgeHalt::EPOCH_MISMATCH);
}

TEST_CASE("CursorTableTest.unboundSeqOneMustBeEngineConfig")
{
    SUBCASE("a 61 at seq 1 on one source fails over")
    {
        Rig rig(2);
        SimRecording &a = rig.archive(0).record();
        for (std::int64_t s = 1; s <= 8; ++s)
        {
            rig.archive(0).append(a, makeFrame(TEMPLATE_TRADE, s, EPOCH, 64 * s));
        }
        SimRecording &b = rig.archive(1).record();
        appendJournal(rig.archive(1), b, 1, 8, EPOCH);
        rig.up();
        rig.runUntil([&] { return rig.core.cursor == 8 || rig.domain->view().state == State::HALTED; });
        const std::uint8_t latch = rig.core.latch;
        CHECK(latch == 0);
        REQUIRE(rig.core.cursor == 8);
        CHECK(rig.metrics.badCount(BadReason::HEAD_NOT_CONFIG) == 1);
        CHECK(rig.domain->view().halt == BridgeHalt::NONE);
    }
    SUBCASE("a 61 at seq 1 on every source halts SOURCE_SCHEMA at 1")
    {
        Rig rig(2);
        for (std::size_t i = 0; i < 2; ++i)
        {
            SimRecording &r = rig.archive(i).record();
            for (std::int64_t s = 1; s <= 8; ++s)
            {
                rig.archive(i).append(r, makeFrame(TEMPLATE_TRADE, s, EPOCH, 64 * s));
            }
        }
        rig.up();
        REQUIRE(rig.runUntil([&] { return rig.domain->view().state == State::HALTED; }));
        const std::uint8_t latch = rig.core.latch;
        CHECK(latch == static_cast<std::uint8_t>(HaltReason::SOURCE_SCHEMA));
        REQUIRE(rig.core.feedHalts.size() >= 1);
        CHECK(rig.core.feedHalts.front().reason == HaltReason::SOURCE_SCHEMA);
        CHECK(rig.core.feedHalts.front().expected == 1);
        CHECK(rig.core.feedHalts.front().seen == 1);
        CHECK(rig.core.feedOffers() == 0);
    }
}

TEST_CASE("CursorTableTest.invalidReportsAreRefused")
{
    struct Shape
    {
        const char *name;
        std::int64_t epoch, cursor;
        std::uint8_t halted, reason;
    };
    const Shape shapes[] = {
        { "negative cursor", 5, -1, 0, 0 },
        { "negative epoch", -5, 3, 0, 0 },
        { "epoch without cursor", 5, 0, 0, 0 },
        { "cursor without epoch", 0, 3, 0, 0 },
        { "halt reason 6", 5, 3, 1, 6 },
        { "feedHalted raw 2", 5, 3, 2, 1 },
        { "halted without a reason", 5, 3, 1, 0 },
        { "a reason without halted", 5, 3, 0, 3 },
    };
    for (const Shape &shape : shapes)
    {
        CAPTURE(shape.name);
        Direct d;
        d.domain->onSessionUp(d.now);
        d.core.egress.clear(); // the stub's own answer is replaced by the invalid one
        FeedPositionReport r;
        r.correlationId = d.core.offered.back().correlationId;
        r.meEpoch = shape.epoch;
        r.lastAppliedJournalSeq = shape.cursor;
        r.feedHaltedRaw = shape.halted;
        r.feedHalted = shape.halted == 1;
        r.haltReasonRaw = shape.reason;
        d.domain->onReport(r, d.now);
        const State state = d.domain->view().state;
        CHECK(state == State::SYNCING);
        CHECK(BridgeMetrics::get(d.metrics.reportInvalid) == 1);
        CHECK((d.domain->view().degraded & bit(Degraded::REPORT_INVALID)) != 0);
    }
}

TEST_CASE("CursorTableTest.staleOldEpochPrefixOnEveryNodeNeverLatchesGenesisOnly")
{
    // review NOTE-8 (amended 2026-10-31): every node kept an unwiped older-epoch recording at its chain head. A cold
    // read meets that foreign prefix before any frame of the bound epoch: it is a stale prefix (BAD), never a
    // re-genesis, so the bridge halts SOURCE_SCHEMA (admin class, reach-cleared) instead of EPOCH_MISMATCH, which the
    // core would latch genesis-only although every node holds the bound epoch's e behind the prefix.
    Rig rig(2);
    boundAt(rig.core, 5);
    for (std::size_t i = 0; i < 2; ++i)
    {
        SimRecording &old = rig.archive(i).record();
        appendJournal(rig.archive(i), old, 1, 8, 3); // epoch 3, an ME genesis ago
        rig.archive(i).stop(old);
        SimRecording &cur = rig.archive(i).record();
        appendJournal(rig.archive(i), cur, 1, 20, EPOCH);
    }
    rig.up();
    REQUIRE(rig.runUntil([&] { return rig.domain->view().state == State::HALTED; }));
    const std::uint8_t latch = rig.core.latch;
    CHECK(latch != static_cast<std::uint8_t>(HaltReason::EPOCH_MISMATCH));
    CHECK(rig.domain->view().halt == BridgeHalt::SOURCE_SCHEMA);
    REQUIRE(!rig.core.feedHalts.empty());
    CHECK(rig.core.feedHalts.front().reason == HaltReason::SOURCE_SCHEMA);
    CHECK(rig.metrics.badCount(BadReason::STALE_EPOCH_PREFIX) == 2);
    CHECK(rig.core.feedOffers() == 0);
}
