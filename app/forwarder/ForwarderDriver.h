#ifndef CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_DRIVER_H
#define CLUSTER_TOOLS_APP_FORWARDER_FORWARDER_DRIVER_H

// ct-forwarder's embedded media driver (RC2 S10' 8.2 rules 3-4): the shared app/common EmbeddedDriver (ct-bridge's,
// moved) bound to the forwarder's names: <FWD_DRIVER_DIR>/ct-forwarder-<FWD_INSTANCE_ID>, lock ct-forwarder-<id>.lock,
// and to the forwarder's app seams (EMBEDDED_DIR_PER_PID, EMBEDDED_INSTANCE_LOCK). Header-only: the seam flavour is the
// including target's (constexpr false in the production ct-forwarder).
#include "common/EmbeddedDriver.h"

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
    return o;
}

}}}

#endif
