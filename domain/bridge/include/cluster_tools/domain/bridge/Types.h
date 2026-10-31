#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_TYPES_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_TYPES_H

// The bridge domain's value types (RC2 S9b contract section 4): the egress the core sends the bridge, the fence
// triggers, the offer outcome S9c maps the S11 client's OfferStatus onto, the staged trade, the source commands,
// the state, halt, fatal and degraded codes and the read-only view S9c renders. Pure: no SBE, no Aeron.

#include "cluster_tools/archive/Follow.h"

#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

using archive::TimePoint;

// The wire FeedHaltReason (money v7) of a FeedHalt the bridge sends and of a report's haltReason.
enum class HaltReason : std::uint8_t
{
    NONE = 0,
    GAP = 1,
    EPOCH_MISMATCH = 2,
    SOURCE_SCHEMA = 3,
    SOURCE_UNBOUND = 4,
    SETTLE_REFUSED = 5
};
const char *haltReasonName(HaltReason reason) noexcept;

// The wire SettleRejectReason (money v7). Raw bytes are kept in the structs below; this names the codes.
enum class RejectReason : std::uint8_t
{
    UNKNOWN_MARKET = 0,
    INVALID_PRICE,
    INVALID_QUANTITY,
    NOTIONAL_OVERFLOW,
    INVALID_PARTY,
    TRADE_GAP = 5,
    FEED_GAP = 6,
    ME_EPOCH_MISMATCH = 7,
    FEED_UNBOUND = 8,
    OTHER = 255
};

// FeedPositionReport 17 v7 (all eight fields). Raw bytes for the two enum-like fields: validation is the
// domain's (7.2), never a decode exception.
struct FeedPositionReport
{
    std::int64_t correlationId = 0;
    std::int64_t consumePosition = 0;
    std::int64_t lastAppliedTradeId = 0;
    bool journalEnabled = false;
    std::int64_t meEpoch = 0;
    std::int64_t lastAppliedJournalSeq = 0;
    bool feedHalted = false;          // derived from feedHaltedRaw == 1 (kept for readers)
    std::uint8_t haltReasonRaw = 0;
    std::uint8_t feedHaltedRaw = 0;   // the raw BoolFlag byte: 2..255 is invalid, never `true` (review N-5)
};

// SettleRejected 114: the core's refusal of a feed entry, ORIGIN egress of the bridge session. For a feed entry
// the core fills tradeId = marketId = journalPosition = the entry's journalSeq and `detail` = c + 1 (a gap) or the
// bound epoch (a foreign epoch).
struct SettleRejected
{
    std::int64_t tradeId = 0;
    std::int32_t marketId = 0;
    std::int64_t journalPosition = 0;
    std::uint8_t reasonRaw = 0;
    std::int64_t detail = 0;
};

// AdminAck 110 (the domain reads the acks of its FeedHalt 66; S9c reads the RegisterSession 80 ones).
struct AdminAck
{
    std::int64_t correlationId = 0;
    std::uint16_t refTemplateId = 0;
    std::uint8_t resultRaw = 0; // AdminResult: ACCEPTED 0, DUPLICATE 1, REJECTED 2, ADOPTED 3, STALE 4
    std::uint8_t reason = 0;
};

inline constexpr std::uint8_t ADMIN_ACCEPTED = 0;
inline constexpr std::uint8_t ADMIN_DUPLICATE = 1;
inline constexpr std::uint8_t ADMIN_REJECTED = 2;

enum class FenceTrigger : std::uint8_t
{
    NEW_LEADER,
    SESSION_REESTABLISHED,
    SETTLE_REJECTED_GAP,
    SETTLE_REJECTED_UNBOUND,
    SETTLE_REJECTED_EPOCH,
    REPORT_LOSS,
    REPORT_EPOCH_CHANGED,
    REPORT_CURSOR_REGRESSED,
    LANE_AHEAD,
    COUNT
};
const char *fenceTriggerName(FenceTrigger trigger) noexcept;
// A refusal fence (9.6): the only fences the storm guard counts.
constexpr bool isRefusalFence(FenceTrigger t) noexcept
{
    return FenceTrigger::SETTLE_REJECTED_GAP == t || FenceTrigger::SETTLE_REJECTED_UNBOUND == t ||
        FenceTrigger::SETTLE_REJECTED_EPOCH == t || FenceTrigger::REPORT_LOSS == t;
}

