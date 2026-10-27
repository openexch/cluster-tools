// RC2 S12 contract 11.1 DispatchRulesTest: 5.4 classification, table driven, checked against the
// test-only port of the ME's own rules (MeRulesPort.h).
#include "doctest/doctest.h"

#include <functional>
#include <string>
#include <vector>

#include "Fixtures.h"
#include "MeRulesPort.h"

#include "cluster_tools/domain/forwarder/DispatchRules.h"

using namespace cluster_tools::domain::forwarder;
namespace fx = cluster_tools::domain::forwarder::fixtures;

namespace {

struct Row
{
    std::string name;
    Dispatch d;
    Classification want;
};

Row row(std::string name, Dispatch base, const std::function<void(Dispatch &)> &mutate, DispatchClass cls, RuleId rule)
{
    mutate(base);
    return Row{ std::move(name), base, Classification{ cls, rule } };
}

me_port::MeReaction meReactionOf(const Dispatch &d)
{
    return me_port::react(verbatimCommand(d));
}

}

TEST_CASE("DispatchRulesTest.passRowPerKindAndTif")
{
    std::vector<Row> rows;
    for (std::uint8_t tif : { 0, 1, 2 })
    {
        rows.push_back(row("CREATE LIMIT tif " + std::to_string(tif), fx::create(), [tif](Dispatch &d) { d.timeInForce = tif; },
            DispatchClass::PASS, RuleId::NONE));
    }
    for (std::uint8_t tif : { 0, 1 })
    {
        rows.push_back(row("CREATE MARKET tif " + std::to_string(tif), fx::create(), [tif](Dispatch &d) {
            d.orderType = 1;
            d.timeInForce = tif;
            d.budget = 999;
        }, DispatchClass::PASS, RuleId::NONE));
    }
    rows.push_back(row("CREATE LIMIT_MAKER tif 0", fx::create(), [](Dispatch &d) { d.orderType = 2; },
        DispatchClass::PASS, RuleId::NONE));
    for (std::uint8_t type : { 0, 1, 2 })
    {
        rows.push_back(row("CANCEL type " + std::to_string(type), fx::cancel(), [type](Dispatch &d) { d.orderType = type; },
            DispatchClass::PASS, RuleId::NONE));
    }
    rows.push_back(row("orphan CANCEL (S8 C4: price/qty/budget/tif/eor 0)", fx::cancel(), [](Dispatch &d) {
        d.price = 0;
        d.quantity = 0;
        d.budget = 0;
    }, DispatchClass::PASS, RuleId::NONE));
    for (std::uint8_t type : { 0, 2 })
    {
        rows.push_back(row("AMEND type " + std::to_string(type), fx::amend(), [type](Dispatch &d) { d.orderType = type; },
            DispatchClass::PASS, RuleId::NONE));
    }
    rows.push_back(row("RELEASE_ID UP_TO", fx::release(), [](Dispatch &) {}, DispatchClass::PASS, RuleId::NONE));
    rows.push_back(row("RELEASE_ID ORDER", fx::release(), [](Dispatch &d) {
        d.releaseScope = SCOPE_ORDER;
        d.commandIdLow = (4 << 2) | 3;
    }, DispatchClass::PASS, RuleId::NONE));
    rows.push_back(row("releaseScope non-zero on CANCEL is ignored", fx::cancel(), [](Dispatch &d) { d.releaseScope = 1; },
        DispatchClass::PASS, RuleId::NONE));
    rows.push_back(row("releaseScope 2 on CREATE is ignored", fx::create(), [](Dispatch &d) { d.releaseScope = 2; },
        DispatchClass::PASS, RuleId::NONE));
    rows.push_back(row("price/quantity/budget unchecked", fx::create(), [](Dispatch &d) {
        d.price = -5;
        d.quantity = 0;
        d.budget = -1;
    }, DispatchClass::PASS, RuleId::NONE));

    for (const Row &r : rows)
    {
        INFO(r.name);
        CHECK(classify(r.d) == r.want);
        if (r.d.kind != KIND_RELEASE_ID)
        {
            CHECK(meReactionOf(r.d) == me_port::MeReaction::JUDGED); // the ME judges what P sends
        }
    }
}

