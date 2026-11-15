#ifndef CLUSTER_TOOLS_APP_FORWARDER_HTTP_SERVER_H
#define CLUSTER_TOOLS_APP_FORWARDER_HTTP_SERVER_H

// The forwarder's names for the shared runtime HttpServer (moved, not copied).
#include "cluster_tools/runtime/HttpServer.h"

namespace cluster_tools { namespace app { namespace forwarder {
using runtime::HttpRequest;
using runtime::HttpResponse;
using runtime::HttpServer;
using runtime::statusText;
}}}

#endif
