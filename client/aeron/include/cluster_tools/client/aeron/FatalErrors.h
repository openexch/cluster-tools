#ifndef CLUSTER_TOOLS_CLIENT_AERON_FATAL_ERRORS_H
#define CLUSTER_TOOLS_CLIENT_AERON_FATAL_ERRORS_H

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace aeron { class Aeron; }

// Fatal Aeron errors end the process, loudly and at once (rc2-bridge-cpp.md P5): the
// 2026-08-12 bridge zombie was a Java client whose DriverTimeoutException left a live pid
// with no work for 27 h. Here the error handler of the aeron::Context classifies every error
// the client conductor reports; a fatal one (driver timeout, client timeout, conductor
// service timeout, any FATAL-category error, or the client closing without our close())
// is written to stderr and the policy runs - by default std::_Exit(70) from the conductor
// thread, so no wedged duty thread or shutdown hook can keep a half-alive process.

namespace cluster_tools { namespace client { namespace aeron_transport {

inline constexpr int FATAL_EXIT_CODE = 70; // EX_SOFTWARE

// True for errors after which the Aeron client is unusable.
bool isFatalAeronError(const std::exception &error) noexcept;

// True for a driver answer that means "try the same registration again"
// (AERON_ERROR_CODE_RESOURCE_TEMPORARILY_UNAVAILABLE, e.g. the send channel endpoint of a
// just-closed publication to the same member is still CLOSING). AeronCluster retries it within
// messageTimeout (addNewLeaderIngressPublication, MemberIngress.asyncGetPublication).
bool isTransientRegistrationError(const std::exception &error) noexcept;

class AeronFatalMonitor
{
public:
    using FatalAction = std::function<void(const std::string &message)>;

    // Default action: log to stderr and std::_Exit(FATAL_EXIT_CODE).
    AeronFatalMonitor();
    explicit AeronFatalMonitor(FatalAction action);

    // The aeron::Context errorHandler.
    void onError(const std::exception &error);
    // The aeron::Context closeClientHandler.
    void onClientClosed();
    // Call before releasing the aeron::Aeron on purpose (shutdown), so its close is not fatal.
    void beginDeliberateClose();

    bool isFatal() const { return m_fatal.load(std::memory_order_acquire); }
    std::uint64_t errorCount() const { return m_errors.load(std::memory_order_relaxed); }
    std::string fatalMessage() const;
    std::string lastError() const;

    static void exitProcess(const std::string &message);

private:
    void fatal(const std::string &message);

    FatalAction m_action;
    std::atomic<bool> m_fatal{ false };
    std::atomic<bool> m_deliberateClose{ false };
    std::atomic<std::uint64_t> m_errors{ 0 };
    mutable std::mutex m_lock;
    std::string m_fatalMessage;
    std::string m_lastError;
};

struct AeronClientOptions
{
    std::string aeronDir;              // empty = the Aeron default for this host
    std::string clientName = "cluster-tools";
    std::int64_t driverTimeoutMs = 10'000;
};

// Connects one aeron::Aeron with the monitor wired in. The conductor thread's error and close
// handlers hold the monitor by shared_ptr, so it lives as long as the Aeron client does, however
// the owner orders its members. Throws (the wrapper's exception) when no driver answers at
// start: a process that cannot reach its driver must not start.
std::shared_ptr<::aeron::Aeron> connectAeron(
    const AeronClientOptions &options, std::shared_ptr<AeronFatalMonitor> monitor);

}}}

#endif
