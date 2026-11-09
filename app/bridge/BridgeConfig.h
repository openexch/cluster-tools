#ifndef CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_CONFIG_H
#define CLUSTER_TOOLS_APP_BRIDGE_BRIDGE_CONFIG_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cluster_tools/archive/ArchiveSettings.h"
#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/Credentials.h"

// BridgeConfig - the strict environment: every variable is
// required, a missing or malformed one refuses to start (exit 78, row C1), and parse() returns EVERY
// problem in the variable order of the table. The driver mode decides which driver keys are required
// (BRIDGE_DRIVER_MODE embedded|external, no default). BRIDGE_HALT_ON_GAP present is refused. Nothing
// else is read: every timer and capacity is a named constant in code. Pure: the environment and the file system are
// read through the two getters, so the tests need no process environment and WITH_AERON=OFF builds it.

namespace cluster_tools { namespace app { namespace bridge {

enum class DriverMode : std::uint8_t
{
    UNSET,
    EMBEDDED, // dev and demo only: the C media driver in this process, <BRIDGE_DRIVER_DIR>/ct-bridge-<id>
    EXTERNAL  // the prod default: the host aeronmd at AERON_DIR, managed by admin / cloud-console
};
const char *driverModeName(DriverMode mode) noexcept;

// Fixed in code: the core session's channels and the client constants.
inline constexpr const char *INGRESS_CHANNEL = "aeron:udp?term-length=4m|mtu=8192";
inline constexpr const char *INGRESS_CHANNEL_MTU1408 = "aeron:udp?term-length=4m"; // CONFIG_MTU_DEFAULT / nc_mtu1408
inline constexpr std::int32_t CLIENT_FACING_OFFSET = 2; // ClusterPorts.CLIENT_FACING_OFFSET
inline constexpr std::int64_t KEEPALIVE_INTERVAL_NS = 1'000'000'000LL;
inline constexpr std::int64_t MESSAGE_TIMEOUT_NS = 5'000'000'000LL;
inline constexpr int MAX_CONNECT_ATTEMPTS = 10;

struct BridgeConfig
{
    archive::ArchiveSettings archive;     // the keys (BRIDGE_ME_JOURNAL_ARCHIVES .. BRIDGE_JOURNAL_MTU)
    DriverMode driverMode = DriverMode::UNSET; // BRIDGE_DRIVER_MODE
    std::string aeronDir;                 // AERON_DIR (external only)
    std::string driverDir;                // BRIDGE_DRIVER_DIR (embedded only)
    std::int64_t driverTimeoutMs = 0;     // BRIDGE_DRIVER_TIMEOUT_MS (required, no default)
    std::vector<std::string> coreHosts;   // BRIDGE_AE_CLUSTER_ADDRESSES, member order
    std::int32_t corePortBase = 0;        // BRIDGE_AE_PORT_BASE
    std::int32_t portStride = 0;          // CLUSTER_PORT_STRIDE
    std::string ingressEndpoints;         // "0=h0:p0,1=h1:p1,..." (derived, ClusterPorts.port)
    std::string egressEndpoint;           // BRIDGE_AE_EGRESS_ENDPOINT host:port
    client::CoreCredentials credentials;  // AE_AUTH_CREDENTIAL keyId:BRIDGE:secretHex
    std::int64_t instanceId = 0;          // BRIDGE_INSTANCE_ID 1..INT64_MAX
    std::string httpHost;                 // BRIDGE_HTTP_BIND host:port
    std::uint16_t httpPort = 0;
};

struct ConfigResult
{
    BridgeConfig config;
    std::vector<std::string> errors; // every problem found, in the variable order

    bool ok() const { return errors.empty(); }
};

using EnvGetter = std::function<const char *(const char *name)>;
// The file-system checks of BRIDGE_DRIVER_DIR (directory + writable) and AERON_DIR (directory). Tests inject one.
struct DirProbe
{
    std::function<bool(const std::string &)> isDirectory;
    std::function<bool(const std::string &)> isWritable;
};
DirProbe systemDirProbe();

// The configuration variables in table order (the order parse() reports problems in).
const std::vector<std::string> &configKeys();

ConfigResult parseBridgeConfig(const EnvGetter &getenv, const DirProbe &probe = systemDirProbe());

// ClusterPorts.port(i, base, CLIENT_FACING_OFFSET) with the stride: base + i * stride + 2 (12.1 golden).
std::int64_t ingressPort(std::int32_t base, std::int32_t stride, std::size_t memberIndex) noexcept;
std::string ingressChannel();                               // INGRESS_CHANNEL (CONFIG_MTU_DEFAULT: without mtu=)
std::string responseChannel(const std::string &egressEndpoint); // aeron:udp?endpoint=<ep>|term-length=4m

// The core session's ClientSettings: keepalive 1 s, message timeout 5 s, 10 attempts, "ct-bridge".
client::ClientSettings coreClientSettings(const BridgeConfig &config);

}}}

#endif
