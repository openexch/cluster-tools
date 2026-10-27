#include "cluster_tools/archive/FramePeek.h"

#include "cluster_tools/archive/TestSeams.h"

namespace cluster_tools { namespace archive {

using test_seams::Rule;
using test_seams::disabled;

const char *refusalLabel(Refusal refusal) noexcept
{
    switch (refusal)
    {
        case Refusal::NONE: return "none";
        case Refusal::FRAGMENTED: return "fragmented";
        case Refusal::LENGTH: return "length";
        case Refusal::SCHEMA: return "schema";
        case Refusal::TEMPLATE: return "template";
        case Refusal::BLOCK: return "block";
        case Refusal::FRAME_LENGTH: return "frame_length";
        case Refusal::HEAD: return "head";
        case Refusal::COUNT: break;
    }
    return "unknown";
}

std::int64_t readLe64(const std::uint8_t *at) noexcept
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
    {
        value = (value << 8) | at[i];
    }
    return static_cast<std::int64_t>(value);
}

std::uint16_t readLe16(const std::uint8_t *at) noexcept
{
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
}

namespace {

std::uint16_t expectedBlock(std::uint16_t templateId) noexcept
{
    const std::uint16_t block = blockLengthFor(templateId);
    return disabled(Rule::PEEK_BLOCK_OFF_BY_ONE) ? static_cast<std::uint16_t>(block + 1) : block;
}

bool frameLengthMatches(std::uint16_t templateId, std::uint16_t block, const std::uint8_t *bytes, std::size_t length) noexcept
{
    if (TEMPLATE_RELEASE != templateId)
    {
        return length == MESSAGE_HEADER_LENGTH + block;
    }
    // Record 4: the group header is read only once the frame is long enough to hold it.
    if (length < RELEASE_MIN_FRAME)
    {
        return false;
    }
    const std::size_t groupAt = MESSAGE_HEADER_LENGTH + BLOCK_RELEASE;
    const std::uint16_t entryBlock = readLe16(bytes + groupAt);
    const std::uint16_t entries = readLe16(bytes + groupAt + 2);
    if (entryBlock != RELEASE_ENTRY_BLOCK || entries > RELEASE_MAX_ENTRIES)
    {
        return false;
    }
    return length == RELEASE_MIN_FRAME + static_cast<std::size_t>(RELEASE_ENTRY_BLOCK) * entries;
}

}

Admission admitFrame(std::uint8_t flags, const std::uint8_t *bytes, std::size_t length) noexcept
{
    Admission result;
    // Row 1: one unfragmented frame, never reassembled.
    if (FRAME_FLAGS_UNFRAGMENTED != flags && !disabled(Rule::PEEK_FLAGS))
    {
        result.refusal = Refusal::FRAGMENTED;
        return result;
    }
    // Row 2: the schema 4 frame bound. Without it nothing below may read the header.
    const bool lengthOk = length >= MIN_FRAME_LENGTH && length <= MAX_FRAME_LENGTH;
    if (!lengthOk && (!disabled(Rule::PEEK_LENGTH) || length < MIN_FRAME_LENGTH))
    {
        result.refusal = Refusal::LENGTH;
        return result;
    }
    const std::uint16_t block = readLe16(bytes + 0);
    const std::uint16_t templateId = readLe16(bytes + 2);
    const std::uint16_t schemaId = readLe16(bytes + 4);
    const std::uint16_t version = readLe16(bytes + 6);
    // Row 3: schema 4 v1 only; schema 3 or 1 is loud.
    if ((JOURNAL_SCHEMA_ID != schemaId || JOURNAL_SCHEMA_VERSION != version) && !disabled(Rule::PEEK_SCHEMA))
    {
        result.refusal = Refusal::SCHEMA;
        return result;
    }
    // Row 4: templates 1..5.
    if ((templateId < TEMPLATE_TRADE || templateId > TEMPLATE_CONFIG) && !disabled(Rule::PEEK_TEMPLATE))
    {
        result.refusal = Refusal::TEMPLATE;
        return result;
    }
    // Row 5: the template's root block.
    if (block != expectedBlock(templateId) && !disabled(Rule::PEEK_BLOCK))
    {
        result.refusal = Refusal::BLOCK;
        return result;
    }
    // Row 6: exact frame length (template 4: the group rule).
    if (!frameLengthMatches(templateId, block, bytes, length) && !disabled(Rule::PEEK_FRAME_LENGTH))
    {
        result.refusal = Refusal::FRAME_LENGTH;
        return result;
    }
    // The head needs 32 bytes; every template's frame is at least 47 B, so only a seam can get here short.
    if (length < FRAME_OFFSET_EGRESS_SEQ + 8)
    {
        result.refusal = Refusal::FRAME_LENGTH;
        return result;
    }
    const std::size_t shift = disabled(Rule::PEEK_OFFSETS_SHIFTED) ? 8 : 0;
    result.hasPeek = true;
    result.peek.templateId = templateId;
    result.peek.journalSeq = readLe64(bytes + FRAME_OFFSET_JOURNAL_SEQ + shift);
    result.peek.meEpoch = readLe64(bytes + FRAME_OFFSET_ME_EPOCH + shift);
    result.peek.egressSeq = length >= FRAME_OFFSET_EGRESS_SEQ + 8 + shift ? readLe64(bytes + FRAME_OFFSET_EGRESS_SEQ + shift) : 0;
    // Row 7: a behaviour-1 or pre-S3 ME writes 0/0.
    if ((result.peek.journalSeq < 1 || result.peek.meEpoch < 1) && !disabled(Rule::PEEK_HEAD))
    {
        result.refusal = Refusal::HEAD;
        return result;
    }
    return result;
}

}}
