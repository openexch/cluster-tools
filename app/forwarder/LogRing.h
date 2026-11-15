#ifndef CLUSTER_TOOLS_APP_FORWARDER_LOG_RING_H
#define CLUSTER_TOOLS_APP_FORWARDER_LOG_RING_H

// The forwarder's names for the shared runtime LogRing (moved, not copied).
#include "cluster_tools/runtime/LogRing.h"

namespace cluster_tools { namespace app { namespace forwarder {
using runtime::LogLevel;
using runtime::LogRecord;
using runtime::LogRing;
using runtime::nameOf;
using runtime::shouldLog;
}}}

#endif
