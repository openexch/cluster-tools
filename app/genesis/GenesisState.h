#ifndef CLUSTER_TOOLS_APP_GENESIS_GENESIS_STATE_H
#define CLUSTER_TOOLS_APP_GENESIS_GENESIS_STATE_H

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "genesis/GenesisPlan.h"

// What the genesis tool reads back from the two clusters, and the judgement made from it. Pure: no wire, no I/O.
//
// Every decision is taken on REPLICATED state read over a session (never on a log position: an idle core appends from
// its drain timer): the ME's recorded genesis identity (ClusterHeartbeat.meEpoch), and on the core the feed position
// report (money journal armed, the bound ME epoch, the applied cursors), the core config report (config and behaviour
// version, the genesisId, the limits record, the global gate, the per-market risk rows), the balance, hold and order
// snapshots. The activation record is the tool's own output file, read back by status and verify.
//
// Phases, in the cut's genesis order (the harness steps snapshot_genesis and start_tap_witness are not tool phases):
//   me_genesis         ME EngineConfig FRESH; done when the ME epoch equals the plan's genesisId
//   arm_money_journal  ADMIN SetMoneyJournal on; done when the feed report says journalEnabled
//   core_genesis       CoreConfig 1 -> 2 -> 3, then the ADMIN OPEN of the global gate (retried until the bridge's
//                      bind of the ME epoch is in the log); done at config >= 3 with the plan's genesisId and the gate OPEN
//   configure_risk     a RiskConfigSet per plan row, then CoreLimitsSet; done when every row and the limits record read
//                      back equal to the plan
//   fund               the TOOL deposits; done when the balances equal the funding table exactly and nothing is held
//   verify_funded      a check, not a state: the funded balances, no hold, no order, the journal armed, the gate OPEN
//   activate           CoreConfig at behaviour 4 with the gate open, then the activation record; done when the core
//                      reads behaviour 4 and the record names this genesisId
// A cluster is EMPTY when nothing above has happened and no money, order or feed fact exists. A verb runs only on the
// exact prefix its phase expects (the prefix rule); a partial or foreign state is refused, and its recovery is wipe and
// redo.

namespace cluster_tools { namespace app { namespace genesis {

inline constexpr const char *NOT_EMPTY_REFUSAL = "cluster is not empty: genesis refused";
inline constexpr const char *NOT_A_PREFIX_REFUSAL = "genesis state is not a prefix of the plan";

enum class Phase : std::uint8_t
{
    ME_GENESIS,
    ARM_MONEY_JOURNAL,
    CORE_GENESIS,
    CONFIGURE_RISK,
    FUND,
    VERIFY_FUNDED,
    ACTIVATE,
};

inline constexpr std::array<Phase, 7> ALL_PHASES = { Phase::ME_GENESIS, Phase::ARM_MONEY_JOURNAL, Phase::CORE_GENESIS,
    Phase::CONFIGURE_RISK, Phase::FUND, Phase::VERIFY_FUNDED, Phase::ACTIVATE };

const char *phaseName(Phase phase) noexcept;
std::optional<Phase> phaseByName(const std::string &name);

// GateState / GateReasons of the money schema (bit = GateReason value).
inline constexpr std::uint8_t GATE_OPEN = 0;
inline constexpr std::uint8_t GATE_SUSPENDED = 1;
inline constexpr std::uint32_t GATE_REASON_GENESIS_BIT = 1u << 5;

struct MarketReport
{
    std::int32_t marketId = 0;
    std::uint32_t riskConfigVersion = 0;
    std::int64_t minQuantity = 0;
    std::int64_t maxQuantity = 0;
    std::int64_t minNotional = 0;
    std::int64_t maxNotional = 0;
};

struct CoreObserved
{
    // FeedPositionReport (QueryFeedPosition)
    bool journalEnabled = false;
    std::int64_t lastAppliedTradeId = 0;
    std::int64_t feedMeEpoch = 0;
    std::int64_t lastAppliedJournalSeq = 0;
    bool feedHalted = false;
    std::uint8_t haltReason = 0;
    // CoreConfigReport (RequestCoreConfig)
    std::uint32_t configVersion = 0;
    std::uint32_t behaviorVersion = 0;
    std::int64_t genesisId = 0;
    std::uint32_t limitsVersion = 0;
    std::int64_t requestTtlMs = 0;
    std::int32_t requestPerUserCap = 0;
    std::int32_t orderEventRingCapacity = 0;
    std::uint8_t gateState = GATE_SUSPENDED;
    std::uint32_t gateReasons = 0;
    std::vector<MarketReport> markets;
    std::size_t assetCount = 0;
    // snapshots
    std::map<std::pair<std::int64_t, std::int32_t>, std::pair<std::int64_t, std::int64_t>> balances; // (user, asset) -> (available, locked)
    std::size_t holds = 0;
    std::size_t orders = 0;
};

struct ActivationSeen
{
    bool present = false;
    std::int64_t genesisId = 0;
    std::string problem; // a record that exists but cannot be read
};

struct Observed
{
    std::int64_t meEpoch = 0;
    CoreObserved core;
    ActivationSeen activation;
};

enum class PhaseStatus : std::uint8_t
{
    NOT_STARTED,
    PARTIAL,      // started, not complete: not a prefix
    DONE,
    FOREIGN,      // a fact of another genesis or of trading: not a prefix
    CHECK_PASSES, // verify_funded only
    CHECK_FAILS,  // verify_funded only
};

const char *statusName(PhaseStatus status) noexcept;

struct PhaseLine
{
    Phase phase = Phase::ME_GENESIS;
    PhaseStatus status = PhaseStatus::NOT_STARTED;
    std::string detail;
};

enum class Overall : std::uint8_t
{
    EMPTY,         // nothing of a genesis, nothing of trading
    PREFIX,        // an exact prefix: the next phase can run
    COMPLETE,      // every phase done and the activation record written
    NOT_A_PREFIX,  // started, not done, and not continuable: wipe and redo
};

const char *overallName(Overall overall) noexcept;

struct Assessment
{
    std::vector<PhaseLine> lines;            // one per phase, in order
    Overall overall = Overall::EMPTY;
    std::optional<Phase> lastComplete;       // the last DONE phase of the exact prefix (verify_funded is a check and
                                             // is named only when activate resumes after it)
    std::optional<Phase> next;               // the phase to run next (PREFIX only)
    std::string detail;                      // why NOT_A_PREFIX, or why not EMPTY
    std::vector<std::string> emptinessFacts; // every non-empty fact (empty list = EMPTY)
};

Assessment assess(const GenesisPlan &plan, const Observed &observed);

// The precondition of a verb. Empty = may run. Otherwise the refusal line (it starts with NOT_EMPTY_REFUSAL or
// NOT_A_PREFIX_REFUSAL) and the recovery line.
struct Refusal
{
    std::string line;
    std::string recovery;
    bool refused() const { return !line.empty(); }
};

// `prefixCheck` / `emptinessCheck` false only in a seamed test build (the negative controls).
Refusal admit(Phase phase, const Assessment &assessment, bool emptinessCheck = true, bool prefixCheck = true);

// The check of verify_funded on the observed state (empty = passes).
std::string fundedCheck(const GenesisPlan &plan, const Observed &observed);

// Expected balances of the funding table: (user, asset) -> total.
std::map<std::pair<std::int64_t, std::int32_t>, std::int64_t> expectedBalances(const GenesisPlan &plan);

}}}

#endif
