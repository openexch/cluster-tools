#ifndef CLUSTER_TOOLS_ADAPTERS_SBE_FORWARDER_CODEC_H
#define CLUSTER_TOOLS_ADAPTERS_SBE_FORWARDER_CODEC_H

#include <cstddef>
#include <cstdint>

#include "cluster_tools/domain/forwarder/Dispatch.h"
#include "cluster_tools/domain/forwarder/ResyncMachine.h"

// ForwarderCodec - the forwarder's wire (RC2 S12 contract 5.1, W5). The only forwarder translation unit
// that includes generated SBE headers; this header pulls in none.
//
// Core side (money-schema id 2, header version exactly 7): decode 104 MeDispatch, 105 MeDispatchBatch,
// 108 PendingDispatchEnd, 109 AdmissionState, 101 OrderReject; encode 65 RequestPendingDispatch.
// ME side (order-schema id 1, header version exactly 12): encode 9 DurableOrderCommand and 10
// ReleaseCommandIds; decode 21 ClusterHeartbeat (meEpoch).
//
// Every enum field is read RAW (no generated enum getter, which throws on an unknown value: a kind 4 or a
// releaseScope 2 must reach the classifier, FIX-10). Frames are bounds-checked before any field is read:
// a 104 or 105 entry with an acting block below 89, a group that does not fit, or a short frame is
// malformed (false, nothing delivered). Nothing here throws, allocates or keeps state.

namespace cluster_tools { namespace sbe_adapter { namespace forwarder_codec {

using domain::forwarder::Dispatch;
using domain::forwarder::DurableCommand;
using domain::forwarder::PendingRequest;
using domain::forwarder::ReleaseEntry;

inline constexpr std::uint16_t MONEY_SCHEMA_ID = 2;
inline constexpr std::uint16_t MONEY_VERSION = 7;
inline constexpr std::uint16_t MATCH_SCHEMA_ID = 1;
inline constexpr std::uint16_t MATCH_VERSION = 12;

inline constexpr std::uint16_t T_REQUEST_PENDING_DISPATCH = 65;
inline constexpr std::uint16_t T_REGISTER_SESSION = 80;
inline constexpr std::uint16_t T_ORDER_REJECT = 101;
inline constexpr std::uint16_t T_ME_DISPATCH = 104;
inline constexpr std::uint16_t T_ME_DISPATCH_BATCH = 105;
inline constexpr std::uint16_t T_PENDING_DISPATCH_END = 108;
inline constexpr std::uint16_t T_ADMISSION_STATE = 109;
inline constexpr std::uint16_t T_ADMIN_ACK = 110;
inline constexpr std::uint16_t T_CONFIG_APPLIED = 112;
inline constexpr std::uint16_t T_ORDER_FAULT = 113;

inline constexpr std::uint16_t T_DURABLE_ORDER_COMMAND = 9;
inline constexpr std::uint16_t T_RELEASE_COMMAND_IDS = 10;
inline constexpr std::uint16_t T_CLUSTER_HEARTBEAT = 21;

inline constexpr std::size_t MESSAGE_HEADER_LENGTH = 8;
inline constexpr std::size_t DISPATCH_BLOCK_V7 = 89;
inline constexpr std::size_t DURABLE_COMMAND_FRAME_LENGTH = 88;      // 8 + block 80
inline constexpr std::size_t REQUEST_PENDING_FRAME_LENGTH = 28;      // 8 + block 20
inline constexpr std::size_t RELEASE_ENTRY_LENGTH = 17;
inline constexpr std::uint32_t RELEASE_FRAME_MAX_ENTRIES = 48;
inline constexpr std::size_t releaseFrameLength(std::uint32_t entries) { return 20 + RELEASE_ENTRY_LENGTH * entries; }
inline constexpr std::size_t RELEASE_FRAME_MAX_LENGTH = 20 + RELEASE_ENTRY_LENGTH * RELEASE_FRAME_MAX_ENTRIES; // 836

// The 32 B session header + this frame must fit one unfragmented message at the 1408 B MTU (5.1).
static_assert(RELEASE_FRAME_MAX_LENGTH + 32 <= 1408 - 32, "a full release frame stays one message at MTU 1408");

struct Header
{
    std::uint16_t blockLength = 0;
    std::uint16_t templateId = 0;
    std::uint16_t schemaId = 0;
    std::uint16_t version = 0;
};

// False if the frame is shorter than a message header.
bool readHeader(const std::uint8_t *frame, std::size_t length, Header &header) noexcept;

// ---- core frames (the caller already checked schemaId 2 and version 7)
// A dispatchSeq below 1 makes the frame malformed (review NOTE-2: the core mints from 1, so only a corrupt core can send
// one; it cannot be ordered, so nothing of the frame is delivered). *seqBelowOne (optional) says that was the reason.
bool decodeDispatch(const std::uint8_t *frame, std::size_t length, const Header &header, Dispatch &out,
    bool *seqBelowOne = nullptr) noexcept;

using DispatchSink = void (*)(void *context, const Dispatch &dispatch);
// Validates the whole 105 first (bounds, and every entry's dispatchSeq >= 1); only then calls sink once per entry, in group order. False = malformed.
bool decodeDispatchBatch(const std::uint8_t *frame, std::size_t length, const Header &header, DispatchSink sink,
    void *context, bool *seqBelowOne = nullptr) noexcept;

struct PendingEnd
{
    std::int64_t correlationId = 0;
    std::int32_t count = 0;
    std::int64_t headDispatchSeq = 0;
};
bool decodePendingEnd(const std::uint8_t *frame, std::size_t length, const Header &header, PendingEnd &out) noexcept;

struct Admission
{
    std::int32_t marketId = 0;
    std::uint8_t gateState = 0;     // raw
    std::uint32_t gateReasons = 0;  // raw bit set
};
bool decodeAdmission(const std::uint8_t *frame, std::size_t length, const Header &header, Admission &out) noexcept;

struct OrderRejectView
{
    std::int64_t correlationId = 0;
    std::uint16_t refTemplateId = 0;
    std::uint8_t reason = 0; // CoreRejectReason raw
};
inline constexpr std::uint8_t REJECT_INVALID = 15;
inline constexpr std::uint8_t REJECT_UNAUTHORIZED_CLASS = 20;
inline constexpr std::uint8_t REJECT_UNREGISTERED_SESSION = 35;
bool decodeOrderReject(const std::uint8_t *frame, std::size_t length, const Header &header, OrderRejectView &out) noexcept;

// 65: returns the frame length, 0 if out is too small.
std::size_t encodeRequestPendingDispatch(std::uint8_t *out, std::size_t capacity, const PendingRequest &request) noexcept;

// ---- ME frames
// 21: false if not a heartbeat or the acting block has no meEpoch (version already checked = 12).
bool decodeHeartbeatEpoch(const std::uint8_t *frame, std::size_t length, const Header &header, std::int64_t &meEpoch) noexcept;
// 9 (v12, block 80): returns the frame length (88), 0 if out is too small.
std::size_t encodeDurableCommand(std::uint8_t *out, std::size_t capacity, const DurableCommand &command) noexcept;
// 10 (v12): root releaseSeq, group of n entries; 0 if out is too small or n == 0.
std::size_t encodeReleaseCommandIds(std::uint8_t *out, std::size_t capacity, std::int64_t releaseSeq,
    const ReleaseEntry *entries, std::uint32_t n) noexcept;

}}}

#endif
