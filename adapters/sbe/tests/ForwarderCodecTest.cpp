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

// ============================================================================================================
// RC2 S13 W1 (contract 4.2, 11.3): the book egress decoders (22 / 25 / 26, raw reads) and the MarketTick 60 encoder
// against the match / core engines' own Java encoders. Every malformed rule has its own test case with its own CHECK
// text: adapters/sbe/tests/forwarder-codec-negative-controls.txt switches each rule off (CT_TEST_DISABLE_<rule>) and
// requires exactly that CHECK to fail.
// ============================================================================================================

namespace {

using cluster_tools::domain::forwarder::BookFrame;
using cluster_tools::domain::forwarder::Malformed;
using cluster_tools::domain::forwarder::TradeFrame;
using cluster_tools::domain::forwarder::MarketTick;

// little-endian patches on a copy of a golden (the header is 8 bytes; a 22 root block is 36, a 25 root block 44, a 26 root 12)
void put16(Bytes &b, std::size_t at, std::uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
void put32(Bytes &b, std::size_t at, std::int32_t v) { std::memcpy(b.data() + at, &v, 4); }
void put64(Bytes &b, std::size_t at, std::int64_t v) { std::memcpy(b.data() + at, &v, 8); }

constexpr std::size_t SNAP_BIDS_HDR = 8 + 36;           // first group header of a 22
constexpr std::size_t SNAP_ENTRY = 20;
constexpr std::size_t DELTA_GRP_HDR = 8 + 44;           // the group header of a 25
constexpr std::size_t DELTA_ENTRY = 22;
constexpr std::size_t TRADE_GRP_HDR = 8 + 12;           // the group header of a 26
constexpr std::size_t TRADE_ENTRY = 89;

Malformed snapshotOf(const Bytes &frame, BookFrame *keep = nullptr)
{
    static BookFrame scratch; // ~3.5 kB: static keeps the stack small
    BookFrame &f = keep != nullptr ? *keep : scratch;
    codec::Header h;
    REQUIRE(codec::readHeader(frame.data(), frame.size(), h));
    codec::decodeBookSnapshot(frame.data(), frame.size(), h, f);
    return f.malformed;
}

Malformed deltaOf(const Bytes &frame, BookFrame *keep = nullptr)
{
    static BookFrame scratch;
    BookFrame &f = keep != nullptr ? *keep : scratch;
    codec::Header h;
    REQUIRE(codec::readHeader(frame.data(), frame.size(), h));
    codec::decodeBookDelta(frame.data(), frame.size(), h, f);
    return f.malformed;
}

Malformed tradeOf(const Bytes &frame, TradeFrame *keep = nullptr)
{
    TradeFrame local;
    TradeFrame &f = keep != nullptr ? *keep : local;
    codec::Header h;
    REQUIRE(codec::readHeader(frame.data(), frame.size(), h));
    codec::decodeTradeBatch(frame.data(), frame.size(), h, f);
    return f.malformed;
}

}

TEST_CASE("ForwarderCodecTest.bookSnapshot20x20DecodesFieldByField")
{
    const Bytes g = JAVA_BOOK_SNAPSHOT_20x20;
    const codec::Header h = headerOf(g);
    CHECK(h.templateId == 22);
    CHECK(h.schemaId == 1);
    CHECK(h.version == 12);
    CHECK(h.blockLength == 36);
    BookFrame f;
    REQUIRE(codec::decodeBookSnapshot(g.data(), g.size(), h, f));
    CHECK(f.malformed == Malformed::NONE);
    CHECK(f.snapshot);
    CHECK(f.marketReadable);
    CHECK(f.marketId == 3);
    CHECK(f.bookVersion == 0x0000010203040506LL);
    CHECK(f.fromVersion == 0);
    CHECK(f.headerBlockLength == 36);
    CHECK(f.groupBlockLength[0] == 20);
    CHECK(f.groupBlockLength[1] == 20);
    CHECK(f.statedCount[0] == 20);
    CHECK(f.statedCount[1] == 20);
    CHECK(f.lengthOk);
    REQUIRE(f.nBids == 20);
    REQUIRE(f.nAsks == 20);
    for (int i = 0; i < 20; i++)
    {
        CHECK(f.bids[i].price == 1'499'000'000LL - i * 1'000'000LL);
        CHECK(f.bids[i].quantity == 100'000'000LL + i);
        CHECK(f.bids[i].orderCount == 1 + i);
        CHECK(f.asks[i].price == 1'500'000'000LL + i * 1'000'000LL);
        CHECK(f.asks[i].quantity == 200'000'000LL + i);
        CHECK(f.asks[i].orderCount == 21 + i);
    }
    CHECK(g.size() == 8 + 36 + 4 + 20 * 20 + 4 + 20 * 20);
}

TEST_CASE("ForwarderCodecTest.emptyAndUnversionedSnapshotsDecode")
{
    BookFrame f;
    const Bytes empty = JAVA_BOOK_SNAPSHOT_EMPTY; // W-ME2: a versioned book that emptied
    REQUIRE(codec::decodeBookSnapshot(empty.data(), empty.size(), headerOf(empty), f));
    CHECK(f.marketId == 3);
    CHECK(f.bookVersion == 777);
    CHECK(f.nBids == 0);
    CHECK(f.nAsks == 0);
    CHECK(f.lengthOk);
    const Bytes unversioned = JAVA_BOOK_SNAPSHOT_UNVERSIONED; // rule U: version 0, both groups empty, NOT malformed
    REQUIRE(codec::decodeBookSnapshot(unversioned.data(), unversioned.size(), headerOf(unversioned), f));
    CHECK(f.marketId == 4);
    CHECK(f.bookVersion == 0);
    CHECK(f.malformed == Malformed::NONE);
}

TEST_CASE("ForwarderCodecTest.bookDelta80DecodesFieldByField")
{
    const Bytes g = JAVA_BOOK_DELTA_80;
    const codec::Header h = headerOf(g);
    CHECK(h.templateId == 25);
    CHECK(h.blockLength == 44);
    BookFrame f;
    REQUIRE(codec::decodeBookDelta(g.data(), g.size(), h, f));
    CHECK_FALSE(f.snapshot);
    CHECK(f.marketId == 3);
    CHECK(f.bookVersion == 0x0000020000000050LL);
    CHECK(f.fromVersion == 0x0000020000000001LL);
    CHECK(f.groupBlockLength[0] == 22);
    CHECK(f.statedCount[0] == 80);
    REQUIRE(f.nChanges == 80);
    int at = 0;
    for (int side = 0; side < 2; side++)
    {
        const std::int64_t base = side == 0 ? 1'400'000'000LL : 1'600'000'000LL;
        for (int i = 0; i < 20; i++, at++)
        {
            CHECK(f.changes[at].price == base + i * 1'000'000LL);
            CHECK(f.changes[at].quantity == 300'000'000LL + i);
            CHECK(f.changes[at].orderCount == 1 + i);
            CHECK(f.changes[at].side == side);
            CHECK(f.changes[at].updateType == 0);
        }
        for (int i = 0; i < 20; i++, at++)
        {
            CHECK(f.changes[at].price == base - (i + 1) * 1'000'000LL);
            CHECK(f.changes[at].quantity == 0);
            CHECK(f.changes[at].orderCount == 0);
            CHECK(f.changes[at].side == side);
            CHECK(f.changes[at].updateType == 2);
        }
    }
}

TEST_CASE("ForwarderCodecTest.rawEnumGoldenIsRefusedWithoutAGeneratedGetter")
{
    // side 2 and updateType 3 are not in the enums; the generated getter would throw. The raw read does not.
    const Bytes g = JAVA_BOOK_DELTA_RAW_ENUMS;
    BookFrame f;
    Malformed m = Malformed::NONE;
    REQUIRE_NOTHROW(m = deltaOf(g, &f));
    CHECK(m == Malformed::ENUM);
    CHECK(f.marketReadable); // the ladder of market 3 is the one that breaks
    CHECK(f.marketId == 3);
    REQUIRE(f.nChanges == 2);
    CHECK(f.changes[0].side == 0);
    CHECK(f.changes[0].updateType == 0);
    CHECK(f.changes[1].side == 2);       // verbatim
    CHECK(f.changes[1].updateType == 3); // verbatim
}

TEST_CASE("ForwarderCodecTest.tradeBatch3DecodesTheHighestTradeId")
{
    const Bytes g = JAVA_TRADE_EXECUTION_BATCH_3; // ids 41, 43, 42: the maximum is not the last
    const codec::Header h = headerOf(g);
    CHECK(h.templateId == 26);
    CHECK(h.blockLength == 12);
    TradeFrame f;
    REQUIRE(codec::decodeTradeBatch(g.data(), g.size(), h, f));
    CHECK(f.malformed == Malformed::NONE);
    CHECK(f.marketReadable);
    CHECK(f.marketId == 3);
    CHECK(f.entryBlockLength == 89);
    CHECK(f.statedCount == 3);
    CHECK(f.maxTradeId == 43);
    CHECK(f.priceAtMax == 1'499'000'010LL); // entry 1 (price + 10 * i)
    CHECK(g.size() == 8 + 12 + 4 + 3 * 89);
}

TEST_CASE("ForwarderCodecTest.marketTickEncodesByteEqualToJava")
{
    MarketTick t;
    t.marketId = 3;
    t.bookVersion = 0x0102030405060708LL;
    t.bestBid = 1'499'000'001LL;
    t.bestAsk = 1'500'000'002LL;
    t.lastTradePrice = 1'499'500'003LL;
    t.lastTradeId = 0x1112131415161718LL;
    t.meEpoch = 0x2122232425262728LL;
    Bytes out(codec::MARKET_TICK_FRAME_LENGTH + 16, 0xEE);
    const std::size_t n = codec::encodeMarketTick(out.data(), out.size(), t);
    REQUIRE(n == 60);
    CHECK(Bytes(out.begin(), out.begin() + static_cast<long>(n)) == JAVA_MARKET_TICK_V7);
    Bytes small(59);
    CHECK(codec::encodeMarketTick(small.data(), small.size(), t) == 0); // too small: nothing, no throw
    CHECK(codec::encodeMarketTick(nullptr, 0, t) == 0);
}

TEST_CASE("ForwarderCodecTest.wrongTemplateIsRefused")
{
    const Bytes delta = JAVA_BOOK_DELTA_80;
    BookFrame f;
    CHECK_FALSE(codec::decodeBookSnapshot(delta.data(), delta.size(), headerOf(delta), f));
    CHECK(f.malformed == Malformed::TEMPLATE);
    const Bytes snap = JAVA_BOOK_SNAPSHOT_EMPTY;
    CHECK_FALSE(codec::decodeBookDelta(snap.data(), snap.size(), headerOf(snap), f));
    CHECK(f.malformed == Malformed::TEMPLATE);
    TradeFrame t;
    CHECK_FALSE(codec::decodeTradeBatch(snap.data(), snap.size(), headerOf(snap), t));
    CHECK(t.malformed == Malformed::TEMPLATE);
}

// ---- 4.2: one test case per malformed rule (the negative controls switch the rule off and expect THIS CHECK to fail)

TEST_CASE("ForwarderCodecTest.headerBlockLengthBelowTheTemplateIsMalformed")
{
    Bytes snap = JAVA_BOOK_SNAPSHOT_EMPTY;
    put16(snap, 0, 35); // 22: block 36 -> 35
    Bytes delta = JAVA_BOOK_DELTA_RAW_ENUMS;
    put16(delta, 0, 43); // 25: 44 -> 43
    Bytes trade = JAVA_TRADE_EXECUTION_BATCH_3;
    put16(trade, 0, 11); // 26: 12 -> 11
    CHECK(snapshotOf(snap) == Malformed::HEADER_BLOCK);
    CHECK(deltaOf(delta) == Malformed::HEADER_BLOCK);
    CHECK(tradeOf(trade) == Malformed::HEADER_BLOCK);
}

TEST_CASE("ForwarderCodecTest.groupBlockLengthBelowTheEntryIsMalformed")
{
    // CL-F-9: a group header blockLength below the entry size (20 / 22 / 89); a larger one is skipped by its stated length
    Bytes snap = JAVA_BOOK_SNAPSHOT_20x20;
    put16(snap, SNAP_BIDS_HDR, 19);
    Bytes delta = JAVA_BOOK_DELTA_80;
    put16(delta, DELTA_GRP_HDR, 21);
    Bytes trade = JAVA_TRADE_EXECUTION_BATCH_BAD_GROUP_BL; // the Java golden states 88
    CHECK(snapshotOf(snap) == Malformed::GROUP_BLOCK);
    CHECK(deltaOf(delta) == Malformed::GROUP_BLOCK);
    CHECK(tradeOf(trade) == Malformed::GROUP_BLOCK);
    // the asks group of a 22 is a group too
    Bytes asks = JAVA_BOOK_SNAPSHOT_20x20;
    put16(asks, SNAP_BIDS_HDR + 4 + 20 * SNAP_ENTRY, 4);
    CHECK(snapshotOf(asks) == Malformed::GROUP_BLOCK);
}

TEST_CASE("ForwarderCodecTest.aLargerGroupBlockLengthIsSkippedByItsStatedLength")
{
    // the SBE extension rule: entries 24 bytes apart (4 bytes of a later schema after each 20-byte entry) decode
    const Bytes full = JAVA_BOOK_SNAPSHOT_20x20;
    Bytes snap(full.begin(), full.begin() + static_cast<long>(SNAP_BIDS_HDR)); // message header + the 36 B root
    auto push16 = [&snap](std::uint16_t v)
    {
        snap.push_back(static_cast<std::uint8_t>(v & 0xFF));
        snap.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    push16(24);
    push16(1); // one bid: golden bid 0, then 4 bytes a later schema appended
    snap.insert(snap.end(), full.begin() + static_cast<long>(SNAP_BIDS_HDR + 4), full.begin() + static_cast<long>(SNAP_BIDS_HDR + 4 + 20));
    snap.insert(snap.end(), { 0xAA, 0xBB, 0xCC, 0xDD });
    push16(24);
    push16(2); // two asks: golden asks 0 and 1, each padded to 24
    for (std::size_t i = 0; i < 2; i++)
    {
        const std::size_t at = SNAP_BIDS_HDR + 4 + 20 * 20 + 4 + i * 20;
        snap.insert(snap.end(), full.begin() + static_cast<long>(at), full.begin() + static_cast<long>(at + 20));
        snap.insert(snap.end(), { 0xEE, 0xEE, 0xEE, 0xEE });
    }
    BookFrame f;
    REQUIRE(snapshotOf(snap, &f) == Malformed::NONE);
    REQUIRE(f.nBids == 1);
    REQUIRE(f.nAsks == 2);
    CHECK(f.bids[0].price == 1'499'000'000LL);
    CHECK(f.asks[0].price == 1'500'000'000LL);
    CHECK(f.asks[1].price == 1'501'000'000LL); // the stride is the STATED 24, not 20
    CHECK(f.asks[1].orderCount == 22);
}

TEST_CASE("ForwarderCodecTest.groupCountAboveTheCapIsMalformed")
{
    Bytes snap = JAVA_BOOK_SNAPSHOT_20x20;
    put16(snap, SNAP_BIDS_HDR + 2, 21); // 21 bids (the frame holds 20: the count is checked before the length)
    Bytes delta = JAVA_BOOK_DELTA_80;
    put16(delta, DELTA_GRP_HDR + 2, 81);
    BookFrame f;
    CHECK(snapshotOf(snap, &f) == Malformed::COUNT);
    CHECK(f.nBids == 0); // nothing above the cap is copied
    CHECK(deltaOf(delta) == Malformed::COUNT);
}

TEST_CASE("ForwarderCodecTest.aFrameShorterThanItsGroupsIsMalformed")
{
    Bytes snap = JAVA_BOOK_SNAPSHOT_20x20;
    snap.resize(snap.size() - 1);
    Bytes delta = JAVA_BOOK_DELTA_80;
    delta.resize(delta.size() - 1);
    Bytes trade = JAVA_TRADE_EXECUTION_BATCH_3;
    put16(trade, TRADE_GRP_HDR + 2, 4); // a count whose entries do not fit the frame
    Bytes noGroup = JAVA_BOOK_SNAPSHOT_20x20;
    noGroup.resize(8 + 36 + 2); // the group header itself is cut
    CHECK(snapshotOf(snap) == Malformed::LENGTH);
    CHECK(deltaOf(delta) == Malformed::LENGTH);
    CHECK(tradeOf(trade) == Malformed::LENGTH);
    CHECK(snapshotOf(noGroup) == Malformed::LENGTH);
    // a frame too short to hold even the market id: malformed and the market is unreadable
    Bytes tiny = JAVA_BOOK_SNAPSHOT_20x20;
    tiny.resize(8 + 3);
    BookFrame f;
    CHECK(snapshotOf(tiny, &f) != Malformed::NONE);
    CHECK_FALSE(f.marketReadable);
    CHECK(f.nBids == 0);
}

TEST_CASE("ForwarderCodecTest.snapshotVersionRulesIncludingRuleU")
{
    Bytes negative = JAVA_BOOK_SNAPSHOT_20x20;
    put64(negative, 8 + 28, -1);
    Bytes zeroNonEmpty = JAVA_BOOK_SNAPSHOT_20x20;
    put64(zeroNonEmpty, 8 + 28, 0); // bookVersion 0 with levels
    CHECK(snapshotOf(negative) == Malformed::VERSION);
    CHECK(snapshotOf(zeroNonEmpty) == Malformed::VERSION);
    // rule U (4.2): version 0 with BOTH groups empty is well formed (the ladder turns it into EMPTY)
    const Bytes unversioned = JAVA_BOOK_SNAPSHOT_UNVERSIONED;
    const bool unversionedEmptyIsWellFormed = snapshotOf(unversioned) == Malformed::NONE;
    CHECK(unversionedEmptyIsWellFormed);
}

TEST_CASE("ForwarderCodecTest.deltaVersionMustRiseAboveFrom")
{
    // RC-N3: bookVersion <= fromVersion, bookVersion 0, fromVersion < 0 are all malformed
    Bytes equal = JAVA_BOOK_DELTA_RAW_ENUMS; // bookVersion 2000, fromVersion 1000; the raw-enum entry is reset below
    put64(equal, 8 + 36, 2000);              // from == version
    Bytes below = JAVA_BOOK_DELTA_80;
    put64(below, 8 + 36, 0x0000020000000051LL); // from one above version
    Bytes zero = JAVA_BOOK_DELTA_80;
    put64(zero, 8 + 28, 0);
    Bytes negFrom = JAVA_BOOK_DELTA_80;
    put64(negFrom, 8 + 36, -5);
    // make `equal` otherwise valid: side/updateType of entry 1 back to valid raw values
    put16(equal, 0, 44);
    equal[DELTA_GRP_HDR + 4 + DELTA_ENTRY + 20] = 1;
    equal[DELTA_GRP_HDR + 4 + DELTA_ENTRY + 21] = 1;
    CHECK(deltaOf(equal) == Malformed::VERSION);
    CHECK(deltaOf(below) == Malformed::VERSION);
    CHECK(deltaOf(zero) == Malformed::VERSION);
    CHECK(deltaOf(negFrom) == Malformed::VERSION);
}

TEST_CASE("ForwarderCodecTest.rawEnumsAboveTheirRangeAreMalformed")
{
    Bytes side = JAVA_BOOK_DELTA_80;
    side[DELTA_GRP_HDR + 4 + 20] = 2; // entry 0 side
    Bytes update = JAVA_BOOK_DELTA_80;
    update[DELTA_GRP_HDR + 4 + 21] = 3; // entry 0 updateType
    CHECK(deltaOf(side) == Malformed::ENUM);
    CHECK(deltaOf(update) == Malformed::ENUM);
    Bytes valid = JAVA_BOOK_DELTA_80; // side 1 and updateType 2 are the highest valid raw values
    valid[DELTA_GRP_HDR + 4 + 20] = 1;
    valid[DELTA_GRP_HDR + 4 + 21] = 2;
    put64(valid, DELTA_GRP_HDR + 4 + 0, 1'234'000'000LL); // a DELETE needs only a price
    CHECK(deltaOf(valid) == Malformed::NONE);
}

TEST_CASE("ForwarderCodecTest.nonPositivePricesQuantitiesAndCountsAreMalformed")
{
    // snapshot levels
    Bytes bidQty = JAVA_BOOK_SNAPSHOT_20x20;
    put64(bidQty, SNAP_BIDS_HDR + 4 + 8, 0);
    Bytes askCount = JAVA_BOOK_SNAPSHOT_20x20;
    put32(askCount, SNAP_BIDS_HDR + 4 + 20 * SNAP_ENTRY + 4 + 16, 0);
    Bytes bidPrice = JAVA_BOOK_SNAPSHOT_20x20;
    put64(bidPrice, SNAP_BIDS_HDR + 4 + 5 * SNAP_ENTRY, -1);
    // delta: NEW (entry 0) with qty 0, count 0, price 0; DELETE (entry 20) with price 0
    Bytes newQty = JAVA_BOOK_DELTA_80;
    put64(newQty, DELTA_GRP_HDR + 4 + 8, 0);
    Bytes newCount = JAVA_BOOK_DELTA_80;
    put32(newCount, DELTA_GRP_HDR + 4 + 16, 0);
    Bytes newPrice = JAVA_BOOK_DELTA_80;
    put64(newPrice, DELTA_GRP_HDR + 4, 0);
    Bytes delPrice = JAVA_BOOK_DELTA_80;
    put64(delPrice, DELTA_GRP_HDR + 4 + 20 * DELTA_ENTRY, 0);
    CHECK(snapshotOf(bidQty) == Malformed::VALUE);
    CHECK(snapshotOf(askCount) == Malformed::VALUE);
    CHECK(snapshotOf(bidPrice) == Malformed::VALUE);
    CHECK(deltaOf(newQty) == Malformed::VALUE);
    CHECK(deltaOf(newCount) == Malformed::VALUE);
    CHECK(deltaOf(newPrice) == Malformed::VALUE);
    CHECK(deltaOf(delPrice) == Malformed::VALUE);
    // a DELETE carries qty 0 and count 0 by design (the golden) and is well formed
    const Bytes good = JAVA_BOOK_DELTA_80;
    CHECK(deltaOf(good) == Malformed::NONE);
}

TEST_CASE("ForwarderCodecTest.aTradeEntryWithoutIdOrPriceDropsTheWholeBatch")
{
    Bytes id0 = JAVA_TRADE_EXECUTION_BATCH_3;
    put64(id0, TRADE_GRP_HDR + 4 + 2 * TRADE_ENTRY, 0); // entry 2's tradeId
    Bytes price0 = JAVA_TRADE_EXECUTION_BATCH_3;
    put64(price0, TRADE_GRP_HDR + 4 + 40, 0); // entry 0's price
    TradeFrame f;
    CHECK(tradeOf(id0, &f) == Malformed::VALUE);
    CHECK(f.maxTradeId == 0); // dropped whole: even the valid entries (41, 43) are not used
    CHECK(tradeOf(price0, &f) == Malformed::VALUE);
    CHECK(f.maxTradeId == 0);
    CHECK(f.priceAtMax == 0);
    const Bytes good = JAVA_TRADE_EXECUTION_BATCH_3;
    CHECK(tradeOf(good, &f) == Malformed::NONE);
    CHECK(f.maxTradeId == 43);
}

TEST_CASE("ForwarderCodecTest.anEmptyTradeBatchIsWellFormedAndMarksNothing")
{
    Bytes empty = JAVA_TRADE_EXECUTION_BATCH_3;
    empty.resize(8 + 12 + 4);
    put16(empty, TRADE_GRP_HDR + 2, 0);
    TradeFrame f;
    CHECK(tradeOf(empty, &f) == Malformed::NONE);
    CHECK(f.maxTradeId == 0);
    CHECK(f.statedCount == 0);
}

TEST_CASE("ForwarderCodecTest.aLargerRootBlockOffsetsTheGroupsByItsStatedLength")
{
    // a later schema appending 4 bytes to the 25 root: the group header moves with header.blockLength
    const Bytes g = JAVA_BOOK_DELTA_RAW_ENUMS;
    Bytes wide(g.begin(), g.begin() + 8 + 44);
    wide.insert(wide.end(), { 1, 2, 3, 4 });
    wide.insert(wide.end(), g.begin() + 8 + 44, g.end());
    put16(wide, 0, 48);
    // restore valid raw enums of entry 1 (the golden carries 2 and 3 there)
    wide[8 + 48 + 4 + DELTA_ENTRY + 20] = 1;
    wide[8 + 48 + 4 + DELTA_ENTRY + 21] = 1;
    BookFrame f;
    REQUIRE(deltaOf(wide, &f) == Malformed::NONE);
    CHECK(f.headerBlockLength == 48);
    CHECK(f.bookVersion == 2000);
    REQUIRE(f.nChanges == 2);
    CHECK(f.changes[1].price == 1'500'000'000LL);
}
