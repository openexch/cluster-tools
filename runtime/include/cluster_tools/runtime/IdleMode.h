#ifndef CLUSTER_TOOLS_RUNTIME_IDLE_MODE_H
#define CLUSTER_TOOLS_RUNTIME_IDLE_MODE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cluster_tools/runtime/IdleStrategy.h"

// The idle mode: ONE vocabulary for every process of the stack (the operator profile's idle setting; the Java engines
// read the same words as TRANSPORT_IDLE_MODE):
//   busy_spin  the duty thread never yields or sleeps while idle; the embedded driver's agents busy-spin (Aeron "spin")
//   backoff    the process's own policy of today (ct-forwarder FORWARDER_IDLE, ct-bridge BRIDGE_IDLE); the embedded
//              driver keeps Aeron's default ("backoff") untouched: the default, it changes nothing
//   sleep      no spin, no yield: park 1 ms after an iteration without work (SleepingMillisIdleStrategy(1)); the
//              embedded driver's agents sleep 1 ms (Aeron "sleep-ns", init args "1ms")
// Each process reads its own key (FWD_IDLE_MODE, BRIDGE_IDLE_MODE) with parseIdleModeSetting: unset = backoff; any
// other value, an empty one included, must be one of the three words or the start is refused (78). A value a process
// does not know is never mapped to a nearby one.
//
// The external driver (aeronmd) takes the same mapping from deploy/media-driver/driver-idle-<mode>.properties, loaded
// after driver.properties and the profile overlay; the keys of DRIVER_IDLE_ROLES are the only keys those files may set
// and driverIdleFor() is the value each must carry (runtime.idle_mode checks the shipped files against it, and
// scripts/stage-driver-properties.sh refuses any other key).

namespace cluster_tools { namespace runtime {

enum class IdleMode : std::uint8_t
{
    BUSY_SPIN,
    BACKOFF,
    SLEEP
};

inline constexpr const char *IDLE_MODE_VALUES = "busy_spin, backoff or sleep";

inline const char *idleModeName(IdleMode mode) noexcept
{
    switch (mode)
    {
        case IdleMode::BUSY_SPIN: return "busy_spin";
        case IdleMode::BACKOFF: return "backoff";
        case IdleMode::SLEEP: return "sleep";
    }
    return "unknown";
}

// Exactly one of the three words (case-sensitive, no trimming: the caller trims as its other settings do).
inline std::optional<IdleMode> parseIdleMode(std::string_view text) noexcept
{
    if ("busy_spin" == text)
    {
        return IdleMode::BUSY_SPIN;
    }
    if ("backoff" == text)
    {
        return IdleMode::BACKOFF;
    }
    if ("sleep" == text)
    {
        return IdleMode::SLEEP;
    }
    return std::nullopt;
}

// A process's idle-mode key: `present` false = unset = backoff; otherwise `value` (already trimmed by the caller) must
// be one of the three words, or `error` names the key, the value and the allowed values (the caller refuses, 78).
inline std::optional<IdleMode> parseIdleModeSetting(const char *key, bool present, const std::string &value,
    std::string &error)
{
    if (!present)
    {
        return IdleMode::BACKOFF;
    }
    if (const auto mode = parseIdleMode(value))
    {
        return mode;
    }
    error = std::string(key) + " must be exactly " + IDLE_MODE_VALUES + " (unset = backoff), got '" + value + "'";
    return std::nullopt;
}

// The duty thread's policy for a mode; `backoff` is the process's own policy of today.
inline constexpr IdlePolicy idlePolicyFor(IdleMode mode, IdlePolicy backoff) noexcept
{
    switch (mode)
    {
        case IdleMode::BUSY_SPIN: return BUSY_SPIN_IDLE;
        case IdleMode::SLEEP: return SLEEP_IDLE;
        case IdleMode::BACKOFF: break;
    }
    return backoff;
}

// The Aeron C driver's idle strategy per mode: the names of aeron_idle_strategy_table (aeron-client/src/main/c/
// aeron_agent.c: "spin" aeron_idle_strategy_busy_spinning, "backoff", "sleep-ns" aeron_idle_strategy_sleeping, whose
// init args are an Aeron duration, parsed by aeron_parse_duration_ns). initArgs nullptr = none.
struct DriverIdleStrategy
{
    const char *strategy;
    const char *initArgs;
};

inline constexpr DriverIdleStrategy driverIdleFor(IdleMode mode) noexcept
{
    switch (mode)
    {
        case IdleMode::BUSY_SPIN: return { "spin", nullptr };
        case IdleMode::SLEEP: return { "sleep-ns", "1ms" };
        case IdleMode::BACKOFF: break;
    }
    return { "backoff", nullptr }; // Aeron's own default (aeron_driver_context_init): the embedded driver leaves it
}

// Every agent role of the driver (conductor, sender, receiver: DEDICATED; shared: SHARED; sharednetwork: the sender
// and receiver of SHARED_NETWORK). The property names (aeron_properties_setenv maps them to the AERON_* environment
// variables aeron_driver_context_init reads, aeronmd.h).
struct DriverIdleRole
{
    const char *strategyKey;
    const char *initArgsKey;
};

inline constexpr std::array<DriverIdleRole, 5> DRIVER_IDLE_ROLES = { {
    { "aeron.conductor.idle.strategy", "aeron.conductor.idle.strategy.init.args" },
    { "aeron.sender.idle.strategy", "aeron.sender.idle.strategy.init.args" },
    { "aeron.receiver.idle.strategy", "aeron.receiver.idle.strategy.init.args" },
    { "aeron.shared.idle.strategy", "aeron.shared.idle.strategy.init.args" },
    { "aeron.sharednetwork.idle.strategy", "aeron.sharednetwork.idle.strategy.init.args" },
} };

}}

#endif
