#ifndef CLUSTER_TOOLS_APP_SESSION_STUB_STUB_OPTIONS_H
#define CLUSTER_TOOLS_APP_SESSION_STUB_STUB_OPTIONS_H

#include <cstdint>
#include <string>
#include <vector>

// ct_session_stub's command line, parsed strictly: every problem is listed in one pass and the stub exits 78 (the
// configuration refusal), never starting with a guessed value. Every option is `--name value`.
//
// Required:
//   --driver-mode embedded|external
//   --instance-id <positive int64>      registration identity and the embedded driver dir suffix
//   --ingress-endpoints <id=host:port,...>
//   --ingress-channel <aeron:udp?...>   must name term-length and mtu
//   --response-channel <aeron:udp?...>  must name endpoint, term-length and mtu
//   --channels <mask>                   comma list of acks,balances,settlements,snapshots,orders,dispatch,control,
//                                       or a number (decimal or 0x hex) below 128
//   --driver-dir <parent dir>           embedded mode only (the driver dir is <parent>/ct-session-stub-<instance id>)
//   --aeron-dir <dir>                   external mode only (the host media driver's dir)
// The millisecond options and the script's sleep and await values are capped at one hour. That is an overflow guard
// for the nanosecond arithmetic against client::MAX_TIMEOUT_NS in a test-only program, not a limit on what a run may use.
// With defaults (a setting, never a limit in code):
//   --credential-env <NAME>             CORE_AUTH_CREDENTIAL; the key must be of class GATEWAY
//   --driver-timeout-ms <ms>            10000 (3000..60000, the runtime's driver timeout rule)
//   --poll-interval-ms <ms>             0: poll() every duty cycle; N > 0: at most once per N ms (the throttle)
//   --poll-fragments <n>                10: fragment limit of one poll()
//   --status-interval-ms <ms>           1000: one status line per interval; 0 = only on the status command
//   --reconnect on|off                  on: a closed or failed session is replaced by a new one after the delay
//   --reconnect-delay-ms <ms>           1000
//   --idle backoff|busy                 backoff

namespace cluster_tools { namespace app { namespace session_stub {

enum class DriverMode : std::uint8_t
{
    EMBEDDED,
    EXTERNAL,
};

struct StubOptions
{
    DriverMode driverMode = DriverMode::EMBEDDED;
    std::int64_t instanceId = 0;
    std::string ingressEndpoints;
    std::string ingressChannel;
    std::string responseChannel;
    std::uint32_t channels = 0;
    std::string driverDir;
    std::string aeronDir;
    std::string credentialEnv = "CORE_AUTH_CREDENTIAL";
    std::int64_t driverTimeoutMs = 10'000;
    std::int64_t pollIntervalMs = 0;
    int pollFragments = 10;
    std::int64_t statusIntervalMs = 1'000;
    bool reconnect = true;
    std::int64_t reconnectDelayMs = 1'000;
    bool busySpin = false;
};

struct OptionsResult
{
    StubOptions options;
    std::vector<std::string> errors; // empty = valid
    bool ok() const { return errors.empty(); }
};

OptionsResult parseStubOptions(const std::vector<std::string> &args);

// "acks,orders,control" or "0x51" or "81" -> the money-schema EgressChannels bits; false with `error` otherwise.
bool parseChannelMask(const std::string &text, std::uint32_t &mask, std::string &error);

// The names of the set bits, comma-separated, in bit order ("acks,orders,control").
std::string channelMaskNames(std::uint32_t mask);

// Empty = the channel names every parameter in `required` (keys of the aeron:udp?k=v|k=v query); otherwise the problem.
std::string missingChannelParameters(const std::string &key, const std::string &channel,
    const std::vector<std::string> &required);

}}}

#endif
