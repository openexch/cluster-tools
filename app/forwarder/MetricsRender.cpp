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

const char *driverModeLabel(std::uint8_t mode)
{
    return 1 == mode ? "embedded" : 2 == mode ? "external" : "unset";
}

const char *kindLabel(unsigned bucket)
{
    static const char *labels[] = { "0", "1", "2", "3", "other" };
    return bucket < 5 ? labels[bucket] : "?";
}

// RC2 S13 10.1 label values. Local tables (not the domain's nameOf) so this pure translation unit needs no domain
// library symbol; MetricsRenderTest pins them against the enums' order.
const char *breakCauseLabel(unsigned cause)
{
    static const char *labels[] = { "none", "from_mismatch", "level_op", "depth", "crossed", "malformed" };
    return cause < 6 ? labels[cause] : "?";
}
static_assert(static_cast<unsigned>(fd::BreakCause::COUNT) == 6, "breakCauseLabel covers every BreakCause");

const char *edgeLabel(unsigned edge)
{
    static const char *labels[] = { "leading", "trailing", "reoffer", "retry" };
    return edge < 4 ? labels[edge] : "?";
}
static_assert(static_cast<unsigned>(fd::TickEdge::COUNT) == 4, "edgeLabel covers every TickEdge");

const char *suppressedLabel(unsigned reason)
{
    static const char *labels[] = { "broken", "unbound", "unregistered", "version_not_above" };
    return reason < 4 ? labels[reason] : "?";
}
static_assert(static_cast<unsigned>(fd::TickSuppressed::COUNT) == 4, "suppressedLabel covers every TickSuppressed");

const char *bookStateLabel(std::uint8_t state)
{
    return 0 == state ? "EMPTY" : 1 == state ? "OK" : 2 == state ? "BROKEN" : "?";
}

void perMarketGauge(std::string &out, const char *name, const ForwarderSnapshot &s, std::int64_t (*value)(const BookView &))
{
    type(out, name, "gauge");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        line(out, "%s{market=\"%d\"} %" PRId64, name, m, value(s.books[m - 1]));
    }
}

void perMarketCounter(std::string &out, const char *name, const ForwarderSnapshot &s, std::uint64_t (*value)(const BookView &))
{
    type(out, name, "counter");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        line(out, "%s{market=\"%d\"} %" PRIu64, name, m, value(s.books[m - 1]));
    }
}

