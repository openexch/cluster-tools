#include "BridgeMetricsRender.h"

#include <cinttypes>
#include <cstdio>

#include "cluster_tools/archive/FramePeek.h"

namespace cluster_tools { namespace app { namespace bridge {

namespace db = domain::bridge;
using archive::ArchiveMetrics;
using db::BridgeMetrics;

const char *coreLinkName(CoreLink link) noexcept
{
    switch (link)
    {
        case CoreLink::C0_DOWN: return "C0_DOWN";
        case CoreLink::C1_CONNECTING: return "C1_CONNECTING";
        case CoreLink::C2_CONNECTED: return "C2_CONNECTED";
        case CoreLink::C3_REGISTERED: return "C3_REGISTERED";
        case CoreLink::C4_AWAITING: return "C4_AWAITING";
    }
    return "?";
}

const char *notReadyBitName(int bit) noexcept
{
    switch (bit)
    {
        case 0: return "core_not_connected";
        case 1: return "core_no_leader";
        case 2: return "core_not_registered";
        case 3: return "no_report_this_term";
        case 4: return "report_stale";
        case 5: return "no_source_attached";
        case 6: return "halted";
        case 7: return "fatal";
        case 8: return "backlog_without_progress_30s";
        case 9: return "no_reachable_source";
        case 10: return "report_degraded";
        case 11: return "no_source_beyond_e";
        case 12: return "hole_grace";
        case 13: return "stopping";
        case 14: return "stalled";
        case 15: return "storage_starved";
        default: return "?";
    }
}

std::uint32_t notReadyBits(const ReadyInputs &in) noexcept
{
    using namespace not_ready;
    std::uint32_t bits = 0;
    switch (in.link)
    {
        case CoreLink::C0_DOWN:
        case CoreLink::C1_CONNECTING: bits |= CORE_NOT_CONNECTED; break;
        case CoreLink::C4_AWAITING: bits |= CORE_NO_LEADER; break;
        case CoreLink::C2_CONNECTED: bits |= CORE_NOT_REGISTERED; break;
        case CoreLink::C3_REGISTERED: break;
    }
    const db::BridgeView &v = in.view;
    if (!v.reportThisTerm)
    {
        bits |= NO_REPORT_THIS_TERM;
    }
    else if (!app_seams::disabled(app_seams::Rule::READY_IGNORES_REPORT_AGE))
    {
        const bool halted = db::State::HALTED == v.state || db::State::HALTING == v.state;
        const auto bound = 3 * (halted ? in.reportIntervalHalted : in.reportInterval);
        if (archive::TimePoint{} == v.lastReportAt || in.now - v.lastReportAt > bound)
        {
            bits |= REPORT_STALE;
        }
    }
    if (!in.anySourceAttached)
    {
        bits |= NO_SOURCE_ATTACHED;
    }
    if (db::BridgeHalt::NONE != v.halt)
    {
        bits |= HALTED;
    }
    if (db::Fatal::NONE != v.fatal)
    {
        bits |= FATAL;
    }
    if (in.backlogBytes > 0)
    {
        const archive::TimePoint progress =
            app_seams::disabled(app_seams::Rule::READY_COUNTS_FORWARDS_ONLY) ? in.forwardAt : in.progressAt;
        if (in.now - progress >= BACKLOG_PROGRESS_BOUND)
        {
            bits |= BACKLOG_WITHOUT_PROGRESS;
        }
    }
    if (0 != (v.degraded & db::bit(db::Degraded::NO_REACHABLE_SOURCE)))
    {
        bits |= NO_REACHABLE_SOURCE;
    }
    if (0 != (v.degraded & (db::bit(db::Degraded::REPORT_INVALID) | db::bit(db::Degraded::REPORT_TIMEOUT))))
    {
        bits |= REPORT_DEGRADED;
    }
    if (0 != (v.degraded & db::bit(db::Degraded::NO_SOURCE_BEYOND_E)))
    {
        bits |= NO_SOURCE_BEYOND_E;
    }
    if (0 != (v.degraded & db::bit(db::Degraded::HOLE_WITH_LAGGING_SOURCE)))
    {
        bits |= HOLE_GRACE;
    }
    if (in.stopping)
    {
        bits |= STOPPING;
    }
    if (in.storageStarved)
    {
        bits |= STORAGE_STARVED;
    }
    return bits;
}

bool healthHalted(const BridgeSnapshot &s) noexcept
{
    if (app_seams::disabled(app_seams::Rule::HEALTH_HALTED_FROM_STATE))
    {
        return db::State::HALTED == s.view.state || db::State::HALTING == s.view.state; // RED shape (S9b 9.1 as written)
    }
    return db::BridgeHalt::NONE != s.view.halt; // the halt record (D-S9c-2: a kept halt reads halted while DOWN)
}

int healthStatus(const BridgeSnapshot &s, const HttpExtras &x)
{
    return (healthHalted(s) || x.stalled) ? 503 : 200;
}

std::uint32_t readyBits(const BridgeSnapshot &s, const HttpExtras &x) noexcept
{
    std::uint32_t bits = s.notReady;
    if (x.stalled)
    {
        bits |= not_ready::STALLED;
    }
    if (0 == s.sequence)
    {
        bits |= not_ready::CORE_NOT_CONNECTED; // nothing published yet: the duty loop has not run
    }
    return bits;
}

std::string renderReady(std::uint32_t bits)
{
    if (0 == bits)
    {
        return "ready\n";
    }
    std::string out = "not ready:";
    for (int b = 0; b < not_ready::BITS; ++b)
    {
        if (0 != (bits & (1u << b)))
        {
            out += " ";
            out += notReadyBitName(b);
        }
    }
    return out + "\n";
}

std::string renderHealthJson(const BridgeSnapshot &s, const HttpExtras &x)
{
    const bool halted = healthHalted(s);
    const char *status = x.stalled ? "STALLED" : (halted ? "HALTED" : "UP");
    char body[1024];
    std::snprintf(body, sizeof(body),
        "{\"halted\":%s,\"aeConnected\":%s,\"haltReason\":\"%s\",\"meEpoch\":%" PRId64 ",\"cursor\":%" PRId64
        ",\"status\":\"%s\",\"state\":\"%s\",\"coreHaltReason\":\"%s\",\"expected\":%" PRId64 ",\"last\":%" PRId64
        ",\"attachedSource\":%zu,\"degraded\":%u,\"registered\":%s,\"reportThisTerm\":%s,\"identity\":%" PRId64
        ",\"ready\":%s,\"relistsSinceHalt\":%llu,\"laneAheadFences\":%llu,\"build\":\"%s\"}",
        halted ? "true" : "false", s.aeConnected ? "true" : "false", db::bridgeHaltName(s.view.halt), s.view.syncEpoch,
        s.view.cursor, status, db::stateName(s.view.state), db::haltReasonName(s.view.coreMirror), s.view.expected,
        s.view.last, s.view.attached, s.view.degraded, s.registered ? "true" : "false",
        s.view.reportThisTerm ? "true" : "false", s.identity, 0 == readyBits(s, x) ? "true" : "false",
        static_cast<unsigned long long>(s.view.relistsSinceHalt),
        static_cast<unsigned long long>(s.laneAheadFences), s.build); // RC2 S16 W4: the binary's source_sha
    return body;
}

// ------------------------------------------------------------------ Prometheus

namespace {

class Prom
{
public:
    void family(const char *name, const char *type, const char *help)
    {
        m_out += "# HELP ";
        m_out += name;
        m_out += ' ';
        m_out += help;
        m_out += "\n# TYPE ";
        m_out += name;
        m_out += ' ';
        m_out += type;
        m_out += '\n';
    }
    void value(const char *name, std::int64_t v) { sample(name, "", v); }
    void uvalue(const char *name, std::uint64_t v) { sample(name, "", static_cast<std::int64_t>(v)); }
    void labelled(const char *name, const std::string &labels, std::int64_t v) { sample(name, labels, v); }
    void gauge(const char *name, const char *help, std::int64_t v)
    {
        family(name, "gauge", help);
        value(name, v);
    }
    void counter(const char *name, const char *help, std::uint64_t v)
    {
        family(name, "counter", help);
        uvalue(name, v);
    }
    void histogram(const char *name, const char *help, const LatencyHistogram &h)
    {
        family(name, "histogram", help);
        std::uint64_t cumulative = 0;
        char labels[48];
        for (std::size_t i = 0; i < LatencyHistogram::BOUNDS; ++i)
        {
            cumulative += h.buckets[i];
            std::snprintf(labels, sizeof(labels), "le=\"%" PRId64 "\"", LatencyHistogram::UPPER_MS[i]);
            sample((std::string(name) + "_bucket").c_str(), labels, static_cast<std::int64_t>(cumulative));
        }
        cumulative += h.buckets[LatencyHistogram::BOUNDS];
        sample((std::string(name) + "_bucket").c_str(), "le=\"+Inf\"", static_cast<std::int64_t>(cumulative));
        sample((std::string(name) + "_sum").c_str(), "", h.sumMs);
        sample((std::string(name) + "_count").c_str(), "", static_cast<std::int64_t>(cumulative));
    }
    std::string take() { return std::move(m_out); }

private:
    void sample(const char *name, const std::string &labels, std::int64_t v)
    {
        m_out += name;
        if (!labels.empty())
        {
            m_out += '{';
            m_out += labels;
            m_out += '}';
        }
        m_out += ' ';
        m_out += std::to_string(v);
        m_out += '\n';
    }
    std::string m_out;
};

std::string label(const char *key, const std::string &value)
{
    return std::string(key) + "=\"" + value + "\"";
}

template<std::size_t N, typename Name>
void counters(Prom &p, const char *name, const char *help, const char *key, const std::array<std::atomic<std::uint64_t>, N> &c,
    Name nameOf, std::size_t first = 0)
{
    p.family(name, "counter", help);
    for (std::size_t i = first; i < N; ++i)
    {
        p.labelled(name, label(key, nameOf(i)), static_cast<std::int64_t>(c[i].load(std::memory_order_relaxed)));
    }
}

const char *closeReasonLabel(std::size_t i)
{
    switch (i)
    {
        case 0: return "none";
        case 1: return "client_close";
        case 2: return "closed_by_cluster";
        case 3: return "new_leader_timeout";
        case 4: return "new_leader_connection_timeout";
        case 5: return "max_position_exceeded";
        case 6: return "transport_error";
        default: return "?";
    }
}

const char *offerStatusLabel(std::size_t i)
{
    switch (i)
    {
        case 0: return "ok";
        case 1: return "back_pressured";
        case 2: return "not_connected";
        case 3: return "closed";
        case 4: return "max_position_exceeded";
        case 5: return "not_ready";
        case 6: return "transport_error";
        default: return "?";
    }
}

}

std::string renderPrometheus(const BridgeSnapshot &s, const BridgeMetrics &m, ArchiveMetrics &a, const HttpExtras &x)
{
    Prom p;
    const std::size_t n = x.sourceCount < archive::MAX_SOURCES ? x.sourceCount : archive::MAX_SOURCES;
    const std::uint32_t bits = readyBits(s, x);

    // ---- the app (9.3)
    p.family("bridge_info", "gauge", "ct-bridge build and identity (version, BRIDGE_INSTANCE_ID, the sync's meEpoch)");
    p.labelled("bridge_info", "identity=\"" + std::to_string(s.identity) + "\",version=\"" + s.version +
        "\",me_epoch=\"" + std::to_string(s.view.syncEpoch) + "\"", 1);
    p.family("bridge_driver_mode", "gauge", "The media driver mode in force (external: the host aeronmd; embedded: dev/demo)");
    p.labelled("bridge_driver_mode", label("mode", x.driverMode), 1);
    p.gauge("bridge_ae_connected", "1 while the core session is CONNECTED (the Java meaning)", s.aeConnected ? 1 : 0);
    p.gauge("bridge_registered", "1 while registered in this session and term (C3)", s.registered ? 1 : 0);
    p.gauge("bridge_ready", "1 when /ready answers 200", 0 == bits ? 1 : 0);
    p.gauge("bridge_not_ready_bits", "The /ready conjunction bitmask (contract 4.3)", static_cast<std::int64_t>(bits));
    p.gauge("bridge_state", "Domain state (0 DOWN, 1 SYNCING, 2 STREAMING, 3 HALTING, 4 HALTED)",
        static_cast<std::int64_t>(s.view.state));
    p.gauge("bridge_core_link", "Core link (0 C0_DOWN .. 4 C4_AWAITING)", s.coreLink);
    p.counter("bridge_client_sessions_opened_total", "Core sessions opened", s.client.sessionsOpened);
    p.counter("bridge_client_new_leader_events_total", "NewLeaderEvents seen on the core session", s.client.newLeaderEvents);
    p.counter("bridge_client_keepalives_total", "Keepalives sent", s.client.keepAlivesSent);
    p.counter("bridge_client_keepalive_failures_total", "Keepalive offers that failed", s.client.keepAliveFailures);
    p.counter("bridge_client_offers_in_ended_terms_total", "Offers made in a term that ended (exported, never consulted)",
        s.client.offersInEndedTerms);
    p.counter("bridge_client_egress_image_losses_total", "Egress image losses", s.client.egressImageLosses);
    p.family("bridge_session_losses_total", "counter", "Core session losses by close reason (F12)");
    for (std::size_t i = 1; i < CLOSE_REASONS; ++i)
    {
        p.labelled("bridge_session_losses_total", label("reason", closeReasonLabel(i)),
            static_cast<std::int64_t>(s.app.sessionLosses[i]));
    }
    p.counter("bridge_connect_cluster_error_total", "Connect cycles refused with a transient CLUSTER_ERROR", s.app.connectClusterErrors);
    p.counter("bridge_connect_cycles_total", "Connect cycles started", s.app.connectCycles);
    p.family("bridge_registration_total", "counter", "RegisterSession results");
    p.labelled("bridge_registration_total", label("result", "accepted"), static_cast<std::int64_t>(s.registrar.acksAccepted));
    p.labelled("bridge_registration_total", label("result", "duplicate"), static_cast<std::int64_t>(s.registrar.acksDuplicate));
    p.labelled("bridge_registration_total", label("result", "rejected"), static_cast<std::int64_t>(s.registrar.acksRejected));
    p.labelled("bridge_registration_total", label("result", "inferred_rejected"),
        static_cast<std::int64_t>(s.registrar.closesWhilePending));
    p.labelled("bridge_registration_total", label("result", "resend"), static_cast<std::int64_t>(s.registrar.resends));
    p.counter("bridge_core_stale_term_total", "Core frames of an ended term dropped (rule S)", s.app.coreStaleTerm);
    p.counter("bridge_core_future_term_total", "Core frames of a later term dropped before its NewLeaderEvent", s.app.coreFutureTerm);
    p.family("bridge_egress_ignored_total", "counter", "Core egress frames the bridge does not consume, by template");
    for (std::size_t i = 0; i < EGRESS_TEMPLATE_LABELS; ++i)
    {
        if (0 != s.app.egressIgnored[i])
        {
            p.labelled("bridge_egress_ignored_total",
                label("template", i + 1 == EGRESS_TEMPLATE_LABELS ? std::string("other") : std::to_string(i)),
                static_cast<std::int64_t>(s.app.egressIgnored[i]));
        }
    }
    p.family("bridge_order_reject_total", "counter", "OrderRejects naming a bridge template, counted (not acted on), by reason");
    for (std::size_t i = 0; i < s.app.orderReject.size(); ++i)
    {
        if (0 != s.app.orderReject[i])
        {
            p.labelled("bridge_order_reject_total", label("reason", std::to_string(i)), static_cast<std::int64_t>(s.app.orderReject[i]));
        }
    }
    p.family("bridge_offer_retry_total", "counter", "Core offers mapped to RETRY, by client status");
    for (std::size_t i = 1; i < OFFER_STATUSES; ++i)
    {
        if (3 == i || 4 == i || 6 == i)
        {
            continue; // CLOSED, MAX_POSITION_EXCEEDED, TRANSPORT_ERROR are not RETRY
        }
        p.labelled("bridge_offer_retry_total", label("status", offerStatusLabel(i)), static_cast<std::int64_t>(s.app.offerRetry[i]));
    }
    p.family("bridge_source_faults_total", "counter", "Archive source faults seen by the observer tap, by kind");
    for (std::size_t i = 1; i < FAULT_KINDS; ++i)
    {
        p.labelled("bridge_source_faults_total", label("kind", archive::faultName(static_cast<archive::FaultKind>(i))),
            static_cast<std::int64_t>(s.app.sourceFaults[i]));
    }
    p.gauge("bridge_conductor_heartbeat_age_ms", "Age of this client's heartbeat counter as main read it (-1: not found yet)",
        x.conductorHeartbeatAgeMs);
    p.gauge("bridge_report_age_ms", "Age of the last accepted FeedPositionReport (-1: none this session)", s.reportAgeMs);
    p.gauge("bridge_consumed_position_age_ms", "Time since the consumed replay position last moved (-1: never)",
        s.consumedPositionAgeMs);
    p.gauge("bridge_last_forward_age_ms", "Time since the last forward (-1: none)", s.lastForwardAgeMs);
    p.histogram("bridge_forward_latency_ms", "Journal trade timestamp (ME leader clock) to the offer returning OK, ms "
        "(CROSS-HOST clocks: supporting evidence only)", s.forwardLatency);
    p.histogram("bridge_ack_latency_ms", "Offer returning OK to the SettlementApplied egress for that trade, ms", s.ackLatency);
    p.counter("bridge_clock_anomalies_total", "Forward-latency samples rejected as garbage (negative or > 1 h)", s.app.clockAnomalies);
    p.counter("bridge_ack_map_skips_total", "Ack-map entries overwritten at the 2^20 bound", s.app.ackMapSkips);
    p.counter("bridge_errors_total", "Session losses plus connect cycles that failed (Java name)",
        s.app.connectCyclesFailed + [&] {
            std::uint64_t t = 0;
            for (std::size_t i = 1; i < CLOSE_REASONS; ++i)
            {
                t += s.app.sessionLosses[i];
            }
            return t;
        }());
    p.counter("bridge_registration_lost_total", "OrderReject UNREGISTERED_SESSION naming a bridge template (6.2)", s.app.registrationLost);
    p.counter("bridge_registration_duplicate_identity_retries_total",
        "RegisterSession re-sent after a REJECTED DUPLICATE_IDENTITY (the old session of this identity not yet timed out)",
        s.app.duplicateIdentityRetries);
    p.counter("bridge_aeron_errors_total", "Errors the Aeron client reported (fatal ones end the process)", s.aeronErrors);
    p.counter("bridge_log_dropped_total", "Log records dropped at the full ring", s.logDropped);
    p.counter("bridge_verdict_log_dropped_total", "Verdict events not drained in time", BridgeMetrics::get(m.verdictLogDropped));
    p.counter("bridge_http_timeouts_total", "HTTP requests abandoned at the 2 s whole-request deadline", x.httpTimeouts);
    p.counter("bridge_status_publish_skipped_total", "Status publishes skipped (a reader held the board)", x.publishSkipped);
    p.gauge("bridge_duty_max_cycle_us", "Longest duty iteration in the last 10 s window, us", static_cast<std::int64_t>(s.dutyMaxCycleUs));
    p.counter("bridge_sighup_ignored_total", "SIGHUPs counted (no operator resync exists)", x.sighupIgnored);
    p.counter("bridge_driver_dir_reclaimed_total", "Embedded mode: a stale per-instance driver dir reused and cleaned at start",
        x.driverDirReclaimed);
    p.counter("bridge_storage_starved_seconds_total",
        "Seconds the core ingress publication or every replay image was refused for storage space (exit 70 after 30 s, S10' Q1)",
        s.app.storageStarvedSeconds);

    // ---- the domain (S9b 12)
    p.gauge("bridge_halted", "1 while a halt is recorded (also a kept halt while DOWN)", BridgeMetrics::value(m.halted));
    p.gauge("bridge_halt_reason", "BridgeHalt code (0 NONE, 1 GAP, 2 EPOCH_MISMATCH, 3 SOURCE_SCHEMA, 4 CORE_REFUSED)",
        BridgeMetrics::value(m.haltReason));
    p.gauge("bridge_core_halt_reason", "The core's haltReason in the last report", BridgeMetrics::value(m.coreHaltReason));
    p.gauge("bridge_core_cursor", "The core's lastAppliedJournalSeq in the last report", BridgeMetrics::value(m.coreCursor));
    p.gauge("bridge_last_offered_seq", "The last journal seq offered OK", BridgeMetrics::value(m.lastOfferedSeq));
    p.gauge("bridge_me_epoch", "The sync's meEpoch", BridgeMetrics::value(m.meEpoch));
    p.gauge("bridge_inflight_ring_depth", "Offered entries not yet proven by a report", BridgeMetrics::value(m.inflightRingDepth));
    p.gauge("bridge_staged_trades", "Trades staged for a 62", BridgeMetrics::value(m.stagedTrades));
    p.gauge("bridge_degraded", "Degraded bitmask", BridgeMetrics::value(m.degraded));
    p.family("bridge_source_verdicts", "gauge", "Per-source verdict code");
    for (std::size_t i = 0; i < n; ++i)
    {
        p.labelled("bridge_source_verdicts", label("source", std::to_string(i)), BridgeMetrics::value(m.sourceVerdicts[i]));
    }
    // RC2 S10' 5.4: the halted re-scan's walks past a holed / bad recording, per source
    p.family("bridge_halt_relists_total", "counter", "Re-scan walks past a holed or bad recording into later ones (5.1 R1)");
    for (std::size_t i = 0; i < n; ++i)
    {
        p.labelled("bridge_halt_relists_total", label("source", std::to_string(i)), BridgeMetrics::get(m.haltRelists[i]));
    }
    counters(p, "bridge_forwarded_total", "Frames forwarded by kind", "kind", m.forwarded,
        [](std::size_t i) { return std::string(db::frameKindLabel(static_cast<db::FrameKind>(i))); });
    counters(p, "bridge_forwarded_records_total", "Records forwarded by frame kind", "kind", m.forwardedRecords,
        [](std::size_t i) { return std::string(db::frameKindLabel(static_cast<db::FrameKind>(i))); });
    counters(p, "bridge_skipped_total", "Records skipped (seq <= last) by record template", "kind", m.skipped,
        [](std::size_t i) { return std::to_string(i + 1); });
    counters(p, "bridge_feed_halts_sent_total", "FeedHalts offered by wire reason", "reason", m.feedHaltsSent,
        [](std::size_t i) { return std::to_string(i); });
    counters(p, "bridge_fences_total", "Fences by trigger", "trigger", m.fences,
        [](std::size_t i) { return std::string(db::fenceTriggerName(static_cast<db::FenceTrigger>(i))); });
    counters(p, "bridge_source_bad_total", "BAD verdicts by reason", "reason", m.sourceBad,
        [](std::size_t i) { return std::string(db::badReasonLabel(static_cast<db::BadReason>(i))); });
    counters(p, "bridge_settle_rejected_total", "SettleRejected by reason code", "reason", m.settleRejected,
        [](std::size_t i) { return i + 1 == db::SETTLE_REASON_LABELS ? std::string("other") : std::to_string(i); });
    counters(p, "bridge_frames_deferred_total", "Frames deferred (ABORT) by why", "why", m.framesDeferred,
        [](std::size_t i) { return std::string(db::deferReasonLabel(static_cast<db::DeferReason>(i))); });
    counters(p, "bridge_halt_recovered_total", "Halts left by recovery, by halt", "reason", m.haltRecovered,
        [](std::size_t i) { return std::string(db::bridgeHaltName(static_cast<db::BridgeHalt>(i))); }, 1);
    p.counter("bridge_source_failovers_total", "Failovers to another source", BridgeMetrics::get(m.sourceFailovers));
    p.counter("bridge_halt_successor_attaches_total", "A halted re-scan forwarded e from a later recording (5.1 R1)",
        BridgeMetrics::get(m.haltSuccessorAttaches));
    p.counter("bridge_halt_stepbacks_total", "Memo-free re-reads after a FILL successor (5.1 R2)",
        BridgeMetrics::get(m.haltStepbacks));
    p.counter("bridge_staged_dropped_total", "Staged trades dropped by a fence", BridgeMetrics::get(m.stagedDropped));
    p.counter("bridge_hole_verifications_total", "Holes re-read memo-free", BridgeMetrics::get(m.holeVerifications));
    p.counter("bridge_refusal_verifications_total", "Memo-read refusals re-read memo-free", BridgeMetrics::get(m.refusalVerifications));
    p.counter("bridge_storm_rotations_total", "Storm-guard rotations", BridgeMetrics::get(m.stormRotations));
    p.counter("bridge_core_refused_total", "CORE_REFUSED parks", BridgeMetrics::get(m.coreRefused));
    p.counter("bridge_report_queries_total", "QueryFeedPosition offered", BridgeMetrics::get(m.reportQueries));
    p.counter("bridge_report_invalid_total", "Invalid or malformed reports", BridgeMetrics::get(m.reportInvalid));
    p.counter("bridge_report_timeouts_total", "Queries unanswered within the timeout", BridgeMetrics::get(m.reportTimeouts));
    p.counter("bridge_report_stale_total", "Reports answering another correlation id", BridgeMetrics::get(m.reportStale));
    p.counter("bridge_backpressure_aborts_total", "Back-pressured offers aborted", BridgeMetrics::get(m.backpressureAborts));
    p.counter("bridge_ring_full_pauses_total", "Reads paused at the full ring", BridgeMetrics::get(m.ringFullPauses));
    p.counter("bridge_recording_start_stepback_total", "Recordings starting below an earlier recording's first seq",
        BridgeMetrics::get(m.recordingStartStepBack));
    p.counter("bridge_epochs_total", "Syncs started (Java name)", BridgeMetrics::get(m.epochs));
    p.counter("bridge_feed_halt_rejected_total", "FeedHalts the core REJECTED (a bridge bug)", BridgeMetrics::get(m.feedHaltRejected));
    p.counter("bridge_recordings_ended_total", "Recordings that ended under the follower", BridgeMetrics::get(m.recordingsEnded));
    p.counter("bridge_source_parks_total", "Switches by the 8.3 park rows", BridgeMetrics::get(m.sourceParks));

    // ---- the archive (S9a 8.1)
    p.family("bridge_source_attached", "gauge", "1 while the source is attached");
    for (std::size_t i = 0; i < n; ++i)
    {
        p.labelled("bridge_source_attached", label("source", std::to_string(i)),
            static_cast<std::int64_t>(ArchiveMetrics::get(a.sourceAttached[i])));
    }
    p.family("bridge_chain_recordings", "gauge", "Recordings in the source's chain");
    for (std::size_t i = 0; i < n; ++i)
    {
        p.labelled("bridge_chain_recordings", label("source", std::to_string(i)),
            static_cast<std::int64_t>(ArchiveMetrics::get(a.chainRecordings[i])));
    }
    p.family("bridge_source_refused", "gauge", "1 while the source is refused (mtu)");
    for (std::size_t i = 0; i < n; ++i)
    {
        p.labelled("bridge_source_refused", label("source", std::to_string(i)) + "," + label("reason", "mtu"),
            static_cast<std::int64_t>(ArchiveMetrics::get(a.sourceRefusedMtu[i])));
    }
    counters(p, "bridge_source_stalls_total", "Source stalls and losses by reason", "reason", a.sourceStalls,
        [](std::size_t i) { return std::string(archive::stallLabel(static_cast<archive::StallReason>(i))); });
    counters(p, "bridge_replay_open_failures_total", "Replay open failures by reason", "reason", a.replayOpenFailures,
        [](std::size_t i) { return std::string(archive::openFailureLabel(static_cast<archive::OpenFailure>(i))); });
    counters(p, "bridge_frames_refused_total", "Frames refused by the 4.4 gate", "reason", a.framesRefused,
        [](std::size_t i) { return std::string(archive::refusalLabel(static_cast<archive::Refusal>(i))); }, 1);
    counters(p, "bridge_memo_invalidations_total", "Resume memo invalidations by reason", "reason", a.memoInvalidations,
        [](std::size_t i) { return std::string(archive::memoReasonLabel(static_cast<archive::MemoReason>(i))); });
    counters(p, "bridge_memo_resumes_total", "Attaches by resume kind", "kind", a.memoResumes,
        [](std::size_t i) { return std::string(0 == i ? "cold" : "proof"); });
    p.counter("bridge_memo_trims_total", "Memo proof trims", ArchiveMetrics::get(a.memoTrims));
    p.counter("bridge_empty_recordings_skipped_total", "Stopped empty recordings skipped", ArchiveMetrics::get(a.emptyRecordingsSkipped));
    p.counter("bridge_recording_successions_total", "Successor recordings followed", ArchiveMetrics::get(a.recordingSuccessions));
    p.counter("bridge_foreign_stream_descriptors_dropped_total", "Recording descriptors of another stream dropped",
        ArchiveMetrics::get(a.foreignStreamDescriptorsDropped));
    p.gauge("bridge_source_backlog_bytes", "Recorded bytes not yet delivered", a.sourceBacklogBytes.load(std::memory_order_relaxed));
    p.gauge("bridge_replay_recording_position", "Recorded position of the followed recording",
        a.replayRecordingPosition.load(std::memory_order_relaxed));
    p.gauge("bridge_replay_consumed_position", "Consumed replay position", a.replayConsumedPosition.load(std::memory_order_relaxed));
    p.gauge("bridge_archive_call_max_ms", "Longest archive control call since the previous scrape",
        static_cast<std::int64_t>(ArchiveMetrics::takeMax(a.archiveCallMaxMs)));
    p.gauge("bridge_invoker_max_gap_ms", "Longest gap between invoker calls since the previous scrape",
        static_cast<std::int64_t>(ArchiveMetrics::takeMax(a.invokerMaxGapMs)));
    p.gauge("bridge_duty_iteration_max_us", "Longest duty iteration since the previous scrape",
        static_cast<std::int64_t>(ArchiveMetrics::takeMax(a.dutyIterationMaxUs)));
    return p.take();
}

}}}
