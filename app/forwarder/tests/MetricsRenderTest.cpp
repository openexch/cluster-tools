// RC2 S12 contract 11.1 MetricsRenderTest: Prometheus text, the /health JSON and the /ready body for a fixed
// snapshot, against goldens (tests/golden/*; CT_FORWARDER_REGEN_GOLDEN=1 rewrites them for review).
#include "doctest/doctest.h"

#include <cstdlib>
#include <regex>
#include <set>
#include <fstream>
#include <sstream>
#include <string>

#include "MetricsRender.h"

using namespace cluster_tools::app::forwarder;
namespace fd = cluster_tools::domain::forwarder;

namespace {

ForwarderSnapshot fixed()
{
    ForwarderSnapshot s;
    s.sequence = 42;
    s.identity = 0x1234567890ABCDEFLL;
    s.meEpoch = 0x0A1B2C3D4E5F6071LL;
    std::snprintf(s.version, sizeof(s.version), "%s", "s12-test");
    s.ready = false;
    s.notReady = not_ready::NOT_LIVE | not_ready::NO_RECENT_ANSWER;
    s.coreLink = static_cast<std::uint8_t>(CoreLink::REGISTERED);
    s.meLink = static_cast<std::uint8_t>(MeLink::BOUND);
    s.registered = true;
    s.core = ClusterStats{ 2, 3, 4, 3, 1200, 2, 1, 77, 5 };
    s.me = ClusterStats{ 2, 1, 2, 1, 1199, 0, 0, 88, 9 };
    s.registrationAccepted = 3;
    s.registrationDuplicate = 1;
    s.registrationSends = 5;
    s.meEpochReconnects = 2;
    s.meEgressAgeMs = 120;
    s.mode = 1;
    s.cursor = 9000;
    s.lastSentSeq = 8990;
    s.coreHead = 9100;
    s.lastResyncEndSeq = 8800;
    s.resyncDurationMs = 15;
    s.headProbeAgeMs = 3000;
    s.ringDepth = 10;
    s.resync.received = 12000;
    s.resync.duplicates = 30;
    s.resync.droppedIdle = 4;
    s.resync.discarded = 0;
    s.resync.resyncs[1] = 3;
    s.resync.resyncs[2] = 1;
    s.resync.resyncs[3] = 2;
    s.resync.resyncs[9] = 1;
    s.resync.pages = 7;
    s.resync.entries = 9100;
    s.resync.timeouts = 1;
    s.resync.shapeViolations[1] = 1;
    s.resync.headProbeCovered = 20;
    s.resync.headProbeBehind = 1;
    s.counters.sentTemplate9 = 11000;
    s.counters.sentTemplate10 = 90;
    s.counters.releaseEntriesSent = 1900;
    s.counters.coreStaleTerm = 6;
    s.counters.coreFutureTerm = 0;
    s.counters.invalid.counts[0][static_cast<unsigned>(fd::RuleId::R2)][static_cast<unsigned>(fd::DispatchClass::CARRIER)] = 2;
    s.counters.invalid.counts[2][static_cast<unsigned>(fd::RuleId::B5)][static_cast<unsigned>(fd::DispatchClass::ME_INVALID)] = 1;
    s.counters.invalid.counts[3][static_cast<unsigned>(fd::RuleId::X2)][static_cast<unsigned>(fd::DispatchClass::RELEASE_REFUSED)] = 1;
    s.counters.invalid.counts[4][static_cast<unsigned>(fd::RuleId::V12)][static_cast<unsigned>(fd::DispatchClass::UNROUTABLE)] = 1;
    s.counters.unknownTemplate[0][113] = 5;
    s.counters.unknownTemplate[1][22] = 300;
    s.counters.meBackPressure = 4;
    s.aeronErrors = 0;
    s.dutyMaxCycleUs = 85;
    s.driverMode = 2; // RC2 S10' 8.2 rule 6: external
    // RC2 S13: market 1 OK and ticking, 2 BROKEN, 3 EMPTY (rule U), 4 / 5 never seen
    BookView &b1 = s.books[0];
    b1.state = 1;
    b1.version = 4242;
    b1.snapshotAgeMs = 3100;
    b1.lastOfferedVersion = 4240;
    b1.pendingAgeMs = 40;
    b1.lastTradeId = 812;
    b1.ladder.snapshots = 7;
    b1.ladder.deltas = 900;
    b1.ladder.chainBreaks[static_cast<unsigned>(fd::BreakCause::FROM_MISMATCH)] = 2;
    b1.ladder.recoveries = 2;
    b1.ladder.versionRegressions = 1;
    b1.brokenNsTotal = 1'250'000'000;
    b1.tick.offered[static_cast<unsigned>(fd::TickEdge::LEADING)] = 30;
    b1.tick.offered[static_cast<unsigned>(fd::TickEdge::TRAILING)] = 12;
    b1.tick.offered[static_cast<unsigned>(fd::TickEdge::REOFFER)] = 1;
    b1.tick.suppressed[static_cast<unsigned>(fd::TickSuppressed::VERSION_NOT_ABOVE)] = 3;
    b1.tick.offerFailures = 1;
    BookView &b2 = s.books[1];
    b2.state = 2;
    b2.version = 77;
    b2.brokenAgeMs = 16000;
    b2.snapshotAgeMs = 26000;
    b2.brokenNsTotal = 16'000'000'000;
    b2.ladder.chainBreaks[static_cast<unsigned>(fd::BreakCause::MALFORMED)] = 1;
    b2.ladder.ignored = 5;
    b2.tradeMalformed = 1;
    b2.tick.suppressed[static_cast<unsigned>(fd::TickSuppressed::BROKEN)] = 4;
    s.books[2].ladder.unversioned = 3;
    s.bookForeignMarket = 1;
    s.bookMalformedNoMarket = 2;
    s.counters.meStaleTerm = 9;
    s.counters.meKnownIgnored[0] = 11;
    s.counters.meKnownIgnored[2] = 1;
    s.counters.tickRejected[0] = 1;
    s.ticksReady = false;
    s.ticksNotReadyMarket = 2;
    s.ticksNotReadyCondition = ticks_not_ready::BROKEN_30S;
    return s;
}

std::string golden(const char *name, const std::string &actual)
{
    const std::string path = std::string(CT_FORWARDER_GOLDEN_DIR) + "/" + name;
    if (nullptr != std::getenv("CT_FORWARDER_REGEN_GOLDEN"))
    {
        std::ofstream(path, std::ios::binary) << actual;
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}

TEST_CASE("MetricsRenderTest.prometheusGolden")
{
    const std::string text = renderPrometheus(fixed());
    CHECK(text == golden("metrics.prom", text));
    CHECK(text.find("forwarder_info{identity=\"1311768467294899695\",version=\"s12-test\",me_epoch=\"728224406569967729\"} 1\n") !=
        std::string::npos);
    CHECK(text.find("forwarder_invalid_dispatch_total{kind=\"0\",rule=\"R2\",action=\"carrier\"} 2\n") != std::string::npos);
    CHECK(text.find("forwarder_invalid_dispatch_total{kind=\"other\",rule=\"V12\",action=\"unroutable\"} 1\n") != std::string::npos);
    CHECK(text.find("forwarder_resync_total{reason=\"T9\"} 1\n") != std::string::npos);
    CHECK(text.find("forwarder_dispatch_seq{point=\"core_head\"} 9100\n") != std::string::npos);
    CHECK(text.find("forwarder_unknown_template_total{cluster=\"me\",template=\"22\"} 300\n") != std::string::npos);
}

TEST_CASE("MetricsRenderTest.healthAndReadyGolden")
{
    const ForwarderSnapshot s = fixed();
    const std::string up = renderHealthJson(s, false);
    CHECK(up == golden("health.json", up));
    CHECK(up.find("\"status\":\"UP\"") != std::string::npos);
    CHECK(up.find("\"lastDispatchSeq\":9000") != std::string::npos);
    CHECK(up.find("\"resyncs\":7") != std::string::npos);
    CHECK(up.find("\"invalidDispatches\":5") != std::string::npos);
    CHECK(renderHealthJson(s, true).find("\"status\":\"STALLED\"") != std::string::npos);
    const std::string ready = renderReady(s);
    CHECK(ready == golden("ready.txt", ready));
    CHECK(ready == "not ready: not_live,no_probe_or_resync_answer_15s\n");
    ForwarderSnapshot ok = s;
    ok.ready = true;
    ok.notReady = 0;
    CHECK(renderReady(ok) == "ready\n");
}

TEST_CASE("MetricsRenderTest.everyMetricTheAlertsNameIsExported")
{
    std::ifstream in(CT_FORWARDER_ALERTS_FILE);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    const std::string alerts = text.str();
    const std::string metrics = renderPrometheus(fixed());
    const std::regex name("forwarder_[a-z_]+");
    std::set<std::string> names;
    for (auto it = std::sregex_iterator(alerts.begin(), alerts.end(), name); it != std::sregex_iterator(); ++it)
    {
        names.insert(it->str());
    }
    CHECK(names.size() >= 9);
    for (const std::string &n : names)
    {
        CAPTURE(n);
        CHECK(metrics.find("# TYPE " + n + " ") != std::string::npos);
    }
}

TEST_CASE("MetricsRenderTest.bookAndTickSeries")
{
    // RC2 S13 10.1 / 10.2: the per-market families, BOOK_STALE in /health only (never a /ready conjunct, PO-1)
    const ForwarderSnapshot s = fixed();
    const std::string text = renderPrometheus(s);
    CHECK(text.find("forwarder_book_state{market=\"2\"} 2\n") != std::string::npos);
    CHECK(text.find("forwarder_book_frames_total{market=\"1\",template=\"25\"} 900\n") != std::string::npos);
    CHECK(text.find("forwarder_book_chain_breaks_total{market=\"1\",cause=\"from_mismatch\"} 2\n") != std::string::npos);
    CHECK(text.find("forwarder_book_chain_breaks_total{market=\"2\",cause=\"malformed\"} 1\n") != std::string::npos);
    CHECK(text.find("forwarder_book_broken_seconds_total{market=\"1\"} 1.250\n") != std::string::npos);
    CHECK(text.find("forwarder_book_snapshot_age_ms{market=\"4\"} -1\n") != std::string::npos); // never a snapshot
    CHECK(text.find("forwarder_tick_offered_total{market=\"1\",edge=\"reoffer\"} 1\n") != std::string::npos);
    CHECK(text.find("forwarder_tick_suppressed_total{market=\"1\",reason=\"version_not_above\"} 3\n") != std::string::npos);
    CHECK(text.find("forwarder_tick_rejected_total{reason=\"unregistered\"} 1\n") != std::string::npos);
    CHECK(text.find("forwarder_ticks_ready 0\n") != std::string::npos);
    CHECK(text.find("forwarder_me_frames_total{template=\"23\"} 11\n") != std::string::npos);
    CHECK(text.find("forwarder_me_stale_term_total 9\n") != std::string::npos);
    const std::string health = renderHealthJson(s, false);
    CHECK(health.find("{\"market\":2,\"state\":\"BROKEN\",\"version\":77,\"brokenAgeMs\":16000") != std::string::npos);
    CHECK(health.find("\"ticksReady\":{\"ready\":false,\"market\":2,\"condition\":\"book_stale_broken_30s\"}}\n") !=
        std::string::npos);
    CHECK(health.back() == '\n');
    CHECK(renderReady(s).find("book_stale") == std::string::npos);
    // the dispatch /ready mask keeps its 10 conjuncts (S12 / S10'): BOOK_STALE is not one of them
    CHECK(not_ready::COUNT == 10);
}
