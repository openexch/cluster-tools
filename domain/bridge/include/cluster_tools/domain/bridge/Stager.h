#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_STAGER_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_STAGER_H

// The staging decision (the function implements in Java): only consecutive trade
// records stage, at most 64; a flush of 1 is a 61, of 2..64 a 62. The live domain and the E2 `translate` (5.3)
// drive this one class, so the differential tests grade the staging the bridge ships. Back-pressure keeps
// everything: a staged record leaves only by an OK offer or by drop() (a fence).

#include "cluster_tools/domain/bridge/Ports.h"
#include "cluster_tools/domain/bridge/Types.h"

#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

// One record carried by an offered frame (its ring entry once the offer returned OK).
struct OfferedRecord
{
    std::int64_t seq = 0;
    std::int64_t endPosition = 0;
    std::uint16_t source = 0;
    std::uint32_t keyIndex = 0;
};

enum class StepResult : std::uint8_t
{
    OK,
    RETRY,          // back-pressure (nothing advanced)
    CLOSED,         // the core port is closed (nothing advanced)
    NO_ROOM,        // the ring cannot take the frame's entries (nothing advanced)
    REFUSED,        // a transport error: fatal
    ENCODER_FAILED  // the encoder returned 0: fatal
};

constexpr bool isFatal(StepResult r) noexcept
{
    return StepResult::REFUSED == r || StepResult::ENCODER_FAILED == r;
}

// Where an encoded frame goes: the domain offers it to the core and pushes its ring entries on OK; E2 collects it.
class FrameOutput
{
public:
    virtual ~FrameOutput() = default;
    virtual StepResult emit(const std::uint8_t *frame, std::size_t length, std::uint16_t moneyTemplate,
        const OfferedRecord *records, std::size_t n) noexcept = 0;
};

struct ForwardResult
{
    StepResult step = StepResult::OK;  // the record: OK = staged or offered (it advanced), else nothing advanced
    StepResult flush = StepResult::OK; // the cap flush after staging the 64th (back-pressure keeps the 64)
};

class Stager
{
public:
    static constexpr std::size_t CAP = 64;

    explicit Stager(FrameEncoder &encoder) noexcept : m_encoder(encoder) {}

    // A record the forward rule decided (seq == last + 1), `templateId` its peeked schema 4 template.
    ForwardResult forward(const std::uint8_t *record, std::size_t length, std::uint16_t templateId,
        const OfferedRecord &meta, FrameOutput &out) noexcept;
    // Offers the staged trades (61 for one, 62 for more); OK when nothing is staged.
    StepResult flush(FrameOutput &out) noexcept;
    // A fence: the staged trades are dropped (the new sync re-reads them). Returns how many.
    std::size_t drop() noexcept;

    std::size_t size() const noexcept { return m_size; }
    bool empty() const noexcept { return 0 == m_size; }
    // The last flush attempt was back-pressured (RETRY, CLOSED or NO_ROOM) and the trades are still staged:
    // `tick` retries only such a flush.
    bool flushPending() const noexcept { return m_pending && m_size > 0; }
    const StagedTrade *trades() const noexcept { return m_trades; }

private:
    std::size_t cap() const noexcept;

    FrameEncoder &m_encoder;
    StagedTrade m_trades[2 * CAP]{}; // the CAP_OFF seam's headroom; production never exceeds CAP
    OfferedRecord m_meta[2 * CAP]{};
    std::size_t m_size = 0;
    bool m_pending = false;
    std::uint8_t m_out[OUTPUT_BUFFER]{};
};

}}}

#endif
