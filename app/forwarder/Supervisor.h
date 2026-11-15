#ifndef CLUSTER_TOOLS_APP_FORWARDER_SUPERVISOR_H
#define CLUSTER_TOOLS_APP_FORWARDER_SUPERVISOR_H

// The forwarder's names for the shared runtime Supervisor (moved, not copied; the SupervisorSettings
// defaults are the forwarder's: 2 s join bound, SIGHUP = resync, "ct-forwarder", fwd-duty / fwd-http).
#include "cluster_tools/runtime/Supervisor.h"

#include "HttpServer.h"
#include "LogRing.h"
#include "StatusBoard.h"

namespace cluster_tools { namespace app { namespace forwarder {
using runtime::NonBlockingLogWriter;
using runtime::SighupPolicy;
using runtime::Supervisor;
using runtime::SupervisorSettings;
}}}

#endif
