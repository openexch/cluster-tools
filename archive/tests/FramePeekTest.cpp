// FramePeek: the 4.4 admission gate and head peek (RC2 S9a contract 9.1, 9.2), against built frames
// and the six golden fixtures of match's MeJournalFixtureDump.
#include "doctest/doctest.h"

#include "TestSupport.h"

#include "cluster_tools/archive/FramePeek.h"

#include <string>
#include <vector>

using namespace cluster_tools::archive;
using namespace ct_archive_test;

namespace {

Admission admit(const Bytes &b, std::uint8_t flags = FRAME_FLAGS_UNFRAGMENTED)
{
    return admitFrame(flags, b.data(), b.size());
}

struct FixtureExpectation
{
    const char *file;
    std::uint16_t templateId;
    std::int64_t seq;
    std::int64_t egressSeq;
    std::size_t length;
};

// MeJournalFixtureDump.java:34-40,53,59-92: ONE bound producer sequence.
const std::vector<FixtureExpectation> FIXTURES = {
    { "java-config.bin", 5, 1, 64, 47 },
    { "java-trade.bin", 1, 2, 128, 117 },
    { "java-terminal.bin", 2, 3, 128, 70 },
    { "java-command.first.bin", 3, 4, 192, 161 },
    { "java-command.conflict.bin", 3, 5, 256, 161 },
    { "java-release.bin", 4, 6, 640, 100 },
};
constexpr std::int64_t FIXTURE_EPOCH = 0x123456789ABCDEF1LL;

}

TEST_CASE("FramePeekTest.refusesEveryRow")
{
    struct Row
    {
        std::string name;
        Bytes bytes;
        std::uint8_t flags;
        Refusal expected;
    };
    std::vector<Row> rows;
    // Row 1: flags.
    rows.push_back({ "flags 0x80 (BEGIN only)", makeFrame(TEMPLATE_TRADE, 1, 1), 0x80, Refusal::FRAGMENTED });
    rows.push_back({ "flags 0x40 (END only)", makeFrame(TEMPLATE_TRADE, 1, 1), 0x40, Refusal::FRAGMENTED });
    // Row 2: length.
    rows.push_back({ "1113 B", Bytes(1113, 0), 0xC0, Refusal::LENGTH });
    rows.push_back({ "7 B", Bytes(7, 0), 0xC0, Refusal::LENGTH });
    rows.push_back({ "a real 49-entry record 4 (1134 B)", makeRelease(1, 1, 49), 0xC0, Refusal::LENGTH });
    // Row 3: schema.
    {
        Bytes b = makeFrame(TEMPLATE_TRADE, 1, 1);
        putLe16(b, 4, 3); // schema 3 v1: the frozen settlement journal
        rows.push_back({ "schema 3 v1", b, 0xC0, Refusal::SCHEMA });
        Bytes c = makeFrame(TEMPLATE_TRADE, 1, 1);
        putLe16(c, 6, 2);
        rows.push_back({ "schema 4 v2", c, 0xC0, Refusal::SCHEMA });
        Bytes d = makeFrame(TEMPLATE_TRADE, 1, 1);
        putLe16(d, 4, 1); // schema 1 (template 28 era)
        rows.push_back({ "schema 1", d, 0xC0, Refusal::SCHEMA });
    }
    // Row 4: template.
    {
        Bytes b = makeFrame(TEMPLATE_CONFIG, 1, 1);
        putLe16(b, 2, 6);
        rows.push_back({ "template 6", b, 0xC0, Refusal::TEMPLATE });
        Bytes c = makeFrame(TEMPLATE_CONFIG, 1, 1);
        putLe16(c, 2, 0);
        rows.push_back({ "template 0", c, 0xC0, Refusal::TEMPLATE });
    }
    // Row 5: block.
    {
        Bytes b = makeFrame(TEMPLATE_TRADE, 1, 1);
        putLe16(b, 0, 108);
        rows.push_back({ "trade block 108", b, 0xC0, Refusal::BLOCK });
        Bytes c = makeFrame(TEMPLATE_TRADE, 1, 1);
        putLe16(c, 0, 110);
        rows.push_back({ "trade block 110", c, 0xC0, Refusal::BLOCK });
    }
    // Row 6: frame length.
    {
        Bytes b = makeFrame(TEMPLATE_TRADE, 1, 1);
        b.push_back(0);
        rows.push_back({ "trade 118 B", b, 0xC0, Refusal::FRAME_LENGTH });
        Bytes c = makeRelease(1, 1, 48);
        putLe16(c, 54, 49); // the header claims 49 entries in a 1112 B frame
        rows.push_back({ "a 49-entry record 4 header", c, 0xC0, Refusal::FRAME_LENGTH });
        Bytes d = makeRelease(1, 1, 0);
        d.pop_back();
        rows.push_back({ "a 55 B record 4", d, 0xC0, Refusal::FRAME_LENGTH });
        rows.push_back({ "record 4 group block 21", makeRelease(1, 1, 2, 21), 0xC0, Refusal::FRAME_LENGTH });
        Bytes e = makeRelease(1, 1, 2);
        e.push_back(0);
        rows.push_back({ "record 4 one byte long", e, 0xC0, Refusal::FRAME_LENGTH });
    }
    // Row 7: head.
    rows.push_back({ "journalSeq 0", makeFrame(TEMPLATE_TRADE, 0, 5), 0xC0, Refusal::HEAD });
    rows.push_back({ "meEpoch 0", makeFrame(TEMPLATE_TRADE, 5, 0), 0xC0, Refusal::HEAD });
    rows.push_back({ "meEpoch -1", makeFrame(TEMPLATE_TRADE, 5, -1), 0xC0, Refusal::HEAD });
    rows.push_back({ "behaviour-1 0/0 terminal", makeFrame(TEMPLATE_TERMINAL, 0, 0), 0xC0, Refusal::HEAD });

    for (const auto &row : rows)
    {
        CAPTURE(row.name);
        const Admission a = admit(row.bytes, row.flags);
        CHECK_FALSE(a.accepted());
        CHECK(a.refusal == row.expected);
        // A peek is reported only when rows 1-6 passed.
        CHECK(a.hasPeek == (row.expected == Refusal::HEAD));
    }
}

