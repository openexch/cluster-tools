#include "genesis/GenesisRunner.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "genesis/Json.h"

namespace cluster_tools { namespace app { namespace genesis {

using wire::CoreFrame;

namespace {

// The core's genesis limits record (CoreLimitsSet description: version 0, 900000 ms, 256, 65536). Only the
// LIMITS_STEP negative control judges against it.
constexpr Limits GENESIS_LIMITS_RECORD{ 0, 900'000, 256, 65'536 };

std::string str(std::int64_t v)
{
    return std::to_string(v);
}

bool adminOk(std::uint8_t result, bool adoptedOk)
{
    return wire::ADMIN_ACCEPTED == result || wire::ADMIN_DUPLICATE == result || (adoptedOk && wire::ADMIN_ADOPTED == result);
}

std::string ackText(const CoreFrame &ack)
{
    return std::string(wire::adminResultName(ack.result)) + " reason " + str(ack.reason) + " " +
        wire::coreReasonName(ack.reason);
}

std::int64_t systemClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string jsonString(const std::string &s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if ('"' == c || '\\' == c)
        {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

}

// ------------------------------------------------------------------ activation record file

ActivationSeen FileActivationStore::read()
{
    ActivationSeen seen;
    std::ifstream in(m_path, std::ios::binary);
    if (!in)
    {
        return seen; // absent
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    seen.present = true;
    const json::ParseResult parsed = json::parse(buffer.str());
    if (!parsed.ok())
    {
        seen.problem = m_path + ": " + parsed.error;
        seen.present = false;
        return seen;
    }
    const json::Value *id = parsed.value.find("genesisId");
    if (nullptr == id || json::Value::Kind::INT != id->kind)
    {
        seen.problem = m_path + ": no integer genesisId";
        seen.present = false;
        return seen;
    }
    seen.genesisId = id->integer;
    return seen;
}

bool FileActivationStore::write(const std::string &text, std::string &error)
{
    const std::string tmp = m_path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        error = "cannot create " + tmp;
        return false;
    }
    std::size_t done = 0;
    while (done < text.size())
    {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0)
        {
            ::close(fd);
            error = "cannot write " + tmp;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (0 != ::fsync(fd) || 0 != ::close(fd))
    {
        error = "cannot sync " + tmp;
        return false;
    }
    if (0 != ::rename(tmp.c_str(), m_path.c_str()))
    {
        error = "cannot rename " + tmp + " to " + m_path;
        return false;
    }
    const auto slash = m_path.rfind('/');
    const std::string dir = std::string::npos == slash ? "." : 0 == slash ? "/" : m_path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0)
    {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

// ------------------------------------------------------------------ runner

struct GenesisRunner::Sessions
{
    std::unique_ptr<Link> coreAdmin;
    std::unique_ptr<Link> coreTool;
    std::unique_ptr<Link> meAdmin;
};

GenesisRunner::GenesisRunner(const GenesisPlan &plan, LinkFactory &links, ActivationStore &store, RunnerOptions options,
    std::ostream &out) :
    m_plan(plan),
    m_links(links),
    m_store(store),
    m_options(std::move(options)),
    m_out(out),
    m_sessions(std::make_unique<Sessions>()),
    m_correlation(0x47E0'0000'0000LL + (systemClockMs() & 0xFFFF'FFFFLL))
{
    if (!m_options.limitsStep)
    {
        // LIMITS_STEP off (a test build only): the limits record is not part of the plan, so nothing judges it
        m_plan.limits = GENESIS_LIMITS_RECORD;
    }
    if (!m_options.clockMs)
    {
        m_options.clockMs = systemClockMs;
    }
}

GenesisRunner::~GenesisRunner()
{
    for (auto *s : { &m_sessions->coreAdmin, &m_sessions->coreTool, &m_sessions->meAdmin })
    {
        if (*s)
        {
            (*s)->close();
        }
    }
}

namespace {

Link *opened(std::unique_ptr<Link> &slot, const std::function<std::unique_ptr<Link>()> &make, std::int64_t timeoutMs,
    const char *name, std::string &error)
{
    if (!slot)
    {
        slot = make();
    }
    std::string why;
    if (!slot->open(timeoutMs, why))
    {
        error = std::string("the ") + name + " session did not connect: " + why;
        return nullptr;
    }
    return slot.get();
}

}

Link *GenesisRunner::coreAdmin(std::string &error)
{
    return opened(m_sessions->coreAdmin, [this] { return m_links.coreAdmin(); }, m_options.timeouts.connectMs,
        "core ADMIN", error);
}

Link *GenesisRunner::coreTool(std::string &error)
{
    return opened(m_sessions->coreTool, [this] { return m_links.coreTool(); }, m_options.timeouts.connectMs,
        "core TOOL", error);
}

Link *GenesisRunner::meAdmin(std::string &error)
{
    return opened(m_sessions->meAdmin, [this] { return m_links.meAdmin(); }, m_options.timeouts.connectMs, "ME ADMIN",
        error);
}

bool GenesisRunner::send(Link &link, const wire::Bytes &frame, std::string &error)
{
    if (!link.send(frame, m_options.timeouts.replyMs, error))
    {
        return false;
    }
    if (m_options.afterSend)
    {
        m_options.afterSend();
    }
    return true;
}

bool GenesisRunner::adminCommand(const char *what, std::uint16_t refTemplateId, const wire::Bytes &frame,
    std::int64_t correlationId, CoreFrame &ack, std::string &error)
{
    Link *admin = coreAdmin(error);
    if (nullptr == admin)
    {
        return false;
    }
    std::string why;
    admin->pumpUntil([](const std::uint8_t *, std::size_t) { return false; }, 0, why); // drop what is queued
    if (!send(*admin, frame, error))
    {
        error = std::string(what) + ": " + error;
        return false;
    }
    bool rejected = false;
    const bool got = admin->pumpUntil(
        [&](const std::uint8_t *data, std::size_t length) {
            CoreFrame f = wire::decodeCore(data, length);
            if (CoreFrame::Kind::ADMIN_ACK == f.kind && f.refTemplateId == refTemplateId && f.correlationId == correlationId)
            {
                ack = f;
                return true;
            }
            if (CoreFrame::Kind::ORDER_REJECT == f.kind && f.correlationId == correlationId)
            {
                ack = f;
                rejected = true;
                return true;
            }
            return false;
        },
        m_options.timeouts.replyMs, why);
    if (!got)
    {
        error = std::string(what) + ": no AdminAck within " + str(m_options.timeouts.replyMs) + " ms" +
            (why.empty() ? "" : " (" + why + ")");
        return false;
    }
    if (rejected)
    {
        error = std::string(what) + ": refused by the core (OrderReject reason " + str(ack.reason) + " " +
            wire::coreReasonName(ack.reason) + ")";
        return false;
    }
    return true;
}

bool GenesisRunner::readMe(std::int64_t &meEpoch, std::int64_t timeoutMs, std::int64_t want, std::string &error)
{
    Link *me = meAdmin(error);
    if (nullptr == me)
    {
        return false;
    }
    std::string why;
    const bool got = me->pumpUntil(
        [&](const std::uint8_t *data, std::size_t length) {
            const wire::MeFrame f = wire::decodeMe(data, length);
            if (wire::MeFrame::Kind::HEARTBEAT != f.kind)
            {
                return false;
            }
            meEpoch = f.meEpoch;
            return want < 0 || f.meEpoch == want;
        },
        timeoutMs, why);
    if (!got)
    {
        error = (want < 0 ? std::string("no ME heartbeat") : "no ME heartbeat with meEpoch " + str(want)) + " within " +
            str(timeoutMs) + " ms" + (why.empty() ? "" : " (" + why + ")");
        return false;
    }
    return true;
}

bool GenesisRunner::coreRequest(const wire::Bytes &frame, std::int64_t correlationId, CoreFrame::Kind endKind,
    const std::function<void(const CoreFrame &)> &onRow, CoreFrame &end, std::string &error)
{
    Link *admin = coreAdmin(error);
    if (nullptr == admin)
    {
        return false;
    }
    std::string why;
    admin->pumpUntil([](const std::uint8_t *, std::size_t) { return false; }, 0, why);
    if (!send(*admin, frame, error))
    {
        return false;
    }
    bool rejected = false;
    const bool got = admin->pumpUntil(
        [&](const std::uint8_t *data, std::size_t length) {
            CoreFrame f = wire::decodeCore(data, length);
            if (f.kind == endKind && f.correlationId == correlationId)
            {
                end = std::move(f);
                return true;
            }
            if (CoreFrame::Kind::ORDER_REJECT == f.kind && f.correlationId == correlationId)
            {
                end = std::move(f);
                rejected = true;
                return true;
            }
            if (onRow)
            {
                onRow(f);
            }
            return false;
        },
        m_options.timeouts.replyMs, why);
    if (!got)
    {
        error = "template " + str(wire::templateIdOf(frame.data(), frame.size())) + ": no reply within " +
            str(m_options.timeouts.replyMs) + " ms" + (why.empty() ? "" : " (" + why + ")");
        return false;
    }
    if (rejected)
    {
        error = "template " + str(wire::templateIdOf(frame.data(), frame.size())) + " refused by the core (reason " +
            str(end.reason) + " " + wire::coreReasonName(end.reason) + ")";
        return false;
    }
    return true;
}

bool GenesisRunner::readCore(CoreObserved &core, std::string &error)
{
    core = CoreObserved{};
    CoreFrame end;
    std::int64_t corr = nextCorrelation();
    if (!coreRequest(wire::queryFeedPosition(corr), corr, CoreFrame::Kind::FEED_POSITION, nullptr, end, error))
    {
        error = "QueryFeedPosition: " + error;
        return false;
    }
    core.journalEnabled = end.report.journalEnabled;
    core.lastAppliedTradeId = end.report.lastAppliedTradeId;
    core.feedMeEpoch = end.report.feedMeEpoch;
    core.lastAppliedJournalSeq = end.report.lastAppliedJournalSeq;
    core.feedHalted = end.report.feedHalted;
    core.haltReason = end.report.haltReason;

    corr = nextCorrelation();
    if (!coreRequest(wire::requestCoreConfig(corr), corr, CoreFrame::Kind::CORE_CONFIG_REPORT, nullptr, end, error))
    {
        error = "RequestCoreConfig: " + error;
        return false;
    }
    const CoreObserved &r = end.report;
    core.configVersion = r.configVersion;
    core.behaviorVersion = r.behaviorVersion;
    core.genesisId = r.genesisId;
    core.limitsVersion = r.limitsVersion;
    core.requestTtlMs = r.requestTtlMs;
    core.requestPerUserCap = r.requestPerUserCap;
    core.orderEventRingCapacity = r.orderEventRingCapacity;
    core.gateState = r.gateState;
    core.gateReasons = r.gateReasons;
    core.markets = r.markets;
    core.assetCount = r.assetCount;

    corr = nextCorrelation();
    if (!coreRequest(wire::requestBalanceSnapshot(corr), corr, CoreFrame::Kind::BALANCE_END,
            [&](const CoreFrame &f) {
                if (CoreFrame::Kind::BALANCE_ROWS == f.kind)
                {
                    for (const auto &b : f.balances)
                    {
                        core.balances[{ b.userId, b.assetId }] = { b.available, b.locked };
                    }
                }
            },
            end, error))
    {
        error = "RequestBalanceSnapshot: " + error;
        return false;
    }
    for (auto it = core.balances.begin(); it != core.balances.end();)
    {
        it = (0 == it->second.first && 0 == it->second.second) ? core.balances.erase(it) : std::next(it);
    }

    corr = nextCorrelation();
    if (!coreRequest(wire::requestHoldSnapshot(corr), corr, CoreFrame::Kind::HOLD_END, nullptr, end, error))
    {
        error = "RequestHoldSnapshot: " + error;
        return false;
    }
    core.holds = static_cast<std::size_t>(end.count < 0 ? 0 : end.count);

    corr = nextCorrelation();
    if (!coreRequest(wire::requestOrderSnapshot(corr, 0), corr, CoreFrame::Kind::ORDER_END, nullptr, end, error))
    {
        error = "RequestOrderSnapshot: " + error;
        return false;
    }
    core.orders = static_cast<std::size_t>(end.count < 0 ? 0 : end.count) + (0 != end.nextOrderId ? 1 : 0);
    return true;
}

bool GenesisRunner::readState(Observed &o, std::string &error)
{
    o = Observed{};
    if (!readMe(o.meEpoch, m_options.timeouts.meEpochMs, -1, error))
    {
        error = "ME: " + error;
        return false;
    }
    if (!readCore(o.core, error))
    {
        error = "core: " + error;
        return false;
    }
    o.activation = m_store.read();
    return true;
}

bool GenesisRunner::expectDone(Phase phase, std::string &error)
{
    Observed o;
    if (!readState(o, error))
    {
        error = "the state could not be read back: " + error;
        return false;
    }
    const Assessment a = assess(m_plan, o);
    const PhaseLine &line = a.lines[static_cast<std::size_t>(phase)];
    if (PhaseStatus::DONE != line.status)
    {
        error = std::string("the clusters do not show ") + phaseName(phase) + " complete: " + statusName(line.status) +
            ": " + line.detail;
        return false;
    }
    return true;
}

int GenesisRunner::fail(Phase phase, const std::string &why)
{
    m_out << "ct-genesis: " << phaseName(phase) << " FAILED: " << why << "\n";
    m_out << "ct-genesis: run status to see the state; a phase that did not complete is not a prefix (wipe and redo)\n";
    return EXIT_FAILED;
}

int GenesisRunner::runPhase(Phase phase)
{
    Observed o;
    std::string error;
    if (!readState(o, error))
    {
        m_out << "ct-genesis: " << phaseName(phase) << ": cannot read the clusters: " << error << "\n";
        return EXIT_FAILED;
    }
    const Assessment a = assess(m_plan, o);
    const Refusal refusal = admit(phase, a, m_options.emptinessCheck, m_options.prefixCheck);
    if (refusal.refused())
    {
        m_out << refusal.line << "\n" << refusal.recovery << "\n";
        return EXIT_REFUSED;
    }
    m_out << "ct-genesis: " << phaseName(phase) << " genesis " << m_plan.genesisId << " (state " << overallName(a.overall)
          << ")\n";
    int rc = EXIT_FAILED;
    switch (phase)
    {
        case Phase::ME_GENESIS: rc = phaseMeGenesis(); break;
        case Phase::ARM_MONEY_JOURNAL: rc = phaseArmMoneyJournal(); break;
        case Phase::CORE_GENESIS: rc = phaseCoreGenesis(); break;
        case Phase::CONFIGURE_RISK: rc = phaseConfigureRisk(); break;
        case Phase::FUND: rc = phaseFund(); break;
        case Phase::VERIFY_FUNDED: rc = phaseVerifyFunded(o); break;
        case Phase::ACTIVATE: rc = phaseActivate(); break;
    }
    if (EXIT_OK == rc)
    {
        m_out << "ct-genesis: " << phaseName(phase) << " DONE\n";
    }
    return rc;
}

int GenesisRunner::phaseMeGenesis()
{
    std::string error;
    const wire::Bytes frame = wire::engineConfig(m_plan, error);
    if (frame.empty())
    {
        return fail(Phase::ME_GENESIS, "the EngineConfig does not encode: " + error);
    }
    Link *me = meAdmin(error);
    if (nullptr == me || !send(*me, frame, error))
    {
        return fail(Phase::ME_GENESIS, "EngineConfig FRESH: " + error);
    }
    m_out << "ct-genesis: EngineConfig FRESH offered (behaviour " << m_plan.me.behaviorVersion << ", "
          << m_plan.me.markets.size() << " market(s), genesisId " << m_plan.genesisId << ")\n";
    std::int64_t epoch = 0;
    if (!readMe(epoch, m_options.timeouts.meEpochMs, m_plan.genesisId, error))
    {
        return fail(Phase::ME_GENESIS, error);
    }
    m_out << "ct-genesis: ME epoch " << epoch << "\n";
    if (!expectDone(Phase::ME_GENESIS, error))
    {
        return fail(Phase::ME_GENESIS, error);
    }
    return EXIT_OK;
}

int GenesisRunner::phaseArmMoneyJournal()
{
    std::string error;
    Link *admin = coreAdmin(error);
    if (nullptr == admin || !send(*admin, wire::setMoneyJournal(nextCorrelation(), true), error))
    {
        return fail(Phase::ARM_MONEY_JOURNAL, "SetMoneyJournal: " + error);
    }
    // SetMoneyJournal has no reply: the feed report is the evidence
    const std::int64_t polls = std::max<std::int64_t>(1, m_options.timeouts.journalArmMs / 100);
    for (std::int64_t i = 0; i < polls; ++i)
    {
        CoreFrame end;
        const std::int64_t corr = nextCorrelation();
        if (!coreRequest(wire::queryFeedPosition(corr), corr, CoreFrame::Kind::FEED_POSITION, nullptr, end, error))
        {
            return fail(Phase::ARM_MONEY_JOURNAL, "QueryFeedPosition: " + error);
        }
        if (end.report.journalEnabled)
        {
            if (!expectDone(Phase::ARM_MONEY_JOURNAL, error))
            {
                return fail(Phase::ARM_MONEY_JOURNAL, error);
            }
            m_out << "ct-genesis: money journal armed\n";
            return EXIT_OK;
        }
        admin->pause(100);
    }
    return fail(Phase::ARM_MONEY_JOURNAL, "the feed report does not show the money journal armed within " +
        str(m_options.timeouts.journalArmMs) + " ms");
}

int GenesisRunner::phaseCoreGenesis()
{
    std::string error;
    for (std::uint32_t v = 1; v <= CORE_LADDER_TOP; ++v)
    {
        CoreFrame ack;
        const std::int64_t corr = nextCorrelation();
        const std::string what = "CoreConfig " + str(v);
        if (!adminCommand(what.c_str(), wire::T_CORE_CONFIG, wire::coreConfig(corr, v, v, m_plan), corr, ack, error))
        {
            return fail(Phase::CORE_GENESIS, error);
        }
        m_out << "ct-genesis: " << what << " " << wire::adminResultName(ack.result) << "\n";
        if (!adminOk(ack.result, true))
        {
            return fail(Phase::CORE_GENESIS, what + " refused: " + ackText(ack));
        }
    }
    // The global gate opens only once a bridge's bind of the ME epoch is in the log: retried, bounded by the setting
    CoreFrame last;
    bool opened = false;
    std::int64_t tries = 0;
    Link *admin = coreAdmin(error);
    if (nullptr == admin)
    {
        return fail(Phase::CORE_GENESIS, error);
    }
    while (!opened && tries < m_options.timeouts.gateOpenRetries)
    {
        tries++;
        const std::int64_t corr = nextCorrelation();
        if (!adminCommand("OrderEntryGate OPEN", wire::T_ORDER_ENTRY_GATE, wire::orderEntryGateOpen(corr, 0), corr, last,
                error))
        {
            return fail(Phase::CORE_GENESIS, error);
        }
        opened = adminOk(last.result, false);
        if (!opened)
        {
            admin->pause(m_options.timeouts.gateOpenRetryIntervalMs);
        }
    }
    if (!opened)
    {
        return fail(Phase::CORE_GENESIS, "the ADMIN OPEN of the global gate was never accepted in " + str(tries) +
            " tries (GENESIS_GATE_OPEN_RETRIES): last " + ackText(last) + " (is a bridge running and bound to the ME?)");
    }
    m_out << "ct-genesis: global gate OPEN after " << tries << " tr" << (1 == tries ? "y" : "ies") << "\n";
    if (!expectDone(Phase::CORE_GENESIS, error))
    {
        return fail(Phase::CORE_GENESIS, error);
    }
    return EXIT_OK;
}

int GenesisRunner::phaseConfigureRisk()
{
    std::string error;
    for (const RiskRow &row : m_plan.risk)
    {
        CoreFrame ack;
        const std::int64_t corr = nextCorrelation();
        const std::string what = "RiskConfigSet market " + str(row.marketId);
        if (!adminCommand(what.c_str(), wire::T_RISK_CONFIG_SET, wire::riskConfigSet(corr, m_plan.riskConfigVersion, row),
                corr, ack, error))
        {
            return fail(Phase::CONFIGURE_RISK, error);
        }
        if (!adminOk(ack.result, false))
        {
            return fail(Phase::CONFIGURE_RISK, what + " refused: " + ackText(ack));
        }
    }
    m_out << "ct-genesis: " << m_plan.risk.size() << " risk row(s) at version " << m_plan.riskConfigVersion << "\n";
    if (m_options.limitsStep)
    {
        CoreFrame ack;
        const std::int64_t corr = nextCorrelation();
        if (!adminCommand("CoreLimitsSet", wire::T_CORE_LIMITS_SET, wire::coreLimitsSet(corr, m_plan.limits), corr, ack,
                error))
        {
            return fail(Phase::CONFIGURE_RISK, error);
        }
        if (!adminOk(ack.result, false))
        {
            return fail(Phase::CONFIGURE_RISK, "CoreLimitsSet refused: " + ackText(ack));
        }
        m_out << "ct-genesis: CoreLimitsSet version " << m_plan.limits.configVersion << " requestPerUserCap "
              << m_plan.limits.requestPerUserCap << " " << wire::adminResultName(ack.result) << "\n";
    }
    if (!expectDone(Phase::CONFIGURE_RISK, error))
    {
        return fail(Phase::CONFIGURE_RISK, error);
    }
    return EXIT_OK;
}

int GenesisRunner::phaseFund()
{
    std::string error;
    Link *tool = coreTool(error);
    if (nullptr == tool)
    {
        return fail(Phase::FUND, error);
    }
    std::size_t n = 0;
    for (const Funding &row : m_plan.funding)
    {
        n++;
        const std::int64_t corr = nextCorrelation();
        std::string why;
        tool->pumpUntil([](const std::uint8_t *, std::size_t) { return false; }, 0, why);
        if (!send(*tool, wire::deposit(corr, row), error))
        {
            return fail(Phase::FUND, "deposit " + str(static_cast<std::int64_t>(n)) + ": " + error);
        }
        CoreFrame reply;
        const bool got = tool->pumpUntil(
            [&](const std::uint8_t *data, std::size_t length) {
                CoreFrame f = wire::decodeCore(data, length);
                if ((CoreFrame::Kind::DEPOSIT_ACK == f.kind || CoreFrame::Kind::DEPOSIT_REJECT == f.kind ||
                        CoreFrame::Kind::ORDER_REJECT == f.kind) && f.correlationId == corr)
                {
                    reply = f;
                    return true;
                }
                return false;
            },
            m_options.timeouts.replyMs, why);
        if (!got)
        {
            return fail(Phase::FUND, "deposit " + str(static_cast<std::int64_t>(n)) + ": no DepositAck within " +
                str(m_options.timeouts.replyMs) + " ms" + (why.empty() ? "" : " (" + why + ")"));
        }
        if (CoreFrame::Kind::DEPOSIT_ACK != reply.kind)
        {
            return fail(Phase::FUND, "deposit " + str(static_cast<std::int64_t>(n)) + " (user " + str(row.userId) +
                " asset " + str(row.assetId) + ") rejected: reason " + str(reply.reason));
        }
    }
    m_out << "ct-genesis: " << n << " deposit(s) acknowledged\n";
    if (!expectDone(Phase::FUND, error))
    {
        return fail(Phase::FUND, error);
    }
    return EXIT_OK;
}

int GenesisRunner::phaseVerifyFunded(const Observed &o)
{
    const std::string why = fundedCheck(m_plan, o);
    if (!why.empty())
    {
        return fail(Phase::VERIFY_FUNDED, why);
    }
    m_out << "ct-genesis: " << o.core.balances.size() << " balance line(s) equal the funding table, no hold, no order, "
          << "journal armed, global gate OPEN\n";
    return EXIT_OK;
}

int GenesisRunner::phaseActivate()
{
    std::string error;
    Observed o;
    if (!readState(o, error))
    {
        return fail(Phase::ACTIVATE, error);
    }
    if (o.core.configVersion < ACTIVATION_BEHAVIOR)
    {
        CoreFrame ack;
        const std::int64_t corr = nextCorrelation();
        if (!adminCommand("CoreConfig 4", wire::T_CORE_CONFIG,
                wire::coreConfig(corr, ACTIVATION_BEHAVIOR, ACTIVATION_BEHAVIOR, m_plan), corr, ack, error))
        {
            return fail(Phase::ACTIVATE, error);
        }
        m_out << "ct-genesis: CoreConfig " << ACTIVATION_BEHAVIOR << " " << wire::adminResultName(ack.result) << "\n";
        if (!adminOk(ack.result, false))
        {
            return fail(Phase::ACTIVATE, "CoreConfig 4 refused: " + ackText(ack));
        }
        if (!readState(o, error))
        {
            return fail(Phase::ACTIVATE, error);
        }
    }
    if (o.core.configVersion != ACTIVATION_BEHAVIOR || o.core.behaviorVersion != ACTIVATION_BEHAVIOR ||
        o.core.genesisId != m_plan.genesisId)
    {
        return fail(Phase::ACTIVATE, "the core does not read behaviour 4 of genesis " + str(m_plan.genesisId) +
            ": config " + str(o.core.configVersion) + " behaviour " + str(o.core.behaviorVersion) + " genesisId " +
            str(o.core.genesisId));
    }
    if (const std::string why = fundedCheck(m_plan, o); !why.empty())
    {
        return fail(Phase::ACTIVATE, "the funded state does not hold at behaviour 4: " + why);
    }
    if (!m_store.write(activationRecord(o), error))
    {
        return fail(Phase::ACTIVATE, "the activation record: " + error);
    }
    if (!expectDone(Phase::ACTIVATE, error))
    {
        return fail(Phase::ACTIVATE, error);
    }
    m_out << "ct-genesis: activation record written for genesis " << m_plan.genesisId << "\n";
    return EXIT_OK;
}

std::string GenesisRunner::activationRecord(const Observed &o) const
{
    const CoreObserved &c = o.core;
    std::ostringstream j;
    j << "{\n"
      << "  \"schema\": \"ct-genesis-activation/1\",\n"
      << "  \"genesisId\": " << m_plan.genesisId << ",\n"
      << "  \"meEpoch\": " << o.meEpoch << ",\n"
      << "  \"core\": {\"configVersion\": " << c.configVersion << ", \"behaviorVersion\": " << c.behaviorVersion
      << ", \"genesisId\": " << c.genesisId << ", \"gate\": \"" << (GATE_OPEN == c.gateState ? "OPEN" : "SUSPENDED")
      << "\", \"journalEnabled\": " << (c.journalEnabled ? "true" : "false") << ", \"feedMeEpoch\": " << c.feedMeEpoch
      << "},\n"
      << "  \"limits\": {\"limitsVersion\": " << c.limitsVersion << ", \"requestTtlMs\": " << c.requestTtlMs
      << ", \"requestPerUserCap\": " << c.requestPerUserCap << ", \"orderEventRingCapacity\": " << c.orderEventRingCapacity
      << "},\n"
      << "  \"risk\": {\"configVersion\": " << m_plan.riskConfigVersion << ", \"markets\": " << m_plan.risk.size() << "},\n"
      << "  \"funding\": {\"deposits\": " << m_plan.funding.size() << ", \"balanceLines\": " << c.balances.size() << "},\n"
      << "  \"phases\": [\"me_genesis\", \"arm_money_journal\", \"core_genesis\", \"configure_risk\", \"fund\", "
         "\"verify_funded\", \"activate\"],\n"
      << "  \"writtenAtMs\": " << m_options.clockMs() << ",\n"
      << "  \"tool\": {\"name\": \"ct-genesis\", \"sourceSha\": " << jsonString(m_options.sourceSha) << "}\n"
      << "}\n";
    return j.str();
}

void GenesisRunner::printAssessment(const Assessment &a, const Observed &o)
{
    m_out << "genesis " << m_plan.genesisId << "\n";
    for (const PhaseLine &l : a.lines)
    {
        m_out << "  " << std::left << std::setw(19) << phaseName(l.phase) << std::setw(13) << statusName(l.status)
              << l.detail << "\n";
    }
    m_out << "limits: version " << o.core.limitsVersion << " requestPerUserCap " << o.core.requestPerUserCap
          << " requestTtlMs " << o.core.requestTtlMs << " orderEventRingCapacity " << o.core.orderEventRingCapacity << "\n";
    m_out << "config: version " << o.core.configVersion << " behaviour " << o.core.behaviorVersion << " genesisId "
          << o.core.genesisId << ", gate " << (GATE_OPEN == o.core.gateState ? "OPEN" : "SUSPENDED") << "\n";
    m_out << "activation record: "
          << (!o.activation.problem.empty() ? "unreadable: " + o.activation.problem :
              o.activation.present ? "present, genesis " + str(o.activation.genesisId) : std::string("absent"))
          << "\n";
    m_out << "last complete phase: " << (a.lastComplete ? phaseName(*a.lastComplete) : "none") << "\n";
    switch (a.overall)
    {
        case Overall::EMPTY:
            m_out << "state: EMPTY (next: me_genesis)\n";
            break;
        case Overall::PREFIX:
            m_out << "state: PREFIX, started, not done (next: " << phaseName(*a.next) << ")\n";
            break;
        case Overall::COMPLETE:
            m_out << "state: COMPLETE\n";
            break;
        case Overall::NOT_A_PREFIX:
            m_out << "state: NOT_A_PREFIX, started, not done: " << a.detail << "\n";
            m_out << "recovery: wipe both clusters and redo the genesis from me_genesis\n";
            break;
    }
}

int GenesisRunner::status()
{
    Observed o;
    std::string error;
    if (!readState(o, error))
    {
        m_out << "ct-genesis: status: cannot read the clusters: " << error << "\n";
        return EXIT_FAILED;
    }
    printAssessment(assess(m_plan, o), o);
    return EXIT_OK;
}

int GenesisRunner::verify()
{
    Observed o;
    std::string error;
    if (!readState(o, error))
    {
        m_out << "ct-genesis: verify: cannot read the clusters: " << error << "\n";
        return EXIT_FAILED;
    }
    const Assessment a = assess(m_plan, o);
    printAssessment(a, o);
    std::string why;
    if (Overall::COMPLETE != a.overall)
    {
        why = std::string("the state is ") + overallName(a.overall) + (a.detail.empty() ? "" : ": " + a.detail);
    }
    else if (GATE_OPEN != o.core.gateState)
    {
        why = "the global gate is not OPEN";
    }
    else if (o.core.requestPerUserCap != m_plan.limits.requestPerUserCap)
    {
        why = "the request cap is " + str(o.core.requestPerUserCap) + ", the plan sets " +
            str(m_plan.limits.requestPerUserCap);
    }
    if (!why.empty())
    {
        m_out << "verify: genesis " << m_plan.genesisId << " NOT DONE: " << why << "\n";
        return EXIT_FAILED;
    }
    m_out << "verify: genesis " << m_plan.genesisId << " DONE: every phase complete, behaviour 4, request cap "
          << o.core.requestPerUserCap << ", gate OPEN, activation record written\n";
    return EXIT_OK;
}

}}}
