#ifndef CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_CONFIG_H
#define CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_CONFIG_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/Credentials.h"

// ForwarderConfig - the strict environment of RC2 S12 contract section 9: every variable is required, a
// missing or malformed one refuses to start (exit 78, F5); both credentials must be of class FORWARDER;
// the two response channels must differ unless both use port 0. Everything else (ring, hold, page, timers,
// poll and drain limits, backoff) is a fixed constant in code (D-S12-9). Pure: the environment is read
// through the getter, so the tests need no process environment and WITH_AERON=OFF builds it.

namespace cluster_tools { namespace app { namespace forwarder {

struct ForwarderConfig
{
    std::string coreIngress;          // FWD_CORE_INGRESS  id=host:port,...
    std::string coreResponseChannel;  // FWD_CORE_RESPONSE_CHANNEL
    client::CoreCredentials coreCredentials; // AE_AUTH_CREDENTIAL keyId:FORWARDER:secretHex
    std::string meIngress;            // FWD_ME_INGRESS
    std::string meResponseChannel;    // FWD_ME_RESPONSE_CHANNEL
    client::CoreCredentials meCredentials;   // ME_AUTH_CREDENTIAL keyId:FORWARDER:secretHex (S6 11)
    std::string ingressChannel;       // FWD_INGRESS_CHANNEL base ingress channel for both clusters
    std::string aeronDir;             // AERON_DIR
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

ConfigResult parseForwarderConfig(const EnvGetter &getenv);

// The ClientSettings of each session (the client defaults otherwise: keepalive 1 s, message timeout 5 s).
client::ClientSettings coreClientSettings(const ForwarderConfig &config);
client::ClientSettings meClientSettings(const ForwarderConfig &config);

// The port of "aeron:udp?endpoint=host:port..." (-1 if absent or not numeric).
int endpointPort(const std::string &channel);
std::string endpointOf(const std::string &channel);

}}}

#endif
