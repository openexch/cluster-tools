#ifndef CLUSTER_TOOLS_APP_GENESIS_GENESIS_SETTINGS_H
#define CLUSTER_TOOLS_APP_GENESIS_GENESIS_SETTINGS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/Credentials.h"
#include "cluster_tools/runtime/DirProbe.h"

// ct-genesis settings, from the environment, strict: every problem is listed in one pass (exit 78). The plan is the
// input file named on the command line; these keys say how to reach the clusters and how long to wait.
//
//   GENESIS_DRIVER_MODE                embedded | external, REQUIRED (no default: a missing value refuses the start)
//   AERON_DIR                          external only, required there: the host aeronmd's directory
//   GENESIS_DRIVER_DIR                 embedded only, required there: the parent of the per-instance driver dir
//   GENESIS_INSTANCE_ID                embedded only, default 1: the embedded driver dir <parent>/ct-genesis-<id> and
//                                      its instance lock (a second genesis tool with the same id is refused)
//   GENESIS_DRIVER_TIMEOUT_MS          default 10000 (the Aeron default), 3000..60000
//   GENESIS_CORE_INGRESS               required, "id=host:port,..." (the core members)
//   GENESIS_ME_INGRESS                 required, "id=host:port,..." (the ME members)
//   GENESIS_RESPONSE_CHANNEL           required, e.g. aeron:udp?endpoint=10.0.0.5:0 (each session takes its own port)
//   GENESIS_INGRESS_CHANNEL            default aeron:udp (the client appends the member endpoint)
//   GENESIS_CORE_ADMIN_CREDENTIAL      required, keyId:ADMIN:secretHex (the core key table)
//   GENESIS_CORE_TOOL_CREDENTIAL       required by fund only, keyId:TOOL:secretHex
//   GENESIS_ME_ADMIN_CREDENTIAL        required, keyId:ADMIN:secretHex (the ME key table)
//   GENESIS_ACTIVATION_RECORD          required, the activation record file (activate writes it, status and verify read it)
//   GENESIS_CONNECT_TIMEOUT_MS         default 60000: one session connect (the fixture drivers' bound)
//   GENESIS_REPLY_TIMEOUT_MS           default 30000: one reply (AdminAck, DepositAck, a report, a snapshot end)
//   GENESIS_ME_EPOCH_TIMEOUT_MS        default 30000: the ME epoch on a heartbeat after the FRESH EngineConfig
//   GENESIS_GATE_OPEN_RETRIES          default 600 (the fixture's bound): ADMIN OPEN tries until the bridge bind is in
//                                      the log
//   GENESIS_GATE_OPEN_RETRY_INTERVAL_MS default 100: the pause between two OPEN tries
//   GENESIS_JOURNAL_ARM_TIMEOUT_MS     default 30000: SetMoneyJournal has no reply; the feed report must say armed

namespace cluster_tools { namespace app { namespace genesis {

enum class DriverMode : std::uint8_t
{
    UNSET,
    EMBEDDED,
    EXTERNAL,
};

const char *driverModeName(DriverMode mode) noexcept;

inline constexpr std::int64_t DEFAULT_DRIVER_TIMEOUT_MS = 10'000;
inline constexpr std::int64_t DEFAULT_INSTANCE_ID = 1;
inline constexpr std::int64_t DEFAULT_CONNECT_TIMEOUT_MS = 60'000;
inline constexpr std::int64_t DEFAULT_REPLY_TIMEOUT_MS = 30'000;
inline constexpr std::int64_t DEFAULT_ME_EPOCH_TIMEOUT_MS = 30'000;
inline constexpr std::int64_t DEFAULT_GATE_OPEN_RETRIES = 600;
inline constexpr std::int64_t DEFAULT_GATE_OPEN_RETRY_INTERVAL_MS = 100;
inline constexpr std::int64_t DEFAULT_JOURNAL_ARM_TIMEOUT_MS = 30'000;
inline constexpr const char *DEFAULT_INGRESS_CHANNEL = "aeron:udp";

// The waits of the phases (the runner reads only these).
struct Timeouts
{
    std::int64_t connectMs = DEFAULT_CONNECT_TIMEOUT_MS;
    std::int64_t replyMs = DEFAULT_REPLY_TIMEOUT_MS;
    std::int64_t meEpochMs = DEFAULT_ME_EPOCH_TIMEOUT_MS;
    std::int64_t gateOpenRetries = DEFAULT_GATE_OPEN_RETRIES;
    std::int64_t gateOpenRetryIntervalMs = DEFAULT_GATE_OPEN_RETRY_INTERVAL_MS;
    std::int64_t journalArmMs = DEFAULT_JOURNAL_ARM_TIMEOUT_MS;
};

struct GenesisSettings
{
    DriverMode driverMode = DriverMode::UNSET;
    std::string aeronDir;
    std::string driverDir;
    std::int64_t instanceId = DEFAULT_INSTANCE_ID;
    std::int64_t driverTimeoutMs = DEFAULT_DRIVER_TIMEOUT_MS;
    std::string coreIngress;
    std::string meIngress;
    std::string responseChannel;
    std::string ingressChannel = DEFAULT_INGRESS_CHANNEL;
    client::CoreCredentials coreAdmin;
    client::CoreCredentials coreTool;
    bool coreToolSet = false;
    client::CoreCredentials meAdmin;
    std::string activationRecord;
    Timeouts timeouts;
};

struct SettingsResult
{
    GenesisSettings settings;
    std::vector<std::string> errors;
    bool ok() const { return errors.empty(); }
};

using EnvGetter = std::function<const char *(const char *)>;

// requireTool: the verb sends deposits (fund), so the TOOL credential is required.
SettingsResult parseGenesisSettings(const EnvGetter &getenv, bool requireTool,
    const runtime::DirProbe &probe = runtime::systemDirProbe());

client::ClientSettings clientSettings(const GenesisSettings &s, const std::string &ingress,
    const client::CoreCredentials &credentials, const char *name);

}}}

#endif
