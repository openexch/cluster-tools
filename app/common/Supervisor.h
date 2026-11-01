#ifndef CLUSTER_TOOLS_APP_COMMON_SUPERVISOR_H
#define CLUSTER_TOOLS_APP_COMMON_SUPERVISOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "common/HttpServer.h"
#include "common/LogRing.h"
#include "common/StatusBoardT.h"

// Supervisor - main's responsibilities after start (RC2 S12 contract section 3, FIX-8; F2, F3, F14). One copy for
// ct-forwarder and ct-bridge (RC2 S9c W1, G7): the process name, the thread names, the SIGHUP meaning and the join
// bound are SupervisorSettings fields; the defaults are the forwarder's, so the forwarder is unchanged.
//   - a loop on sigtimedwait with a 1 s timeout: SIGTERM / SIGINT set the stop flag, SIGHUP per sighupPolicy
//     (RESYNC_FLAG: the resync flag, the forwarder's T4; COUNT_ONLY: counted and logged once, the bridge, D-S9c-15);
//   - every wake: the liveness check of the owner (ct-bridge: the client-conductor heartbeat, 3.4 FIX-5), when set;
//   - every wake: the duty watchdog (the duty heartbeat must advance within 5 s, else exit 70, F3), the
//     duty's fatal flag (F2, exit 70), and the LogRing drain to stderr;
//   - the stop path: shutdown() of the listening socket so accept() returns, then joins with a 2 s bound
//     (exit 70 when it is hit, F14);
//   - the exit code of the duty body otherwise (0, 75, 78 or 70).
// The exit itself goes through a seam (production: flush stderr + std::_Exit) so ct_forwarder_main_tests can
// observe it. Pure POSIX: no Aeron.

namespace cluster_tools { namespace app { namespace common {

enum class SighupPolicy : std::uint8_t
{
    RESYNC_FLAG, // ct-forwarder: SIGHUP sets resyncRequested (T4)
    COUNT_ONLY   // ct-bridge: SIGHUP is counted (sighupCount) and logged once; no operator resync exists (G1)
};

struct SupervisorSettings
{
    std::int64_t waitNs = 1'000'000'000LL;          // sigtimedwait timeout while running
    std::int64_t stopWaitNs = 50'000'000LL;         // while a stop is in progress
    std::int64_t watchdogNs = 5'000'000'000LL;      // F3 (both processes, RC2 S9c D-S9c-4)
    std::int64_t stalledNs = 5'000'000'000LL;       // /health STALLED (the same threshold in production)
    std::int64_t joinBoundNs = 2'000'000'000LL;     // F14 / F7: per owner (forwarder 2 s, bridge 7 s)
    SighupPolicy sighupPolicy = SighupPolicy::RESYNC_FLAG;
    const char *processName = "ct-forwarder";      // the exit line and log lines
    const char *threadPrefix = "fwd";              // <prefix>-duty, <prefix>-http (<= 10 characters)
};

// Review NOTE-5: main drains the LogRing through this writer. It never blocks: a record goes out only when the fd is
// writable right now (poll with 0 timeout; a record is < PIPE_BUF, so one write of it cannot block on a pipe), else it is
// dropped and counted. A stuck stderr can therefore not stall the watchdog or the SIGTERM handling.
class NonBlockingLogWriter
{
public:
    explicit NonBlockingLogWriter(int fd) : m_fd(fd) {}
    void operator()(const LogRecord &record) noexcept;
    bool write(const char *text, std::size_t length) noexcept;
    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    int m_fd;
    std::atomic<std::uint64_t> m_dropped{ 0 };
};

class Supervisor
{
public:
    using ExitSeam = std::function<void(int code, const std::string &why)>;
    using LogWriter = std::function<void(const LogRecord &record)>;

    // The owner's extra liveness check, run on main at every wake (after the duty checks): false = exit 70 with `why`.
    using LivenessCheck = std::function<bool(std::int64_t nowNs, std::string &why)>;

    Supervisor(const DutyHeartbeat &board, LogRing &log, SupervisorSettings settings, ExitSeam exitSeam, LogWriter writer);

    // Blocks SIGTERM, SIGINT, SIGHUP and SIGUSR1 in the calling thread: call in main before ANY thread starts,
    // so every thread inherits the mask and only sigtimedwait sees them.
    static void blockSignals();
    // Production exit seam: flush stderr, std::_Exit(code). The line names the process (setProcessName, default
    // "ct-forwarder").
    static void exitProcess(int code, const std::string &why);
    static void setProcessName(const char *name) noexcept;
    // Review NOTE-5: a closed stderr pipe must not kill the process with SIGPIPE (exit 141); writes fail instead.
    static void ignoreSigpipe();
    static std::int64_t monotonicNs();

    // Starts the duty thread (dutyBody) and the fwd-http thread (http->run(), when http is set), supervises,
    // and returns the exit code (after the seam was called, for the 70 paths).
    int run(const std::function<int()> &dutyBody, HttpServer *http);

    // Set before run(); called from main only.
    void setLivenessCheck(LivenessCheck check) { m_liveness = std::move(check); }
    const SupervisorSettings &settings() const noexcept { return m_settings; }

    // ---- read by the duty thread and the HTTP handler
    std::atomic<bool> stopRequested{ false };
    std::atomic<std::int64_t> stopRequestedAtNs{ -1 }; // monotonicNs() of the SIGTERM / SIGINT, -1 before one
    std::atomic<bool> resyncRequested{ false };
    std::atomic<std::uint64_t> sighupCount{ 0 };    // every SIGHUP (COUNT_ONLY: bridge_sighup_ignored_total)
    std::atomic<int> dutyFatal{ -1 };               // set by the duty wrapper on an escaped exception (F2)
    std::atomic<int> httpFatal{ -1 };               // set by the fwd-http wrapper on an escaped exception (NOTE-5)
    std::atomic<std::int64_t> lastAdvanceNs{ 0 };   // when the duty heartbeat last advanced (main's view)

    // The duty heartbeat age as main last saw it (/health STALLED at >= watchdogNs).
    std::int64_t heartbeatAgeNs(std::int64_t nowNs) const noexcept { return nowNs - lastAdvanceNs.load(std::memory_order_acquire); }
    bool stalled(std::int64_t nowNs) const noexcept { return heartbeatAgeNs(nowNs) >= m_settings.stalledNs; }

    // Wakes main's sigtimedwait (the duty body calls it when it returns).
    static void wake() noexcept;

private:
    int exitWith(int code, const std::string &why);
    void drainLogs();

    // Owned here, not on run()'s stack: an abandoned (detached) thread may still touch them after run() returned.
    std::function<int()> m_dutyBody;
    std::atomic<bool> m_dutyDone{ false };
    std::atomic<int> m_dutyCode{ 1 };
    const DutyHeartbeat &m_board;
    LogRing &m_log;
    SupervisorSettings m_settings;
    ExitSeam m_exit;
    LogWriter m_writer;
    LivenessCheck m_liveness;
};

}}}

#endif
