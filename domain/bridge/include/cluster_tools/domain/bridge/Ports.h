#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_PORTS_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_PORTS_H

// The two ports of the bridge domain: the frame encoder (implemented by
// adapters/sbe BridgeCodec with the generated codecs; a fake one in the pure tests) and the core port
// ClusterClient::tryOffer with its OfferStatus mapped onto OfferOutcome).

#include "cluster_tools/domain/bridge/Types.h"

#include <cstddef>
#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

class FrameEncoder
{
public:
    virtual ~FrameEncoder() = default;
    // One admitted schema 4 record of template 1..5 -> 61 / 63 / 64 / 67 / 68. Returns the frame length, 0 when the
    // record's length does not match its template or the output does not fit (behind the gate: impossible).
    virtual std::size_t encodeRecord(const std::uint8_t *record, std::size_t length, std::uint8_t *out,
        std::size_t capacity) noexcept = 0;
    // n >= 2 staged trade records -> one 62.
    virtual std::size_t encodeTradeBatch(const StagedTrade *trades, std::size_t n, std::uint8_t *out,
        std::size_t capacity) noexcept = 0;
    virtual std::size_t encodeFeedHalt(std::int64_t correlationId, HaltReason reason, std::int64_t meEpochSeen,
        std::int64_t expected, std::int64_t seen, std::int64_t identity, std::uint8_t *out,
        std::size_t capacity) noexcept = 0;
    virtual std::size_t encodeQuery(std::int64_t correlationId, std::uint8_t *out, std::size_t capacity) noexcept = 0;
};

class CorePort
{
public:
    virtual ~CorePort() = default;
    virtual OfferOutcome tryOffer(const std::uint8_t *frame, std::size_t length) noexcept = 0;
};

// The money v7 template ids the domain emits (the codec static-asserts them against the generated codecs).
inline constexpr std::uint16_t T_QUERY_FEED_POSITION = 7;
inline constexpr std::uint16_t T_FEED_POSITION_REPORT = 17;
inline constexpr std::uint16_t T_ME_JOURNAL_TRADE = 61;
inline constexpr std::uint16_t T_ME_JOURNAL_TRADE_BATCH = 62;
inline constexpr std::uint16_t T_ME_JOURNAL_TERMINAL = 63;
inline constexpr std::uint16_t T_ME_COMMAND_OUTCOME = 64;
inline constexpr std::uint16_t T_FEED_HALT = 66;
inline constexpr std::uint16_t T_ME_COMMAND_RELEASE = 67;
inline constexpr std::uint16_t T_ME_ENGINE_CONFIG = 68;
inline constexpr std::uint16_t T_ADMIN_ACK = 110;
inline constexpr std::uint16_t T_SETTLE_REJECTED = 114;

// The money frame a schema 4 record template becomes (1 -> 61, 2 -> 63, 3 -> 64, 4 -> 67, 5 -> 68; else 0).
constexpr std::uint16_t frameTemplateFor(std::uint16_t journalTemplate) noexcept
{
    switch (journalTemplate)
    {
        case 1: return T_ME_JOURNAL_TRADE;
        case 2: return T_ME_JOURNAL_TERMINAL;
        case 3: return T_ME_COMMAND_OUTCOME;
        case 4: return T_ME_COMMAND_RELEASE;
        case 5: return T_ME_ENGINE_CONFIG;
        default: return 0;
    }
}

// The ingress frame bound: the largest feed frame plus the 32-byte cluster session header fits
// the core session's 8192-byte MTU with its own 32-byte frame header.
inline constexpr std::size_t MAX_BATCH_FRAME = 12 + 109 * 64; // 6988
inline constexpr std::size_t MAX_RELEASE_FRAME = 56 + 22 * 48; // 1112
inline constexpr std::size_t MAX_FEED_FRAME = MAX_BATCH_FRAME;
static_assert(MAX_BATCH_FRAME >= MAX_RELEASE_FRAME && MAX_BATCH_FRAME >= 161 && MAX_BATCH_FRAME >= 117);
static_assert(MAX_FEED_FRAME + 32 <= 8192 - 32, "a feed frame must fit the core session's ingress MTU");
inline constexpr std::size_t OUTPUT_BUFFER = 8192; // the one 8 KiB output buffer

}}}

#endif