// RC2 S13 10.1: every series is a fixed-size family over markets 1..5 (render on fwd-http from the snapshot)
void renderBooks(std::string &out, const ForwarderSnapshot &s)
{
    perMarketGauge(out, "forwarder_book_state", s, [](const BookView &b) { return static_cast<std::int64_t>(b.state); });
    perMarketGauge(out, "forwarder_book_version", s, [](const BookView &b) { return b.version; });
    type(out, "forwarder_book_frames_total", "counter");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        line(out, "forwarder_book_frames_total{market=\"%d\",template=\"22\"} %" PRIu64, m, s.books[m - 1].ladder.snapshots);
        line(out, "forwarder_book_frames_total{market=\"%d\",template=\"25\"} %" PRIu64, m, s.books[m - 1].ladder.deltas);
    }
    perMarketCounter(out, "forwarder_book_frames_ignored_total", s, [](const BookView &b) { return b.ladder.ignored; });
    type(out, "forwarder_book_chain_breaks_total", "counter");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        for (unsigned c = 1; c < static_cast<unsigned>(fd::BreakCause::COUNT); c++)
        {
            line(out, "forwarder_book_chain_breaks_total{market=\"%d\",cause=\"%s\"} %" PRIu64, m, breakCauseLabel(c),
                s.books[m - 1].ladder.chainBreaks[c]);
        }
    }
    perMarketCounter(out, "forwarder_book_recoveries_total", s, [](const BookView &b) { return b.ladder.recoveries; });
    perMarketGauge(out, "forwarder_book_broken_age_ms", s, [](const BookView &b) { return b.brokenAgeMs; });
    perMarketGauge(out, "forwarder_book_snapshot_age_ms", s, [](const BookView &b) { return b.snapshotAgeMs; });
    perMarketCounter(out, "forwarder_book_version_regressions_total", s,
        [](const BookView &b) { return b.ladder.versionRegressions; });
    counter(out, "forwarder_book_malformed_total", s.bookMalformedNoMarket);
    perMarketCounter(out, "forwarder_book_unversioned_total", s, [](const BookView &b) { return b.ladder.unversioned; });
    counter(out, "forwarder_book_foreign_market_total", s.bookForeignMarket);
    type(out, "forwarder_book_broken_seconds_total", "counter");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        const std::int64_t ns = s.books[m - 1].brokenNsTotal;
        line(out, "forwarder_book_broken_seconds_total{market=\"%d\"} %" PRId64 ".%03" PRId64, m,
            static_cast<std::int64_t>(ns / 1'000'000'000), static_cast<std::int64_t>((ns % 1'000'000'000) / 1'000'000));
    }
    perMarketCounter(out, "forwarder_trade_malformed_total", s, [](const BookView &b) { return b.tradeMalformed; });
    counter(out, "forwarder_me_stale_term_total", s.counters.meStaleTerm);
    perMarketGauge(out, "forwarder_trade_last_id", s, [](const BookView &b) { return b.lastTradeId; });
    type(out, "forwarder_tick_offered_total", "counter");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        for (unsigned e = 0; e < static_cast<unsigned>(fd::TickEdge::COUNT); e++)
        {
            line(out, "forwarder_tick_offered_total{market=\"%d\",edge=\"%s\"} %" PRIu64, m, edgeLabel(e),
                s.books[m - 1].tick.offered[e]);
        }
    }
    type(out, "forwarder_tick_suppressed_total", "counter");
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        for (unsigned r = 0; r < static_cast<unsigned>(fd::TickSuppressed::COUNT); r++)
        {
            line(out, "forwarder_tick_suppressed_total{market=\"%d\",reason=\"%s\"} %" PRIu64, m, suppressedLabel(r),
                s.books[m - 1].tick.suppressed[r]);
        }
    }
    perMarketCounter(out, "forwarder_tick_offer_failures_total", s, [](const BookView &b) { return b.tick.offerFailures; });
    perMarketCounter(out, "forwarder_tick_refused_total", s, [](const BookView &b) { return b.tick.refused; });
    type(out, "forwarder_tick_rejected_total", "counter");
    line(out, "forwarder_tick_rejected_total{reason=\"unregistered\"} %" PRIu64, s.counters.tickRejected[0]);
    line(out, "forwarder_tick_rejected_total{reason=\"unauthorized\"} %" PRIu64, s.counters.tickRejected[1]);
    line(out, "forwarder_tick_rejected_total{reason=\"other\"} %" PRIu64, s.counters.tickRejected[2]);
    gauge(out, "forwarder_ticks_ready", s.ticksReady ? 1 : 0);
    perMarketGauge(out, "forwarder_tick_last_offered_version", s, [](const BookView &b) { return b.lastOfferedVersion; });
    perMarketGauge(out, "forwarder_tick_pending_age_ms", s, [](const BookView &b) { return b.pendingAgeMs; });
    type(out, "forwarder_me_frames_total", "counter");
    line(out, "forwarder_me_frames_total{template=\"23\"} %" PRIu64, s.counters.meKnownIgnored[0]);
    line(out, "forwarder_me_frames_total{template=\"24\"} %" PRIu64, s.counters.meKnownIgnored[1]);
    line(out, "forwarder_me_frames_total{template=\"27\"} %" PRIu64, s.counters.meKnownIgnored[2]);
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
    out.reserve(32 * 1024);
    type(out, "forwarder_info", "gauge");
    line(out, "forwarder_info{identity=\"%" PRId64 "\",version=\"%s\",me_epoch=\"%" PRId64 "\"} 1", s.identity, s.version,
        s.meEpoch);
    gauge(out, "forwarder_ready", s.ready ? 1 : 0);
    type(out, "forwarder_driver_mode", "gauge"); // RC2 S10' 8.2 rule 6: the mode in force
    line(out, "forwarder_driver_mode{mode=\"%s\"} 1", driverModeLabel(s.driverMode));
    counter(out, "forwarder_driver_dir_reclaimed_total", s.driverDirReclaimed);
    counter(out, "forwarder_storage_starved_seconds_total", s.storageStarvedSeconds); // S10' Q1: exit 70 after 30 s of it
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
    renderBooks(out, s);
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
        ",\"identity\":%" PRId64 ",\"driverMode\":\"%s\",\"ready\":%s,\"build\":\"%s\"}",
        stalled ? "STALLED" : "UP", s.coreLink >= static_cast<std::uint8_t>(CoreLink::UNREGISTERED) ? "true" : "false",
        s.meLink >= static_cast<std::uint8_t>(MeLink::UNVERIFIED) ? "true" : "false", s.registered ? "true" : "false",
        s.meEpoch, modeName(s.mode), s.cursor, s.lastSentSeq, s.coreHead, resyncs, s.lastResyncEndSeq, s.ringDepth,
        s.counters.invalid.total(), s.identity, driverModeLabel(s.driverMode), s.ready ? "true" : "false",
        s.build); // RC2 S16 W4: the binary's source_sha, additive
    // RC2 S13 10.2: the five books and the tick readiness (BOOK_STALE is not a /ready conjunct, PO-1), appended inside
    // the object so the S12 fields keep their order
    out.pop_back(); // the newline line() added
    out.pop_back(); // the closing brace
    out += ",\"books\":[";
    for (int m = 1; m <= fd::TICK_MARKETS; m++)
    {
        const BookView &b = s.books[m - 1];
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "%s{\"market\":%d,\"state\":\"%s\",\"version\":%" PRId64 ",\"brokenAgeMs\":%" PRId64 ",\"snapshotAgeMs\":%" PRId64
            ",\"lastOfferedVersion\":%" PRId64 ",\"pendingAgeMs\":%" PRId64 ",\"lastTradeId\":%" PRId64 "}",
            1 == m ? "" : ",", m, bookStateLabel(b.state), b.version, b.brokenAgeMs, b.snapshotAgeMs, b.lastOfferedVersion,
            b.pendingAgeMs, b.lastTradeId);
        out += buf;
    }
    char ticks[160];
    std::snprintf(ticks, sizeof(ticks), "],\"ticksReady\":{\"ready\":%s,\"market\":%d,\"condition\":\"%s\"}}\n",
        s.ticksReady ? "true" : "false", s.ticksNotReadyMarket, ticksNotReadyName(s.ticksNotReadyCondition));
    out += ticks;
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
