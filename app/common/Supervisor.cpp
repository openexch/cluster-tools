#include "common/Supervisor.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

namespace cluster_tools { namespace app { namespace common {

namespace {

std::atomic<const char *> g_processName{ "ct-forwarder" };

std::string seconds(std::int64_t ns)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g s", static_cast<double>(ns) / 1e9);
    return text;
}

sigset_t supervisedSignals()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    return set;
}

}

Supervisor::Supervisor(const DutyHeartbeat &board, LogRing &log, SupervisorSettings settings, ExitSeam exitSeam, LogWriter writer) :
    m_board(board), m_log(log), m_settings(settings), m_exit(std::move(exitSeam)), m_writer(std::move(writer))
{
}

void Supervisor::blockSignals()
{
    const sigset_t set = supervisedSignals();
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void NonBlockingLogWriter::operator()(const LogRecord &r) noexcept
{
    char line[320];
    const int n = std::snprintf(line, sizeof(line), "%s [%lld] %s\n", nameOf(r.level),
        static_cast<long long>(r.nowNs / 1'000'000), r.text);
    if (n > 0)
    {
        write(line, static_cast<std::size_t>(n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1));
    }
}

bool NonBlockingLogWriter::write(const char *text, std::size_t length) noexcept
{
    pollfd p{ m_fd, POLLOUT, 0 };
    if (::poll(&p, 1, 0) != 1 || 0 == (p.revents & POLLOUT))
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const ssize_t w = ::write(m_fd, text, length);
    if (w != static_cast<ssize_t>(length))
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void Supervisor::ignoreSigpipe()
{
    ::signal(SIGPIPE, SIG_IGN);
}

void Supervisor::setProcessName(const char *name) noexcept
{
    g_processName.store(name, std::memory_order_release);
}

void Supervisor::exitProcess(int code, const std::string &why)
{
    char line[512];
    const int n = std::snprintf(line, sizeof(line), "%s exit %d: %s\n", g_processName.load(std::memory_order_acquire), code,
        why.c_str());
    NonBlockingLogWriter writer(2); // never blocks on a stuck stderr
    if (n > 0)
    {
        writer.write(line, static_cast<std::size_t>(n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1));
    }
    std::_Exit(code);
}

std::int64_t Supervisor::monotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Supervisor::wake() noexcept
{
    ::kill(::getpid(), SIGUSR1);
}

void Supervisor::drainLogs()
{
    if (m_writer)
    {
        m_log.drain(m_writer);
    }
}

int Supervisor::exitWith(int code, const std::string &why)
{
    drainLogs();
    if (m_writer)
    {
        LogRecord r;
        r.level = LogLevel::CRITICAL;
        r.nowNs = monotonicNs();
        std::snprintf(r.text, sizeof(r.text), "supervisor: %s: exit %d", why.c_str(), code);
        m_writer(r);
    }
    if (m_exit)
    {
        m_exit(code, why);
    }
    return code;
}

int Supervisor::run(const std::function<int()> &dutyBody, HttpServer *http)
{
    m_dutyBody = dutyBody;
    m_dutyDone.store(false);
    std::atomic<bool> &dutyDone = m_dutyDone;
    std::atomic<int> &dutyCode = m_dutyCode;
    std::thread duty;
    std::thread httpThread;
    try
    {
        duty = std::thread([this] {
            char name[16];
            std::snprintf(name, sizeof(name), "%s-duty", m_settings.threadPrefix);
            pthread_setname_np(pthread_self(), name);
            std::atomic<bool> &dutyDone = m_dutyDone;
            std::atomic<int> &dutyCode = m_dutyCode;
            int code = 70;
            try
            {
                code = m_dutyBody();
            }
            catch (const std::exception &e)
            {
                m_log.push(LogLevel::CRITICAL, monotonicNs(), "duty thread: exception escaped the duty loop: %s", e.what());
                dutyFatal.store(70, std::memory_order_release);
            }
            catch (...)
            {
                m_log.push(LogLevel::CRITICAL, monotonicNs(), "duty thread: unknown exception escaped the duty loop");
                dutyFatal.store(70, std::memory_order_release);
            }
            dutyCode.store(code, std::memory_order_release);
            dutyDone.store(true, std::memory_order_release);
            wake();
        });
        if (nullptr != http)
        {
            httpThread = std::thread([this, http] {
                char name[16];
                std::snprintf(name, sizeof(name), "%s-http", m_settings.threadPrefix);
                pthread_setname_np(pthread_self(), name);
                try
                {
                    http->run();
                }
                catch (const std::exception &e)
                {
                    m_log.push(LogLevel::CRITICAL, monotonicNs(), "%s-http: exception escaped: %s", m_settings.threadPrefix,
                        e.what());
                    httpFatal.store(70, std::memory_order_release);
                    wake();
                }
                catch (...)
                {
                    m_log.push(LogLevel::CRITICAL, monotonicNs(), "%s-http: unknown exception escaped", m_settings.threadPrefix);
                    httpFatal.store(70, std::memory_order_release);
                    wake();
                }
            });
        }
    }
    catch (const std::exception &e)
    {
        // A thread that cannot start is a process-health failure (70), never an uncaught 134
        const int code = exitWith(70, std::string("a thread could not start: ") + e.what());
        stopRequested.store(true, std::memory_order_release);
        if (duty.joinable())
        {
            duty.detach();
        }
        return code;
    }
    // Abandons the threads (an exit seam that returned, i.e. a test): they are released by their owner.
    auto abandon = [&] {
        if (duty.joinable())
        {
            duty.detach();
        }
        if (httpThread.joinable())
        {
            if (nullptr != http)
            {
                http->shutdown();
            }
            httpThread.detach();
        }
    };

    const sigset_t set = supervisedSignals();
    std::uint64_t lastBeat = m_board.heartbeat();
    lastAdvanceNs.store(monotonicNs(), std::memory_order_release);
    std::int64_t stopAt = -1;
    std::int64_t lastWakeNs = monotonicNs();
    for (;;)
    {
        const std::int64_t waitNs = stopAt >= 0 ? m_settings.stopWaitNs : m_settings.waitNs;
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(waitNs / 1'000'000'000LL);
        ts.tv_nsec = static_cast<long>(waitNs % 1'000'000'000LL);
        const int sig = ::sigtimedwait(&set, nullptr, &ts);
        const std::int64_t now = monotonicNs();
        const std::int64_t wakeGapNs = now - lastWakeNs; // >= the watchdog: this process itself did not run
        lastWakeNs = now;
        if ((SIGTERM == sig || SIGINT == sig) && stopAt < 0)
        {
            stopRequestedAtNs.store(now, std::memory_order_release);
            stopRequested.store(true, std::memory_order_release);
            stopAt = now;
        }
        else if (SIGHUP == sig)
        {
            const std::uint64_t count = sighupCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (SighupPolicy::RESYNC_FLAG == m_settings.sighupPolicy)
            {
                resyncRequested.store(true, std::memory_order_release); // T4
            }
            else if (1 == count)
            {
                m_log.push(LogLevel::WARN, now, "SIGHUP ignored: %s has no operator resync (the core's cursor is the only "
                    "resync input); counted", m_settings.processName);
            }
        }
        drainLogs();
        if (httpFatal.load(std::memory_order_acquire) >= 0)
        {
            const int code = exitWith(70, std::string("the ") + m_settings.threadPrefix +
                "-http thread failed (an exception escaped)");
            abandon();
            return code;
        }
        if (dutyFatal.load(std::memory_order_acquire) >= 0)
        {
            const int code = exitWith(70, "the duty thread reported a fatal error (F2)");
            abandon();
            return code;
        }
        const std::uint64_t beat = m_board.heartbeat();
        if (beat != lastBeat)
        {
            lastBeat = beat;
            lastAdvanceNs.store(now, std::memory_order_release);
        }
        else if (!dutyDone.load(std::memory_order_acquire) && now - lastAdvanceNs.load(std::memory_order_acquire) >= m_settings.watchdogNs)
        {
            const int code = exitWith(70, wakeGapNs >= m_settings.watchdogNs
                ? "duty heartbeat did not advance across a process pause of " + seconds(wakeGapNs) +
                    " (this process did not run: SIGSTOP, a VM or GC freeze), not a wedged duty thread (F3 watchdog)"
                : "duty heartbeat did not advance for " + seconds(m_settings.watchdogNs) + " (F3 watchdog)");
            abandon();
            return code;
        }
        if (m_liveness && !dutyDone.load(std::memory_order_acquire))
        {
            std::string why;
            if (!m_liveness(now, why))
            {
                const int code = exitWith(70, why);
                abandon();
                return code;
            }
        }
        if (dutyDone.load(std::memory_order_acquire))
        {
            break;
        }
        if (stopAt >= 0 && now - stopAt >= m_settings.joinBoundNs)
        {
            const int code = exitWith(70, "the duty thread did not stop within " + seconds(m_settings.joinBoundNs) +
                " (F14 join bound)");
            abandon();
            return code;
        }
    }

    // Stop path: the listening socket is shut down so accept() returns; joins are bounded.
    if (nullptr != http)
    {
        http->shutdown();
        const std::int64_t start = monotonicNs();
        while (!http->finished() && monotonicNs() - start < m_settings.joinBoundNs)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!http->finished())
        {
            const int code = exitWith(70, std::string(m_settings.threadPrefix) + "-http did not stop within " +
                seconds(m_settings.joinBoundNs) + " (F14 join bound)");
            abandon();
            return code;
        }
        httpThread.join();
    }
    duty.join();
    drainLogs();
    return dutyCode.load(std::memory_order_acquire);
}

}}}
