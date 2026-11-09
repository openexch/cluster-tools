#ifndef CLUSTER_TOOLS_APP_BRIDGE_EMBEDDED_DRIVER_H
#define CLUSTER_TOOLS_APP_BRIDGE_EMBEDDED_DRIVER_H

#include <cstdint>
#include <string>
#include <utility>

#include "common/EmbeddedDriver.h"

#include "AppSeams.h"

// ct-bridge's embedded driver: the shared app/common EmbeddedDriver (moved there, one copy
// for ct-bridge and ct-forwarder) bound to the bridge's names (<BRIDGE_DRIVER_DIR>/ct-bridge-<BRIDGE_INSTANCE_ID>, lock
// ct-bridge-<id>.lock) and to the bridge's app seams. Header-only: the seam flavour is the including target's
// (constexpr false in the production ct-bridge), so the MainAeronTest cases and the EMBEDDED_* REDs run unchanged.

namespace cluster_tools { namespace app { namespace bridge {

inline common::EmbeddedDriverOptions bridgeDriverOptions()
{
    common::EmbeddedDriverOptions o;
    o.processName = "ct-bridge";
    o.dirPrefix = "ct-bridge-";
    o.instanceKey = "BRIDGE_INSTANCE_ID";
    o.dirPerPid = app_seams::disabled(app_seams::Rule::EMBEDDED_DIR_PER_PID);
    o.instanceLock = !app_seams::disabled(app_seams::Rule::EMBEDDED_INSTANCE_LOCK);
    return o;
}

class EmbeddedDriver : public common::EmbeddedDriver
{
public:
    static std::string dirFor(const std::string &parent, std::int64_t instanceId)
    {
        return common::EmbeddedDriver::dirFor(parent, instanceId, bridgeDriverOptions());
    }

    explicit EmbeddedDriver(HookAction onTerminate) : common::EmbeddedDriver(bridgeDriverOptions(), std::move(onTerminate)) {}
};

}}}

#endif
