// Codec goldens shared with the Java side (rc2-forwarder.md section 4, F1): frames encoded by
// the engines' own Java encoders (golden/golden_frames.inc, regen.sh) must decode field for
// field with the C++ codecs generated here, and the C++ encoders must reproduce them byte for
// byte. A schema drift between this build's MONEY_SCHEMA / MATCH_SCHEMA and the engines then
// fails in C++ CI, not in the fixture.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

#include "cluster_tools/build_info.h"

#include "com_openexchange_assets_infrastructure_generated/AdminAck.h"
#include "com_openexchange_assets_infrastructure_generated/MeDispatchBatch.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/PendingDispatchEnd.h"
#include "com_openexchange_assets_infrastructure_generated/RegisterSession.h"
#include "com_openexchange_assets_infrastructure_generated/AdmissionState.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/DurableOrderCommand.h"
#include "com_match_infrastructure_generated/EngineConfig.h"
#include "com_match_infrastructure_generated/MessageHeader.h"
#include "com_match_infrastructure_generated/OrderStatusBatch.h"
#include "com_match_infrastructure_mejournal_generated/MessageHeader.h"
#include "com_match_infrastructure_mejournal_generated/Trade.h"

#include "golden/golden_frames.inc"

namespace money = com::openexchange::assets::infrastructure::generated;
namespace me = com::match::infrastructure::generated;
namespace journal = com::match::infrastructure::mejournal::generated;

namespace
{

char *bytes(std::vector<std::uint8_t> &v)
{
    return reinterpret_cast<char *>(v.data());
}

}

TEST_CASE("build info names the schemas this binary speaks")
{
    CHECK(cluster_tools::build_info::MONEY_SCHEMA_VERSION == money::MessageHeader::sbeSchemaVersion());
    CHECK(cluster_tools::build_info::MATCH_SCHEMA_VERSION == me::MessageHeader::sbeSchemaVersion());
    CHECK(cluster_tools::build_info::ME_JOURNAL_SCHEMA_VERSION == journal::MessageHeader::sbeSchemaVersion());
    CHECK(money::MessageHeader::sbeSchemaId() == 2);
    CHECK(me::MessageHeader::sbeSchemaId() == 1);
    CHECK(journal::MessageHeader::sbeSchemaId() == 4);
    // The pins of RC2 S1 / dependencies.lock: money v7, order v12, ME journal v1.
    CHECK(money::MessageHeader::sbeSchemaVersion() == 7);
    CHECK(me::MessageHeader::sbeSchemaVersion() == 12);
    CHECK(journal::MessageHeader::sbeSchemaVersion() == 1);
}

TEST_CASE("v7/v12 block lengths: the layouts the engines encode")
{
    CHECK(money::AdmissionState::sbeBlockLength() == 27);    // v6 was 23: gateReasons (4) appended
    CHECK(money::RegisterSession::sbeBlockLength() == 31);
    CHECK(money::MeDispatchBatch::Dispatches::sbeBlockLength() == 89); // v6 was 80: releaseScope + expectedOldRemaining
    CHECK(me::ClusterHeartbeat::sbeBlockLength() == 17);     // v11 was 9: meEpoch appended
    CHECK(me::EngineConfig::sbeBlockLength() == 35);         // v11 was 17: four v12 root fields
    CHECK(me::DurableOrderCommand::sbeBlockLength() == 80);  // v11 was 71: timeInForce + expectedOldRemaining
}

TEST_CASE("RegisterSession: Java golden decodes, C++ re-encodes byte-equal")
{
    std::vector<std::uint8_t> golden = JAVA_REGISTER_SESSION;
    money::MessageHeader header(bytes(golden), 0, golden.size(), 0);
    CHECK(header.templateId() == 80);
    money::RegisterSession decoded(bytes(golden), money::MessageHeader::encodedLength(), golden.size(),
        header.blockLength(), header.version());
    CHECK(decoded.correlationId() == 0x0102030405060708LL);
    CHECK(decoded.sessionClass() == money::SessionClass::FORWARDER);
    CHECK(decoded.channels().dispatch());
    CHECK(decoded.channels().control());
    CHECK_FALSE(decoded.channels().acks());
    CHECK(decoded.resumable() == money::BoolFlag::TRUE);
    CHECK(decoded.identity() == 0x1112131415161718LL);
    CHECK(decoded.sourceEpoch() == 0x2122232425262728LL);

    std::vector<std::uint8_t> mine(golden.size() + 16, 0xEE);
    money::RegisterSession encoder;
    encoder.wrapAndApplyHeader(bytes(mine), 0, mine.size())
        .correlationId(0x0102030405060708LL)
        .sessionClass(money::SessionClass::FORWARDER)
        .readerKind(0)
        .resumable(money::BoolFlag::TRUE)
        .identity(0x1112131415161718LL)
        .sourceEpoch(0x2122232425262728LL);
    encoder.channels().clear().dispatch(true).control(true);
    mine.resize(money::MessageHeader::encodedLength() + encoder.encodedLength());
    CHECK(mine == golden);
}

