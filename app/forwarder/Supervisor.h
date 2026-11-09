#ifndef CLUSTER_TOOLS_APP_FORWARDER_SUPERVISOR_H
#define CLUSTER_TOOLS_APP_FORWARDER_SUPERVISOR_H

// The forwarder's names for the shared app/common Supervisor (moved, not copied; the SupervisorSettings
// defaults are the forwarder's: 2 s join bound, SIGHUP = resync, "ct-forwarder", fwd-duty / fwd-http).
#include "common/Supervisor.h"

#include "HttpServer.h"
#include "LogRing.h"
#include "StatusBoard.h"

namespace cluster_tools { namespace app { namespace forwarder {
using common::NonBlockingLogWriter;
using common::SighupPolicy;
using common::Supervisor;
using common::SupervisorSettings;
}}}

#endif
