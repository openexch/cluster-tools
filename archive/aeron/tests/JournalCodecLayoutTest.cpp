// JournalCodecLayoutTest (RC2 S9a contract 9.2, WITH_BRIDGE): the 4.4 constants equal the
// generated schema 4 codecs, and the six fixtures decode with them field for field.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "cluster_tools/archive/FramePeek.h"

#include "com_match_infrastructure_mejournal_generated/CommandOutcome.h"
#include "com_match_infrastructure_mejournal_generated/CommandRelease.h"
#include "com_match_infrastructure_mejournal_generated/EngineConfig.h"
#include "com_match_infrastructure_mejournal_generated/MessageHeader.h"
#include "com_match_infrastructure_mejournal_generated/Terminal.h"
#include "com_match_infrastructure_mejournal_generated/Trade.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace cluster_tools::archive;
namespace mj = com::match::infrastructure::mejournal::generated;

namespace {

std::vector<char> fixture(const std::string &name)
{
    std::ifstream in(std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

template<typename R>
void checkHead(std::vector<char> &b, std::uint16_t templateId, std::int64_t seq)
{
    mj::MessageHeader header(b.data(), 0, b.size(), 1);
    CHECK(header.templateId() == templateId);
    CHECK(header.blockLength() == R::sbeBlockLength());
    R r;
    r.wrapForDecode(b.data(), MESSAGE_HEADER_LENGTH, header.blockLength(), header.version(), b.size());
    const Admission a = admitFrame(FRAME_FLAGS_UNFRAGMENTED, reinterpret_cast<const std::uint8_t *>(b.data()), b.size());
    REQUIRE(a.accepted());
    CHECK(r.journalSeq() == seq);
    CHECK(a.peek.journalSeq == r.journalSeq());
    CHECK(a.peek.meEpoch == r.meEpoch());
    CHECK(a.peek.egressSeq == r.egressSeq());
    CHECK(r.meEpoch() == 0x123456789ABCDEF1LL);
}

}

TEST_CASE("JournalCodecLayoutTest.constantsEqualTheGeneratedCodecs")
{
    CHECK(mj::Trade::sbeBlockLength() == BLOCK_TRADE);
    CHECK(mj::Terminal::sbeBlockLength() == BLOCK_TERMINAL);
    CHECK(mj::CommandOutcome::sbeBlockLength() == BLOCK_OUTCOME);
    CHECK(mj::CommandRelease::sbeBlockLength() == BLOCK_RELEASE);
    CHECK(mj::EngineConfig::sbeBlockLength() == BLOCK_CONFIG);
    CHECK(mj::CommandRelease::Releases::sbeBlockLength() == RELEASE_ENTRY_BLOCK);
    CHECK(MESSAGE_HEADER_LENGTH + mj::Trade::journalSeqEncodingOffset() == FRAME_OFFSET_JOURNAL_SEQ);
    CHECK(MESSAGE_HEADER_LENGTH + mj::Trade::meEpochEncodingOffset() == FRAME_OFFSET_ME_EPOCH);
    CHECK(MESSAGE_HEADER_LENGTH + mj::Trade::egressSeqEncodingOffset() == FRAME_OFFSET_EGRESS_SEQ);
    CHECK(mj::Trade::sbeSchemaId() == JOURNAL_SCHEMA_ID);
    CHECK(mj::Trade::sbeSchemaVersion() == JOURNAL_SCHEMA_VERSION);
}

TEST_CASE("JournalCodecLayoutTest.theSixFixturesDecodeWithThem")
{
    auto config = fixture("java-config.bin");
    checkHead<mj::EngineConfig>(config, TEMPLATE_CONFIG, 1);
    auto trade = fixture("java-trade.bin");
    checkHead<mj::Trade>(trade, TEMPLATE_TRADE, 2);
    auto terminal = fixture("java-terminal.bin");
    checkHead<mj::Terminal>(terminal, TEMPLATE_TERMINAL, 3);
    auto first = fixture("java-command.first.bin");
    checkHead<mj::CommandOutcome>(first, TEMPLATE_OUTCOME, 4);
    auto conflict = fixture("java-command.conflict.bin");
    checkHead<mj::CommandOutcome>(conflict, TEMPLATE_OUTCOME, 5);
    auto release = fixture("java-release.bin");
    checkHead<mj::CommandRelease>(release, TEMPLATE_RELEASE, 6);
    mj::CommandRelease r;
    r.wrapForDecode(release.data(), MESSAGE_HEADER_LENGTH, BLOCK_RELEASE, 1, release.size());
    auto &group = r.releases();
    CHECK(group.count() == (release.size() - RELEASE_MIN_FRAME) / RELEASE_ENTRY_BLOCK);
}
