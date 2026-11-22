#ifndef CLUSTER_TOOLS_APP_GENESIS_GENESIS_RUNNER_H
#define CLUSTER_TOOLS_APP_GENESIS_GENESIS_RUNNER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>

#include "genesis/GenesisPlan.h"
#include "genesis/GenesisSettings.h"
#include "genesis/GenesisState.h"
#include "genesis/GenesisWire.h"

// GenesisRunner - the verbs of ct-genesis over three cluster sessions: the core ADMIN session (config, gate, risk,
// limits, journal, every read), the core TOOL session (deposits, fund only) and the ME ADMIN session (EngineConfig and
// the ME epoch). Each verb reads the replicated state first, judges it (GenesisState) and refuses before it sends
// anything when the state is not the exact prefix the verb expects. Every phase re-reads the state after its commands
// and fails unless its own completion holds there: a phase is done when the clusters say so, not when the tool sent
// the frames.
//
// The sessions are behind Link, so the unit tests drive the runner against an in-process fake of both clusters and the
// binary against the real ones (ClusterLink.h).

namespace cluster_tools { namespace app { namespace genesis {

inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_FAILED = 1;  // a phase ran and did not complete, a read failed, or verify found it not done
inline constexpr int EXIT_REFUSED = 3; // refused before anything was sent (not empty, not the exact prefix)

class Link
{
public:
    using FrameHandler = std::function<bool(const std::uint8_t *data, std::size_t length)>;

    virtual ~Link() = default;
    // Connects within timeoutMs (a no-op when connected).
    virtual bool open(std::int64_t timeoutMs, std::string &error) = 0;
    // One message; back pressure is retried until timeoutMs.
    virtual bool send(const wire::Bytes &frame, std::int64_t timeoutMs, std::string &error) = 0;
    // Polls the session's egress, every message to onFrame, until onFrame returns true (true) or timeoutMs passes or
    // the session is lost (false, error). timeoutMs 0 drains what is there and returns false with no error.
    virtual bool pumpUntil(const FrameHandler &onFrame, std::int64_t timeoutMs, std::string &error) = 0;
    virtual void pause(std::int64_t ms) = 0;
    virtual void close() = 0;
};

class LinkFactory
{
public:
    virtual ~LinkFactory() = default;
    virtual std::unique_ptr<Link> coreAdmin() = 0;
    virtual std::unique_ptr<Link> coreTool() = 0;
    virtual std::unique_ptr<Link> meAdmin() = 0;
};

class ActivationStore
{
public:
    virtual ~ActivationStore() = default;
    virtual ActivationSeen read() = 0;
    virtual bool write(const std::string &text, std::string &error) = 0;
};

// The file named by GENESIS_ACTIVATION_RECORD: written to <path>.tmp, fsynced, renamed over <path>, the directory
// fsynced; read back with the strict JSON reader (its genesisId).
class FileActivationStore : public ActivationStore
{
public:
    explicit FileActivationStore(std::string path) : m_path(std::move(path)) {}
    ActivationSeen read() override;
    bool write(const std::string &text, std::string &error) override;

private:
    std::string m_path;
};

struct RunnerOptions
{
    Timeouts timeouts;
    std::string sourceSha = "unknown";
    // The negative-control switches (the seamed build reads them from seams::disabled()).
    bool emptinessCheck = true;
    bool prefixCheck = true;
    bool limitsStep = true;
    // Called after every command offered (ct_genesis_tc: exit after n). Never set in production.
    std::function<void()> afterSend;
    std::function<std::int64_t()> clockMs; // the activation record's time; default system clock
};

class GenesisRunner
{
public:
    GenesisRunner(const GenesisPlan &plan, LinkFactory &links, ActivationStore &store, RunnerOptions options,
        std::ostream &out);
    ~GenesisRunner();

    int runPhase(Phase phase);
    int status();
    int verify();

    // The state as read (the last read), for tests.
    bool readState(Observed &observed, std::string &error);

private:
    struct Sessions;

    int phaseMeGenesis();
    int phaseArmMoneyJournal();
    int phaseCoreGenesis();
    int phaseConfigureRisk();
    int phaseFund();
    int phaseVerifyFunded(const Observed &observed);
    int phaseActivate();

    Link *coreAdmin(std::string &error);
    Link *coreTool(std::string &error);
    Link *meAdmin(std::string &error);

    bool send(Link &link, const wire::Bytes &frame, std::string &error);
    bool adminCommand(const char *what, std::uint16_t refTemplateId, const wire::Bytes &frame, std::int64_t correlationId,
        wire::CoreFrame &ack, std::string &error);
    bool readMe(std::int64_t &meEpoch, std::int64_t timeoutMs, std::int64_t want, std::string &error);
    bool readCore(CoreObserved &core, std::string &error);
    bool coreRequest(const wire::Bytes &frame, std::int64_t correlationId, wire::CoreFrame::Kind endKind,
        const std::function<void(const wire::CoreFrame &)> &onRow, wire::CoreFrame &end, std::string &error);
    bool expectDone(Phase phase, std::string &error);
    std::string activationRecord(const Observed &observed) const;
    void printAssessment(const Assessment &a, const Observed &o);
    int fail(Phase phase, const std::string &why);
    std::int64_t nextCorrelation() { return m_correlation++; }

    GenesisPlan m_plan; // a copy: the LIMITS_STEP negative control rewrites its limits
    LinkFactory &m_links;
    ActivationStore &m_store;
    RunnerOptions m_options;
    std::ostream &m_out;
    std::unique_ptr<Sessions> m_sessions;
    std::int64_t m_correlation;
};

}}}

#endif
