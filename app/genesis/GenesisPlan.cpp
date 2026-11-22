#include "genesis/GenesisPlan.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

#include "genesis/Json.h"

namespace cluster_tools { namespace app { namespace genesis {

namespace {

using json::Value;

std::string lower(const std::string &s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// A key that names InitTradeHighWater anywhere in the document (any case, any depth).
bool namesInitTradeHighWater(const Value &v)
{
    for (const auto &member : v.members)
    {
        if (lower(member.first).find("inittradehighwater") != std::string::npos || namesInitTradeHighWater(member.second))
        {
            return true;
        }
    }
    for (const Value &item : v.items)
    {
        if (namesInitTradeHighWater(item))
        {
            return true;
        }
    }
    return false;
}

// One object's fields: required/optional reads with range checks; unknown keys are reported by finish().
class Fields
{
public:
    Fields(const Value &object, std::string where, std::vector<std::string> &errors) :
        m_object(object), m_where(std::move(where)), m_errors(errors)
    {
    }

    bool isObject()
    {
        if (Value::Kind::OBJECT != m_object.kind)
        {
            error(m_where + " must be an object, got " + json::kindName(m_object.kind));
            return false;
        }
        return true;
    }

    const Value *take(const char *key, bool required)
    {
        m_known.insert(key);
        const Value *v = m_object.find(key);
        if (nullptr == v && required)
        {
            error(m_where + "." + key + " is required");
        }
        return v;
    }

    std::int64_t integer(const char *key, std::int64_t min, std::int64_t max, bool required = true,
        std::int64_t fallback = 0)
    {
        const Value *v = take(key, required);
        if (nullptr == v)
        {
            return fallback;
        }
        if (Value::Kind::INT != v->kind)
        {
            error(m_where + "." + key + " must be an integer, got " + json::kindName(v->kind));
            return fallback;
        }
        if (v->integer < min || v->integer > max)
        {
            error(m_where + "." + key + " must be in [" + std::to_string(min) + ", " + std::to_string(max) + "], got " +
                std::to_string(v->integer));
            return fallback;
        }
        return v->integer;
    }

    std::string text(const char *key)
    {
        const Value *v = take(key, true);
        if (nullptr == v)
        {
            return {};
        }
        if (Value::Kind::STRING != v->kind)
        {
            error(m_where + "." + key + " must be a string, got " + json::kindName(v->kind));
            return {};
        }
        return v->text;
    }

    const Value *array(const char *key, bool required = true)
    {
        const Value *v = take(key, required);
        if (nullptr == v)
        {
            return nullptr;
        }
        if (Value::Kind::ARRAY != v->kind)
        {
            error(m_where + "." + key + " must be an array, got " + json::kindName(v->kind));
            return nullptr;
        }
        return v;
    }

    const Value *object(const char *key, bool required = true)
    {
        const Value *v = take(key, required);
        if (nullptr == v)
        {
            return nullptr;
        }
        if (Value::Kind::OBJECT != v->kind)
        {
            error(m_where + "." + key + " must be an object, got " + json::kindName(v->kind));
            return nullptr;
        }
        return v;
    }

    void finish()
    {
        for (const auto &member : m_object.members)
        {
            if (0 == m_known.count(member.first))
            {
                error(m_where + " has an unknown key \"" + member.first + "\"");
            }
        }
    }

    void error(const std::string &text) { m_errors.push_back(text); }
    const std::string &where() const { return m_where; }

private:
    const Value &m_object;
    std::string m_where;
    std::vector<std::string> &m_errors;
    std::set<std::string> m_known;
};

constexpr std::int64_t I32_MAX = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t U32_MAX = std::numeric_limits<std::uint32_t>::max();
constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();

bool printableSymbol(const std::string &s, std::size_t maxLength)
{
    if (s.empty() || s.size() > maxLength)
    {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return c > 0x20 && c < 0x7f; });
}

void parseMe(const Value &v, GenesisPlan &plan, std::vector<std::string> &errors)
{
    Fields f(v, "me", errors);
    if (!f.isObject())
    {
        return;
    }
    domain::EngineConfig &me = plan.me;
    me.configVersion = f.integer("configVersion", 1, U32_MAX);
    const std::string impl = f.text("impl");
    me.impl = "array" == impl ? domain::EngineImpl::ARRAY : "direct" == impl ? domain::EngineImpl::DIRECT :
        domain::EngineImpl::UNKNOWN;
    if (domain::EngineImpl::UNKNOWN == me.impl && nullptr != v.find("impl"))
    {
        f.error("me.impl must be \"array\" or \"direct\", got \"" + impl + "\"");
    }
    me.bookCapacity = f.integer("bookCapacity", 0, U32_MAX);
    me.maxMatchesPerOrder = f.integer("maxMatchesPerOrder", 0, U32_MAX);
    me.maxOrdersPerLevel = f.integer("maxOrdersPerLevel", 0, U32_MAX);
    me.behaviorVersion = f.integer("behaviorVersion", ME_GENESIS_BEHAVIOR, ME_GENESIS_BEHAVIOR);
    me.commandLedgerCapacity = f.integer("commandLedgerCapacity", 1, U32_MAX);
    me.maxWireVersion = f.integer("maxWireVersion", ME_MAX_WIRE_VERSION, ME_MAX_WIRE_VERSION);
    if (nullptr != v.find("genesisId"))
    {
        f.take("genesisId", false);
        f.error("me.genesisId is not read: the document's top-level genesisId is the ME epoch (remove it)");
    }
    if (const Value *markets = f.array("markets"))
    {
        for (std::size_t i = 0; i < markets->items.size(); ++i)
        {
            Fields m(markets->items[i], "me.markets[" + std::to_string(i) + "]", errors);
            if (!m.isObject())
            {
                continue;
            }
            domain::MarketDef def;
            def.marketId = static_cast<std::int32_t>(m.integer("marketId", 1, I32_MAX));
            def.symbol = m.text("symbol");
            def.minPrice = m.integer("minPrice", 1, I64_MAX);
            def.maxPrice = m.integer("maxPrice", 1, I64_MAX);
            def.tickSize = m.integer("tickSize", 1, I64_MAX);
            m.finish();
            me.markets.push_back(def);
        }
    }
    f.finish();
}

void parseCore(const Value &v, GenesisPlan &plan, std::vector<std::string> &errors)
{
    Fields f(v, "core", errors);
    if (!f.isObject())
    {
        return;
    }
    if (const Value *markets = f.array("markets"))
    {
        for (std::size_t i = 0; i < markets->items.size(); ++i)
        {
            Fields m(markets->items[i], "core.markets[" + std::to_string(i) + "]", errors);
            if (!m.isObject())
            {
                continue;
            }
            CoreMarket row;
            row.marketId = static_cast<std::int32_t>(m.integer("marketId", 1, I32_MAX));
            row.symbol = m.text("symbol");
            if (!row.symbol.empty() && !printableSymbol(row.symbol, 16))
            {
                m.error(m.where() + ".symbol must be 1..16 printable ASCII characters, got \"" + row.symbol + "\"");
            }
            row.baseAssetId = static_cast<std::int32_t>(m.integer("baseAssetId", 0, I32_MAX));
            row.quoteAssetId = static_cast<std::int32_t>(m.integer("quoteAssetId", 0, I32_MAX));
            row.tickSize = m.integer("tickSize", 1, I64_MAX);
            row.minPrice = m.integer("minPrice", 1, I64_MAX);
            row.maxPrice = m.integer("maxPrice", 1, I64_MAX);
            row.lotSize = m.integer("lotSize", 0, I64_MAX);
            m.finish();
            plan.markets.push_back(row);
        }
    }
    if (const Value *assets = f.array("assets"))
    {
        for (std::size_t i = 0; i < assets->items.size(); ++i)
        {
            Fields a(assets->items[i], "core.assets[" + std::to_string(i) + "]", errors);
            if (!a.isObject())
            {
                continue;
            }
            CoreAsset row;
            row.assetId = static_cast<std::int32_t>(a.integer("assetId", 0, I32_MAX));
            row.code = a.text("code");
            if (!row.code.empty() && !printableSymbol(row.code, 8))
            {
                a.error(a.where() + ".code must be 1..8 printable ASCII characters, got \"" + row.code + "\"");
            }
            row.scale = static_cast<std::uint8_t>(a.integer("scale", 0, 18));
            a.finish();
            plan.assets.push_back(row);
        }
    }
    f.finish();
}

void parseRisk(const Value &v, GenesisPlan &plan, std::vector<std::string> &errors)
{
    Fields f(v, "risk", errors);
    if (!f.isObject())
    {
        return;
    }
    plan.riskConfigVersion = static_cast<std::uint32_t>(f.integer("configVersion", 1, U32_MAX));
    if (const Value *markets = f.array("markets"))
    {
        for (std::size_t i = 0; i < markets->items.size(); ++i)
        {
            Fields m(markets->items[i], "risk.markets[" + std::to_string(i) + "]", errors);
            if (!m.isObject())
            {
                continue;
            }
            RiskRow row;
            row.marketId = static_cast<std::int32_t>(m.integer("marketId", 1, I32_MAX));
            row.minQuantity = m.integer("minQuantity", 0, I64_MAX);
            row.maxQuantity = m.integer("maxQuantity", 0, I64_MAX);
            row.minNotional = m.integer("minNotional", 0, I64_MAX);
            row.maxNotional = m.integer("maxNotional", 0, I64_MAX);
            row.priceCollarBps = static_cast<std::uint32_t>(m.integer("priceCollarBps", 0, U32_MAX));
            row.maxOrdersPerSec = static_cast<std::int32_t>(m.integer("maxOrdersPerSec", 0, I32_MAX));
            row.maxOrdersPerMin = static_cast<std::int32_t>(m.integer("maxOrdersPerMin", 0, I32_MAX));
            row.maxOpenOrders = static_cast<std::int32_t>(m.integer("maxOpenOrders", 0, I32_MAX));
            row.maxPositionPerMarket = m.integer("maxPositionPerMarket", 0, I64_MAX);
            row.marketBuySlippageBps = static_cast<std::uint32_t>(m.integer("marketBuySlippageBps", 0, U32_MAX));
            m.finish();
            plan.risk.push_back(row);
        }
    }
    f.finish();
}

void parseLimits(const Value &v, GenesisPlan &plan, std::vector<std::string> &errors)
{
    Fields f(v, "limits", errors);
    if (!f.isObject())
    {
        return;
    }
    Limits &l = plan.limits;
    l.configVersion = static_cast<std::uint32_t>(f.integer("configVersion", 1, U32_MAX, false, DEFAULT_LIMITS_VERSION));
    l.requestTtlMs = f.integer("requestTtlMs", 1, I64_MAX, false, DEFAULT_REQUEST_TTL_MS);
    l.requestPerUserCap = static_cast<std::int32_t>(
        f.integer("requestPerUserCap", 1, I32_MAX, false, DEFAULT_REQUEST_PER_USER_CAP));
    l.orderEventRingCapacity = static_cast<std::int32_t>(
        f.integer("orderEventRingCapacity", 0, MAX_ORDER_EVENT_RING_CAPACITY, false, DEFAULT_ORDER_EVENT_RING_CAPACITY));
    f.finish();
}

void parseFunding(const Value &v, GenesisPlan &plan, std::vector<std::string> &errors)
{
    for (std::size_t i = 0; i < v.items.size(); ++i)
    {
        Fields d(v.items[i], "funding[" + std::to_string(i) + "]", errors);
        if (!d.isObject())
        {
            continue;
        }
        Funding row;
        row.userId = d.integer("userId", 1, I64_MAX);
        row.assetId = static_cast<std::int32_t>(d.integer("assetId", 0, I32_MAX));
        row.amount = d.integer("amount", 1, I64_MAX);
        row.requestIdHigh = d.integer("requestIdHigh", I64_MIN, I64_MAX);
        row.requestIdLow = d.integer("requestIdLow", I64_MIN, I64_MAX);
        d.finish();
        plan.funding.push_back(row);
    }
}

// Cross-field rules, after the shape is read.
void crossCheck(GenesisPlan &plan, std::vector<std::string> &errors)
{
    plan.me.genesisId = plan.genesisId;
    plan.me = domain::normalized(plan.me);
    if (const std::string why = domain::invalidReason(plan.me); !why.empty())
    {
        errors.push_back("me: " + why);
    }

    std::sort(plan.markets.begin(), plan.markets.end(),
        [](const CoreMarket &a, const CoreMarket &b) { return a.marketId < b.marketId; });
    std::sort(plan.assets.begin(), plan.assets.end(),
        [](const CoreAsset &a, const CoreAsset &b) { return a.assetId < b.assetId; });
    std::sort(plan.risk.begin(), plan.risk.end(), [](const RiskRow &a, const RiskRow &b) { return a.marketId < b.marketId; });

    if (plan.markets.empty())
    {
        errors.push_back("core.markets must hold at least one market");
    }
    if (plan.assets.empty())
    {
        errors.push_back("core.assets must hold at least one asset");
    }
    std::set<std::int32_t> assetIds;
    for (std::size_t i = 0; i < plan.assets.size(); ++i)
    {
        if (!assetIds.insert(plan.assets[i].assetId).second)
        {
            errors.push_back("core.assets: assetId " + std::to_string(plan.assets[i].assetId) + " appears twice");
        }
    }
    std::set<std::int32_t> marketIds;
    for (const CoreMarket &m : plan.markets)
    {
        const std::string name = "core market " + std::to_string(m.marketId);
        if (!marketIds.insert(m.marketId).second)
        {
            errors.push_back("core.markets: marketId " + std::to_string(m.marketId) + " appears twice");
        }
        if (0 == assetIds.count(m.baseAssetId) || 0 == assetIds.count(m.quoteAssetId))
        {
            errors.push_back(name + " names an asset that core.assets does not list");
        }
        if (m.baseAssetId == m.quoteAssetId)
        {
            errors.push_back(name + " has the same base and quote asset");
        }
        if (m.maxPrice <= m.minPrice)
        {
            errors.push_back(name + ": maxPrice must be above minPrice");
        }
    }
    for (const domain::MarketDef &def : plan.me.markets)
    {
        const auto it = std::find_if(plan.markets.begin(), plan.markets.end(),
            [&](const CoreMarket &m) { return m.marketId == def.marketId; });
        if (plan.markets.end() == it)
        {
            errors.push_back("ME market " + std::to_string(def.marketId) + " is not a core market");
        }
        else if (it->symbol != def.symbol)
        {
            errors.push_back("ME market " + std::to_string(def.marketId) + " is \"" + def.symbol + "\" at the ME and \"" +
                it->symbol + "\" at the core");
        }
    }
    if (plan.risk.empty())
    {
        errors.push_back("risk.markets must hold at least one row");
    }
    std::set<std::int32_t> riskIds;
    for (const RiskRow &r : plan.risk)
    {
        const std::string name = "risk row of market " + std::to_string(r.marketId);
        if (!riskIds.insert(r.marketId).second)
        {
            errors.push_back("risk.markets: marketId " + std::to_string(r.marketId) + " appears twice");
        }
        if (0 == marketIds.count(r.marketId))
        {
            errors.push_back(name + ": not a core market");
        }
        if (r.minQuantity > r.maxQuantity)
        {
            errors.push_back(name + ": minQuantity above maxQuantity");
        }
        if (r.minNotional > r.maxNotional)
        {
            errors.push_back(name + ": minNotional above maxNotional");
        }
    }
    std::set<std::pair<std::int64_t, std::int64_t>> requests;
    for (std::size_t i = 0; i < plan.funding.size(); ++i)
    {
        const Funding &d = plan.funding[i];
        const std::string name = "funding[" + std::to_string(i) + "]";
        if (0 == assetIds.count(d.assetId))
        {
            errors.push_back(name + ": assetId " + std::to_string(d.assetId) + " is not a core asset");
        }
        if (0 == d.requestIdHigh && 0 == d.requestIdLow)
        {
            errors.push_back(name + ": requestId 0/0 is MISSING_REQUEST_ID at the core");
        }
        if (!requests.insert({ d.requestIdHigh, d.requestIdLow }).second)
        {
            errors.push_back(name + ": requestId " + std::to_string(d.requestIdHigh) + "/" + std::to_string(d.requestIdLow) +
                " appears twice");
        }
    }
}

}

PlanResult parsePlan(const std::string &text)
{
    PlanResult result;
    json::ParseResult parsed = json::parse(text);
    if (!parsed.ok())
    {
        result.errors.push_back("the genesis input is not valid JSON: " + parsed.error);
        return result;
    }
    const Value &root = parsed.value;
    if (namesInitTradeHighWater(root))
    {
        result.errors.push_back(INIT_TRADE_HIGH_WATER_REFUSAL);
        return result;
    }
    std::vector<std::string> &errors = result.errors;
    GenesisPlan &plan = result.plan;
    Fields f(root, "the genesis input", errors);
    if (!f.isObject())
    {
        return result;
    }
    plan.genesisId = f.integer("genesisId", 1, I64_MAX);
    if (const Value *me = f.object("me"))
    {
        parseMe(*me, plan, errors);
    }
    if (const Value *core = f.object("core"))
    {
        parseCore(*core, plan, errors);
    }
    if (const Value *risk = f.object("risk"))
    {
        parseRisk(*risk, plan, errors);
    }
    if (const Value *limits = f.object("limits", false))
    {
        parseLimits(*limits, plan, errors);
    }
    if (const Value *funding = f.array("funding"))
    {
        parseFunding(*funding, plan, errors);
    }
    f.finish();
    if (errors.empty())
    {
        crossCheck(plan, errors);
    }
    return result;
}

}}}
