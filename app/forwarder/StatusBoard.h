#ifndef CLUSTER_TOOLS_APP_FORWARDER_STATUS_BOARD_H
#define CLUSTER_TOOLS_APP_FORWARDER_STATUS_BOARD_H

// The forwarder's StatusBoard: the shared app/common StatusBoardT over ForwarderSnapshot.
#include "common/StatusBoardT.h"

#include "Snapshot.h"

namespace cluster_tools { namespace app { namespace forwarder {
using StatusBoard = common::StatusBoardT<ForwarderSnapshot>;
}}}

#endif
