#ifndef CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_CONFIG_H
#define CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_CONFIG_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/Credentials.h"

#include "cluster_tools/runtime/DirProbe.h"
#include "cluster_tools/runtime/IdleMode.h"

// ForwarderConfig - the strict environment: every variable is required, a
// missing or malformed one refuses to start (exit 78); both credentials must be of class FORWARDER;
// the two response channels must differ unless both use port 0. Everything else (ring, hold, page, timers,
// poll and drain limits, backoff) is a fixed constant in code. Pure: the environment is read
// through the getter, so the tests need no process environment and WITH_AERON=OFF builds it.
//
// The media driver is configurable (not negotiable), one rule with ct-bridge.
//   FWD_DRIVER_MODE       exactly embedded (dev and demo: the C media driver in this process) or external (prod: the
//                         host aeronmd that admin / cloud-console manage); no default.
//   AERON_DIR             external only (required there, refused with embedded): an existing directory at the parse.
//   FWD_DRIVER_DIR        embedded only (required there, refused with external): an absolute, existing, writable
//                         directory at the parse; the parent of the per-instance dir
//                         <FWD_DRIVER_DIR>/ct-forwarder-<FWD_INSTANCE_ID> and its lock ct-forwarder-<id>.lock.
//   FWD_DRIVER_TIMEOUT_MS the shared strict parser (runtime/include/cluster_tools/runtime/DriverTimeout.h, 3000..60000), given the raw value (no
//                         trimming: ' 5000' is refused, as BRIDGE_DRIVER_TIMEOUT_MS): the connect wait, the CnC
//                         liveness check and the conductor heartbeat threshold; replaces the former constant 5000.
// The directory checks go through a DirProbe (runtime/include/cluster_tools/runtime/DirProbe.h, ct-bridge's rule); tests inject one.
//   FWD_INSTANCE_ID       1..INT64_MAX, both modes: the embedded dir's id and the identity on BOTH cluster sessions
//                         (the core's RegisterSession.identity and the clientInfo of both connect requests), replacing
//                         the random identity per start (a breaking env change, accepted at beta).
// The idle mode, one vocabulary with every process of the stack (runtime/include/cluster_tools/runtime/IdleMode.h):
//   FWD_IDLE_MODE         busy_spin | backoff | sleep; unset = backoff (FORWARDER_IDLE, today's policy); any other value,
//                         an empty one included, refuses (78). The fwd-duty thread and, in embedded mode, the driver's
//                         agents follow it. FORWARDER_IDLE_MODE (not this repo's FWD_ prefix) is refused naming
//                         FWD_IDLE_MODE, so a renderer that writes that name is never silently ignored.

namespace cluster_tools { namespace app { namespace forwarder {

enum class DriverMode : std::uint8_t
{
    UNSET,
    EMBEDDED, // dev and demo only: the C media driver in this process, <FWD_DRIVER_DIR>/ct-forwarder-<id>
    EXTERNAL  // prod: the host aeronmd at AERON_DIR, managed by admin / cloud-console
};
const char *driverModeName(DriverMode mode) noexcept;

struct ForwarderConfig
{
    std::string coreIngress;          // FWD_CORE_INGRESS  id=host:port,...
    std::string coreResponseChannel;  // FWD_CORE_RESPONSE_CHANNEL
    client::CoreCredentials coreCredentials; // CORE_AUTH_CREDENTIAL keyId:FORWARDER:secretHex
    std::string meIngress;            // FWD_ME_INGRESS
    std::string meResponseChannel;    // FWD_ME_RESPONSE_CHANNEL
    client::CoreCredentials meCredentials;   // ME_AUTH_CREDENTIAL keyId:FORWARDER:secretHex (S6 11)
    std::string ingressChannel;       // FWD_INGRESS_CHANNEL base ingress channel for both clusters
    DriverMode driverMode = DriverMode::UNSET; // FWD_DRIVER_MODE
    std::string aeronDir;             // AERON_DIR (external only)
    std::string driverDir;            // FWD_DRIVER_DIR (embedded only)
    std::int64_t driverTimeoutMs = 0; // FWD_DRIVER_TIMEOUT_MS
    std::int64_t instanceId = 0;      // FWD_INSTANCE_ID
    runtime::IdleMode idleMode = runtime::IdleMode::BACKOFF; // FWD_IDLE_MODE (unset = backoff)
    std::string httpHost;             // FWD_HTTP_BIND host:port
    std::uint16_t httpPort = 0;
};

struct ConfigResult
{
    ForwarderConfig config;
    std::vector<std::string> errors; // every problem found, in variable order

    bool ok() const { return errors.empty(); }
};

using EnvGetter = std::function<const char *(const char *name)>;

ConfigResult parseForwarderConfig(const EnvGetter &getenv, const runtime::DirProbe &probe = runtime::systemDirProbe());

// The identity both cluster sessions carry: FWD_INSTANCE_ID (the IDENTITY_RANDOM RED: a random value per call).
std::int64_t forwarderIdentity(const ForwarderConfig &config);

// The fwd-duty thread's idle policy for the configured mode (backoff = runtime::FORWARDER_IDLE).
runtime::IdlePolicy forwarderIdlePolicy(const ForwarderConfig &config) noexcept;

// The ClientSettings of each session (the client defaults otherwise: keepalive 1 s, message timeout 5 s). The client
// name carries the identity into each connect request's clientInfo ("name=ct-forwarder-<cluster> identity=<id> ...").
client::ClientSettings coreClientSettings(const ForwarderConfig &config);
client::ClientSettings meClientSettings(const ForwarderConfig &config);

// The port of "aeron:udp?endpoint=host:port..." (-1 if absent or not numeric).
int endpointPort(const std::string &channel);
std::string endpointOf(const std::string &channel);

}}}

#endif
