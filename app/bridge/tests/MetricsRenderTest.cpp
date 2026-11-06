// RC2 S9c 12.1 MetricsRenderTest: the /health JSON (keys and order of 9.1, `halted` from the halt record while the
// state is DOWN), the /ready text per bit (4.3), the Prometheus text against goldens (tests/golden/*;
// CT_BRIDGE_REGEN_GOLDEN=1 rewrites them for review) and every series the alerts name.
#include "doctest/doctest.h"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "BridgeConfig.h"
#include "BridgeMetricsRender.h"

using namespace cluster_tools::app::bridge;
namespace db = cluster_tools::domain::bridge;
namespace arc = cluster_tools::archive;

namespace {

BridgeSnapshot fixed()
{
    BridgeSnapshot s;
    s.sequence = 42;
    s.identity = 7;
    std::snprintf(s.version, sizeof(s.version), "%s", "s9c-test");
    std::snprintf(s.build, sizeof(s.build), "%s", "0123456789abcdef0123456789abcdef01234567");
    s.driverMode = static_cast<std::uint8_t>(DriverMode::EXTERNAL);
    s.coreLink = static_cast<std::uint8_t>(CoreLink::C3_REGISTERED);
    s.aeConnected = true;
    s.registered = true;
    s.view.state = db::State::STREAMING;
    s.view.syncEpoch = 0x0A1B2C3D;
    s.view.cursor = 900;
    s.view.last = 910;
    s.view.expected = 911;
    s.view.attached = 1;
    s.view.reportThisTerm = true;
    s.view.coreMirror = db::HaltReason::NONE;
    s.notReady = 0;
    s.reportAgeMs = 1200;
    s.consumedPositionAgeMs = 3;
    s.lastForwardAgeMs = 4;
    s.client.sessionsOpened = 1;
    s.client.newLeaderEvents = 2;
    s.client.keepAlivesSent = 300;
    s.client.offersInEndedTerms = 5;
    s.registrar.acksAccepted = 1;
    s.registrar.acksDuplicate = 2;
    s.registrar.resends = 1;
    s.app.sessionLosses[2] = 1;
    s.app.connectCycles = 2;
    s.app.coreStaleTerm = 3;
    s.app.egressIgnored[102] = 9;
    s.app.egressIgnored[EGRESS_TEMPLATE_LABELS - 1] = 1;
    s.app.orderReject[18] = 4;
    s.app.offerRetry[1] = 6;
    s.app.sourceFaults[static_cast<std::size_t>(arc::FaultKind::IMAGE_LOST)] = 2;
    s.app.clockAnomalies = 1;
    s.forwardLatency.record(3);
    s.forwardLatency.record(70);
    s.ackLatency.record(1);
    s.logDropped = 0;
    s.aeronErrors = 0;
    return s;
}

HttpExtras extras()
{
    HttpExtras x;
    x.sourceCount = 2;
    x.driverMode = "external";
    x.conductorHeartbeatAgeMs = 250;
    x.httpTimeouts = 1;
    x.sighupIgnored = 2;
    return x;
}

std::string golden(const std::string &name, const std::string &actual)
{
    const std::string path = std::string(CT_BRIDGE_GOLDEN_DIR) + "/" + name;
    if (nullptr != std::getenv("CT_BRIDGE_REGEN_GOLDEN"))
    {
        std::ofstream out(path);
        out << actual;
        return actual;
    }
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}

TEST_CASE("MetricsRenderTest.healthGolden")
{
    const BridgeSnapshot s = fixed();
    const std::string up = renderHealthJson(s, extras());
    CHECK(up == golden("health.json", up));
    CHECK(up.rfind("{\"halted\":false,\"aeConnected\":true,\"haltReason\":\"NONE\",\"meEpoch\":169552957,\"cursor\":900,", 0) == 0);
    CHECK(up.find("\"status\":\"UP\"") != std::string::npos);
    // RC2 S16 W4: /health.build is the binary's source_sha, additive and last (the 9.1 key order above is a prefix)
    CHECK(up.find(",\"build\":\"0123456789abcdef0123456789abcdef01234567\"}") != std::string::npos);
    CHECK(healthStatus(s, extras()) == 200);
    HttpExtras stalled = extras();
    stalled.stalled = true;
    CHECK(renderHealthJson(s, stalled).find("\"status\":\"STALLED\",") != std::string::npos);
    CHECK(renderHealthJson(s, stalled).find("\"halted\":false,") == 1);
    CHECK(healthStatus(s, stalled) == 503);
    // the key order of 9.1
    const std::vector<std::string> keys = { "halted", "aeConnected", "haltReason", "meEpoch", "cursor", "status", "state",
        "coreHaltReason", "expected", "last", "attachedSource", "degraded", "registered", "reportThisTerm", "identity", "ready" };
    std::size_t at = 0;
    for (const std::string &k : keys)
    {
        CAPTURE(k);
        const std::size_t found = up.find("\"" + k + "\":", at);
        REQUIRE(found != std::string::npos);
        at = found;
    }
}

TEST_CASE("MetricsRenderTest.haltedFollowsTheHaltRecord")
{
    // OQ-4 / NOTE-3 / D-S9c-2: a halt kept across a session loss reads halted while the state is DOWN
    BridgeSnapshot s = fixed();
    s.view.state = db::State::DOWN;
    s.view.halt = db::BridgeHalt::GAP;
    s.aeConnected = false;
    const std::string body = renderHealthJson(s, extras());
    const bool keptHaltReadsHalted = body.rfind("{\"halted\":true,", 0) == 0;
    CHECK(keptHaltReadsHalted); // HEALTH_HALTED_FROM_STATE RED: DOWN with a kept halt renders halted:false
    CHECK(healthStatus(s, extras()) == 503);
    CHECK(body.find("\"haltReason\":\"GAP\"") != std::string::npos);
    CHECK(body.find("\"status\":\"HALTED\"") != std::string::npos);
    CHECK(body.find("\"state\":\"DOWN\"") != std::string::npos);
    s.view.halt = db::BridgeHalt::NONE;
    s.view.state = db::State::SYNCING;
    CHECK(healthStatus(s, extras()) == 200);
}

TEST_CASE("MetricsRenderTest.foreignHeadHaltDetail")
{
    // RC2 S17 W12 B1: /health names a foreign journal head next to the halt reason (additive key, before "build")
    BridgeSnapshot s = fixed();
    s.view.state = db::State::HALTED;
    s.view.halt = db::BridgeHalt::SOURCE_SCHEMA;
    s.view.haltDetail = db::HaltDetail::FOREIGN_JOURNAL_HEAD;
    const std::string body = renderHealthJson(s, extras());
    CHECK(healthStatus(s, extras()) == 503);
    CHECK(body.find("\"haltReason\":\"SOURCE_SCHEMA\"") != std::string::npos);
    CHECK(body.find(",\"relistsSinceHalt\":0,\"haltDetail\":\"FOREIGN_JOURNAL_HEAD\",") != std::string::npos);
    CHECK(body.find(",\"build\":\"0123456789abcdef0123456789abcdef01234567\"}") != std::string::npos);
    s.view.haltDetail = db::HaltDetail::NONE;
    CHECK(renderHealthJson(s, extras()).find("\"haltDetail\":\"NONE\"") != std::string::npos);
}

TEST_CASE("MetricsRenderTest.readyTextPerBit")
{
    BridgeSnapshot s = fixed();
    CHECK(renderReady(readyBits(s, extras())) == "ready\n");
    for (int b = 0; b < not_ready::BITS; ++b)
    {
        CAPTURE(b);
        CHECK(renderReady(1u << b) == std::string("not ready: ") + notReadyBitName(b) + "\n");
    }
    CHECK(renderReady(not_ready::CORE_NO_LEADER | not_ready::HALTED) == "not ready: core_no_leader halted\n");
    HttpExtras stalled = extras();
    stalled.stalled = true;
    CHECK(readyBits(s, stalled) == not_ready::STALLED);
    s.sequence = 0;
    CHECK(0 != (readyBits(s, extras()) & not_ready::CORE_NOT_CONNECTED)); // nothing published yet
    const std::string ready = renderReady(not_ready::REPORT_STALE | not_ready::BACKLOG_WITHOUT_PROGRESS);
    CHECK(ready == golden("ready.txt", ready));
}

TEST_CASE("MetricsRenderTest.prometheusGolden")
{
    db::BridgeMetrics domain;
    db::BridgeMetrics::set(domain.coreCursor, 900);
    db::BridgeMetrics::add(domain.forwarded[static_cast<std::size_t>(db::FrameKind::K62)], 2);
    db::BridgeMetrics::add(domain.forwardedRecords[static_cast<std::size_t>(db::FrameKind::K62)], 128);
    db::BridgeMetrics::add(domain.fences[static_cast<std::size_t>(db::FenceTrigger::NEW_LEADER)]);
    db::BridgeMetrics::add(domain.haltRecovered[static_cast<std::size_t>(db::BridgeHalt::GAP)]);
    arc::ArchiveMetrics archive;
    archive.sourceAttached[1].store(1);
    archive.sourceRefusedMtu[0].store(1);
    archive.openFailure(arc::OpenFailure::STORAGE);
    archive.memoResumed(arc::ResumeKind::COLD);
    archive.stall(arc::StallReason::IMAGE_LOST);
    arc::ArchiveMetrics::raiseMax(archive.invokerMaxGapMs, 12);
    const std::string text = renderPrometheus(fixed(), domain, archive, extras());
    CHECK(text == golden("metrics.prom", text));
    CHECK(text.find("bridge_info{identity=\"7\",version=\"s9c-test\",me_epoch=\"169552957\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_driver_mode{mode=\"external\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_forwarded_total{kind=\"62\"} 2\n") != std::string::npos);
    CHECK(text.find("bridge_forwarded_records_total{kind=\"62\"} 128\n") != std::string::npos);
    CHECK(text.find("bridge_fences_total{trigger=\"new_leader\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_halt_recovered_total{reason=\"GAP\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_source_refused{source=\"0\",reason=\"mtu\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_replay_open_failures_total{reason=\"storage\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_memo_resumes_total{kind=\"cold\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_source_stalls_total{reason=\"image_lost\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_source_faults_total{kind=\"IMAGE_LOST\"} 2\n") != std::string::npos);
    CHECK(text.find("bridge_session_losses_total{reason=\"closed_by_cluster\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_egress_ignored_total{template=\"102\"} 9\n") != std::string::npos);
    CHECK(text.find("bridge_egress_ignored_total{template=\"other\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_order_reject_total{reason=\"18\"} 4\n") != std::string::npos);
    CHECK(text.find("bridge_offer_retry_total{status=\"back_pressured\"} 6\n") != std::string::npos);
    CHECK(text.find("bridge_forward_latency_ms_bucket{le=\"5\"} 1\n") != std::string::npos);
    CHECK(text.find("bridge_forward_latency_ms_bucket{le=\"100\"} 2\n") != std::string::npos);
    CHECK(text.find("bridge_forward_latency_ms_bucket{le=\"+Inf\"} 2\n") != std::string::npos);
    CHECK(text.find("bridge_forward_latency_ms_count 2\n") != std::string::npos);
    CHECK(text.find("bridge_invoker_max_gap_ms 12\n") != std::string::npos);
    CHECK(text.find("bridge_conductor_heartbeat_age_ms 250\n") != std::string::npos);
    CHECK(text.find("bridge_sighup_ignored_total 2\n") != std::string::npos);
    CHECK(text.find("bridge_ready 1\n") != std::string::npos);
    // the max gauges are "max since the previous scrape"
    CHECK(renderPrometheus(fixed(), domain, archive, extras()).find("bridge_invoker_max_gap_ms 0\n") != std::string::npos);
}

TEST_CASE("MetricsRenderTest.everyAlertedSeriesIsExported")
{
    std::ifstream in(CT_BRIDGE_ALERTS_FILE);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    const std::string alerts = text.str();
    db::BridgeMetrics domain;
    arc::ArchiveMetrics archive;
    const std::string metrics = renderPrometheus(fixed(), domain, archive, extras());
    const std::regex name("bridge_[a-z_]+");
    std::set<std::string> names;
    for (auto it = std::sregex_iterator(alerts.begin(), alerts.end(), name); it != std::sregex_iterator(); ++it)
    {
        names.insert(it->str());
    }
    CHECK(names.size() >= 13);
    for (const std::string &n : names)
    {
        CAPTURE(n);
        CHECK(metrics.find("# TYPE " + n + " ") != std::string::npos);
    }
}

TEST_CASE("MetricsRenderTest.readyBitsRule")
{
    // the pure readiness rule (4.3); the agent-level scenarios are BridgeAgentTest.readyBitsEachFail
    using namespace std::chrono_literals;
    ReadyInputs in;
    in.link = CoreLink::C3_REGISTERED;
    in.now = arc::TimePoint(100s);
    in.view.reportThisTerm = true;
    in.view.lastReportAt = in.now - 1s;
    in.anySourceAttached = true;
    in.progressAt = in.now;
    in.forwardAt = in.now;
    CHECK(notReadyBits(in) == 0);
    ReadyInputs stale = in;
    stale.view.lastReportAt = in.now - 31s;
    const bool staleBit = 0 != (notReadyBits(stale) & not_ready::REPORT_STALE);
    CHECK(staleBit); // READY_IGNORES_REPORT_AGE RED
    stale.view.state = db::State::HALTED;
    stale.view.lastReportAt = in.now - 16s;
    CHECK(0 != (notReadyBits(stale) & not_ready::REPORT_STALE));
    ReadyInputs walk = in; // a skip walk: backlog, no forward for 40 s, the consumed position moving
    walk.backlogBytes = 1'000'000;
    walk.forwardAt = in.now - 40s;
    walk.progressAt = in.now - 1s;
    const bool skipWalkReady = 0 == (notReadyBits(walk) & not_ready::BACKLOG_WITHOUT_PROGRESS);
    CHECK(skipWalkReady); // READY_COUNTS_FORWARDS_ONLY RED
    walk.progressAt = in.now - 30s;
    CHECK(0 != (notReadyBits(walk) & not_ready::BACKLOG_WITHOUT_PROGRESS));
    ReadyInputs c4 = in;
    c4.link = CoreLink::C4_AWAITING;
    CHECK(notReadyBits(c4) == not_ready::CORE_NO_LEADER);
}
