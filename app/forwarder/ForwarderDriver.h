#ifndef CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_DRIVER_H
#define CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_DRIVER_H

// ct-forwarder's embedded media driver: the shared app/common EmbeddedDriver (ct-bridge's,
// moved) bound to the forwarder's names: <FWD_DRIVER_DIR>/ct-forwarder-<FWD_INSTANCE_ID>, lock ct-forwarder-<id>.lock,
// and to the forwarder's app seams (EMBEDDED_DIR_PER_PID, EMBEDDED_INSTANCE_LOCK, EMBEDDED_LIVE_CHECK). Header-only: the
// seam flavour is the including target's (constexpr false in the production ct-forwarder).
//
// The verdict on a failed embedded start (the rule at start): a start that failed for storage
// space (the CnC cannot be allocated on a full tmpfs: startFailedForStorage()) is retried once a second with a new
// driver object, as long as that starvation lasts; STORAGE_STARVED_EXIT_NS (30 s) after the first such failure it exits
// 70 F_STORAGE_STARVED (restartable; a 78 would keep the forwarder down for good after a passing squeeze of a shared
// /dev/shm). Any other start failure, or a storage failure after another kind, is a 78 at once (host configuration).
#include <cstdint>
#include <string_view>

#include "common/EmbeddedDriver.h"
#include "common/StorageStarvation.h"

#include "AppSeams.h"

namespace cluster_tools { namespace app { namespace forwarder {

inline common::EmbeddedDriverOptions forwarderDriverOptions()
{
    common::EmbeddedDriverOptions o;
    o.processName = "ct-forwarder";
    o.dirPrefix = "ct-forwarder-";
    o.instanceKey = "FWD_INSTANCE_ID";
    o.dirPerPid = app_seams::disabled(app_seams::Rule::EMBEDDED_DIR_PER_PID);
    o.instanceLock = !app_seams::disabled(app_seams::Rule::EMBEDDED_INSTANCE_LOCK);
    o.liveCheck = !app_seams::disabled(app_seams::Rule::EMBEDDED_LIVE_CHECK);
    return o;
}

enum class DriverStartVerdict : std::uint8_t
{
    REFUSE_78,         // host configuration: the instance lock, a live driver, not a dir, socket limits, ...
    RETRY_STORAGE,     // storage-starved for less than 30 s: retry in 1 s with a new driver object
    EXIT_70_STORAGE    // storage-starved on every attempt for 30 s: F_STORAGE_STARVED, exit 70
};

// One failed start at `nowNs` (the owner's monotonic clock); `episode` spans the attempts of this process start.
inline DriverStartVerdict driverStartVerdict(bool failedForStorage, std::string_view why, std::int64_t nowNs,
    common::StorageStarvation &episode) noexcept
{
    if (!failedForStorage || app_seams::disabled(app_seams::Rule::DRIVER_START_STORAGE_78))
    {
        episode.onRelieved(nowNs);
        return DriverStartVerdict::REFUSE_78; // DRIVER_START_STORAGE_78 RED: the 78 for storage too
    }
    episode.onStarved(nowNs, why);
    return episode.expired(nowNs) ? DriverStartVerdict::EXIT_70_STORAGE : DriverStartVerdict::RETRY_STORAGE;
}

}}}

#endif
