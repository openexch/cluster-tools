#ifndef CLUSTER_TOOLS_ADAPTERS_SBE_BRIDGE_CODEC_H
#define CLUSTER_TOOLS_ADAPTERS_SBE_BRIDGE_CODEC_H

// The C++ bridge's wire: the translation of an admitted ME journal schema 4 record into
// the core feed (money v7: 61, 62, 63, 64, 67, 68) with the generated codecs on both sides, field by field, every
// value verbatim (no range check, no default, no conversion; the five enum fields written raw), the two control
// encodes (QueryFeedPosition 7, FeedHalt 66) and the egress decode the domain needs (FeedPositionReport 17 v7,
// AdminAck 110, SettleRejected 114). The staging decision is the domain's (domain/bridge Stager); this class only
// encodes. No business field is validated here: the core is the one judge.

#include "cluster_tools/domain/bridge/Ports.h"
#include "cluster_tools/domain/bridge/Types.h"

#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace sbe_adapter { namespace bridge_codec {

using domain::bridge::AdminAck;
using domain::bridge::FeedPositionReport;
using domain::bridge::HaltReason;
using domain::bridge::SettleRejected;
using domain::bridge::StagedTrade;

inline constexpr std::uint16_t MONEY_SCHEMA_ID = 2;
inline constexpr std::uint16_t MONEY_VERSION = 7;
inline constexpr std::uint16_t JOURNAL_SCHEMA_ID = 4;
inline constexpr std::uint16_t JOURNAL_VERSION = 1;
inline constexpr std::size_t MESSAGE_HEADER_LENGTH = 8;
inline constexpr std::size_t FEED_HALT_FRAME_LENGTH = MESSAGE_HEADER_LENGTH + 41;
inline constexpr std::size_t QUERY_FRAME_LENGTH = MESSAGE_HEADER_LENGTH + 8;

enum class EgressKind : std::uint8_t
{
    IGNORED,         // another template (or another schema): not the domain's
    MALFORMED,       // a 17 / 110 / 114 with a wrong version or a short block (a malformed 17 counts as invalid)
    REPORT,          // FeedPositionReport 17 v7
    ADMIN_ACK,       // AdminAck 110
    SETTLE_REJECTED  // SettleRejected 114
};

struct EgressEvent
{
    EgressKind kind = EgressKind::IGNORED;
    std::uint16_t templateId = 0;
    FeedPositionReport report;
    AdminAck ack;
    SettleRejected reject;
};

// The two egress frames the APP reads (never the domain).
struct OrderRejectView
{
    std::int64_t correlationId = 0;
    std::uint16_t refTemplateId = 0;
    std::uint8_t reasonRaw = 0; // CoreRejectReason raw byte (UNREGISTERED_SESSION 35, UNAUTHORIZED_CLASS 20, INVALID 15)
};
inline constexpr std::uint16_t T_SETTLEMENT_APPLIED = 13;
inline constexpr std::uint16_t T_ORDER_REJECT = 101;
inline constexpr std::uint8_t CORE_REJECT_INVALID = 15;
inline constexpr std::uint8_t CORE_REJECT_UNAUTHORIZED_CLASS = 20;
inline constexpr std::uint8_t CORE_REJECT_UNREGISTERED_SESSION = 35;

// O3 / O4: one call per trade of a money 61 or 62 frame this codec produced (metrics only, never a
// validation): the trade's id and its ME timestamp (epoch ms).
using TradeVisitor = void (*)(void *context, std::int64_t tradeId, std::int64_t meTimestampMs);

class BridgeCodec final : public domain::bridge::FrameEncoder
{
public:
    std::size_t encodeRecord(const std::uint8_t *record, std::size_t length, std::uint8_t *out,
        std::size_t capacity) noexcept override;
    std::size_t encodeTradeBatch(const StagedTrade *trades, std::size_t n, std::uint8_t *out,
        std::size_t capacity) noexcept override;
    std::size_t encodeFeedHalt(std::int64_t correlationId, HaltReason reason, std::int64_t meEpochSeen,
        std::int64_t expected, std::int64_t seen, std::int64_t identity, std::uint8_t *out,
        std::size_t capacity) noexcept override;
    std::size_t encodeQuery(std::int64_t correlationId, std::uint8_t *out, std::size_t capacity) noexcept override;

    // Raw bytes for every enum (an unknown haltReason or reject reason is mapped by the domain, never thrown).
    static EgressKind decodeEgress(const std::uint8_t *frame, std::size_t length, EgressEvent &out) noexcept;

    // OrderReject 101 v7 / SettlementApplied 13 v7: false when the frame is not that template at money v7 with its
    // full block (EGRESS_VERSION_FLOOR RED: a v6 frame accepted).
    static bool decodeOrderReject(const std::uint8_t *frame, std::size_t length, OrderRejectView &out) noexcept;
    static bool decodeSettlementApplied(const std::uint8_t *frame, std::size_t length, std::int64_t &tradeId) noexcept;
    // Visits every trade of a 61 / 62 money frame (the generated offsets); returns the number visited (0 for any
    // other frame).
    static std::size_t visitTrades(const std::uint8_t *frame, std::size_t length, TradeVisitor visitor,
        void *context) noexcept;

    // The money frame length a schema 4 record becomes (0 when the record is not an admitted shape).
    static std::size_t frameLengthFor(const std::uint8_t *record, std::size_t length) noexcept;
};

}}}

#endif
