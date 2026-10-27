#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_FIXTURES_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_FIXTURES_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/Dispatch.h"
#include "cluster_tools/domain/forwarder/DispatchRules.h"

// Valid (class P) dispatches as a v3 core mints them (S8 3.1, 3.6, 3.7): idHigh = omsOrderId = orderId,
// idLow = attempt << 2 | kind.

namespace cluster_tools { namespace domain { namespace forwarder { namespace fixtures {

inline Dispatch create(std::int64_t seq = 10, std::int64_t orderId = 101, std::int64_t attempt = 2)
{
    Dispatch d;
    d.dispatchSeq = seq;
    d.commandIdHigh = orderId;
    d.commandIdLow = (attempt << 2) | KIND_CREATE;
    d.kind = KIND_CREATE;
    d.userId = 7;
    d.omsOrderId = orderId;
    d.oldOrderId = 0;
    d.marketId = 3;
    d.orderType = 0;
    d.side = 0;
    d.price = 100;
    d.quantity = 5;
    d.budget = 0;
    d.timeInForce = 0;
    return d;
}

inline Dispatch cancel(std::int64_t seq = 11, std::int64_t orderId = 102, std::int64_t attempt = 2)
{
    Dispatch d;
    d.dispatchSeq = seq;
    d.commandIdHigh = orderId;
    d.commandIdLow = (attempt << 2) | KIND_CANCEL;
    d.kind = KIND_CANCEL;
    d.userId = 7;
    d.omsOrderId = orderId;
    d.oldOrderId = 55;
    d.marketId = 3;
    d.orderType = 0;
    d.side = 1;
    return d;
}

inline Dispatch amend(std::int64_t seq = 12, std::int64_t orderId = 103, std::int64_t attempt = 2)
{
    Dispatch d;
    d.dispatchSeq = seq;
    d.commandIdHigh = orderId;
    d.commandIdLow = (attempt << 2) | KIND_AMEND;
    d.kind = KIND_AMEND;
    d.userId = 7;
    d.omsOrderId = orderId;
    d.oldOrderId = 56;
    d.marketId = 3;
    d.orderType = 0;
    d.side = 0;
    d.price = 101;
    d.quantity = 4;
    d.expectedOldRemaining = 3;
    return d;
}

inline Dispatch release(std::int64_t seq = 13, std::int64_t orderId = 104, std::int64_t floor = (2 << 2) | 1,
    std::uint8_t scope = SCOPE_UP_TO)
{
    Dispatch d;
    d.dispatchSeq = seq;
    d.commandIdHigh = orderId;
    d.commandIdLow = floor;
    d.kind = KIND_RELEASE_ID;
    d.releaseScope = scope;
    return d;
}

inline Dispatch ofKind(std::uint8_t kind, std::int64_t seq)
{
    switch (kind)
    {
        case KIND_CREATE: return create(seq, 1000 + seq);
        case KIND_CANCEL: return cancel(seq, 1000 + seq);
        case KIND_AMEND: return amend(seq, 1000 + seq);
        default: return release(seq, 1000 + seq);
    }
}

}}}}

#endif
