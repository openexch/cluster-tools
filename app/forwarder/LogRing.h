#ifndef CLUSTER_TOOLS_APP_FORWARDER_LOG_RING_H
#define CLUSTER_TOOLS_APP_FORWARDER_LOG_RING_H

// The forwarder's names for the shared app/common LogRing (moved, not copied).
#include "common/LogRing.h"

namespace cluster_tools { namespace app { namespace forwarder {
using common::LogLevel;
using common::LogRecord;
using common::LogRing;
using common::nameOf;
using common::shouldLog;
}}}

#endif
