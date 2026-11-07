#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_DISPATCH_RULES_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_DISPATCH_RULES_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/Dispatch.h"

// Classification, the per-kind rules, the one-to-one mappings and the INVALID carrier
// (RC2 S12 contract 5.2-5.5, BLOCKER-1 Fix A).
//
// The forwarder relays, it never decides (G2): every decoded dispatch reaches the ME in one of the
// forms below (G4), and the ME journals the verdict the core applies.
//   PASS             every rule holds: 5.2 (kinds 0..2) or 5.3 (kind 3)
//   ME_INVALID (I)   kinds 0..2, every R rule holds, a B rule fails: sent verbatim, the ME journals INVALID 8
//   CARRIER (C)      kinds 0..2 with an R rule failing, or V12 (kind > 3), or V1 (seq < 1): sent as the
//                    5.5 carrier, a reader-valid frame for the same id that the ME journals INVALID 8
//   RELEASE_ANOMALY  kind 3, X1 and X2 hold, A1 or V1 fails: sent as a normal release entry (RA)
//   RELEASE_REFUSED  kind 3, X1 or X2 fails: sent alone in a one-entry frame (RX), so the ME's
//                    decode drop costs no neighbour
//   UNROUTABLE       kinds 0..2 / V12 with both ids 0: no carrier can carry the id; counted, not sent
// Rules are evaluated in the ME's own order (FIX-10): V12 first; kinds 0..2 the reader group R1-R5
// (DurableOrderIntent.isValid), then the behaviour-2 causes B1-B6 (behaviourTwoCause order), then the
// forwarder-local V1; kind 3: X1, X2, A1, V1. The FIRST failing rule is reported.

namespace cluster_tools { namespace domain { namespace forwarder {

enum class DispatchClass : std::uint8_t
{
    PASS,
    ME_INVALID,
    CARRIER,
    RELEASE_ANOMALY,
    RELEASE_REFUSED,
    UNROUTABLE,
};

enum class RuleId : std::uint8_t
{
    NONE,
    V12,
    R1,
    R2,
    R3,
    R4,
    R5,
    B1,
    B2,
    B3,
    B4,
    B5,
    B6,
    V1,
    X1,
    X2,
    A1,
    COUNT,
};

inline constexpr unsigned RULE_COUNT = static_cast<unsigned>(RuleId::COUNT);
inline constexpr unsigned CLASS_COUNT = 6;

struct Classification
{
    DispatchClass cls = DispatchClass::PASS;
    RuleId rule = RuleId::NONE;

    bool operator==(const Classification &) const = default;
};

Classification classify(const Dispatch &d) noexcept;

// 5.2: template 9 field by field (dispatchSeq and releaseScope dropped).
DurableCommand verbatimCommand(const Dispatch &d) noexcept;
// 5.5: the INVALID carrier; a pure function of d. Undefined for both ids 0 (UNROUTABLE).
DurableCommand carrierCommand(const Dispatch &d) noexcept;
// 5.3: one template 10 entry (commandIdLow is the whole idLow floor, scope raw).
ReleaseEntry releaseEntry(const Dispatch &d) noexcept;

// The 5.5 form of a classified dispatch, as the send ring stores it.
enum class SlotForm : std::uint8_t
{
    COMMAND,          // template 9 (verbatim or carrier)
    RELEASE,          // template 10 entry, groups with adjacent RELEASE slots (P, RA)
    RELEASE_ISOLATED, // template 10 entry, always a frame of its own (RX)
};

struct RingSlot
{
    std::int64_t seq = 0;
    SlotForm form = SlotForm::COMMAND;
    DurableCommand command;
    ReleaseEntry release;
};

// False for UNROUTABLE (nothing goes on the wire); otherwise fills slot with the 5.5 form.
bool toRingSlot(const Dispatch &d, const Classification &c, RingSlot &slot) noexcept;

const char *nameOf(RuleId rule) noexcept;
const char *nameOf(DispatchClass cls) noexcept;
// The `action` label of forwarder_invalid_dispatch_total ("" for PASS).
const char *actionOf(DispatchClass cls) noexcept;

}}}

#endif
