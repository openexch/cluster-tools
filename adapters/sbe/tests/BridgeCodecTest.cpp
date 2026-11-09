// BridgeCodec: the translation of 6 through the live Stager + the generated codecs,
// graded ONLY against bytes the Java harness wrote (feed_frames.inc, fixtures/e1) or the engines' own Java
// encoders (golden_frames.inc): C++-produced bytes would confirm themselves.
#include "doctest/doctest.h"

#include "BridgeFeedFiles.h"

#include "cluster_tools/adapters/sbe/BridgeCodec.h"
#include "cluster_tools/archive/FramePeek.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>

#include "com_match_infrastructure_mejournal_generated/Trade.h"

#include "golden/feed_frames.inc"
#include "golden/golden_frames.inc"

using namespace ct_bridge_codec_test;
using cluster_tools::sbe_adapter::bridge_codec::BridgeCodec;
using cluster_tools::sbe_adapter::bridge_codec::EgressEvent;
using cluster_tools::sbe_adapter::bridge_codec::EgressKind;
namespace bridge = cluster_tools::domain::bridge;
namespace archive = cluster_tools::archive;
using cluster_tools::domain::bridge::StagedTrade;

namespace {

void put64(Bytes &b, std::size_t at, std::int64_t v)
{
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
    {
        b[at + i] = static_cast<std::uint8_t>(u >> (8 * i));
    }
}

void put16(Bytes &b, std::size_t at, std::uint16_t v)
{
    b[at] = static_cast<std::uint8_t>(v & 0xff);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// a schema 4 record of template t with random body bytes (head kept >= 1 so the gate admits it)
Bytes randomRecord(std::mt19937_64 &rng, std::uint16_t t, std::uint16_t entries = 0)
{
    const std::uint16_t block = archive::blockLengthFor(t);
    const std::size_t length = archive::TEMPLATE_RELEASE == t ? archive::RELEASE_MIN_FRAME + 22u * entries : 8u + block;
    Bytes b(length);
    for (auto &x : b)
    {
        x = static_cast<std::uint8_t>(rng());
    }
    put16(b, 0, block);
    put16(b, 2, t);
    put16(b, 4, archive::JOURNAL_SCHEMA_ID);
    put16(b, 6, archive::JOURNAL_SCHEMA_VERSION);
    put64(b, 8, 1 + static_cast<std::int64_t>(rng() >> 2));
    put64(b, 16, 1 + static_cast<std::int64_t>(rng() >> 2));
    if (archive::TEMPLATE_RELEASE == t)
    {
        put16(b, 8 + 44, 22);
        put16(b, 8 + 44 + 2, entries);
    }
    return b;
}

bool same(const Bytes &a, std::size_t ab, const Bytes &b, std::size_t bb, std::size_t n)
{
    return a.size() >= ab + n && b.size() >= bb + n && std::equal(a.begin() + ab, a.begin() + ab + n, b.begin() + bb);
}

// The memcpy identities (frame offsets) and the permuted head; a test property, not the implementation
std::string identity(const Bytes &in, const Bytes &out)
{
    const std::uint16_t t = static_cast<std::uint16_t>(in[2] | (in[3] << 8));
    auto head = [&](std::size_t egressAt, std::size_t seqAt, std::size_t epochAt) -> bool {
        return same(out, egressAt, in, 24, 8) && same(out, seqAt, in, 8, 8) && same(out, epochAt, in, 16, 8);
    };
    switch (t)
    {
        case archive::TEMPLATE_TRADE:
            if (!same(out, 16, in, 32, 85)) return "61 body";
            if (!head(8, 8 + 101, 8 + 93)) return "61 head";
            return "";
        case archive::TEMPLATE_TERMINAL:
            if (!same(out, 16, in, 32, 37) || out[69] != in[69]) return "63 body";
            if (!head(8, 8 + 53, 8 + 45)) return "63 head";
            return "";
        case archive::TEMPLATE_OUTCOME:
            if (!same(out, 16, in, 32, 108) || !same(out, 140, in, 140, 21)) return "64 body";
            if (!head(8, 8 + 124, 8 + 116)) return "64 head";
            return "";
        case archive::TEMPLATE_RELEASE:
        case archive::TEMPLATE_CONFIG:
            if (out.size() != in.size() || !same(out, 32, in, 32, in.size() - 32)) return "67/68 body";
            if (!head(8, 16, 24)) return "67/68 head";
            return "";
        default:
            return "template";
    }
}

std::vector<Bytes> translateRecords(const std::vector<Bytes> &records)
{
    std::vector<FjrEntry> in;
    for (const Bytes &r : records)
    {
        in.push_back({ false, r });
        in.push_back({ true, {} }); // one frame per record: no staging
    }
    return translate(in);
}

}

TEST_CASE("TranslatorGoldenTest.everyNamedCaseOfTheHarnessIsReproducedByteForByte")
{
    int cases = 0;
    for (const FeedFrameCase &c : FEED_FRAME_CASES)
    {
        CAPTURE(c.name);
        const std::vector<FjrEntry> input = decodeFjr(*c.in);
        const Bytes got = encodeFfr(translate(input));
        const std::string diff = firstDifference(decodeFfr(got), decodeFfr(*c.out));
        CHECK_MESSAGE(diff.empty(), c.name << ": " << diff);
        const bool equal = got == *c.out;
        CHECK(equal);
        ++cases;
    }
    CHECK(cases == 49);
}

TEST_CASE("TranslatorIdentityTest.memcpyIdentitiesHoldOnTheFixturesAndOnRandomRecords")
{
    const char *names[] = { "java-trade.bin", "java-terminal.bin", "java-command.first.bin", "java-command.conflict.bin",
        "java-release.bin", "java-config.bin" };
    std::vector<Bytes> fixtures;
    for (const char *n : names)
    {
        fixtures.push_back(readFile(std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/" + n));
    }
    const std::vector<Bytes> out = translateRecords(fixtures);
    REQUIRE(out.size() == fixtures.size());
    for (std::size_t i = 0; i < fixtures.size(); ++i)
    {
        const std::string broken = identity(fixtures[i], out[i]);
        CAPTURE(names[i]);
        CHECK(broken == "");
    }
    std::mt19937_64 rng(0x5eedc0de);
    std::vector<Bytes> records;
    const std::uint16_t templates[] = { 1, 2, 3, 4, 5 };
    for (int i = 0; i < 10'000; ++i)
    {
        const std::uint16_t t = templates[rng() % 5];
        records.push_back(randomRecord(rng, t, static_cast<std::uint16_t>(rng() % 49)));
    }
    const std::vector<Bytes> got = translateRecords(records);
    REQUIRE(got.size() == records.size());
    int failures = 0;
    std::string first;
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        const std::string broken = identity(records[i], got[i]);
        if (!broken.empty() && failures++ == 0)
        {
            first = broken + " at record " + std::to_string(i) + ": " + hex(records[i]);
        }
    }
    INFO(first);
    CHECK(failures == 0);
}

TEST_CASE("TranslatorE1Test.theCorpusTranslatesExactlyAsTheJavaReferenceDid")
{
    namespace fs = std::filesystem;
    int members = 0, frames = 0;
    std::vector<fs::path> dirs;
    for (const auto &e : fs::directory_iterator(CT_E1_FIXTURE_DIR))
    {
        if (e.is_directory())
        {
            dirs.push_back(e.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path &dir : dirs)
    {
        for (int m = 0; m < 3; ++m)
        {
            const std::string base = (dir / ("member" + std::to_string(m))).string();
            if (!fs::exists(base + ".fjr"))
            {
                continue;
            }
            CAPTURE(base);
            ++members;
            const std::vector<Bytes> got = translate(decodeFjr(readFile(base + ".fjr")));
            const std::vector<Bytes> want = decodeFfr(readFile(base + ".ffr"));
            const std::string diff = firstDifference(got, want);
            CHECK_MESSAGE(diff.empty(), diff);
            const bool equal = got == want;
            CHECK(equal);
            frames += static_cast<int>(want.size());
        }
    }
    MESSAGE("E1: " << members << " member streams, " << frames << " frames");
    CHECK(members == 45);
}

TEST_CASE("RawEnumsAreWrittenVerbatim")
{
    // 0..255 on each of the five enum fields and on 63 rejectReason (a plain uint8): out byte == in byte
    struct Field
    {
        const char *name;
        std::uint16_t t;
        std::size_t in;  // frame offset in the record
        std::size_t out; // frame offset in the money frame
    };
    const Field fields[] = {
        { "61 takerIsBuy", 1, 8 + 84, 8 + 68 },
        { "63 status", 2, 8 + 44, 8 + 28 },
        { "63 rejectReason", 2, 8 + 61, 8 + 61 },
        { "67 scope", 4, 8 + 44 + 4 + 16, 8 + 44 + 4 + 16 },
        { "67 result", 4, 8 + 44 + 4 + 17, 8 + 44 + 4 + 17 },
        { "68 decision", 5, 8 + 38, 8 + 38 },
    };
    std::mt19937_64 rng(7);
    BridgeCodec codec;
    for (const Field &f : fields)
    {
        CAPTURE(f.name);
        int wrong = 0;
        for (int v = 0; v <= 255; ++v)
        {
            Bytes r = randomRecord(rng, f.t, 1);
            r[f.in] = static_cast<std::uint8_t>(v);
            std::uint8_t out[512];
            const std::size_t n = codec.encodeRecord(r.data(), r.size(), out, sizeof(out));
            REQUIRE(n > f.out);
            wrong += out[f.out] == static_cast<std::uint8_t>(v) ? 0 : 1;
        }
        CHECK(wrong == 0);
    }
    // the 62 entry of a batch too
    StagedTrade trades[2];
    for (int v = 0; v <= 255; ++v)
    {
        for (auto &t : trades)
        {
            const Bytes r = randomRecord(rng, 1);
            std::memcpy(t.record, r.data(), r.size());
            t.record[8 + 84] = static_cast<std::uint8_t>(v);
        }
        std::uint8_t out[512];
        const std::size_t n = codec.encodeTradeBatch(trades, 2, out, sizeof(out));
        REQUIRE(n == 12 + 2 * 109);
        CHECK(out[12 + 68] == static_cast<std::uint8_t>(v));
        CHECK(out[12 + 109 + 68] == static_cast<std::uint8_t>(v));
    }
}

TEST_CASE("BridgeEgressCodecTest.decodesTheCoreEgressAndEncodesTheControlFramesLikeTheJavaEncoders")
{
    EgressEvent e;
    REQUIRE(BridgeCodec::decodeEgress(JAVA_FEED_POSITION_REPORT_V7.data(), JAVA_FEED_POSITION_REPORT_V7.size(), e) ==
        EgressKind::REPORT);
    CHECK(e.report.correlationId == 0x0102030405060708LL);
    CHECK(e.report.consumePosition == 0x1112131415161718LL);
    CHECK(e.report.lastAppliedTradeId == 0x2122232425262728LL);
    CHECK(e.report.journalEnabled);
    CHECK(e.report.meEpoch == 0x3132333435363738LL);
    CHECK(e.report.lastAppliedJournalSeq == 0x4142434445464748LL);
    CHECK(e.report.feedHaltedRaw == 1);
    CHECK(e.report.haltReasonRaw == 3);

    REQUIRE(BridgeCodec::decodeEgress(JAVA_SETTLE_REJECTED.data(), JAVA_SETTLE_REJECTED.size(), e) ==
        EgressKind::SETTLE_REJECTED);
    CHECK(e.reject.tradeId == 0x0102030405060708LL);
    CHECK(e.reject.marketId == 0x11121314);
    CHECK(e.reject.journalPosition == 0x2122232425262728LL);
    CHECK(e.reject.reasonRaw == 6);
    CHECK(e.reject.detail == 0x3132333435363738LL);

    REQUIRE(BridgeCodec::decodeEgress(JAVA_ADMIN_ACK_66.data(), JAVA_ADMIN_ACK_66.size(), e) == EgressKind::ADMIN_ACK);
    CHECK(e.ack.correlationId == 0x0A0B0C0D0E0F1011LL);
    CHECK(e.ack.refTemplateId == 66);
    CHECK(e.ack.resultRaw == bridge::ADMIN_REJECTED);
    CHECK(e.ack.reason == 4);
    REQUIRE(BridgeCodec::decodeEgress(JAVA_ADMIN_ACK.data(), JAVA_ADMIN_ACK.size(), e) == EgressKind::ADMIN_ACK);
    CHECK(e.ack.refTemplateId == 80);

    BridgeCodec codec;
    std::uint8_t out[128];
    std::size_t n = codec.encodeFeedHalt(0x0102030405060708LL, bridge::HaltReason::GAP, 0x1112131415161718LL,
        0x2122232425262728LL, 0x3132333435363738LL, 0x4142434445464748LL, out, sizeof(out));
    CHECK(Bytes(out, out + n) == JAVA_FEED_HALT);
    n = codec.encodeQuery(0x5152535455565758LL, out, sizeof(out));
    CHECK(Bytes(out, out + n) == JAVA_QUERY_FEED_POSITION);

    // a v6 report (the version byte and the v6 block length of 25) and a short block are MALFORMED
    Bytes v6 = JAVA_FEED_POSITION_REPORT_V7;
    v6[0] = 25;
    v6[6] = 6;
    v6.resize(8 + 25);
    const EgressKind v6Kind = BridgeCodec::decodeEgress(v6.data(), v6.size(), e);
    CHECK(v6Kind == EgressKind::MALFORMED);
    Bytes shortBlock = JAVA_FEED_POSITION_REPORT_V7;
    shortBlock.resize(8 + 40);
    CHECK(BridgeCodec::decodeEgress(shortBlock.data(), shortBlock.size(), e) == EgressKind::MALFORMED);
    Bytes wrongVersion = JAVA_SETTLE_REJECTED;
    wrongVersion[6] = 8;
    CHECK(BridgeCodec::decodeEgress(wrongVersion.data(), wrongVersion.size(), e) == EgressKind::MALFORMED);
    // other templates are not the domain's
    CHECK(BridgeCodec::decodeEgress(JAVA_ME_DISPATCH_BATCH.data(), JAVA_ME_DISPATCH_BATCH.size(), e) == EgressKind::IGNORED);
    CHECK(BridgeCodec::decodeEgress(JAVA_REGISTER_SESSION.data(), 4, e) == EgressKind::MALFORMED);
}

TEST_CASE("BridgeCodecTest.aRecordOffItsTemplateShapeIsNotEncoded")
{
    // behind the gate this cannot happen; the domain turns a 0 into Fatal::ENCODER_OVERFLOW
    BridgeCodec codec;
    std::mt19937_64 rng(3);
    Bytes r = randomRecord(rng, 1);
    std::uint8_t out[8192];
    CHECK(codec.encodeRecord(r.data(), r.size() - 1, out, sizeof(out)) == 0);
    CHECK(codec.encodeRecord(r.data(), r.size(), out, 100) == 0);
    Bytes rel = randomRecord(rng, 4, 49);
    CHECK(codec.encodeRecord(rel.data(), rel.size(), out, sizeof(out)) == 0); // 49 > 48 entries
    put16(r, 4, 3);
    CHECK(codec.encodeRecord(r.data(), r.size(), out, sizeof(out)) == 0);
    StagedTrade one[1];
    CHECK(codec.encodeTradeBatch(one, 1, out, sizeof(out)) == 0);
}

// The two egress frames the app reads (OrderReject 101, SettlementApplied 13) against the Java encoders'
// goldens, and the O3 / O4 trade getter on the 61 and 62 frames this codec produces (metrics only).
TEST_CASE("BridgeAppCodecTest.decodeOrderReject101AndSettlementApplied13")
{
    using cluster_tools::sbe_adapter::bridge_codec::OrderRejectView;
    OrderRejectView r;
    REQUIRE(BridgeCodec::decodeOrderReject(JAVA_ORDER_REJECT_61_UNREGISTERED.data(), JAVA_ORDER_REJECT_61_UNREGISTERED.size(), r));
    CHECK(r.correlationId == 0x0102030405060708LL);
    CHECK(r.refTemplateId == 61);
    CHECK(r.reasonRaw == 35);
    REQUIRE(BridgeCodec::decodeOrderReject(JAVA_ORDER_REJECT_7_INVALID.data(), JAVA_ORDER_REJECT_7_INVALID.size(), r));
    CHECK(r.refTemplateId == 7);
    CHECK(r.reasonRaw == 15);
    std::int64_t tradeId = 0;
    REQUIRE(BridgeCodec::decodeSettlementApplied(JAVA_SETTLEMENT_APPLIED.data(), JAVA_SETTLEMENT_APPLIED.size(), tradeId));
    CHECK(tradeId == 0x4142434445464748LL);
    // another template, a short frame, another version: refused
    CHECK_FALSE(BridgeCodec::decodeSettlementApplied(JAVA_ORDER_REJECT_7_INVALID.data(), JAVA_ORDER_REJECT_7_INVALID.size(), tradeId));
    CHECK_FALSE(BridgeCodec::decodeOrderReject(JAVA_ORDER_REJECT_7_INVALID.data(), 20, r));
    Bytes v6(JAVA_ORDER_REJECT_61_UNREGISTERED.begin(), JAVA_ORDER_REJECT_61_UNREGISTERED.end());
    v6[6] = 6;
    const bool v6Refused = !BridgeCodec::decodeOrderReject(v6.data(), v6.size(), r);
    CHECK(v6Refused); // EGRESS_VERSION_FLOOR RED: a v6 101 accepted
}

namespace {
struct Visited
{
    std::vector<std::pair<std::int64_t, std::int64_t>> trades;
};
void onTrade(void *context, std::int64_t tradeId, std::int64_t ts)
{
    static_cast<Visited *>(context)->trades.emplace_back(tradeId, ts);
}
}

TEST_CASE("BridgeAppCodecTest.tradeGetterReadsTheGeneratedOffsets")
{
    const Bytes trade = readFile(std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/java-trade.bin");
    namespace jr = com::match::infrastructure::mejournal::generated;
    jr::Trade d;
    d.wrapForDecode(reinterpret_cast<char *>(const_cast<std::uint8_t *>(trade.data())), 8, jr::Trade::sbeBlockLength(), 1,
        trade.size());
    BridgeCodec codec;
    std::uint8_t out[8192];
    const std::size_t n61 = codec.encodeRecord(trade.data(), trade.size(), out, sizeof(out));
    REQUIRE(n61 > 0);
    Visited v;
    CHECK(1 == BridgeCodec::visitTrades(out, n61, &onTrade, &v));
    REQUIRE(v.trades.size() == 1);
    CHECK(v.trades[0].first == d.tradeId());
    CHECK(v.trades[0].second == d.timestamp());
    // a cap-64 62
    std::vector<StagedTrade> staged(64);
    for (std::size_t i = 0; i < staged.size(); ++i)
    {
        std::memcpy(staged[i].record, trade.data(), bridge::TRADE_RECORD_LENGTH);
        staged[i].seq = static_cast<std::int64_t>(i);
    }
    const std::size_t n62 = codec.encodeTradeBatch(staged.data(), staged.size(), out, sizeof(out));
    REQUIRE(n62 > 0);
    Visited b;
    CHECK(64 == BridgeCodec::visitTrades(out, n62, &onTrade, &b));
    CHECK(b.trades.back().second == d.timestamp());
    // any other frame: nothing visited
    const std::size_t nq = codec.encodeQuery(1, out, sizeof(out));
    CHECK(0 == BridgeCodec::visitTrades(out, nq, &onTrade, &b));
}
