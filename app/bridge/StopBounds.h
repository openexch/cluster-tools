#ifndef CLUSTER_TOOLS_APP_BRIDGE_STOP_BOUNDS_H
#define CLUSTER_TOOLS_APP_BRIDGE_STOP_BOUNDS_H

#include <chrono>
#include <cstdint>

namespace cluster_tools { namespace app { namespace bridge {

// 3.4 / review FIX-3: the stop's join bound (the Supervisor's) and the archive message timeout it is built from; a stop
// older than their difference detaches its source without stopReplay (main.cpp asserts the archive timeout).
inline constexpr std::int64_t STOP_ARCHIVE_TIMEOUT_NS = 5'000'000'000LL;
inline constexpr std::int64_t STOP_JOIN_BOUND_NS = STOP_ARCHIVE_TIMEOUT_NS + 2'000'000'000LL;
inline constexpr std::int64_t STOP_DETACH_AGE_NS = STOP_JOIN_BOUND_NS - STOP_ARCHIVE_TIMEOUT_NS;

// The steady clock of common::Supervisor::monotonicNs (the signal time the duty thread passes in).
inline std::int64_t steadyNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}}}

#endif
