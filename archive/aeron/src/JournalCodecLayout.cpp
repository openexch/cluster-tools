// The pure 4.4 constants (archive/FramePeek.h) against the codecs generated from the pinned ME
// journal schema 4 (sha256 in dependencies.lock): block lengths, the three head offsets, the group
// entry block and header. A schema edit that moves any of them fails the WITH_BRIDGE build here.
#include "cluster_tools/archive/FramePeek.h"

#include "com_match_infrastructure_mejournal_generated/CommandOutcome.h"
#include "com_match_infrastructure_mejournal_generated/CommandRelease.h"
#include "com_match_infrastructure_mejournal_generated/EngineConfig.h"
#include "com_match_infrastructure_mejournal_generated/MessageHeader.h"
#include "com_match_infrastructure_mejournal_generated/Terminal.h"
#include "com_match_infrastructure_mejournal_generated/Trade.h"

namespace cluster_tools { namespace archive { namespace {

namespace mj = com::match::infrastructure::mejournal::generated;

static_assert(mj::MessageHeader::encodedLength() == MESSAGE_HEADER_LENGTH);
static_assert(mj::Trade::sbeSchemaId() == JOURNAL_SCHEMA_ID && mj::Trade::sbeSchemaVersion() == JOURNAL_SCHEMA_VERSION);
static_assert(mj::Trade::sbeTemplateId() == TEMPLATE_TRADE && mj::Terminal::sbeTemplateId() == TEMPLATE_TERMINAL &&
    mj::CommandOutcome::sbeTemplateId() == TEMPLATE_OUTCOME && mj::CommandRelease::sbeTemplateId() == TEMPLATE_RELEASE &&
    mj::EngineConfig::sbeTemplateId() == TEMPLATE_CONFIG);
static_assert(mj::Trade::sbeBlockLength() == BLOCK_TRADE && mj::Terminal::sbeBlockLength() == BLOCK_TERMINAL &&
    mj::CommandOutcome::sbeBlockLength() == BLOCK_OUTCOME && mj::CommandRelease::sbeBlockLength() == BLOCK_RELEASE &&
    mj::EngineConfig::sbeBlockLength() == BLOCK_CONFIG);
static_assert(mj::CommandRelease::Releases::sbeBlockLength() == RELEASE_ENTRY_BLOCK);
static_assert(mj::CommandRelease::Releases::sbeHeaderSize() == GROUP_HEADER_LENGTH);

template<typename R>
constexpr bool headAt()
{
    return MESSAGE_HEADER_LENGTH + R::journalSeqEncodingOffset() == FRAME_OFFSET_JOURNAL_SEQ &&
        MESSAGE_HEADER_LENGTH + R::meEpochEncodingOffset() == FRAME_OFFSET_ME_EPOCH &&
        MESSAGE_HEADER_LENGTH + R::egressSeqEncodingOffset() == FRAME_OFFSET_EGRESS_SEQ;
}
static_assert(headAt<mj::Trade>() && headAt<mj::Terminal>() && headAt<mj::CommandOutcome>() &&
    headAt<mj::CommandRelease>() && headAt<mj::EngineConfig>());

}}}
