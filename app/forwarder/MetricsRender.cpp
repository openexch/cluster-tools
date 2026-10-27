#include "MetricsRender.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>

#include "cluster_tools/domain/forwarder/DispatchRules.h"

namespace cluster_tools { namespace app { namespace forwarder {

namespace {

namespace fd = cluster_tools::domain::forwarder;

void line(std::string &out, const char *format, ...) __attribute__((format(printf, 2, 3)));

void line(std::string &out, const char *format, ...)
{
    char buf[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n > 0)
    {
        out.append(buf, static_cast<std::size_t>(n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1));
    }
    out.push_back('\n');
}

void type(std::string &out, const char *name, const char *kind)
{
    line(out, "# TYPE %s %s", name, kind);
}

void counter(std::string &out, const char *name, std::uint64_t value)
{
    type(out, name, "counter");
    line(out, "%s %" PRIu64, name, value);
}

void gauge(std::string &out, const char *name, std::int64_t value)
{
    type(out, name, "gauge");
    line(out, "%s %" PRId64, name, value);
}

void perCluster(std::string &out, const char *name, const char *kind, std::uint64_t core, std::uint64_t me)
{
    type(out, name, kind);
    line(out, "%s{cluster=\"core\"} %" PRIu64, name, core);
    line(out, "%s{cluster=\"me\"} %" PRIu64, name, me);
}

const char *kindLabel(unsigned bucket)
{
    static const char *labels[] = { "0", "1", "2", "3", "other" };
    return bucket < 5 ? labels[bucket] : "?";
}

}

const char *modeName(std::uint8_t mode) noexcept
{
    switch (mode)
    {
        case 0: return "IDLE";
        case 1: return "RESYNCING";
        case 2: return "LIVE";
        case 3: return "OVERFLOW_WAIT";
    }
    return "?";
}

std::string renderPrometheus(const ForwarderSnapshot &s)
{
    std::string out;
    out.reserve(16 * 1024);
    type(out, "forwarder_info", "gauge");
    line(out, "forwarder_info{identity=\"%" PRId64 "\",version=\"%s\",me_epoch=\"%" PRId64 "\"} 1", s.identity, s.version,
        s.meEpoch);
    gauge(out, "forwarder_ready", s.ready ? 1 : 0);
    perCluster(out, "forwarder_session_state", "gauge", s.core.clientState, s.me.clientState);
    perCluster(out, "forwarder_session_new_leader_total", "counter", s.core.newLeaderEvents, s.me.newLeaderEvents);
    perCluster(out, "forwarder_session_reconnect_total", "counter", s.core.sessionsOpened > 0 ? s.core.sessionsOpened - 1 : 0,
        s.me.sessionsOpened > 0 ? s.me.sessionsOpened - 1 : 0);
    perCluster(out, "forwarder_connect_cluster_error_total", "counter", s.core.connectClusterErrors, s.me.connectClusterErrors);
    perCluster(out, "forwarder_keepalive_total", "counter", s.core.keepAlivesSent, s.me.keepAlivesSent);
    perCluster(out, "forwarder_keepalive_failures_total", "counter", s.core.keepAliveFailures, s.me.keepAliveFailures);
    gauge(out, "forwarder_registered", s.registered ? 1 : 0);
    type(out, "forwarder_registration_total", "counter");
    line(out, "forwarder_registration_total{result=\"accepted\"} %" PRIu64, s.registrationAccepted);
    line(out, "forwarder_registration_total{result=\"duplicate\"} %" PRIu64, s.registrationDuplicate);
    line(out, "forwarder_registration_total{result=\"rejected\"} %" PRIu64, s.registrationRejected);
    counter(out, "forwarder_me_epoch_reconnects_total", s.meEpochReconnects);
    gauge(out, "forwarder_me_egress_age_ms", s.meEgressAgeMs);
    gauge(out, "forwarder_mode", s.mode);
    type(out, "forwarder_dispatch_seq", "gauge");
    line(out, "forwarder_dispatch_seq{point=\"cursor\"} %" PRId64, s.cursor);
    line(out, "forwarder_dispatch_seq{point=\"last_sent\"} %" PRId64, s.lastSentSeq);
    line(out, "forwarder_dispatch_seq{point=\"core_head\"} %" PRId64, s.coreHead);
    counter(out, "forwarder_dispatch_received_total", s.resync.received);
    counter(out, "forwarder_dispatch_duplicate_total", s.resync.duplicates);
    counter(out, "forwarder_dispatch_dropped_idle_total", s.resync.droppedIdle);
    counter(out, "forwarder_dispatch_discarded_total", s.resync.discarded);
    counter(out, "forwarder_core_stale_term_total", s.counters.coreStaleTerm);
    counter(out, "forwarder_core_future_term_total", s.counters.coreFutureTerm);
    type(out, "forwarder_dispatch_sent_total", "counter");
    line(out, "forwarder_dispatch_sent_total{template=\"9\"} %" PRIu64, s.counters.sentTemplate9);
    line(out, "forwarder_dispatch_sent_total{template=\"10\"} %" PRIu64, s.counters.sentTemplate10);
    counter(out, "forwarder_release_entries_sent_total", s.counters.releaseEntriesSent);
    type(out, "forwarder_invalid_dispatch_total", "counter");
    for (unsigned k = 0; k < fd::KIND_BUCKETS; k++)
    {
        for (unsigned r = 0; r < fd::RULE_COUNT; r++)
        {
            for (unsigned c = 1; c < fd::CLASS_COUNT; c++)
            {
                const std::uint64_t n = s.counters.invalid.counts[k][r][c];
                if (0 != n)
                {
                    line(out, "forwarder_invalid_dispatch_total{kind=\"%s\",rule=\"%s\",action=\"%s\"} %" PRIu64, kindLabel(k),
                        fd::nameOf(static_cast<fd::RuleId>(r)), fd::actionOf(static_cast<fd::DispatchClass>(c)), n);
                }
            }
        }
    }
    type(out, "forwarder_resync_total", "counter");
    for (unsigned t = 1; t < fd::TRIGGER_COUNT; t++)
    {
        line(out, "forwarder_resync_total{reason=\"T%u\"} %" PRIu64, t, s.resync.resyncs[t]);
    }
    counter(out, "forwarder_resync_pages_total", s.resync.pages);
    counter(out, "forwarder_resync_entries_total", s.resync.entries);
    counter(out, "forwarder_resync_timeouts_total", s.resync.timeouts);
    type(out, "forwarder_resync_shape_violation_total", "counter");
    line(out, "forwarder_resync_shape_violation_total{check=\"S1\"} %" PRIu64, s.resync.shapeViolations[0]);
    line(out, "forwarder_resync_shape_violation_total{check=\"S2\"} %" PRIu64, s.resync.shapeViolations[1]);
    line(out, "forwarder_resync_shape_violation_total{check=\"S3\"} %" PRIu64, s.resync.shapeViolations[2]);
    gauge(out, "forwarder_resync_duration_ms", s.resyncDurationMs);
    type(out, "forwarder_head_probe_total", "counter");
    line(out, "forwarder_head_probe_total{result=\"covered\"} %" PRIu64, s.resync.headProbeCovered);
    line(out, "forwarder_head_probe_total{result=\"behind\"} %" PRIu64, s.resync.headProbeBehind);
    gauge(out, "forwarder_head_probe_age_ms", s.headProbeAgeMs);
    gauge(out, "forwarder_ring_depth", static_cast<std::int64_t>(s.ringDepth));
    gauge(out, "forwarder_ring_head_age_ms", s.ringHeadAgeMs);
    counter(out, "forwarder_ring_overflow_total", s.resync.ringOverflows);
    counter(out, "forwarder_hold_overflow_total", s.resync.holdOverflows);
    counter(out, "forwarder_ring_cleared_entries_total", s.resync.ringClearedEntries);
    counter(out, "forwarder_me_backpressure_total", s.counters.meBackPressure);
    counter(out, "forwarder_me_poll_saturated_total", s.counters.mePollSaturated);
    type(out, "forwarder_unknown_template_total", "counter");
    for (unsigned c = 0; c < 2; c++)
    {
        for (unsigned t = 0; t < fd::TEMPLATE_BUCKETS; t++)
        {
            const std::uint64_t n = s.counters.unknownTemplate[c][t];
            if (0 != n)
            {
                line(out, "forwarder_unknown_template_total{cluster=\"%s\",template=\"%u\"} %" PRIu64, 0 == c ? "core" : "me", t, n);
            }
        }
    }
    counter(out, "forwarder_core_malformed_total", s.counters.coreMalformed);
    counter(out, "forwarder_dispatch_seq_below_one_total", s.counters.dispatchSeqBelowOne);
    counter(out, "forwarder_aeron_errors_total", s.aeronErrors);
    gauge(out, "forwarder_duty_max_cycle_us", static_cast<std::int64_t>(s.dutyMaxCycleUs));
    counter(out, "forwarder_log_dropped_total", s.logDropped);
    return out;
}

std::string renderHealthJson(const ForwarderSnapshot &s, bool stalled)
{
    std::uint64_t resyncs = 0;
    for (unsigned t = 1; t < fd::TRIGGER_COUNT; t++)
    {
        resyncs += s.resync.resyncs[t];
    }
    std::string out;
    line(out,
        "{\"status\":\"%s\",\"aeConnected\":%s,\"meConnected\":%s,\"registered\":%s,\"meEpoch\":%" PRId64
        ",\"mode\":\"%s\",\"lastDispatchSeq\":%" PRId64 ",\"lastSentSeq\":%" PRId64 ",\"coreHead\":%" PRId64
        ",\"resyncs\":%" PRIu64 ",\"lastResyncEndSeq\":%" PRId64 ",\"ringDepth\":%" PRIu64 ",\"invalidDispatches\":%" PRIu64
        ",\"identity\":%" PRId64 ",\"ready\":%s}",
        stalled ? "STALLED" : "UP", s.coreLink >= static_cast<std::uint8_t>(CoreLink::UNREGISTERED) ? "true" : "false",
        s.meLink >= static_cast<std::uint8_t>(MeLink::UNVERIFIED) ? "true" : "false", s.registered ? "true" : "false",
        s.meEpoch, modeName(s.mode), s.cursor, s.lastSentSeq, s.coreHead, resyncs, s.lastResyncEndSeq, s.ringDepth,
        s.counters.invalid.total(), s.identity, s.ready ? "true" : "false");
    return out;
}

std::string renderReady(const ForwarderSnapshot &s)
{
    if (s.ready)
    {
        return "ready\n";
    }
    std::string out = "not ready:";
    bool first = true;
    for (unsigned bit = 0; bit < not_ready::COUNT; bit++)
    {
        if (0 != (s.notReady & (1u << bit)))
        {
            out += first ? " " : ",";
            out += notReadyName(bit);
            first = false;
        }
    }
    if (0 == s.sequence)
    {
        out += first ? " no snapshot yet" : ",no_snapshot_yet";
    }
    out += "\n";
    return out;
}

}}}
