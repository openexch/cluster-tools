#include "cluster_tools/domain/forwarder/DispatchRules.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

namespace cluster_tools { namespace domain { namespace forwarder {

namespace {

using test_seams::Rule;
using test_seams::disabled;

// Each check returns true when the rule HOLDS. A seamed test build can switch a rule off (it then
// holds whatever the dispatch says), which is how the negative controls prove each test row.

bool v12(const Dispatch &d) { return disabled(Rule::RULE_V12) || d.kind <= KIND_RELEASE_ID; }

bool r1(const Dispatch &d) { return disabled(Rule::RULE_R1) || !(0 == d.commandIdHigh && 0 == d.commandIdLow); }

bool r2(const Dispatch &d)
{
    return disabled(Rule::RULE_R2) ||
        (d.userId >= 1 && d.omsOrderId >= 1 && d.marketId >= 1 && d.orderType <= 2 && d.side <= 1);
}

bool r3(const Dispatch &d)
{
    return disabled(Rule::RULE_R3) || (KIND_CREATE == d.kind ? 0 == d.oldOrderId : d.oldOrderId >= 1);
}

bool r4(const Dispatch &d) { return disabled(Rule::RULE_R4) || d.timeInForce <= 2; }

bool r5(const Dispatch &d)
{
    return disabled(Rule::RULE_R5) ||
        (KIND_AMEND == d.kind ? d.expectedOldRemaining >= 0 : 0 == d.expectedOldRemaining);
}

bool b1(const Dispatch &d) { return disabled(Rule::RULE_B1) || (d.commandIdHigh >= 1 && d.commandIdLow >= 0); }

bool b2(const Dispatch &d) { return disabled(Rule::RULE_B2) || (d.commandIdLow & 3) == static_cast<std::int64_t>(d.kind); }

bool b3(const Dispatch &d) { return disabled(Rule::RULE_B3) || d.omsOrderId == d.commandIdHigh; }

bool b4(const Dispatch &d)
{
    if (disabled(Rule::RULE_B4))
    {
        return true;
    }
    if (KIND_CREATE != d.kind)
    {
        return 0 == d.timeInForce;
    }
    switch (d.orderType)
    {
        case 0: return d.timeInForce <= 2;  // LIMIT: GTC, IOC, FOK
        case 1: return d.timeInForce <= 1;  // MARKET: GTC, IOC
        default: return 0 == d.timeInForce; // LIMIT_MAKER: GTC
    }
}

bool b5(const Dispatch &d) { return disabled(Rule::RULE_B5) || KIND_AMEND != d.kind || d.expectedOldRemaining >= 1; }

bool b6(const Dispatch &d) { return disabled(Rule::RULE_B6) || KIND_AMEND != d.kind || 1 != d.orderType; }

bool v1(const Dispatch &d) { return disabled(Rule::RULE_V1) || d.dispatchSeq >= 1; }

bool x1(const Dispatch &d) { return disabled(Rule::RULE_X1) || (d.commandIdHigh >= 1 && d.commandIdLow >= 0); }

bool x2(const Dispatch &d) { return disabled(Rule::RULE_X2) || d.releaseScope <= SCOPE_ORDER; }

bool a1(const Dispatch &d)
{
    return disabled(Rule::RULE_A1) ||
        (0 == d.userId && 0 == d.omsOrderId && 0 == d.oldOrderId && 0 == d.marketId && 0 == d.orderType &&
            0 == d.side && 0 == d.price && 0 == d.quantity && 0 == d.budget && 0 == d.timeInForce &&
            0 == d.expectedOldRemaining);
}

Classification readerGroup(const Dispatch &d, bool &failed)
{
    failed = true;
    if (!r1(d)) return { DispatchClass::UNROUTABLE, RuleId::R1 };
    if (!r2(d)) return { DispatchClass::CARRIER, RuleId::R2 };
    if (!r3(d)) return { DispatchClass::CARRIER, RuleId::R3 };
    if (!r4(d)) return { DispatchClass::CARRIER, RuleId::R4 };
    if (!r5(d)) return { DispatchClass::CARRIER, RuleId::R5 };
    failed = false;
    return {};
}

Classification behaviourTwoGroup(const Dispatch &d, bool &failed)
{
    failed = true;
    if (!b1(d)) return { DispatchClass::ME_INVALID, RuleId::B1 };
    if (!b2(d)) return { DispatchClass::ME_INVALID, RuleId::B2 };
    if (!b3(d)) return { DispatchClass::ME_INVALID, RuleId::B3 };
    if (!b4(d)) return { DispatchClass::ME_INVALID, RuleId::B4 };
    if (!b5(d)) return { DispatchClass::ME_INVALID, RuleId::B5 };
    if (!b6(d)) return { DispatchClass::ME_INVALID, RuleId::B6 };
    failed = false;
    return {};
}

}

Classification classify(const Dispatch &d) noexcept
{
    if (!v12(d))
    {
        // A kind the schema does not name never reaches the ME as such: a carrier, unless the id itself
        // cannot be carried (both halves 0, the R1 case).
        const bool idZero = 0 == d.commandIdHigh && 0 == d.commandIdLow;
        return { idZero ? DispatchClass::UNROUTABLE : DispatchClass::CARRIER, RuleId::V12 };
    }
    if (KIND_RELEASE_ID == d.kind)
    {
        if (!x1(d)) return { DispatchClass::RELEASE_REFUSED, RuleId::X1 };
        if (!x2(d)) return { DispatchClass::RELEASE_REFUSED, RuleId::X2 };
        if (!a1(d)) return { DispatchClass::RELEASE_ANOMALY, RuleId::A1 };
        if (!v1(d)) return { DispatchClass::RELEASE_ANOMALY, RuleId::V1 };
        return {};
    }
    bool failed = false;
    if (disabled(Rule::RULE_ORDER_B_BEFORE_R))
    {
        const Classification b = behaviourTwoGroup(d, failed);
        if (failed) return b;
        const Classification r = readerGroup(d, failed);
        if (failed) return r;
    }
    else
    {
        const Classification r = readerGroup(d, failed);
        if (failed) return r;
        const Classification b = behaviourTwoGroup(d, failed);
        if (failed) return b;
    }
    if (!v1(d)) return { DispatchClass::CARRIER, RuleId::V1 };
    return {};
}

DurableCommand verbatimCommand(const Dispatch &d) noexcept
{
    DurableCommand c;
    c.commandIdHigh = d.commandIdHigh;
    c.commandIdLow = d.commandIdLow;
    c.userId = d.userId;
    c.omsOrderId = d.omsOrderId;
    c.oldOrderId = d.oldOrderId;
    c.price = d.price;
    c.quantity = d.quantity;
    c.budget = d.budget;
    c.marketId = d.marketId;
    c.commandKind = d.kind;
    c.orderType = d.orderType;
    c.orderSide = d.side;
    c.timeInForce = d.timeInForce;
    c.expectedOldRemaining = d.expectedOldRemaining;
    return c;
}

DurableCommand carrierCommand(const Dispatch &d) noexcept
{
    DurableCommand c;
    c.commandIdHigh = d.commandIdHigh;
    c.commandIdLow = d.commandIdLow;
    const std::int64_t lowBits = d.commandIdLow & 3; // 0..3 for every int64 (two's complement)
    const std::uint8_t kind = d.kind <= KIND_AMEND ? d.kind :
        (lowBits <= KIND_AMEND ? static_cast<std::uint8_t>(lowBits) : KIND_CREATE);
    c.commandKind = kind;
    c.userId = d.userId >= 1 ? d.userId : 1;
    c.marketId = d.marketId >= 1 ? d.marketId : 1;
    c.omsOrderId = d.omsOrderId >= 1 ? d.omsOrderId : (d.commandIdHigh >= 1 ? d.commandIdHigh : 1);
    c.oldOrderId = KIND_CREATE == kind ? 0 : (d.oldOrderId >= 1 ? d.oldOrderId : 1);
    c.orderSide = d.side <= 1 ? d.side : 0;
    c.price = d.price;
    c.quantity = d.quantity;
    c.budget = d.budget;
    c.orderType = 2;  // LIMIT_MAKER
    c.timeInForce = disabled(test_seams::Rule::CARRIER_TIF) ? (d.timeInForce <= 2 ? d.timeInForce : 0) : 1; // IOC
    c.expectedOldRemaining = KIND_AMEND == kind ? (d.expectedOldRemaining >= 0 ? d.expectedOldRemaining : 0) : 0;
    return c;
}

ReleaseEntry releaseEntry(const Dispatch &d) noexcept
{
    ReleaseEntry e;
    e.commandIdHigh = d.commandIdHigh;
    e.idLowFloor = d.commandIdLow;
    e.scope = d.releaseScope;
    return e;
}

bool toRingSlot(const Dispatch &d, const Classification &c, RingSlot &slot) noexcept
{
    slot.seq = d.dispatchSeq;
    // Negative-control binaries only (contract 11.4): ct_forwarder_nc_refuse discards classes I and C (the
    // pre-review rule), ct_forwarder_nc_norelease discards every RELEASE_ID entry.
    if (disabled(test_seams::Rule::AGENT_SEND_INVALID) &&
        (DispatchClass::ME_INVALID == c.cls || DispatchClass::CARRIER == c.cls))
    {
        return false;
    }
    if (disabled(test_seams::Rule::AGENT_RELEASE_RELAY) && KIND_RELEASE_ID == d.kind)
    {
        return false;
    }
    switch (c.cls)
    {
        case DispatchClass::UNROUTABLE:
            return false;
        case DispatchClass::CARRIER:
            slot.form = SlotForm::COMMAND;
            slot.command = disabled(test_seams::Rule::CARRIER) ? verbatimCommand(d) : carrierCommand(d);
            return true;
        case DispatchClass::RELEASE_REFUSED:
            slot.form = disabled(test_seams::Rule::RING_RX_ISOLATION) ? SlotForm::RELEASE : SlotForm::RELEASE_ISOLATED;
            slot.release = releaseEntry(d);
            return true;
        case DispatchClass::RELEASE_ANOMALY:
            slot.form = SlotForm::RELEASE;
            slot.release = releaseEntry(d);
            return true;
        case DispatchClass::PASS:
        case DispatchClass::ME_INVALID:
            if (KIND_RELEASE_ID == d.kind)
            {
                slot.form = SlotForm::RELEASE;
                slot.release = releaseEntry(d);
            }
            else
            {
                slot.form = SlotForm::COMMAND;
                slot.command = verbatimCommand(d);
            }
            return true;
    }
    return false;
}

const char *nameOf(RuleId rule) noexcept
{
    switch (rule)
    {
        case RuleId::NONE: return "none";
        case RuleId::V12: return "V12";
        case RuleId::R1: return "R1";
        case RuleId::R2: return "R2";
        case RuleId::R3: return "R3";
        case RuleId::R4: return "R4";
        case RuleId::R5: return "R5";
        case RuleId::B1: return "B1";
        case RuleId::B2: return "B2";
        case RuleId::B3: return "B3";
        case RuleId::B4: return "B4";
        case RuleId::B5: return "B5";
        case RuleId::B6: return "B6";
        case RuleId::V1: return "V1";
        case RuleId::X1: return "X1";
        case RuleId::X2: return "X2";
        case RuleId::A1: return "A1";
        case RuleId::COUNT: break;
    }
    return "?";
}

const char *nameOf(DispatchClass cls) noexcept
{
    switch (cls)
    {
        case DispatchClass::PASS: return "P";
        case DispatchClass::ME_INVALID: return "I";
        case DispatchClass::CARRIER: return "C";
        case DispatchClass::RELEASE_ANOMALY: return "RA";
        case DispatchClass::RELEASE_REFUSED: return "RX";
        case DispatchClass::UNROUTABLE: return "unroutable";
    }
    return "?";
}

const char *actionOf(DispatchClass cls) noexcept
{
    switch (cls)
    {
        case DispatchClass::PASS: return "";
        case DispatchClass::ME_INVALID: return "sent";
        case DispatchClass::CARRIER: return "carrier";
        case DispatchClass::RELEASE_ANOMALY: return "sent";
        case DispatchClass::RELEASE_REFUSED: return "isolated";
        case DispatchClass::UNROUTABLE: return "unroutable";
    }
    return "?";
}

}}}
