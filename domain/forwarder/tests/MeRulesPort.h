#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_ME_RULES_PORT_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_ME_RULES_PORT_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/Dispatch.h"

// A verbatim C++ port of the ME's own rules, used ONLY as the tests' oracle (RC2 S12 11.1 CarrierTest):
// match-common com.match.domain.commands.DurableOrderIntent at 68a9375 (S6 merge), isValid (lines 29-38)
// and behaviourTwoCause (lines 54-99), plus the v12 decode ranges of SbeDemuxer (tif > 2, eor < 0).
// The production classification (DispatchRules) is written from the contract's table independently;
// the tests check the two agree, and the Java golden JAVA_DURABLE_ORDER_COMMAND_V12_CARRIER is checked in
// Java by the real isValid / behaviourTwoCause (GoldenMatch.java).

namespace cluster_tools { namespace domain { namespace forwarder { namespace me_port {

inline constexpr int B2_OK = 0;
inline constexpr int B2_INVALID_SIGN = 1;
inline constexpr int B2_INVALID_KIND_BITS = 2;
inline constexpr int B2_INVALID_OWNER_ID = 3;
inline constexpr int B2_INVALID_TIF = 4;
inline constexpr int B2_INVALID_AMEND_REFERENCE = 5;
inline constexpr int B2_INVALID_AMEND_TYPE = 6;

inline bool isValid(std::int64_t idHigh, std::int64_t idLow, std::int64_t userId, std::int64_t omsOrderId,
    std::int64_t oldOrderId, std::int32_t marketId, int kind, int type, int side, int timeInForce,
    std::int64_t expectedOldRemaining)
{
    return !((idHigh == 0 && idLow == 0) || userId <= 0 || omsOrderId <= 0 || marketId <= 0
        || kind < 0 || kind > 2 || type < 0 || type > 2 || side < 0 || side > 1
        || (kind == 0 ? oldOrderId != 0 : oldOrderId <= 0)
        || timeInForce < 0 || timeInForce > 2 || expectedOldRemaining < 0
        || (kind != 2 && expectedOldRemaining != 0));
}

inline int behaviourTwoCause(std::int64_t idHigh, std::int64_t idLow, std::int64_t omsOrderId, int kind)
{
    if (idHigh < 1 || idLow < 0)
    {
        return B2_INVALID_SIGN;
    }
    if ((idLow & 3) != kind)
    {
        return B2_INVALID_KIND_BITS;
    }
    if (omsOrderId != idHigh)
    {
        return B2_INVALID_OWNER_ID;
    }
    return B2_OK;
}

inline int behaviourTwoCause(std::int64_t idHigh, std::int64_t idLow, std::int64_t omsOrderId, int kind, int type,
    int timeInForce, std::int64_t expectedOldRemaining)
{
    const int s4 = behaviourTwoCause(idHigh, idLow, omsOrderId, kind);
    if (s4 != B2_OK)
    {
        return s4;
    }
    if ((timeInForce != 0 && kind != 0) || (type == 2 && timeInForce != 0) || (type == 1 && timeInForce == 2))
    {
        return B2_INVALID_TIF;
    }
    if (kind == 2 && expectedOldRemaining == 0)
    {
        return B2_INVALID_AMEND_REFERENCE;
    }
    if (kind == 2 && type == 1)
    {
        return B2_INVALID_AMEND_TYPE;
    }
    return B2_OK;
}

// What the ME does with a template-9 frame carrying c (v12 header, behaviour 2):
enum class MeReaction
{
    DECODE_DROP, // no outcome, no journal (SbeDemuxer range throw or isValid false)
    INVALID,     // journaled INVALID 8, not recorded
    JUDGED,      // reaches the ledger (APPLIED / CAPACITY / ... / dedupe)
};

inline MeReaction react(const DurableCommand &c)
{
    if (c.timeInForce > 2 || c.expectedOldRemaining < 0)
    {
        return MeReaction::DECODE_DROP;
    }
    if (!isValid(c.commandIdHigh, c.commandIdLow, c.userId, c.omsOrderId, c.oldOrderId, c.marketId, c.commandKind,
            c.orderType, c.orderSide, c.timeInForce, c.expectedOldRemaining))
    {
        return MeReaction::DECODE_DROP;
    }
    if (B2_OK != behaviourTwoCause(c.commandIdHigh, c.commandIdLow, c.omsOrderId, c.commandKind, c.orderType,
            c.timeInForce, c.expectedOldRemaining))
    {
        return MeReaction::INVALID;
    }
    return MeReaction::JUDGED;
}

}}}}

#endif
