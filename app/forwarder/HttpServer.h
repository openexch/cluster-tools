#ifndef CLUSTER_TOOLS_APP_FORWARDER_HTTP_SERVER_H
#define CLUSTER_TOOLS_APP_FORWARDER_HTTP_SERVER_H

// The forwarder's names for the shared app/common HttpServer (RC2 S9c W1: moved, not copied).
#include "common/HttpServer.h"

namespace cluster_tools { namespace app { namespace forwarder {
using common::HttpRequest;
using common::HttpResponse;
using common::HttpServer;
using common::statusText;
}}}

#endif