TEST_CASE("DispatchRulesTest.failingRowPerRuleAndKind")
{
    using C = DispatchClass;
    std::vector<Row> rows;
    const std::function<Dispatch()> bases[3] = { [] { return fx::create(); }, [] { return fx::cancel(); },
        [] { return fx::amend(); } };
    const char *names[3] = { "CREATE", "CANCEL", "AMEND" };
    for (int k = 0; k < 3; k++)
    {
        const std::string n = names[k];
        const Dispatch b = bases[k]();
        rows.push_back(row(n + " R1 both ids 0", b, [](Dispatch &d) {
            d.commandIdHigh = 0;
            d.commandIdLow = 0;
            d.omsOrderId = 1;
        }, C::UNROUTABLE, RuleId::R1));
        rows.push_back(row(n + " R2 userId 0", b, [](Dispatch &d) { d.userId = 0; }, C::CARRIER, RuleId::R2));
        rows.push_back(row(n + " R2 omsOrderId 0", b, [](Dispatch &d) { d.omsOrderId = 0; }, C::CARRIER, RuleId::R2));
        rows.push_back(row(n + " R2 marketId 0", b, [](Dispatch &d) { d.marketId = 0; }, C::CARRIER, RuleId::R2));
        rows.push_back(row(n + " R2 marketId -1", b, [](Dispatch &d) { d.marketId = -1; }, C::CARRIER, RuleId::R2));
        rows.push_back(row(n + " R2 orderType 3", b, [](Dispatch &d) { d.orderType = 3; }, C::CARRIER, RuleId::R2));
        rows.push_back(row(n + " R2 side 2", b, [](Dispatch &d) { d.side = 2; }, C::CARRIER, RuleId::R2));
        rows.push_back(row(n + " R3 old leg", b, [k](Dispatch &d) { d.oldOrderId = (0 == k) ? 5 : (1 == k ? 0 : -1); },
            C::CARRIER, RuleId::R3));
        rows.push_back(row(n + " R4 tif 3", b, [](Dispatch &d) { d.timeInForce = 3; }, C::CARRIER, RuleId::R4));
        rows.push_back(row(n + " R4 tif 255", b, [](Dispatch &d) { d.timeInForce = 255; }, C::CARRIER, RuleId::R4));
        rows.push_back(row(n + " R5 eor", b, [k](Dispatch &d) { d.expectedOldRemaining = (2 == k) ? -1 : 1; },
            C::CARRIER, RuleId::R5));
        rows.push_back(row(n + " B1 idHigh negative", b, [](Dispatch &d) { d.commandIdHigh = -d.commandIdHigh; },
            C::ME_INVALID, RuleId::B1));
        rows.push_back(row(n + " B1 idLow negative", b, [k](Dispatch &d) { d.commandIdLow = -8 + k; },
            C::ME_INVALID, RuleId::B1));
        rows.push_back(row(n + " B2 kind bits", b, [k](Dispatch &d) { d.commandIdLow = (2 << 2) | ((k + 1) % 4); },
            C::ME_INVALID, RuleId::B2));
        rows.push_back(row(n + " B3 owner", b, [](Dispatch &d) { d.omsOrderId = d.commandIdHigh + 1; },
            C::ME_INVALID, RuleId::B3));
        rows.push_back(row(n + " V1 seq 0", b, [](Dispatch &d) { d.dispatchSeq = 0; }, C::CARRIER, RuleId::V1));
        rows.push_back(row(n + " V1 seq -3", b, [](Dispatch &d) { d.dispatchSeq = -3; }, C::CARRIER, RuleId::V1));
    }
    rows.push_back(row("CREATE B4 MARKET FOK", fx::create(), [](Dispatch &d) {
        d.orderType = 1;
        d.timeInForce = 2;
    }, C::ME_INVALID, RuleId::B4));
    rows.push_back(row("CREATE B4 LIMIT_MAKER IOC", fx::create(), [](Dispatch &d) {
        d.orderType = 2;
        d.timeInForce = 1;
    }, C::ME_INVALID, RuleId::B4));
    rows.push_back(row("CREATE B4 LIMIT_MAKER FOK", fx::create(), [](Dispatch &d) {
        d.orderType = 2;
        d.timeInForce = 2;
    }, C::ME_INVALID, RuleId::B4));
    rows.push_back(row("CANCEL B4 tif 1", fx::cancel(), [](Dispatch &d) { d.timeInForce = 1; }, C::ME_INVALID, RuleId::B4));
    rows.push_back(row("AMEND B4 tif 2", fx::amend(), [](Dispatch &d) { d.timeInForce = 2; }, C::ME_INVALID, RuleId::B4));
    rows.push_back(row("AMEND B5 eor 0", fx::amend(), [](Dispatch &d) { d.expectedOldRemaining = 0; },
        C::ME_INVALID, RuleId::B5));
    rows.push_back(row("AMEND B6 MARKET", fx::amend(), [](Dispatch &d) { d.orderType = 1; }, C::ME_INVALID, RuleId::B6));

    rows.push_back(row("RELEASE X1 idHigh 0", fx::release(), [](Dispatch &d) { d.commandIdHigh = 0; },
        C::RELEASE_REFUSED, RuleId::X1));
    rows.push_back(row("RELEASE X1 both ids 0", fx::release(), [](Dispatch &d) {
        d.commandIdHigh = 0;
        d.commandIdLow = 0;
    }, C::RELEASE_REFUSED, RuleId::X1));
    rows.push_back(row("RELEASE X1 idLow -1", fx::release(), [](Dispatch &d) { d.commandIdLow = -1; },
        C::RELEASE_REFUSED, RuleId::X1));
    rows.push_back(row("RELEASE X2 scope 2", fx::release(), [](Dispatch &d) { d.releaseScope = 2; },
        C::RELEASE_REFUSED, RuleId::X2));
    const std::vector<std::pair<std::string, std::function<void(Dispatch &)>>> payload = {
        { "userId", [](Dispatch &d) { d.userId = 1; } },
        { "omsOrderId", [](Dispatch &d) { d.omsOrderId = 1; } },
        { "oldOrderId", [](Dispatch &d) { d.oldOrderId = 1; } },
        { "marketId", [](Dispatch &d) { d.marketId = 1; } },
        { "orderType", [](Dispatch &d) { d.orderType = 1; } },
        { "side", [](Dispatch &d) { d.side = 1; } },
        { "price", [](Dispatch &d) { d.price = 1; } },
        { "quantity", [](Dispatch &d) { d.quantity = 1; } },
        { "budget", [](Dispatch &d) { d.budget = 1; } },
        { "timeInForce", [](Dispatch &d) { d.timeInForce = 1; } },
        { "expectedOldRemaining", [](Dispatch &d) { d.expectedOldRemaining = 1; } },
    };
    for (const auto &[field, mutate] : payload)
    {
        rows.push_back(row("RELEASE A1 " + field, fx::release(), mutate, C::RELEASE_ANOMALY, RuleId::A1));
    }
    rows.push_back(row("RELEASE V1 seq 0", fx::release(), [](Dispatch &d) { d.dispatchSeq = 0; },
        C::RELEASE_ANOMALY, RuleId::V1));
    for (int kind : { 4, 5, 127, 255 })
    {
        rows.push_back(row("kind " + std::to_string(kind) + " V12", fx::create(),
            [kind](Dispatch &d) { d.kind = static_cast<std::uint8_t>(kind); }, C::CARRIER, RuleId::V12));
    }
    rows.push_back(row("kind 4 with both ids 0: V12 unroutable", fx::create(), [](Dispatch &d) {
        d.kind = 4;
        d.commandIdHigh = 0;
        d.commandIdLow = 0;
    }, C::UNROUTABLE, RuleId::V12));

    for (const Row &r : rows)
    {
        INFO(r.name);
        const Classification got = classify(r.d);
        CHECK(got == r.want);
        // The class claims what the ME would do with the dispatch sent verbatim; the port must agree.
        if (r.d.kind <= KIND_AMEND && RuleId::V1 != r.want.rule)
        {
            if (DispatchClass::ME_INVALID == r.want.cls)
            {
                CHECK(meReactionOf(r.d) == me_port::MeReaction::INVALID);
            }
            if (DispatchClass::CARRIER == r.want.cls || DispatchClass::UNROUTABLE == r.want.cls)
            {
                CHECK(meReactionOf(r.d) == me_port::MeReaction::DECODE_DROP);
            }
        }
    }
}

