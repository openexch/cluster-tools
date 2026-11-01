#include "common/EmbeddedDriver.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>

extern "C" {
#include "aeronmd.h"
}

#include "common/DriverTimeout.h"

namespace cluster_tools { namespace app { namespace common {

namespace {

// The C driver stamps its CnC heartbeat once per timer interval (aeron_driver_conductor_do_work: every
// timer_interval_ns, pinned to DRIVER_TIMER_INTERVAL_NS below), so a LIVE driver's heartbeat age runs 0..~1 s and a
// single check against a shorter age is a coin toss (a 500 ms check read a live driver as stale about half the time:
// the second instance then deleted the live dir, MainAeronTest embedded). The rule:
//   - wait LIVE_DRIVER_AGE_MS + a margin from the first look: a driver that was already dead then (its last beat is
//     before the first look) is older than LIVE_DRIVER_AGE_MS from that point on, whatever its age was (R-K13 (d): a
//     driver that died a moment ago is not read as live);
//   - then sample for LIVE_DRIVER_SAMPLE_MS (longer than one timer interval): LIVE when any sample is fresher than
//     LIVE_DRIVER_AGE_MS (a live driver beats inside every timer interval).
constexpr std::uint64_t DRIVER_TIMER_INTERVAL_NS =
    static_cast<std::uint64_t>(DRIVER_TIMER_INTERVAL_MS) * 1'000'000ULL; // the floor of every *_DRIVER_TIMEOUT_MS spans two
constexpr std::int64_t LIVE_DRIVER_AGE_MS = 1'500;
constexpr std::int64_t LIVE_DRIVER_MARGIN_MS = 100;
constexpr std::int64_t LIVE_DRIVER_SAMPLE_MS = 1'500;
static_assert(LIVE_DRIVER_AGE_MS * 1'000'000 > static_cast<std::int64_t>(DRIVER_TIMER_INTERVAL_NS), "age above one beat");
static_assert(LIVE_DRIVER_SAMPLE_MS * 1'000'000 > static_cast<std::int64_t>(DRIVER_TIMER_INTERVAL_NS), "a beat inside");

void quietLog(const char *)
{
}

void terminationHook(void *clientd)
{
    static_cast<EmbeddedDriver *>(clientd)->onTerminationHook();
}

std::string lastAeronError(const char *step)
{
    const char *message = aeron_errmsg();
    return std::string(step) + ": " + (nullptr == message || '\0' == *message ? std::string("(no detail)") : std::string(message));
}

}

std::string EmbeddedDriver::dirFor(const std::string &parent, std::int64_t instanceId, const EmbeddedDriverOptions &options)
{
    std::string base = parent;
    while (base.size() > 1 && '/' == base.back())
    {
        base.pop_back();
    }
    if (options.dirPerPid)
    {
        return base + "/" + options.dirPrefix + std::to_string(static_cast<long long>(::getpid())); // RED shape (pre FIX-1)
    }
    return base + "/" + options.dirPrefix + std::to_string(instanceId);
}

EmbeddedDriver::EmbeddedDriver(EmbeddedDriverOptions options, HookAction onTerminate) :
    m_options(std::move(options)), m_onTerminate(std::move(onTerminate))
{
}

EmbeddedDriver::~EmbeddedDriver()
{
    m_stopLoop.store(true, std::memory_order_release);
    if (m_loop.joinable())
    {
        m_loop.join();
    }
    if (m_lockFd >= 0)
    {
        ::close(m_lockFd); // releases the instance lock (an exit does the same)
    }
}

void EmbeddedDriver::onTerminationHook()
{
    if (m_onTerminate)
    {
        m_onTerminate("embedded media driver termination hook: a TerminateDriver request was validated (F2)");
    }
}

bool EmbeddedDriver::start(const std::string &dir, bool manualMainLoop, std::string &error)
{
    m_dir = dir;
    m_manual = manualMainLoop;
    m_failureErrcode = 0;
    if (m_options.instanceLock)
    {
        const std::string lockPath = dir + ".lock";
        const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            error = "embedded driver instance lock " + lockPath + ": " + std::strerror(errno);
            return false;
        }
        if (0 != ::flock(fd, LOCK_EX | LOCK_NB))
        {
            const int err = errno;
            ::close(fd);
            error = EWOULDBLOCK == err
                ? "another " + m_options.processName + " holds the instance lock " + lockPath + " (a second " +
                    m_options.processName + " with this " + m_options.instanceKey + " on this host?): refusing to start"
                : "embedded driver instance lock " + lockPath + ": " + std::strerror(err);
            return false;
        }
        m_lockFd = fd; // held until this object goes (the process lifetime in both processes)
    }
    struct stat st{};
    if (0 == ::stat(dir.c_str(), &st))
    {
        if (!S_ISDIR(st.st_mode))
        {
            error = "embedded driver dir " + dir + " exists and is not a directory";
            return false;
        }
        bool live = false; // EMBEDDED_LIVE_CHECK RED shape (liveCheck off): every existing dir is read as stale
        if (m_options.liveCheck)
        {
            const auto firstLook = std::chrono::steady_clock::now();
            std::this_thread::sleep_until(firstLook + std::chrono::milliseconds(LIVE_DRIVER_AGE_MS + LIVE_DRIVER_MARGIN_MS));
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(LIVE_DRIVER_SAMPLE_MS);
            live = aeron_is_driver_active(dir.c_str(), LIVE_DRIVER_AGE_MS, &quietLog);
            while (!live && std::chrono::steady_clock::now() < until)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                live = aeron_is_driver_active(dir.c_str(), LIVE_DRIVER_AGE_MS, &quietLog);
            }
        }
        if (live)
        {
            error = "a LIVE media driver is already in " + dir + " (a second " + m_options.processName + " with this " +
                m_options.instanceKey + " on this host?): refusing to start"; // a live driver dir is never deleted
            return false;
        }
        m_reclaimed = true; // a stale dir of an earlier run of this instance: reused and cleaned (delete on start)
    }
    else if (ENOENT != errno)
    {
        error = "embedded driver dir " + dir + ": " + std::strerror(errno);
        return false;
    }
    if (aeron_driver_context_init(&m_context) < 0)
    {
        error = lastAeronError("aeron_driver_context_init");
        return false;
    }
    if (aeron_driver_context_set_dir(m_context, dir.c_str()) < 0 ||
        aeron_driver_context_set_threading_mode(m_context, AERON_THREADING_MODE_SHARED) < 0 ||
        aeron_driver_context_set_timer_interval_ns(m_context, DRIVER_TIMER_INTERVAL_NS) < 0 ||
        aeron_driver_context_set_dir_delete_on_start(m_context, true) < 0 ||
        aeron_driver_context_set_dir_delete_on_shutdown(m_context, true) < 0 ||
        aeron_driver_context_set_driver_termination_hook(m_context, &terminationHook, this) < 0)
    {
        error = lastAeronError("the embedded driver context");
        aeron_driver_context_close(m_context);
        m_context = nullptr;
        return false;
    }
    if (aeron_driver_init(&m_driver, m_context) < 0)
    {
        m_failureErrcode = aeron_errcode(); // before any close: ENOSPC = the CnC cannot be allocated (C13-6)
        error = lastAeronError("aeron_driver_init");
        m_driver = nullptr;
        aeron_driver_context_close(m_context);
        m_context = nullptr;
        return false;
    }
    if (aeron_driver_start(m_driver, manualMainLoop) < 0)
    {
        m_failureErrcode = aeron_errcode();
        error = lastAeronError("aeron_driver_start");
        aeron_driver_close(m_driver);
        m_driver = nullptr;
        aeron_driver_context_close(m_context);
        m_context = nullptr;
        return false;
    }
    if (manualMainLoop)
    {
        m_driving.store(true, std::memory_order_release);
        aeron_driver_t *driver = m_driver;
        m_loop = std::thread([this, driver] {
            while (!m_stopLoop.load(std::memory_order_acquire))
            {
                if (m_driving.load(std::memory_order_acquire))
                {
                    const int work = aeron_driver_main_do_work(driver);
                    aeron_driver_main_idle_strategy(driver, work);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // the driver is "dead"
                }
            }
        });
    }
    return true;
}

bool EmbeddedDriver::close(std::int64_t boundMs)
{
    if (nullptr == m_driver)
    {
        return true;
    }
    m_stopLoop.store(true, std::memory_order_release);
    if (m_loop.joinable())
    {
        m_loop.join();
    }
    struct State
    {
        std::mutex lock;
        std::condition_variable done;
        bool finished = false;
    };
    auto state = std::make_shared<State>();
    aeron_driver_t *driver = m_driver;
    aeron_driver_context_t *context = m_context;
    m_driver = nullptr;
    m_context = nullptr;
    std::thread closer([state, driver, context] {
        aeron_driver_close(driver);
        aeron_driver_context_close(context);
        std::lock_guard<std::mutex> guard(state->lock);
        state->finished = true;
        state->done.notify_all();
    });
    std::unique_lock<std::mutex> lock(state->lock);
    const bool finished = state->done.wait_for(lock, std::chrono::milliseconds(boundMs), [&] { return state->finished; });
    lock.unlock();
    if (finished)
    {
        closer.join();
    }
    else
    {
        closer.detach(); // the caller exits the process
    }
    return finished;
}

}}}
