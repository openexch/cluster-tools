#ifndef CLUSTER_TOOLS_APP_FORWARDER_STATUS_BOARD_H
#define CLUSTER_TOOLS_APP_FORWARDER_STATUS_BOARD_H

// The forwarder's StatusBoard: the shared runtime StatusBoardT over ForwarderSnapshot.
#include "cluster_tools/runtime/StatusBoardT.h"

#include "Snapshot.h"

namespace cluster_tools { namespace app { namespace forwarder {
using StatusBoard = runtime::StatusBoardT<ForwarderSnapshot>;
}}}

#endif
