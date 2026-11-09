// forwardLatencyAndAckLatency (O3 / O4, 5.4): the agent reads the trade id and the ME timestamp of every
// 61 / 62 its port offered OK through the codec's getter (metrics only), arms the ack map, and closes the sample at the
// SettlementApplied 13; a future timestamp is a clock anomaly and records nothing; an ack for an unknown trade is a skip.
#include "doctest/doctest.h"

#include <fstream>
#include <iterator>

#include "AppRig.h"

#include "com_match_infrastructure_mejournal_generated/Trade.h"

using namespace ct_bridge_app_test;
using cluster_tools::sbe_adapter::bridge_codec::BridgeCodec;

namespace {

Bytes readBytes(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}

TEST_CASE("CodecAppTest.forwardLatencyAndAckLatency")
{
    const Bytes trade = readBytes(std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/java-trade.bin");
    REQUIRE(trade.size() == TRADE_RECORD_LENGTH);
    namespace jr = com::match::infrastructure::mejournal::generated;
    jr::Trade d;
    d.wrapForDecode(reinterpret_cast<char *>(const_cast<std::uint8_t *>(trade.data())), 8, jr::Trade::sbeBlockLength(), 1,
        trade.size());
    const std::int64_t ts = d.timestamp();
    const std::int64_t tradeId = d.tradeId();
    AppRig r(1);
    r.journal(0, 1, 3);
    r.bringUp();
    BridgeCodec codec;
    std::uint8_t frame[8192];
    const std::size_t n61 = codec.encodeRecord(trade.data(), trade.size(), frame, sizeof(frame));
    REQUIRE(n61 > 0);
    // a 61 offered OK 25 ms after the ME stamped it
    r.epochBaseMs = ts + 25 - r.t.now / MS;
    r.agent->onOfferOk(frame, n61);
    CHECK(r.agent->forwardLatency().count == 1);
    CHECK(r.agent->forwardLatency().sumMs == 25);
    // its SettlementApplied 7 ms later
    r.t.now += 7 * MS;
    r.inject(settlementAppliedFrame(tradeId), r.term);
    r.step(0);
    CHECK(r.agent->ackLatency().count == 1);
    CHECK(r.agent->ackLatency().sumMs == 7);
    // a cap-64 62: 64 observations (the 62 entries)
    std::vector<StagedTrade> staged(64);
    for (std::size_t i = 0; i < staged.size(); ++i)
    {
        std::memcpy(staged[i].record, trade.data(), TRADE_RECORD_LENGTH);
    }
    std::uint8_t batch[8192];
    const std::size_t n62 = codec.encodeTradeBatch(staged.data(), staged.size(), batch, sizeof(batch));
    r.agent->onOfferOk(batch, n62);
    CHECK(r.agent->forwardLatency().count == 65);
    // a timestamp in the future of this host's clock: an anomaly, nothing recorded
    r.epochBaseMs = ts - 5 - r.t.now / MS;
    r.agent->onOfferOk(frame, n61);
    const bool futureNotRecorded = r.agent->forwardLatency().count == 65;
    CHECK(futureNotRecorded); // LATENCY_RECORDS_GARBAGE RED
    app::BridgeSnapshot s;
    r.agent->fillSnapshot(s);
    CHECK(s.app.clockAnomalies == 1);
    // an ack for a trade this process never offered is a skip (counted, nothing recorded)
    r.inject(settlementAppliedFrame(tradeId + 1'000'000), r.term);
    r.step(0);
    CHECK(r.agent->ackLatency().count == 1);
    CHECK(r.agent->ackMap().unknown() >= 1);
    // a session loss clears the in-flight map (the Java P4 rule)
    r.agent->onOfferOk(frame, n61);
    REQUIRE(r.agent->ackMap().size() >= 1);
    r.closeSession();
    r.step();
    r.step();
    CHECK(r.agent->ackMap().size() == 0);
}
