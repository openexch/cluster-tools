#ifndef CLUSTER_TOOLS_ARCHIVE_FRAME_PEEK_H
#define CLUSTER_TOOLS_ARCHIVE_FRAME_PEEK_H

// The frame admission gate and common-head peek of the archive follow library.
// Pure: no Aeron, no SBE. One WITH_BRIDGE
// translation unit (archive/aeron/src/JournalCodecLayout.cpp) static-asserts every ME journal constant here
// against the generated schema 4 codecs, so a schema edit that moves them fails the build.
//
// The gate is journal-neutral: a JournalProfile names the schema id and version, the largest frame, the root
// block per template (and the one group rule the ME journal has) and the three head offsets. Two profiles
// ship: ME_JOURNAL (the bridge's, the constants below, unchanged) and CORE_ORDER_JOURNAL (the core order
// journal: schema 5, one template, a 208-byte block). Every journal this library reads starts its block with
// the same head: a sequence at 0, an epoch at 8 and a position at 16.
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

// The core order journal (schema 5, one record per order event): template 1, block 208, message 216. Its head
// mirrors the ME journal's: journalSeq at 0, epoch (the core genesis id) at 8, the core log position at 16.
inline constexpr std::uint16_t CORE_ORDER_JOURNAL_SCHEMA_ID = 5;
inline constexpr std::uint16_t CORE_ORDER_JOURNAL_SCHEMA_VERSION = 1;
inline constexpr std::uint16_t CORE_ORDER_TEMPLATE_EVENT = 1;
inline constexpr std::uint16_t CORE_ORDER_BLOCK_EVENT = 208;
inline constexpr std::size_t CORE_ORDER_FRAME_LENGTH = MESSAGE_HEADER_LENGTH + CORE_ORDER_BLOCK_EVENT; // 216

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

inline constexpr std::size_t MAX_PROFILE_TEMPLATES = 8;

// What the gate needs to know about one journal. Templates are 1..templateCount; a block of 0 means the
// template does not exist. A group template (0 = none) is a root block followed by one repeating group whose
// entries have groupEntryBlock bytes, at most groupMaxEntries of them.
struct JournalProfile
{
    const char *name = "";
    std::uint16_t schemaId = 0;
    std::uint16_t schemaVersion = 0;
    std::size_t maxFrameLength = 0;
    std::uint16_t templateCount = 0;
    std::uint16_t blocks[MAX_PROFILE_TEMPLATES + 1] = {}; // index = template id, [0] unused
    std::uint16_t groupTemplateId = 0;
    std::uint16_t groupEntryBlock = 0;
    std::uint16_t groupMaxEntries = 0;
    std::size_t offsetSeq = FRAME_OFFSET_JOURNAL_SEQ;    // frame offsets of the three head fields
    std::size_t offsetEpoch = FRAME_OFFSET_ME_EPOCH;
    std::size_t offsetPosition = FRAME_OFFSET_EGRESS_SEQ;
    const char *positionLabel = "position";              // what the third head field is called in output

    constexpr std::uint16_t blockFor(std::uint16_t templateId) const noexcept
    {
        return templateId >= 1 && templateId <= templateCount && templateId <= MAX_PROFILE_TEMPLATES ? blocks[templateId]
                                                                                                   : 0;
    }
    constexpr std::size_t groupMinFrame() const noexcept
    {
        return MESSAGE_HEADER_LENGTH + blockFor(groupTemplateId) + GROUP_HEADER_LENGTH;
    }
};

inline constexpr JournalProfile ME_JOURNAL = { "me", JOURNAL_SCHEMA_ID, JOURNAL_SCHEMA_VERSION, MAX_FRAME_LENGTH, 5,
    { 0, BLOCK_TRADE, BLOCK_TERMINAL, BLOCK_OUTCOME, BLOCK_RELEASE, BLOCK_CONFIG }, TEMPLATE_RELEASE, RELEASE_ENTRY_BLOCK,
    RELEASE_MAX_ENTRIES, FRAME_OFFSET_JOURNAL_SEQ, FRAME_OFFSET_ME_EPOCH, FRAME_OFFSET_EGRESS_SEQ, "egress" };

inline constexpr JournalProfile CORE_ORDER_JOURNAL = { "core-order", CORE_ORDER_JOURNAL_SCHEMA_ID,
    CORE_ORDER_JOURNAL_SCHEMA_VERSION, CORE_ORDER_FRAME_LENGTH, 1, { 0, CORE_ORDER_BLOCK_EVENT }, 0, 0, 0,
    FRAME_OFFSET_JOURNAL_SEQ, FRAME_OFFSET_ME_EPOCH, FRAME_OFFSET_EGRESS_SEQ, "log" };

static_assert(ME_JOURNAL.groupMinFrame() == RELEASE_MIN_FRAME);
static_assert(ME_JOURNAL.maxFrameLength == ME_JOURNAL.groupMinFrame() + RELEASE_MAX_ENTRIES * RELEASE_ENTRY_BLOCK);
static_assert(CORE_ORDER_JOURNAL.maxFrameLength == MESSAGE_HEADER_LENGTH + CORE_ORDER_JOURNAL.blockFor(1));

// Refusal reasons in table order (4.4 rows 1..7); the label is the metric label.
enum class Refusal : std::uint8_t
{
    NONE = 0,
    FRAGMENTED,   // 1: header flags != 0xC0
    LENGTH,       // 2: length outside [8, 1112]
    SCHEMA,       // 3: schemaId or version not the profile's (ME: schema 3, schema 1: loud, never skipped)
    TEMPLATE,     // 4: templateId outside the profile's templates (ME: 1..5)
    BLOCK,        // 5: root blockLength != the template's
    FRAME_LENGTH, // 6: frame length != 8 + block (template 4: the group rule)
    HEAD,         // 7: journalSeq < 1 or epoch < 1
    COUNT
};

const char *refusalLabel(Refusal refusal) noexcept;

// The head of an admitted frame. The field names are the ME journal's; on every profile the second head field
// is the record's epoch and the third its position (the ME egressSeq, the core log position): epoch() and
// position() are the journal-neutral names.
struct Peek
{
    std::uint16_t templateId = 0;
    std::int64_t journalSeq = 0;
    std::int64_t meEpoch = 0;
    std::int64_t egressSeq = 0;

    constexpr std::int64_t epoch() const noexcept { return meEpoch; }
    constexpr std::int64_t position() const noexcept { return egressSeq; }
};

struct Admission
{
    Refusal refusal = Refusal::NONE;
    bool hasPeek = false; // true when rows 1-6 passed (accepted, or refused for HEAD)
    Peek peek;

    bool accepted() const noexcept { return Refusal::NONE == refusal; }
};

// Rows 1..7 of 4.4 in order, first refusal wins. Reads at most `length` bytes of `bytes`. The first form is the
// ME journal's gate (the bridge's, unchanged); the second admits frames of any profile by the same rows.
Admission admitFrame(std::uint8_t flags, const std::uint8_t *bytes, std::size_t length) noexcept;
Admission admitFrame(const JournalProfile &profile, std::uint8_t flags, const std::uint8_t *bytes, std::size_t length) noexcept;

std::int64_t readLe64(const std::uint8_t *at) noexcept;
std::uint16_t readLe16(const std::uint8_t *at) noexcept;

}}

#endif
