// RC2 S12 contract 11.2 ForwarderCodecTest: the forwarder codec against the engines' own Java encoders
// (golden/golden_frames.inc, regen.sh at the W8 pins: assets b05b8e8, match 68a9375). Decode every money
// golden into Dispatch values field by field; the raw-enum golden decodes without throwing and classifies
// V12 / X2; 65 and 9/10 encode byte-equal to Java; and the one-to-one proof: Java 105 bytes -> C++ decode ->
// classification -> send ring -> drain plan -> encode -> byte-equal to the frames match's encoder produced.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "cluster_tools/adapters/sbe/ForwarderCodec.h"

#include "com_openexchange_assets_infrastructure_generated/MeDispatchBatch.h"
#include "cluster_tools/domain/forwarder/DispatchRules.h"
#include "cluster_tools/domain/forwarder/SendRing.h"

#include "golden/golden_frames.inc"

using namespace cluster_tools::domain::forwarder;
namespace codec = cluster_tools::sbe_adapter::forwarder_codec;

namespace {

using Bytes = std::vector<std::uint8_t>;

codec::Header headerOf(const Bytes &frame)
{
    codec::Header h;
    REQUIRE(codec::readHeader(frame.data(), frame.size(), h));
    return h;
}

std::vector<Dispatch> decodeBatch(const Bytes &frame)
{
    std::vector<Dispatch> out;
    const codec::Header h = headerOf(frame);
    REQUIRE(h.schemaId == 2);
    REQUIRE(h.version == 7);
    REQUIRE(codec::decodeDispatchBatch(frame.data(), frame.size(), h,
        [](void *context, const Dispatch &d) { static_cast<std::vector<Dispatch> *>(context)->push_back(d); }, &out));
    return out;
}

Dispatch e2e(int i)
{
    // GoldenMoney.E2E, row i
    static const std::int64_t rows[5][16] = {
        { 501, 1001, (3 << 2) | 0, 0, 3001, 1001, 0, 7, 0, 1, 700'000'001LL, 800'000'001LL, 900'000'001LL, 1, 0, 0 },
        { 502, 1002, (5 << 2) | 1, 1, 3002, 1002, 5002, 8, 2, 0, 700'000'002LL, 800'000'002LL, 0, 0, 0, 0 },
        { 503, 1003, (7 << 2) | 2, 2, 3003, 1003, 5003, 9, 0, 1, 700'000'003LL, 800'000'003LL, 0, 0, 0, 1'100'000'003LL },
        { 504, 1004, (9 << 2) | 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 505, 1005, (11 << 2) | 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 },
    };
    const std::int64_t *v = rows[i];
    Dispatch d;
    d.dispatchSeq = v[0];
    d.commandIdHigh = v[1];
    d.commandIdLow = v[2];
    d.kind = static_cast<std::uint8_t>(v[3]);
    d.userId = v[4];
    d.omsOrderId = v[5];
    d.oldOrderId = v[6];
    d.marketId = static_cast<std::int32_t>(v[7]);
    d.orderType = static_cast<std::uint8_t>(v[8]);
    d.side = static_cast<std::uint8_t>(v[9]);
    d.price = v[10];
    d.quantity = v[11];
    d.budget = v[12];
    d.timeInForce = static_cast<std::uint8_t>(v[13]);
    d.releaseScope = static_cast<std::uint8_t>(v[14]);
    d.expectedOldRemaining = v[15];
    return d;
}

Bytes encode(const DurableCommand &c)
{
    Bytes out(codec::DURABLE_COMMAND_FRAME_LENGTH + 16, 0xEE);
    const std::size_t n = codec::encodeDurableCommand(out.data(), out.size(), c);
    REQUIRE(n == codec::DURABLE_COMMAND_FRAME_LENGTH);
    out.resize(n);
    return out;
}

Bytes encode(std::int64_t releaseSeq, const std::vector<ReleaseEntry> &entries)
{
    Bytes out(codec::RELEASE_FRAME_MAX_LENGTH + 64, 0xEE);
    const std::size_t n = codec::encodeReleaseCommandIds(out.data(), out.size(), releaseSeq, entries.data(),
        static_cast<std::uint32_t>(entries.size()));
    REQUIRE(n == codec::releaseFrameLength(static_cast<std::uint32_t>(entries.size())));
    out.resize(n);
    return out;
}

}

TEST_CASE("ForwarderCodecTest.singleAmendDecodesFieldByField")
{
    const Bytes golden = JAVA_ME_DISPATCH_AMEND_V7;
    const codec::Header h = headerOf(golden);
    CHECK(h.templateId == 104);
    CHECK(h.blockLength == 89);
    Dispatch d;
    REQUIRE(codec::decodeDispatch(golden.data(), golden.size(), h, d));
    CHECK(d.dispatchSeq == 0x0A0B0C0D);
    CHECK(d.commandIdHigh == 2101);
    CHECK(d.commandIdLow == ((4 << 2) | 2));
    CHECK(d.kind == KIND_AMEND);
    CHECK(d.userId == 2201);
    CHECK(d.omsOrderId == 2101);
    CHECK(d.oldOrderId == 2301);
    CHECK(d.marketId == 24);
    CHECK(d.orderType == 2);
    CHECK(d.side == 1);
    CHECK(d.price == 2501);
    CHECK(d.quantity == 2601);
    CHECK(d.budget == 2701);
    CHECK(d.timeInForce == 0);
    CHECK(d.releaseScope == 0);
    CHECK(d.expectedOldRemaining == 0x3132333435363738LL);
}

TEST_CASE("ForwarderCodecTest.batchV7DecodesEveryEntryFieldByField")
{
    const std::vector<Dispatch> got = decodeBatch(JAVA_ME_DISPATCH_BATCH_V7);
    REQUIRE(got.size() == 5);
    for (int i = 0; i < 5; i++)
    {
        CAPTURE(i);
        CHECK(got[static_cast<std::size_t>(i)] == e2e(i));
    }
    // the S11 golden too (two CANCELs with releaseScope ORDER, ignored on kind 1)
    const std::vector<Dispatch> old = decodeBatch(JAVA_ME_DISPATCH_BATCH);
    REQUIRE(old.size() == 2);
    CHECK(old[1].dispatchSeq == 1001);
    CHECK(old[1].releaseScope == 1);
    CHECK(old[1].expectedOldRemaining == 1'100'000'001LL);
}

TEST_CASE("ForwarderCodecTest.rawEnumsDecodeWithoutThrowingAndClassifyV12AndX2")
{
    const std::vector<Dispatch> got = decodeBatch(JAVA_ME_DISPATCH_RAW_ENUMS);
    REQUIRE(got.size() == 2);
    CHECK(got[0].kind == 4);
    CHECK(classify(got[0]) == Classification{ DispatchClass::CARRIER, RuleId::V12 });
    CHECK(got[1].kind == KIND_RELEASE_ID);
    CHECK(got[1].releaseScope == 2);
    CHECK(classify(got[1]) == Classification{ DispatchClass::RELEASE_REFUSED, RuleId::X2 });
}

TEST_CASE("ForwarderCodecTest.theGeneratedEnumGettersWouldThrowOnTheRawEnumGolden")
{
    // The hazard the raw reads avoid (FIX-10): the generated getter throws on a kind or scope the schema does not
    // name; an exception escaping the duty loop would be F2 (exit 70) for a core defect the contract relays.
    namespace money = com::openexchange::assets::infrastructure::generated;
    Bytes golden = JAVA_ME_DISPATCH_RAW_ENUMS;
    money::MeDispatchBatch batch(reinterpret_cast<char *>(golden.data()), 8, golden.size(), 0, 7);
    auto &entries = batch.dispatches();
    entries.next();
    CHECK(entries.kindRaw() == 4);
    CHECK_THROWS((void)entries.kind());
    entries.next();
    CHECK(entries.releaseScopeRaw() == 2);
    CHECK_THROWS((void)entries.releaseScope());
}

TEST_CASE("ForwarderCodecTest.pendingEndAdmissionAndRejectsDecode")
{
    codec::PendingEnd end;
    const Bytes page = JAVA_PENDING_DISPATCH_END;
    REQUIRE(codec::decodePendingEnd(page.data(), page.size(), headerOf(page), end));
    CHECK(end.correlationId == 77);
    CHECK(end.count == 4096);
    CHECK(end.headDispatchSeq == 123456789);
    const Bytes probe = JAVA_PENDING_DISPATCH_END_PROBE;
    REQUIRE(codec::decodePendingEnd(probe.data(), probe.size(), headerOf(probe), end));
    CHECK(end.correlationId == 0x6502);
    CHECK(end.count == 0);
    CHECK(end.headDispatchSeq == 9999);

    codec::Admission a;
    const Bytes state = JAVA_ADMISSION_STATE_V7;
    REQUIRE(codec::decodeAdmission(state.data(), state.size(), headerOf(state), a));
    CHECK(a.marketId == 0);
    CHECK(a.gateState == 1);
    CHECK(a.gateReasons == ((1u << 2) | (1u << 4)));

    codec::OrderRejectView r;
    const Bytes invalid = JAVA_ORDER_REJECT_65_INVALID;
    REQUIRE(codec::decodeOrderReject(invalid.data(), invalid.size(), headerOf(invalid), r));
    CHECK(r.correlationId == 0x6501);
    CHECK(r.refTemplateId == 65);
    CHECK(r.reason == codec::REJECT_INVALID);
    const Bytes unregistered = JAVA_ORDER_REJECT_65_UNREGISTERED;
    REQUIRE(codec::decodeOrderReject(unregistered.data(), unregistered.size(), headerOf(unregistered), r));
    CHECK(r.correlationId == 0x6502);
    CHECK(r.reason == codec::REJECT_UNREGISTERED_SESSION);
}

TEST_CASE("ForwarderCodecTest.requestPendingDispatchEncodesByteEqualToJava")
{
    Bytes out(64, 0xEE);
    PendingRequest probe;
    probe.kind = RequestKind::PROBE;
    probe.correlationId = 0x6502;
    probe.fromDispatchSeq = 777;
    probe.maxEntries = 0;
    std::size_t n = codec::encodeRequestPendingDispatch(out.data(), out.size(), probe);
    CHECK(Bytes(out.begin(), out.begin() + static_cast<long>(n)) == JAVA_REQUEST_PENDING_DISPATCH_PROBE);
    PendingRequest page;
    page.kind = RequestKind::PAGE;
    page.correlationId = 0x6503;
    page.fromDispatchSeq = 778;
    page.maxEntries = 4096;
    n = codec::encodeRequestPendingDispatch(out.data(), out.size(), page);
    CHECK(Bytes(out.begin(), out.begin() + static_cast<long>(n)) == JAVA_REQUEST_PENDING_DISPATCH_PAGE);
    CHECK(codec::encodeRequestPendingDispatch(out.data(), 27, page) == 0); // too small: nothing, no throw
}

TEST_CASE("ForwarderCodecTest.template9AndCarrierEncodeByteEqualToJava")
{
    CHECK(encode(verbatimCommand(e2e(0))) == JAVA_DURABLE_ORDER_COMMAND_V12_CREATE);
    CHECK(encode(verbatimCommand(e2e(1))) == JAVA_DURABLE_ORDER_COMMAND_V12_CANCEL);
    CHECK(encode(verbatimCommand(e2e(2))) == JAVA_DURABLE_ORDER_COMMAND_V12_AMEND);
    Dispatch bad = e2e(0);
    bad.userId = 0;
    REQUIRE(classify(bad).cls == DispatchClass::CARRIER);
    CHECK(encode(carrierCommand(bad)) == JAVA_DURABLE_ORDER_COMMAND_V12_CARRIER);
    Bytes small(87);
    CHECK(codec::encodeDurableCommand(small.data(), small.size(), verbatimCommand(e2e(0))) == 0);
}

TEST_CASE("ForwarderCodecTest.template10EncodesByteEqualToJava")
{
    CHECK(encode(504, { releaseEntry(e2e(3)) }) == JAVA_RELEASE_COMMAND_IDS_V12_1);
    std::vector<ReleaseEntry> many;
    for (int i = 0; i < 48; i++)
    {
        many.push_back(ReleaseEntry{ 5000 + i, (static_cast<std::int64_t>(i) << 2) | (i % 3), static_cast<std::uint8_t>(i % 2) });
    }
    const Bytes frame = encode(9000, many);
    CHECK(frame == JAVA_RELEASE_COMMAND_IDS_V12_48);
    CHECK(frame.size() == 836);
    CHECK(codec::encodeReleaseCommandIds(nullptr, 0, 1, many.data(), 1) == 0);
}

TEST_CASE("ForwarderCodecTest.endToEndJavaBatchToJavaMeFrames")
{
    // the one-to-one proof (11.2): every byte the ME receives is the match encoder's own frame for the values
    // the core minted
    const std::vector<Dispatch> decoded = decodeBatch(JAVA_ME_DISPATCH_BATCH_V7);
    SendRing ring;
    for (const Dispatch &d : decoded)
    {
        const Classification c = classify(d);
        REQUIRE(DispatchClass::PASS == c.cls);
        RingSlot slot;
        REQUIRE(toRingSlot(d, c, slot));
        REQUIRE(ring.push(slot));
    }
    std::vector<Bytes> frames;
    while (!ring.empty())
    {
        const FramePlan plan = ring.nextFrame();
        if (FrameType::COMMAND == plan.type)
        {
            frames.push_back(encode(ring.at(0).command));
        }
        else
        {
            std::vector<ReleaseEntry> entries;
            for (std::uint32_t i = 0; i < plan.count; i++)
            {
                entries.push_back(ring.at(i).release);
            }
            frames.push_back(encode(plan.firstSeq, entries));
        }
        ring.pop(plan.count);
    }
    REQUIRE(frames.size() == 4);
    CHECK(frames[0] == JAVA_DURABLE_ORDER_COMMAND_V12_CREATE);
    CHECK(frames[1] == JAVA_DURABLE_ORDER_COMMAND_V12_CANCEL);
    CHECK(frames[2] == JAVA_DURABLE_ORDER_COMMAND_V12_AMEND);
    CHECK(frames[3] == JAVA_RELEASE_COMMAND_IDS_V12_E2E);
}

TEST_CASE("ForwarderCodecTest.heartbeatEpochDecodes")
{
    const Bytes hb = JAVA_CLUSTER_HEARTBEAT_V12;
    const codec::Header h = headerOf(hb);
    CHECK(h.schemaId == 1);
    CHECK(h.version == 12);
    std::int64_t epoch = 0;
    REQUIRE(codec::decodeHeartbeatEpoch(hb.data(), hb.size(), h, epoch));
    CHECK(epoch == 0x0A1B2C3D4E5F6071LL);
}

TEST_CASE("ForwarderCodecTest.malformedFramesAreRefusedNotRead")
{
    Bytes single = JAVA_ME_DISPATCH_AMEND_V7;
    Dispatch d;
    codec::Header h = headerOf(single);
    h.blockLength = 88; // an acting block below 89
    CHECK_FALSE(codec::decodeDispatch(single.data(), single.size(), h, d));
    h = headerOf(single);
    CHECK_FALSE(codec::decodeDispatch(single.data(), single.size() - 1, h, d)); // truncated

    Bytes batch = JAVA_ME_DISPATCH_BATCH_V7;
    int delivered = 0;
    auto count = [](void *context, const Dispatch &) { (*static_cast<int *>(context))++; };
    batch[8] = 80; // group blockLength 80
    CHECK_FALSE(codec::decodeDispatchBatch(batch.data(), batch.size(), headerOf(batch), count, &delivered));
    batch = JAVA_ME_DISPATCH_BATCH_V7;
    batch.resize(batch.size() - 1); // the last entry does not fit: nothing is delivered
    CHECK_FALSE(codec::decodeDispatchBatch(batch.data(), batch.size(), headerOf(batch), count, &delivered));
    batch = JAVA_ME_DISPATCH_BATCH_V7;
    batch[10] = 0xFF; // numInGroup 255
    CHECK_FALSE(codec::decodeDispatchBatch(batch.data(), batch.size(), headerOf(batch), count, &delivered));
    CHECK(delivered == 0);
    codec::Header tiny;
    CHECK_FALSE(codec::readHeader(single.data(), 7, tiny));
    // a larger acting block (a later schema appending fields) still decodes the v7 prefix
    Bytes wide = JAVA_ME_DISPATCH_AMEND_V7;
    wide.insert(wide.end(), { 1, 2, 3, 4 });
    wide[0] = 93;
    REQUIRE(codec::decodeDispatch(wide.data(), wide.size(), headerOf(wide), d));
    CHECK(d.expectedOldRemaining == 0x3132333435363738LL);
}

TEST_CASE("ForwarderCodecTest.dispatchSeqBelowOneMakesTheFrameMalformed")
{
    // Review NOTE-2: the whole 105 is refused (nothing delivered) when any entry has dispatchSeq < 1; so is a 104.
    Bytes batch = JAVA_ME_DISPATCH_BATCH_V7;
    const std::int64_t zero = 0;
    std::memcpy(batch.data() + 12 + 2 * 89, &zero, 8); // entry 2's dispatchSeq
    int delivered = 0;
    bool below = false;
    auto count = [](void *context, const Dispatch &) { (*static_cast<int *>(context))++; };
    CHECK_FALSE(codec::decodeDispatchBatch(batch.data(), batch.size(), headerOf(batch), count, &delivered, &below));
    CHECK(below);
    CHECK(delivered == 0);
    Bytes single = JAVA_ME_DISPATCH_AMEND_V7;
    const std::int64_t minus = -1;
    std::memcpy(single.data() + 8, &minus, 8);
    Dispatch d;
    CHECK_FALSE(codec::decodeDispatch(single.data(), single.size(), headerOf(single), d, &below));
    CHECK(below);
    const Bytes good = JAVA_ME_DISPATCH_AMEND_V7;
    CHECK(codec::decodeDispatch(good.data(), good.size(), headerOf(good), d, &below));
    CHECK_FALSE(below);
}