// The S11 client's OfferStatus as the domain sees it (S9c maps: OK; BACK_PRESSURED, NOT_CONNECTED, NOT_READY ->
// RETRY; CLOSED, MAX_POSITION_EXCEEDED -> CLOSED; TRANSPORT_ERROR -> REFUSED).
enum class OfferOutcome : std::uint8_t
{
    OK,
    RETRY,   // back-pressure, not connected, not ready
    CLOSED,  // closed, max position: S9c reconnects
    REFUSED  // transport error: a frame the client cannot send
};

inline constexpr std::size_t TRADE_RECORD_LENGTH = 117; // schema 4 header 8 + Trade block 109

struct StagedTrade
{
    std::uint8_t record[TRADE_RECORD_LENGTH];
    std::int64_t seq;
    std::int64_t endPosition;
    std::uint16_t source;
    std::uint32_t keyIndex;
};

enum class SourceCommandKind : std::uint8_t
{
    NONE,
    SWITCH,
    REATTACH
};

struct SourceCommand
{
    SourceCommandKind kind = SourceCommandKind::NONE;
    std::size_t index = 0;
};

enum class State : std::uint8_t
{
    DOWN,
    SYNCING,
    STREAMING,
    HALTING,
    HALTED
};
const char *stateName(State state) noexcept;

enum class BridgeHalt : std::uint8_t
{
    NONE,
    GAP,
    EPOCH_MISMATCH,
    SOURCE_SCHEMA,
    CORE_REFUSED
};
const char *bridgeHaltName(BridgeHalt halt) noexcept;

enum class Fatal : std::uint8_t
{
    NONE,
    FEEDHALT_REJECTED,
    ENCODER_OVERFLOW,
    OFFER_REFUSED
};
const char *fatalName(Fatal fatal) noexcept;

enum class Degraded : std::uint32_t
{
    NONE = 0,
    NO_REACHABLE_SOURCE = 1,
    NO_SOURCE_BEYOND_E = 2,
    HOLE_WITH_LAGGING_SOURCE = 4,
    REPORT_INVALID = 8,
    REPORT_TIMEOUT = 16
};

constexpr std::uint32_t bit(Degraded d) noexcept
{
    return static_cast<std::uint32_t>(d);
}

// Read by S9c for /health, /ready, logs (copied; single writer, the duty thread B).
struct BridgeView
{
    State state = State::DOWN;
    BridgeHalt halt = BridgeHalt::NONE; // the halt in force (also a kept halt while DOWN or re-syncing)
    HaltReason coreMirror = HaltReason::NONE;
    std::int64_t syncEpoch = 0;
    std::int64_t cursor = 0;   // C of the last report
    std::int64_t last = 0;     // the forward rule's last (G1)
    std::int64_t expected = 0; // e = last + 1
    std::size_t attached = 0;
    bool reportThisTerm = false;
    std::uint32_t degraded = 0;
    std::size_t ringDepth = 0;
    std::size_t staged = 0;
    TimePoint lastForwardAt{};
    Fatal fatal = Fatal::NONE;
};

// One source verdict (8.1), logged once per (source, e, kind); S9c drains them into the log.
enum class VerdictKind : std::uint8_t
{
    UNKNOWN,
    HOLED,
    BAD,
    FOREIGN_EPOCH,
    BEHIND,
    UNREACHABLE,
    REFUSED_STATIC
};
const char *verdictName(VerdictKind kind) noexcept;

// BAD detail labels (bridge_source_bad_total{reason}): the 4.4 refusal labels first, then the domain's own.
enum class BadReason : std::uint8_t
{
    REFUSAL_FRAGMENTED,
    REFUSAL_LENGTH,
    REFUSAL_SCHEMA,
    REFUSAL_TEMPLATE,
    REFUSAL_BLOCK,
    REFUSAL_FRAME_LENGTH,
    REFUSAL_HEAD,
    HEAD_NOT_CONFIG,
    SEQ_REGRESSION,
    FOREIGN_EPOCH,
    MALFORMED_EXTENT,
    STALE_EPOCH_PREFIX, // review NOTE-8: a foreign-epoch frame before any own-epoch frame of the read
    COUNT
};
const char *badReasonLabel(BadReason reason) noexcept;

struct VerdictEvent
{
    std::size_t source = 0;
    std::int64_t e = 0;
    VerdictKind kind = VerdictKind::UNKNOWN;
    BadReason bad = BadReason::COUNT;  // for BAD and FOREIGN_EPOCH
    std::int64_t seq = 0;              // HOLED seen, BAD/FOREIGN seq
    std::int64_t epoch = 0;            // FOREIGN_EPOCH
    archive::FaultKind fault = archive::FaultKind::NONE; // UNREACHABLE
};

}}}

#endif