TEST_CASE("FramePeekTest.acceptsEveryTemplateAtItsLength")
{
    for (std::uint16_t t : { TEMPLATE_TRADE, TEMPLATE_TERMINAL, TEMPLATE_OUTCOME, TEMPLATE_CONFIG })
    {
        CAPTURE(t);
        CHECK(admit(makeFrame(t, 1, 1)).accepted());
    }
    for (std::uint16_t n : { 0, 1, 47, 48 })
    {
        CAPTURE(n);
        const Bytes b = makeRelease(9, 3, n);
        CHECK(b.size() == 56u + 22u * n);
        CHECK(admit(b).accepted());
    }
    CHECK(makeRelease(1, 1, 48).size() == MAX_FRAME_LENGTH);
}

TEST_CASE("FramePeekTest.peeksHeadAtFixedOffsets")
{
    const Bytes b = makeFrame(TEMPLATE_OUTCOME, 77, 0x0102030405060708LL, 999);
    const Admission a = admit(b);
    REQUIRE(a.accepted());
    CHECK(a.hasPeek);
    CHECK(a.peek.templateId == TEMPLATE_OUTCOME);
    CHECK(a.peek.journalSeq == 77);
    CHECK(a.peek.meEpoch == 0x0102030405060708LL);
    CHECK(a.peek.egressSeq == 999);
}

TEST_CASE("FramePeekTest.peeksEveryJavaFixture")
{
    for (const auto &f : FIXTURES)
    {
        CAPTURE(f.file);
        const Bytes b = fixture(f.file);
        CHECK(b.size() == f.length);
        const Admission a = admit(b);
        REQUIRE(a.accepted());
        CHECK(a.peek.templateId == f.templateId);
        CHECK(a.peek.journalSeq == f.seq);
        CHECK(a.peek.meEpoch == FIXTURE_EPOCH);
        CHECK(a.peek.egressSeq == f.egressSeq);
    }
}

