#ifndef CLUSTER_TOOLS_ARCHIVE_FRAME_PEEK_H
#define CLUSTER_TOOLS_ARCHIVE_FRAME_PEEK_H

// The frame admission gate and common-head peek of the C++ bridge (RC2 S9a contract 4.4, the
// rc2-bridge-cpp 2.3 gate, RC2 S1 contract section 2 rule 7). Pure: no Aeron, no SBE. One WITH_BRIDGE
// translation unit (archive/aeron/src/JournalCodecLayout.cpp) static-asserts every constant here
// against the generated schema 4 codecs, so a schema edit that moves them fails the build.
//
// The gate checks only the transport shape and the schema identity of a frame; no business field
// is validated (the core's demuxer and engine are the one semantic judge). Row 7 (HEAD) exists
// because the forward rule skips seq <= last: a journalSeq of 0 would otherwise be skipped record
// after record by an unbound bridge, a live process doing no work.

#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace archive {

inline constexpr std::uint8_t FRAME_FLAGS_UNFRAGMENTED = 0xC0; // BEGIN | END
inline constexpr std::size_t MIN_FRAME_LENGTH = 8;
inline constexpr std::size_t MAX_FRAME_LENGTH = 1112;        // CommandRelease with 48 entries
inline constexpr std::size_t MESSAGE_HEADER_LENGTH = 8;
inline constexpr std::uint16_t JOURNAL_SCHEMA_ID = 4;
inline constexpr std::uint16_t JOURNAL_SCHEMA_VERSION = 1;

inline constexpr std::uint16_t TEMPLATE_TRADE = 1;
inline constexpr std::uint16_t TEMPLATE_TERMINAL = 2;
inline constexpr std::uint16_t TEMPLATE_OUTCOME = 3;
inline constexpr std::uint16_t TEMPLATE_RELEASE = 4;
inline constexpr std::uint16_t TEMPLATE_CONFIG = 5;

inline constexpr std::uint16_t BLOCK_TRADE = 109;
inline constexpr std::uint16_t BLOCK_TERMINAL = 62;
inline constexpr std::uint16_t BLOCK_OUTCOME = 153;
inline constexpr std::uint16_t BLOCK_RELEASE = 44;
inline constexpr std::uint16_t BLOCK_CONFIG = 39;

inline constexpr std::size_t GROUP_HEADER_LENGTH = 4;
inline constexpr std::uint16_t RELEASE_ENTRY_BLOCK = 22;
inline constexpr std::uint16_t RELEASE_MAX_ENTRIES = 48;
inline constexpr std::size_t RELEASE_MIN_FRAME = MESSAGE_HEADER_LENGTH + BLOCK_RELEASE + GROUP_HEADER_LENGTH; // 56
static_assert(MAX_FRAME_LENGTH == RELEASE_MIN_FRAME + RELEASE_MAX_ENTRIES * RELEASE_ENTRY_BLOCK);

// Frame offsets of the common head every record starts with (block offsets 0, 8, 16).
inline constexpr std::size_t FRAME_OFFSET_JOURNAL_SEQ = 8;
inline constexpr std::size_t FRAME_OFFSET_ME_EPOCH = 16;
inline constexpr std::size_t FRAME_OFFSET_EGRESS_SEQ = 24;

// Root block length per template 1..5 (index 0 unused).
inline constexpr std::uint16_t blockLengthFor(std::uint16_t templateId) noexcept
{
    switch (templateId)
    {
        case TEMPLATE_TRADE: return BLOCK_TRADE;
        case TEMPLATE_TERMINAL: return BLOCK_TERMINAL;
        case TEMPLATE_OUTCOME: return BLOCK_OUTCOME;
        case TEMPLATE_RELEASE: return BLOCK_RELEASE;
        case TEMPLATE_CONFIG: return BLOCK_CONFIG;
        default: return 0;
    }
}

// Refusal reasons in table order (4.4 rows 1..7); the label is the metric label.
enum class Refusal : std::uint8_t
{
    NONE = 0,
    FRAGMENTED,   // 1: header flags != 0xC0
    LENGTH,       // 2: length outside [8, 1112]
    SCHEMA,       // 3: schemaId != 4 or version != 1 (schema 3, schema 1: loud, never skipped)
    TEMPLATE,     // 4: templateId outside 1..5
    BLOCK,        // 5: root blockLength != the template's
    FRAME_LENGTH, // 6: frame length != 8 + block (template 4: the group rule)
    HEAD,         // 7: journalSeq < 1 or meEpoch < 1
    COUNT
};

const char *refusalLabel(Refusal refusal) noexcept;

struct Peek
{
    std::uint16_t templateId = 0;
    std::int64_t journalSeq = 0;
    std::int64_t meEpoch = 0;
    std::int64_t egressSeq = 0;
};

struct Admission
{
    Refusal refusal = Refusal::NONE;
    bool hasPeek = false; // true when rows 1-6 passed (accepted, or refused for HEAD)
    Peek peek;

    bool accepted() const noexcept { return Refusal::NONE == refusal; }
};

// Rows 1..7 of 4.4 in order, first refusal wins. Reads at most `length` bytes of `bytes`.
Admission admitFrame(std::uint8_t flags, const std::uint8_t *bytes, std::size_t length) noexcept;

std::int64_t readLe64(const std::uint8_t *at) noexcept;
std::uint16_t readLe16(const std::uint8_t *at) noexcept;

}}

#endif
