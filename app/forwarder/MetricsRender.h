#ifndef CLUSTER_TOOLS_APP_FORWARDER_METRICS_RENDER_H
#define CLUSTER_TOOLS_APP_FORWARDER_METRICS_RENDER_H

#include <string>

#include "Snapshot.h"

// MetricsRender - Prometheus text (GET /metrics), the /health JSON and the /ready body from one snapshot
// (RC2 S12 contract section 10). Pure functions of the snapshot; tested against a golden in WITH_AERON=OFF.

namespace cluster_tools { namespace app { namespace forwarder {

std::string renderPrometheus(const ForwarderSnapshot &s);

// stalled = the duty heartbeat is older than 5 s (status STALLED, HTTP 503; main's watchdog exits 70 at the
// same threshold).
std::string renderHealthJson(const ForwarderSnapshot &s, bool stalled);

// "ready\n", or "not ready: <conjunct>,<conjunct>\n".
std::string renderReady(const ForwarderSnapshot &s);

const char *modeName(std::uint8_t mode) noexcept;

}}}

#endif