TEST_CASE("AdminAck and PendingDispatchEnd: Java goldens decode")
{
    std::vector<std::uint8_t> ack = JAVA_ADMIN_ACK;
    money::MessageHeader h(bytes(ack), 0, ack.size(), 0);
    money::AdminAck decoded(bytes(ack), 8, ack.size(), h.blockLength(), h.version());
    CHECK(h.templateId() == 110);
    CHECK(decoded.correlationId() == 0x0102030405060708LL);
    CHECK(decoded.refTemplateId() == 80);
    CHECK(decoded.result() == money::AdminResult::DUPLICATE);
    CHECK(decoded.reason() == 7);
    CHECK(decoded.configVersion() == 9u);

    std::vector<std::uint8_t> end = JAVA_PENDING_DISPATCH_END;
    money::MessageHeader h2(bytes(end), 0, end.size(), 0);
    money::PendingDispatchEnd e(bytes(end), 8, end.size(), h2.blockLength(), h2.version());
    CHECK(h2.templateId() == 108);
    CHECK(e.correlationId() == 77);
    CHECK(e.count() == 4096);
    CHECK(e.headDispatchSeq() == 123456789);
}

TEST_CASE("MeDispatchBatch: Java golden decodes entry by entry, C++ re-encodes byte-equal")
{
    std::vector<std::uint8_t> golden = JAVA_ME_DISPATCH_BATCH;
    money::MessageHeader header(bytes(golden), 0, golden.size(), 0);
    CHECK(header.templateId() == 105);
    money::MeDispatchBatch batch(bytes(golden), 8, golden.size(), header.blockLength(), header.version());
    auto &entries = batch.dispatches();
    REQUIRE(entries.count() == 2);
    int i = 0;
    while (entries.hasNext())
    {
        entries.next();
        CHECK(entries.dispatchSeq() == 1000 + i);
        CHECK(entries.commandIdHigh() == 2000 + i);
        CHECK(entries.commandIdLow() == ((static_cast<std::int64_t>(i) << 2) | 1));
        CHECK(entries.kind() == money::DispatchKind::CANCEL);
        CHECK(entries.userId() == 3000 + i);
        CHECK(entries.omsOrderId() == 4000 + i);
        CHECK(entries.oldOrderId() == 5000 + i);
        CHECK(entries.marketId() == 6 + i);
        CHECK(entries.orderType() == 1);
        CHECK(entries.side() == i);
        CHECK(entries.price() == 700'000'000 + i);
        CHECK(entries.quantity() == 800'000'000 + i);
        CHECK(entries.budget() == 900'000'000 + i);
        CHECK(entries.timeInForce() == 2);
        CHECK(entries.releaseScope() == money::ReleaseScope::ORDER);
        CHECK(entries.expectedOldRemaining() == 1'100'000'000LL + i);
        i++;
    }

    std::vector<std::uint8_t> mine(golden.size() + 64, 0xEE);
    money::MeDispatchBatch encoder;
    encoder.wrapAndApplyHeader(bytes(mine), 0, mine.size());
    auto &out = encoder.dispatchesCount(2);
    for (int k = 0; k < 2; k++)
    {
        out.next()
            .dispatchSeq(1000 + k)
            .commandIdHigh(2000 + k)
            .commandIdLow((static_cast<std::int64_t>(k) << 2) | 1)
            .kind(money::DispatchKind::CANCEL)
            .userId(3000 + k)
            .omsOrderId(4000 + k)
            .oldOrderId(5000 + k)
            .marketId(6 + k)
            .orderType(1)
            .side(static_cast<std::uint8_t>(k))
            .price(700'000'000 + k)
            .quantity(800'000'000 + k)
            .budget(900'000'000 + k)
            .timeInForce(2)
            .releaseScope(money::ReleaseScope::ORDER)
            .expectedOldRemaining(1'100'000'000LL + k);
    }
    mine.resize(money::MessageHeader::encodedLength() + encoder.encodedLength());
    CHECK(mine == golden);
}

TEST_CASE("MeDispatch entry -> DurableOrderCommand: the one-to-one copy equals match's Java encoder bytes")
{
    // rc2-forwarder.md 2.5 step 3, at ME v12: the 12 intent fields copied, dispatchSeq dropped,
    // timeInForce and expectedOldRemaining now carried (X6 closed by the v12 template 9 tail).
    // Entry 1 of the batch golden is the source.
    std::vector<std::uint8_t> golden = JAVA_ME_DISPATCH_BATCH;
    money::MessageHeader header(bytes(golden), 0, golden.size(), 0);
    money::MeDispatchBatch batch(bytes(golden), 8, golden.size(), header.blockLength(), header.version());
    auto &entries = batch.dispatches();
    entries.next();
    entries.next();

    std::vector<std::uint8_t> mine(128, 0xEE);
    me::DurableOrderCommand command;
    command.wrapAndApplyHeader(bytes(mine), 0, mine.size())
        .commandIdHigh(entries.commandIdHigh())
        .commandIdLow(entries.commandIdLow())
        .userId(entries.userId())
        .omsOrderId(entries.omsOrderId())
        .oldOrderId(entries.oldOrderId())
        .price(entries.price())
        .quantity(entries.quantity())
        .budget(entries.budget())
        .marketId(entries.marketId())
        .commandKind(entries.kindRaw())
        .orderType(entries.orderType())
        .orderSide(entries.side())
        .timeInForce(entries.timeInForce())
        .expectedOldRemaining(entries.expectedOldRemaining());
    mine.resize(me::MessageHeader::encodedLength() + command.encodedLength());
    CHECK(mine == JAVA_DURABLE_ORDER_COMMAND);
    me::MessageHeader meHeader(bytes(mine), 0, mine.size(), 0);
    CHECK(meHeader.version() == 12); // this build speaks v12; the ME's floor for template 9 is 11
}

TEST_CASE("ClusterHeartbeat v12 (block 17) decodes with the frame's own blockLength and version")
{
    std::vector<std::uint8_t> golden = JAVA_CLUSTER_HEARTBEAT;
    me::MessageHeader h(bytes(golden), 0, golden.size(), 0);
    CHECK(h.templateId() == 21);
    CHECK(h.blockLength() == 17);
    CHECK(h.version() == 12);
    me::ClusterHeartbeat hb(bytes(golden), 8, golden.size(), h.blockLength(), h.version());
    CHECK(hb.nodeId() == 2);
    CHECK(hb.timestamp() == 1'700'000'000'123LL);
    CHECK(hb.meEpochInActingVersion());
    CHECK(hb.meEpoch() == 0x5152535455565758LL);

    std::vector<std::uint8_t> mine(golden.size() + 16, 0xEE);
    me::ClusterHeartbeat encoder;
    encoder.wrapAndApplyHeader(bytes(mine), 0, mine.size())
        .nodeId(2)
        .timestamp(1'700'000'000'123LL)
        .meEpoch(0x5152535455565758LL);
    mine.resize(me::MessageHeader::encodedLength() + encoder.encodedLength());
    CHECK(mine == golden);
}

TEST_CASE("ClusterHeartbeat from a pre-12 ME (block 9, version 11) still decodes: meEpoch is out of the acting version")
{
    // A frame an ME at order-schema v11 puts on egress: header block 9 / version 11, 9 bytes of body.
    std::vector<std::uint8_t> frame = JAVA_CLUSTER_HEARTBEAT;
    frame.resize(8 + 9);
    frame[0] = 9;  // blockLength (LE)
    frame[1] = 0;
    frame[6] = 11; // version (LE)
    frame[7] = 0;
    me::MessageHeader h(bytes(frame), 0, frame.size(), 0);
    CHECK(h.blockLength() == 9);
    CHECK(h.version() == 11);
    me::ClusterHeartbeat hb(bytes(frame), 8, frame.size(), h.blockLength(), h.version());
    CHECK(hb.nodeId() == 2);
    CHECK(hb.timestamp() == 1'700'000'000'123LL);
    CHECK_FALSE(hb.meEpochInActingVersion());
    CHECK(hb.meEpoch() == me::ClusterHeartbeat::meEpochNullValue());
}

TEST_CASE("OrderStatusBatch: Java golden decodes entry by entry through the generated codec")
{
    std::vector<std::uint8_t> golden = JAVA_ORDER_STATUS_BATCH;
    me::MessageHeader h(bytes(golden), 0, golden.size(), 0);
    CHECK(h.templateId() == 24);
    CHECK(h.version() == 12);
    me::OrderStatusBatch batch(bytes(golden), 8, golden.size(), h.blockLength(), h.version());
    CHECK(batch.marketId() == 7);
    CHECK(batch.timestamp() == 1'700'000'000'456LL);
    auto &orders = batch.orders();
    REQUIRE(orders.count() == 2);
    int i = 0;
    while (orders.hasNext())
    {
        orders.next();
        CHECK(orders.orderId() == 9000 + i);
        CHECK(orders.userId() == 3000 + i);
        CHECK(orders.status() == me::OrderStatus::PARTIALLY_FILLED);
        CHECK(orders.price() == 700'000'000LL + i);
        CHECK(orders.remainingQty() == 100 + i);
        CHECK(orders.filledQty() == 200 + i);
        CHECK(orders.side() == me::OrderSide::ASK);
        CHECK(orders.omsOrderId() == 4000 + i);
        CHECK(orders.statusSeq() == 500 + i);
        CHECK(orders.rejectReason() == 0);
        CHECK(orders.egressSeq() == 600 + i);
        CHECK(orders.clusterTimestamp() == 1'700'000'000'000LL + i);
        i++;
    }
    CHECK(i == 2);
}

TEST_CASE("ME journal codecs are generated: schemaId 4 v1, templates 1..5")
{
    CHECK(journal::Trade::sbeTemplateId() == 1);
    CHECK(journal::Trade::sbeSchemaId() == 4);
    CHECK(journal::Trade::sbeSchemaVersion() == 1);
}