TEST_CASE("FramePeekTest.everyMutationOfEveryFixtureRefusedOrPassedAsTheTableSays")
{
    int mutations = 0;
    for (const auto &f : FIXTURES)
    {
        CAPTURE(f.file);
        const Bytes original = fixture(f.file);
        auto expect = [&](const std::string &what, const Bytes &b, std::uint8_t flags, Refusal want) {
            CAPTURE(what);
            ++mutations;
            const Admission a = admit(b, flags);
            CHECK(a.refusal == want);
        };
        // flags
        expect("flags 0x80", original, 0x80, Refusal::FRAGMENTED);
        expect("flags 0x40", original, 0x40, Refusal::FRAGMENTED);
        expect("flags 0x00", original, 0x00, Refusal::FRAGMENTED);
        expect("flags 0xC1", original, 0xC1, Refusal::FRAGMENTED);
        // length +- 1
        Bytes longer = original;
        longer.push_back(0);
        expect("length + 1", longer, 0xC0, Refusal::FRAME_LENGTH);
        Bytes shorter = original;
        shorter.pop_back();
        expect("length - 1", shorter, 0xC0, Refusal::FRAME_LENGTH);
        // header fields
        Bytes b = original;
        putLe16(b, 0, static_cast<std::uint16_t>(readLe16(original.data()) + 1));
        expect("blockLength + 1", b, 0xC0, Refusal::BLOCK);
        b = original;
        putLe16(b, 0, static_cast<std::uint16_t>(readLe16(original.data()) - 1));
        expect("blockLength - 1", b, 0xC0, Refusal::BLOCK);
        b = original;
        putLe16(b, 2, f.templateId == 1 ? 2 : 1);
        expect("another known template", b, 0xC0, Refusal::BLOCK);
        b = original;
        putLe16(b, 2, 6);
        expect("template 6", b, 0xC0, Refusal::TEMPLATE);
        b = original;
        putLe16(b, 4, 3);
        expect("schemaId 3", b, 0xC0, Refusal::SCHEMA);
        b = original;
        putLe16(b, 6, 0);
        expect("version 0", b, 0xC0, Refusal::SCHEMA);
        b = original;
        putLe16(b, 6, 2);
        expect("version 2", b, 0xC0, Refusal::SCHEMA);
        // group dimensions (record 4 only)
        if (f.templateId == TEMPLATE_RELEASE)
        {
            b = original;
            putLe16(b, 52, 21);
            expect("group block 21", b, 0xC0, Refusal::FRAME_LENGTH);
            b = original;
            putLe16(b, 52, 23);
            expect("group block 23", b, 0xC0, Refusal::FRAME_LENGTH);
            b = original;
            putLe16(b, 54, static_cast<std::uint16_t>(readLe16(original.data() + 54) + 1));
            expect("numInGroup + 1", b, 0xC0, Refusal::FRAME_LENGTH);
            b = original;
            putLe16(b, 54, static_cast<std::uint16_t>(readLe16(original.data() + 54) - 1));
            expect("numInGroup - 1", b, 0xC0, Refusal::FRAME_LENGTH);
        }
        // head
        b = original;
        putLe64(b, FRAME_OFFSET_JOURNAL_SEQ, 0);
        expect("journalSeq 0", b, 0xC0, Refusal::HEAD);
        b = original;
        putLe64(b, FRAME_OFFSET_ME_EPOCH, 0);
        expect("meEpoch 0", b, 0xC0, Refusal::HEAD);
        b = original;
        putLe64(b, FRAME_OFFSET_JOURNAL_SEQ, -5);
        expect("journalSeq negative", b, 0xC0, Refusal::HEAD);
        // passes: no business field is validated by the bridge (the core is the one judge)
        b = original;
        putLe64(b, FRAME_OFFSET_EGRESS_SEQ, 0);
        expect("egressSeq 0 passes", b, 0xC0, Refusal::NONE);
        b = original;
        b.back() = static_cast<std::uint8_t>(b.back() ^ 0xff);
        expect("last business byte flipped passes", b, 0xC0, Refusal::NONE);
        b = original;
        putLe64(b, FRAME_OFFSET_JOURNAL_SEQ, 0x7fffffffffffffffLL);
        expect("journalSeq max passes", b, 0xC0, Refusal::NONE);
    }
    CHECK(mutations > 100);
}
