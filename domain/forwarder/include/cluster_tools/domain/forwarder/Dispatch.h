#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_DISPATCH_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_DISPATCH_H

#include <cstdint>

// The forwarder's value types (RC2 S12 contract sections 2 and 5). Pure: no SBE, no Aeron.
//
// Dispatch          the 16 fields of one core MeDispatch (money-schema v7 template 104, or one 105
//                   entry), every enum as its RAW wire value: the forwarder must classify a value the
//                   schema does not name (kind 4, releaseScope 2) instead of throwing (FIX-10).
// DurableCommand    the 14 fields of the ME's DurableOrderCommand (order-schema v12 template 9).
// ReleaseEntry      one entry of the ME's ReleaseCommandIds (template 10).

namespace cluster_tools { namespace domain { namespace forwarder {

inline constexpr std::uint8_t KIND_CREATE = 0;
inline constexpr std::uint8_t KIND_CANCEL = 1;
inline constexpr std::uint8_t KIND_AMEND = 2;
inline constexpr std::uint8_t KIND_RELEASE_ID = 3;

inline constexpr std::uint8_t SCOPE_UP_TO = 0;
inline constexpr std::uint8_t SCOPE_ORDER = 1;

struct Dispatch
{
    std::int64_t dispatchSeq = 0;
    std::int64_t commandIdHigh = 0;
    std::int64_t commandIdLow = 0;
    std::uint8_t kind = 0;          // DispatchKind raw
    std::int64_t userId = 0;
    std::int64_t omsOrderId = 0;
    std::int64_t oldOrderId = 0;
    std::int32_t marketId = 0;
    std::uint8_t orderType = 0;
    std::uint8_t side = 0;
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::int64_t budget = 0;
    std::uint8_t timeInForce = 0;
    std::uint8_t releaseScope = 0;  // ReleaseScope raw
    std::int64_t expectedOldRemaining = 0;

    bool operator==(const Dispatch &) const = default;
};

struct DurableCommand
{
    std::int64_t commandIdHigh = 0;
    std::int64_t commandIdLow = 0;
    std::int64_t userId = 0;
    std::int64_t omsOrderId = 0;
    std::int64_t oldOrderId = 0;
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::int64_t budget = 0;
    std::int32_t marketId = 0;
    std::uint8_t commandKind = 0;
    std::uint8_t orderType = 0;
    std::uint8_t orderSide = 0;
    std::uint8_t timeInForce = 0;
    std::int64_t expectedOldRemaining = 0;

    bool operator==(const DurableCommand &) const = default;
};

struct ReleaseEntry
{
    std::int64_t commandIdHigh = 0;
    std::int64_t idLowFloor = 0;
    std::uint8_t scope = 0;

    bool operator==(const ReleaseEntry &) const = default;
};

}}}

#endif