TEST_CASE("DispatchRulesTest.evaluationOrderReportsTheFirstFailingRule")
{
    Dispatch d = fx::create();
    d.userId = 0;                      // R2
    d.omsOrderId = d.commandIdHigh + 9; // B3
    CHECK(classify(d) == Classification{ DispatchClass::CARRIER, RuleId::R2 });

    d = fx::create();
    d.commandIdHigh = -101; // B1 (and B3, since omsOrderId stays 101)
    d.orderType = 2;
    d.timeInForce = 1; // B4
    CHECK(classify(d) == Classification{ DispatchClass::ME_INVALID, RuleId::B1 });

    d = fx::create();
    d.kind = 4;
    d.userId = 0;
    d.timeInForce = 9;
    d.dispatchSeq = 0;
    CHECK(classify(d) == Classification{ DispatchClass::CARRIER, RuleId::V12 });

    d = fx::amend();
    d.timeInForce = 1;              // B4
    d.expectedOldRemaining = 0;     // B5
    d.orderType = 1;                // B6
    CHECK(classify(d) == Classification{ DispatchClass::ME_INVALID, RuleId::B4 });

    d = fx::release();
    d.releaseScope = 2; // X2
    d.price = 5;        // A1
    d.dispatchSeq = 0;  // V1
    CHECK(classify(d) == Classification{ DispatchClass::RELEASE_REFUSED, RuleId::X2 });

    d = fx::cancel();
    d.dispatchSeq = -1;   // V1 (forwarder-local, last)
    d.timeInForce = 1;    // B4
    CHECK(classify(d) == Classification{ DispatchClass::ME_INVALID, RuleId::B4 });
}

TEST_CASE("DispatchRulesTest.labels")
{
    CHECK(std::string(actionOf(DispatchClass::ME_INVALID)) == "sent");
    CHECK(std::string(actionOf(DispatchClass::CARRIER)) == "carrier");
    CHECK(std::string(actionOf(DispatchClass::RELEASE_ANOMALY)) == "sent");
    CHECK(std::string(actionOf(DispatchClass::RELEASE_REFUSED)) == "isolated");
    CHECK(std::string(actionOf(DispatchClass::UNROUTABLE)) == "unroutable");
    CHECK(std::string(nameOf(RuleId::B6)) == "B6");
}
