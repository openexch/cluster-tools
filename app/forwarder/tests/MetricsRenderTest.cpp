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
