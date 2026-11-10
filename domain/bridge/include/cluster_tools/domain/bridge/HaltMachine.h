#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_HALT_MACHINE_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_HALT_MACHINE_H

// The halt record: what the bridge halted on, the FeedHalt it offers (the 9.2 field
// table, which satisfies the core's D3a field rules by construction), the AdminAck it waits for, the always-valid
// fallback after a REJECTED and the exits (9.4). A halt is local and latched (H1): it survives a
// session loss and a fence ("kept"), and only the resumption rule of 7.2 or an exit of 9.4 leaves it.

#include "cluster_tools/domain/bridge/Types.h"

#include <cstdint>

namespace cluster_tools { namespace domain { namespace bridge {

struct FeedHaltFields
{
    HaltReason reason = HaltReason::NONE;
    std::int64_t meEpochSeen = 0;
    std::int64_t expected = 0;
    std::int64_t seen = 0;
};

struct HaltRecord
{
    bool active = false;
    BridgeHalt halt = BridgeHalt::NONE;
    std::int64_t e = 0;          // the halted e (CORE_REFUSED: the refused seq C_storm + 1)
    std::int64_t seen = 0;       // GAP: min seen; EPOCH_MISMATCH: the foreign record's seq
    std::int64_t foreignEpoch = 0;
    std::int64_t syncEpoch = 0;  // the halted sync's epoch (the resumption rule compares the new report's)
    FeedHaltFields wire;         // the FeedHalt in force (the fallback replaces it)
    std::int64_t correlationId = 0;
    bool offeredOnce = false;    // HALTING -> HALTED happened (an OK offer)
    bool acked = false;          // ACCEPTED or DUPLICATE for `correlationId`
    bool fallback = false;       // the wire is the SOURCE_SCHEMA fallback after a REJECTED (9.3)
    bool needOffer = false;      // re-offer at the next opportunity (session up, fence, 5 s)
    TimePoint nextOfferAt{};
    // A SOURCE_SCHEMA halt on which every source refused (SCHEMA) the first frame of its read: a journal
    // of another schema at the chain head, never a bad copy to walk past (no successor walk, no re-scan exit, sticky
    // until a re-scan reads a source whose head is readable)
    bool foreignHead = false;
    int foreignSchema = -1; // the schemaId of the refused head frame (-1: not readable)

    bool sendsFeedHalt() const noexcept { return active && BridgeHalt::CORE_REFUSED != halt; }
};

// The 9.2 table: the FeedHalt for a halt reason at `e`.
FeedHaltFields feedHaltFor(BridgeHalt halt, std::int64_t e, std::int64_t seen, std::int64_t syncEpoch,
    std::int64_t foreignEpoch) noexcept;
// The always-valid fallback after a REJECTED: SOURCE_SCHEMA {expected = e, seen = e} (its only rule: expected >= 1).
FeedHaltFields fallbackFeedHalt(std::int64_t e, std::int64_t syncEpoch) noexcept;
// The core's D3a field rules (core applyFeedHalt): what the core would refuse as INVALID. The domain checks its own
// frames against it in the tests; it is not a runtime gate.
bool feedHaltFieldsValid(const FeedHaltFields &f) noexcept;
HaltReason wireReasonOf(BridgeHalt halt) noexcept;

}}}

#endif
