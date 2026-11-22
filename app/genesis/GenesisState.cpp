#include "genesis/GenesisState.h"

#include <algorithm>
#include <cstdio>

namespace cluster_tools { namespace app { namespace genesis {

const char *phaseName(Phase phase) noexcept
{
    switch (phase)
    {
        case Phase::ME_GENESIS: return "me_genesis";
        case Phase::ARM_MONEY_JOURNAL: return "arm_money_journal";
        case Phase::CORE_GENESIS: return "core_genesis";
        case Phase::CONFIGURE_RISK: return "configure_risk";
        case Phase::FUND: return "fund";
        case Phase::VERIFY_FUNDED: return "verify_funded";
        case Phase::ACTIVATE: return "activate";
    }
    return "?";
}

std::optional<Phase> phaseByName(const std::string &name)
{
    for (Phase p : ALL_PHASES)
    {
        if (name == phaseName(p))
        {
            return p;
        }
    }
    return std::nullopt;
}

const char *statusName(PhaseStatus status) noexcept
{
    switch (status)
    {
        case PhaseStatus::NOT_STARTED: return "NOT_STARTED";
        case PhaseStatus::PARTIAL: return "PARTIAL";
        case PhaseStatus::DONE: return "DONE";
        case PhaseStatus::FOREIGN: return "FOREIGN";
        case PhaseStatus::CHECK_PASSES: return "CHECK_PASSES";
        case PhaseStatus::CHECK_FAILS: return "CHECK_FAILS";
    }
    return "?";
}

const char *overallName(Overall overall) noexcept
{
    switch (overall)
    {
        case Overall::EMPTY: return "EMPTY";
        case Overall::PREFIX: return "PREFIX";
        case Overall::COMPLETE: return "COMPLETE";
        case Overall::NOT_A_PREFIX: return "NOT_A_PREFIX";
    }
    return "?";
}

std::map<std::pair<std::int64_t, std::int32_t>, std::int64_t> expectedBalances(const GenesisPlan &plan)
{
    std::map<std::pair<std::int64_t, std::int32_t>, std::int64_t> want;
    for (const Funding &d : plan.funding)
    {
        want[{ d.userId, d.assetId }] += d.amount;
    }
    return want;
}

namespace {

std::string str(std::int64_t v)
{
    return std::to_string(v);
}

bool genesisGate(const CoreObserved &c)
{
    return GATE_SUSPENDED == c.gateState && 0 != (c.gateReasons & GATE_REASON_GENESIS_BIT);
}

std::string gateText(const CoreObserved &c)
{
    return std::string(GATE_OPEN == c.gateState ? "OPEN" : "SUSPENDED") + " (reasons 0x" +
        [&] {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%x", c.gateReasons);
            return std::string(buf);
        }() + ")";
}

const MarketReport *reportOf(const CoreObserved &c, std::int32_t marketId)
{
    for (const MarketReport &m : c.markets)
    {
        if (m.marketId == marketId)
        {
            return &m;
        }
    }
    return nullptr;
}

std::vector<std::string> emptinessFacts(const Observed &o)
{
    const CoreObserved &c = o.core;
    std::vector<std::string> facts;
    if (0 != o.meEpoch) facts.push_back("the ME holds genesis " + str(o.meEpoch));
    if (c.journalEnabled) facts.push_back("the money journal is armed");
    if (0 != c.configVersion) facts.push_back("the core records config version " + str(c.configVersion));
    if (0 != c.genesisId) facts.push_back("the core records genesisId " + str(c.genesisId));
    if (!genesisGate(c)) facts.push_back("the global gate is " + gateText(c) + ", not SUSPENDED/GENESIS");
    if (0 != c.limitsVersion) facts.push_back("the core records limits version " + str(c.limitsVersion));
    for (const MarketReport &m : c.markets)
    {
        if (0 != m.riskConfigVersion)
        {
            facts.push_back("market " + str(m.marketId) + " has risk version " + str(m.riskConfigVersion));
        }
    }
    if (!c.balances.empty()) facts.push_back(str(static_cast<std::int64_t>(c.balances.size())) + " balance line(s)");
    if (0 != c.holds) facts.push_back(str(static_cast<std::int64_t>(c.holds)) + " hold(s)");
    if (0 != c.orders) facts.push_back(str(static_cast<std::int64_t>(c.orders)) + " order(s)");
    if (0 != c.lastAppliedTradeId) facts.push_back("last applied trade " + str(c.lastAppliedTradeId));
    if (0 != c.feedMeEpoch) facts.push_back("the core feed is bound to ME epoch " + str(c.feedMeEpoch));
    if (0 != c.lastAppliedJournalSeq) facts.push_back("the core applied ME journal record " + str(c.lastAppliedJournalSeq));
    if (c.feedHalted) facts.push_back("the core feed is halted (reason " + str(c.haltReason) + ")");
    return facts;
}

// Facts that no genesis prefix can hold (trading, a halt, another genesis): any of them makes a not-activated
// state NOT_A_PREFIX.
std::string foreignFact(const GenesisPlan &plan, const Observed &o)
{
    const CoreObserved &c = o.core;
    if (0 != c.lastAppliedTradeId) return "the core applied trade " + str(c.lastAppliedTradeId) + " during a genesis";
    if (0 != c.orders) return "the core holds " + str(static_cast<std::int64_t>(c.orders)) + " order(s) during a genesis";
    if (c.feedHalted) return "the core feed is halted (reason " + str(c.haltReason) + ")";
    if (0 != c.feedMeEpoch && plan.genesisId != c.feedMeEpoch)
        return "the core feed is bound to ME epoch " + str(c.feedMeEpoch) + ", the plan names " + str(plan.genesisId);
    if (c.lastAppliedJournalSeq > 1)
        return "the core applied ME journal record " + str(c.lastAppliedJournalSeq) + " (a genesis has only record 1)";
    return {};
}

PhaseLine meLine(const GenesisPlan &plan, const Observed &o)
{
    PhaseLine l{ Phase::ME_GENESIS, PhaseStatus::NOT_STARTED, "ME epoch 0" };
    if (o.meEpoch == plan.genesisId)
    {
        l.status = PhaseStatus::DONE;
        l.detail = "ME epoch " + str(o.meEpoch);
    }
    else if (0 != o.meEpoch)
    {
        l.status = PhaseStatus::FOREIGN;
        l.detail = "the ME holds genesis " + str(o.meEpoch) + ", the plan names " + str(plan.genesisId);
    }
    return l;
}

PhaseLine armLine(const Observed &o)
{
    return o.core.journalEnabled ? PhaseLine{ Phase::ARM_MONEY_JOURNAL, PhaseStatus::DONE, "journalEnabled" } :
        PhaseLine{ Phase::ARM_MONEY_JOURNAL, PhaseStatus::NOT_STARTED, "journal not armed" };
}

PhaseLine coreLine(const GenesisPlan &plan, const Observed &o, bool activated)
{
    const CoreObserved &c = o.core;
    PhaseLine l{ Phase::CORE_GENESIS, PhaseStatus::NOT_STARTED, "" };
    if (0 == c.configVersion)
    {
        if (genesisGate(c))
        {
            l.detail = "no CoreConfig recorded, gate SUSPENDED/GENESIS";
        }
        else
        {
            l.status = PhaseStatus::FOREIGN;
            l.detail = "no CoreConfig recorded but the global gate is " + gateText(c);
        }
        return l;
    }
    if (c.genesisId != plan.genesisId)
    {
        l.status = PhaseStatus::FOREIGN;
        l.detail = "the core records genesisId " + str(c.genesisId) + ", the plan names " + str(plan.genesisId);
        return l;
    }
    if (c.configVersion < CORE_LADDER_TOP)
    {
        l.status = PhaseStatus::PARTIAL;
        l.detail = "CoreConfig " + str(c.configVersion) + " of " + str(CORE_LADDER_TOP) + " recorded";
        return l;
    }
    if (activated || GATE_OPEN == c.gateState)
    {
        l.status = PhaseStatus::DONE;
        l.detail = "CoreConfig " + str(c.configVersion) + ", gate " + gateText(c);
        return l;
    }
    l.status = PhaseStatus::PARTIAL;
    l.detail = "CoreConfig " + str(c.configVersion) + " recorded, the global gate is " + gateText(c);
    return l;
}

PhaseLine riskLine(const GenesisPlan &plan, const Observed &o, bool activated)
{
    const CoreObserved &c = o.core;
    PhaseLine l{ Phase::CONFIGURE_RISK, PhaseStatus::NOT_STARTED, "" };
    std::size_t touched = 0;
    std::size_t matching = 0;
    std::string firstMismatch;
    for (const RiskRow &r : plan.risk)
    {
        const MarketReport *m = reportOf(c, r.marketId);
        const std::uint32_t version = nullptr == m ? 0 : m->riskConfigVersion;
        if (0 != version)
        {
            touched++;
        }
        const bool equal = nullptr != m && (activated ? m->riskConfigVersion >= plan.riskConfigVersion :
            (m->riskConfigVersion == plan.riskConfigVersion && m->minQuantity == r.minQuantity &&
                m->maxQuantity == r.maxQuantity && m->minNotional == r.minNotional && m->maxNotional == r.maxNotional));
        if (equal)
        {
            matching++;
        }
        else if (firstMismatch.empty())
        {
            firstMismatch = "market " + str(r.marketId) + " risk version " + str(version) + " (plan " +
                str(plan.riskConfigVersion) + ")";
        }
    }
    const Limits &want = plan.limits;
    const bool limitsTouched = 0 != c.limitsVersion;
    const bool limitsEqual = activated ? (c.limitsVersion >= want.configVersion) :
        (c.limitsVersion == want.configVersion && c.requestTtlMs == want.requestTtlMs &&
            c.requestPerUserCap == want.requestPerUserCap && c.orderEventRingCapacity == want.orderEventRingCapacity);
    const std::string limitsText = "limits version " + str(c.limitsVersion) + " requestPerUserCap " +
        str(c.requestPerUserCap) + " (plan " + str(want.configVersion) + ", " + str(want.requestPerUserCap) + ")";
    if (0 == touched && !limitsTouched)
    {
        l.detail = "no risk row and no limits record; " + limitsText;
        return l;
    }
    if (matching == plan.risk.size() && limitsEqual)
    {
        l.status = PhaseStatus::DONE;
        l.detail = str(static_cast<std::int64_t>(matching)) + " risk row(s); " + limitsText;
        return l;
    }
    l.status = PhaseStatus::PARTIAL;
    l.detail = str(static_cast<std::int64_t>(matching)) + " of " + str(static_cast<std::int64_t>(plan.risk.size())) +
        " risk row(s) as planned" + (firstMismatch.empty() ? "" : " (" + firstMismatch + ")") + "; " + limitsText;
    return l;
}

PhaseLine fundLine(const GenesisPlan &plan, const Observed &o, bool activated)
{
    const CoreObserved &c = o.core;
    if (activated)
    {
        return { Phase::FUND, PhaseStatus::DONE, "activated: balances are not judged after the activation" };
    }
    const auto want = expectedBalances(plan);
    if (c.balances.empty() && 0 == c.holds)
    {
        return { Phase::FUND, PhaseStatus::NOT_STARTED,
            "no balance (" + str(static_cast<std::int64_t>(plan.funding.size())) + " deposit(s) planned)" };
    }
    std::size_t matching = 0;
    std::string mismatch;
    for (const auto &[key, amount] : want)
    {
        const auto it = c.balances.find(key);
        if (c.balances.end() != it && it->second.first == amount && 0 == it->second.second)
        {
            matching++;
        }
        else if (mismatch.empty())
        {
            mismatch = "user " + str(key.first) + " asset " + str(key.second) + " holds " +
                (c.balances.end() == it ? std::string("nothing") :
                    str(it->second.first) + " available + " + str(it->second.second) + " locked") +
                ", the plan funds " + str(amount);
        }
    }
    std::size_t extra = 0;
    for (const auto &[key, value] : c.balances)
    {
        if (0 == want.count(key) && (0 != value.first || 0 != value.second))
        {
            extra++;
            if (mismatch.empty())
            {
                mismatch = "user " + str(key.first) + " asset " + str(key.second) + " is not in the funding table";
            }
        }
    }
    if (matching == want.size() && 0 == extra && 0 == c.holds)
    {
        return { Phase::FUND, PhaseStatus::DONE, str(static_cast<std::int64_t>(matching)) + " balance line(s) as planned" };
    }
    return { Phase::FUND, PhaseStatus::PARTIAL,
        str(static_cast<std::int64_t>(matching)) + " of " + str(static_cast<std::int64_t>(want.size())) +
            " balance line(s) as planned" + (mismatch.empty() ? "" : ": " + mismatch) +
            (0 == c.holds ? "" : "; " + str(static_cast<std::int64_t>(c.holds)) + " hold(s)") };
}

PhaseLine activateLine(const GenesisPlan &plan, const Observed &o, bool activated)
{
    const CoreObserved &c = o.core;
    const ActivationSeen &a = o.activation;
    if (activated)
    {
        return { Phase::ACTIVATE, PhaseStatus::DONE, "behaviour 4, activation record of genesis " + str(a.genesisId) };
    }
    if (c.configVersion > ACTIVATION_BEHAVIOR)
    {
        return { Phase::ACTIVATE, PhaseStatus::FOREIGN, "the core records config version " + str(c.configVersion) };
    }
    if (c.configVersion == ACTIVATION_BEHAVIOR)
    {
        if (c.behaviorVersion != ACTIVATION_BEHAVIOR || c.genesisId != plan.genesisId)
        {
            return { Phase::ACTIVATE, PhaseStatus::FOREIGN,
                "config version 4 at behaviour " + str(c.behaviorVersion) + ", genesisId " + str(c.genesisId) };
        }
        std::string why = !a.problem.empty() ? "the activation record is unreadable: " + a.problem :
            a.present ? "the activation record names genesis " + str(a.genesisId) : "no activation record";
        return { Phase::ACTIVATE, PhaseStatus::PARTIAL, "behaviour 4 recorded, " + why };
    }
    if (a.present && a.genesisId == plan.genesisId)
    {
        return { Phase::ACTIVATE, PhaseStatus::FOREIGN,
            "an activation record of genesis " + str(a.genesisId) + " exists but the core is at config " + str(c.configVersion) };
    }
    return { Phase::ACTIVATE, PhaseStatus::NOT_STARTED, "config version " + str(c.configVersion) };
}

std::size_t indexOf(Phase p)
{
    return static_cast<std::size_t>(p);
}

}

std::string fundedCheck(const GenesisPlan &plan, const Observed &o)
{
    const CoreObserved &c = o.core;
    if (!c.journalEnabled) return "the money journal is not armed";
    if (GATE_OPEN != c.gateState) return "the global gate is " + gateText(c);
    if (0 != c.holds) return str(static_cast<std::int64_t>(c.holds)) + " hold(s)";
    if (0 != c.orders) return str(static_cast<std::int64_t>(c.orders)) + " order(s)";
    const PhaseLine fund = fundLine(plan, o, false);
    if (PhaseStatus::DONE != fund.status) return "the balances are not the funding table: " + fund.detail;
    return {};
}

Assessment assess(const GenesisPlan &plan, const Observed &o)
{
    Assessment a;
    const CoreObserved &c = o.core;
    const bool activated = c.configVersion == ACTIVATION_BEHAVIOR && c.behaviorVersion == ACTIVATION_BEHAVIOR &&
        c.genesisId == plan.genesisId && o.activation.present && o.activation.genesisId == plan.genesisId &&
        o.meEpoch == plan.genesisId;

    a.lines.push_back(meLine(plan, o));
    a.lines.push_back(armLine(o));
    a.lines.push_back(coreLine(plan, o, activated));
    a.lines.push_back(riskLine(plan, o, activated));
    a.lines.push_back(fundLine(plan, o, activated));
    if (activated)
    {
        a.lines.push_back({ Phase::VERIFY_FUNDED, PhaseStatus::DONE, "activated" });
    }
    else
    {
        const std::string why = fundedCheck(plan, o);
        a.lines.push_back(why.empty() ? PhaseLine{ Phase::VERIFY_FUNDED, PhaseStatus::CHECK_PASSES, "check passes now" } :
            PhaseLine{ Phase::VERIFY_FUNDED, PhaseStatus::CHECK_FAILS, why });
    }
    a.lines.push_back(activateLine(plan, o, activated));

    a.emptinessFacts = emptinessFacts(o);
    if (a.emptinessFacts.empty())
    {
        a.overall = Overall::EMPTY;
        a.next = Phase::ME_GENESIS;
        return a;
    }
    a.detail = a.emptinessFacts.front();

    if (activated)
    {
        for (Phase p : { Phase::ME_GENESIS, Phase::ARM_MONEY_JOURNAL, Phase::CORE_GENESIS, Phase::CONFIGURE_RISK })
        {
            if (PhaseStatus::DONE != a.lines[indexOf(p)].status)
            {
                a.overall = Overall::NOT_A_PREFIX;
                a.detail = std::string(phaseName(p)) + " is " + statusName(a.lines[indexOf(p)].status) +
                    " under an activation record: " + a.lines[indexOf(p)].detail;
                return a;
            }
        }
        a.overall = Overall::COMPLETE;
        a.lastComplete = Phase::ACTIVATE;
        return a;
    }

    if (const std::string foreign = foreignFact(plan, o); !foreign.empty())
    {
        a.overall = Overall::NOT_A_PREFIX;
        a.detail = foreign;
        return a;
    }

    // The leading DONE run over the state-changing phases, then nothing started after it.
    const Phase stateful[] = { Phase::ME_GENESIS, Phase::ARM_MONEY_JOURNAL, Phase::CORE_GENESIS, Phase::CONFIGURE_RISK,
        Phase::FUND };
    std::size_t k = 0;
    while (k < 5 && PhaseStatus::DONE == a.lines[indexOf(stateful[k])].status)
    {
        k++;
    }
    for (std::size_t i = k; i < 5; ++i)
    {
        const PhaseLine &l = a.lines[indexOf(stateful[i])];
        if (PhaseStatus::NOT_STARTED != l.status)
        {
            a.overall = Overall::NOT_A_PREFIX;
            a.detail = std::string(phaseName(l.phase)) + " is " + statusName(l.status) + ": " + l.detail;
            if (i > k)
            {
                a.detail += " (before it, " + std::string(phaseName(stateful[k])) + " is " +
                    statusName(a.lines[indexOf(stateful[k])].status) + ")";
            }
            return a;
        }
    }
    const PhaseLine &activate = a.lines[indexOf(Phase::ACTIVATE)];
    if (k < 5)
    {
        if (PhaseStatus::NOT_STARTED != activate.status)
        {
            a.overall = Overall::NOT_A_PREFIX;
            a.detail = std::string("activate is ") + statusName(activate.status) + " before " + phaseName(stateful[k]) +
                ": " + activate.detail;
            return a;
        }
        a.overall = Overall::PREFIX;
        if (k > 0)
        {
            a.lastComplete = stateful[k - 1];
        }
        a.next = stateful[k];
        return a;
    }
    // fund is complete
    a.lastComplete = Phase::FUND;
    if (PhaseStatus::NOT_STARTED == activate.status)
    {
        a.overall = Overall::PREFIX;
        a.next = Phase::VERIFY_FUNDED;
        return a;
    }
    if (PhaseStatus::PARTIAL == activate.status && PhaseStatus::CHECK_PASSES == a.lines[indexOf(Phase::VERIFY_FUNDED)].status)
    {
        // behaviour 4 is recorded and the record is missing: the record is written from replicated state, so activate
        // continues (its CoreConfig is a DUPLICATE)
        a.overall = Overall::PREFIX;
        a.lastComplete = Phase::VERIFY_FUNDED;
        a.next = Phase::ACTIVATE;
        return a;
    }
    a.overall = Overall::NOT_A_PREFIX;
    a.detail = std::string("activate is ") + statusName(activate.status) + ": " + activate.detail;
    return a;
}

Refusal admit(Phase phase, const Assessment &a, bool emptinessCheck, bool prefixCheck)
{
    Refusal r;
    if (Phase::ME_GENESIS == phase)
    {
        if (emptinessCheck && Overall::EMPTY != a.overall)
        {
            r.line = NOT_EMPTY_REFUSAL;
            r.recovery = "what is there: " + a.detail + "; a genesis runs on an empty cluster only: wipe both clusters and "
                "redo (status lists the state)";
        }
        return r;
    }
    if (!prefixCheck)
    {
        return r;
    }
    const std::string name = phaseName(phase);
    const std::string wipe = "recovery: wipe both clusters and redo the genesis from me_genesis (nothing outside the "
        "clusters has seen it before the gate OPEN; after it, this is a new genesis)";
    switch (a.overall)
    {
        case Overall::NOT_A_PREFIX:
            r.line = std::string(NOT_A_PREFIX_REFUSAL) + ": " + a.detail;
            r.recovery = wipe;
            return r;
        case Overall::COMPLETE:
            r.line = std::string(NOT_A_PREFIX_REFUSAL) + ": the genesis is complete; " + name + " has nothing to do";
            r.recovery = "nothing to recover: verify reads the result";
            return r;
        case Overall::EMPTY:
            r.line = std::string(NOT_A_PREFIX_REFUSAL) + ": the cluster is empty; " + name + " needs me_genesis first";
            r.recovery = "recovery: run me_genesis";
            return r;
        case Overall::PREFIX:
            break;
    }
    const Phase next = *a.next;
    bool ok = next == phase;
    if (Phase::VERIFY_FUNDED == phase)
    {
        ok = Phase::VERIFY_FUNDED == next || Phase::ACTIVATE == next;
    }
    if (Phase::ACTIVATE == phase && Phase::VERIFY_FUNDED == next)
    {
        ok = PhaseStatus::CHECK_PASSES == a.lines[indexOf(Phase::VERIFY_FUNDED)].status;
        if (!ok)
        {
            r.line = std::string(NOT_A_PREFIX_REFUSAL) + ": activate needs verify_funded to pass: " +
                a.lines[indexOf(Phase::VERIFY_FUNDED)].detail;
            r.recovery = "recovery: run verify_funded and read its failure";
            return r;
        }
    }
    if (!ok)
    {
        const std::string done = a.lastComplete ? std::string("the last complete phase is ") + phaseName(*a.lastComplete) :
            std::string("no phase is complete");
        const bool ahead = indexOf(phase) < indexOf(next);
        r.line = std::string(NOT_A_PREFIX_REFUSAL) + ": " + name + (ahead ? " is already complete; " : " is not next; ") +
            done + ", the next phase is " + phaseName(next);
        r.recovery = std::string("recovery: run ") + phaseName(next);
    }
    return r;
}

}}}
