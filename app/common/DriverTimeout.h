#ifndef CLUSTER_TOOLS_APP_COMMON_DRIVER_TIMEOUT_H
#define CLUSTER_TOOLS_APP_COMMON_DRIVER_TIMEOUT_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>

// The media-driver connect timeout (RC2 S9c D-S9c-16 as decided by the coordinator on 2026-10-31): a STRICT
// environment key per process (ct-bridge: BRIDGE_DRIVER_TIMEOUT_MS; the ct-forwarder handoff: FWD_DRIVER_TIMEOUT_MS),
// missing or out of range = refuse to start (exit 78). It is the start-up connect wait of connectAeron AND the CnC
// liveness check from then on (aeron::Context mediaDriverTimeout: a dead or stale driver is a fatal conductor error,
// exit 70, F1), and the threshold of the client-conductor heartbeat check (ConductorWatch, F3b). Admin and
// cloud-console set it; 5000 is the value the drills and the S12 forwarder use. One rule for both processes.

namespace cluster_tools { namespace app { namespace common {

// The media driver stamps its CnC heartbeat once per timer interval (aeron_driver_conductor_do_work; 1 s, the C and Java
// drivers' default, pinned by the embedded driver). A timeout at that cadence reads a live driver as dead on every
// interval, and one a little above it on any driver stall under load (review FIX-2, 2026-11-01): the floor is 3 s, and
// never below two intervals.
inline constexpr std::int64_t DRIVER_TIMER_INTERVAL_MS = 1'000;
inline constexpr std::int64_t DRIVER_TIMEOUT_MIN_MS = 3'000;
inline constexpr std::int64_t DRIVER_TIMEOUT_MAX_MS = 60'000;
static_assert(DRIVER_TIMEOUT_MIN_MS >= 2 * DRIVER_TIMER_INTERVAL_MS, "the driver timeout floor must span two heartbeats");

// The value of `key` ("3000".."60000", decimal, no sign, no unit), or nullopt with `error` naming the key.
inline std::optional<std::int64_t> parseDriverTimeoutMs(const char *key, const char *raw, std::string &error)
{
    const std::string text = nullptr == raw ? std::string() : std::string(raw);
    if (text.empty())
    {
        error = std::string(key) + " is required (no default): the media driver timeout in ms, " +
            std::to_string(DRIVER_TIMEOUT_MIN_MS) + ".." + std::to_string(DRIVER_TIMEOUT_MAX_MS);
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char *first = text.data();
    const char *last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc() || parsed.ptr != last || '+' == text.front() || '-' == text.front() ||
        value < DRIVER_TIMEOUT_MIN_MS || value > DRIVER_TIMEOUT_MAX_MS)
    {
        error = std::string(key) + " must be an integer in " + std::to_string(DRIVER_TIMEOUT_MIN_MS) + ".." +
            std::to_string(DRIVER_TIMEOUT_MAX_MS) + " (ms; the floor spans two driver heartbeats of " +
            std::to_string(DRIVER_TIMER_INTERVAL_MS) + " ms), got '" + text + "'";
        return std::nullopt;
    }
    return value;
}

}}}

#endif
