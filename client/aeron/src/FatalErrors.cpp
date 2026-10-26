#include "cluster_tools/client/aeron/FatalErrors.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include <Aeron.h>
#include "aeron_client_error.h"

namespace cluster_tools { namespace client { namespace aeron_transport {

bool isFatalAeronError(const std::exception &error) noexcept
{
    if (nullptr != dynamic_cast<const ::aeron::util::DriverTimeoutException *>(&error) ||
        nullptr != dynamic_cast<const ::aeron::util::ClientTimeoutException *>(&error) ||
        nullptr != dynamic_cast<const ::aeron::util::ConductorServiceTimeoutException *>(&error))
    {
        return true;
    }
    if (const auto *sourced = dynamic_cast<const ::aeron::util::SourcedException *>(&error))
    {
        // The C client conductor reports its timeouts with POSITIVE codes
        // (aeron_client_conductor.c: AERON_CLIENT_ERROR_DRIVER_TIMEOUT = 1000, ...) while the
        // cpp_wrapper maps only the NEGATED codes to DriverTimeoutException & co.
        // (util/Exceptions.h AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW), so a real driver death
        // arrives as a plain AeronException of category ERROR carrying code 1000. Classify by
        // the code, either sign. Found by it/run-it.sh fatal-exit: the type check alone let a
        // dead driver leave a live, idle process (the 2026-08-12 zombie shape).
        const std::int32_t code = sourced->errorCode() < 0 ? -sourced->errorCode() : sourced->errorCode();
        if (AERON_CLIENT_ERROR_DRIVER_TIMEOUT == code || AERON_CLIENT_ERROR_CLIENT_TIMEOUT == code ||
            AERON_CLIENT_ERROR_CONDUCTOR_SERVICE_TIMEOUT == code)
        {
            return true;
        }
        return ::aeron::util::ExceptionCategory::EXCEPTION_CATEGORY_FATAL == sourced->category();
    }
    return false;
}

bool isTransientRegistrationError(const std::exception &error) noexcept
{
    const auto *sourced = dynamic_cast<const ::aeron::util::SourcedException *>(&error);
    if (nullptr == sourced)
    {
        return false;
    }
    const std::int32_t code = sourced->errorCode() < 0 ? -sourced->errorCode() : sourced->errorCode();
    return AERON_ERROR_CODE_RESOURCE_TEMPORARILY_UNAVAILABLE == code;
}

AeronFatalMonitor::AeronFatalMonitor() : m_action(&AeronFatalMonitor::exitProcess)
{
}

AeronFatalMonitor::AeronFatalMonitor(FatalAction action) : m_action(std::move(action))
{
}

void AeronFatalMonitor::exitProcess(const std::string &message)
{
    std::fprintf(stderr, "FATAL aeron client error, exiting %d: %s\n", FATAL_EXIT_CODE, message.c_str());
    std::fflush(stderr);
    std::_Exit(FATAL_EXIT_CODE);
}

void AeronFatalMonitor::onError(const std::exception &error)
{
    m_errors.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_lastError = error.what();
    }
    if (isFatalAeronError(error))
    {
        fatal(error.what());
        return;
    }
    std::fprintf(stderr, "aeron client error (non-fatal): %s\n", error.what());
}

void AeronFatalMonitor::onClientClosed()
{
    if (!m_deliberateClose.load(std::memory_order_acquire))
    {
        fatal("aeron client closed without the owner closing it");
    }
}

void AeronFatalMonitor::beginDeliberateClose()
{
    m_deliberateClose.store(true, std::memory_order_release);
}

std::string AeronFatalMonitor::fatalMessage() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_fatalMessage;
}

std::string AeronFatalMonitor::lastError() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_lastError;
}

void AeronFatalMonitor::fatal(const std::string &message)
{
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_fatalMessage.empty())
        {
            m_fatalMessage = message;
        }
    }
    const bool first = !m_fatal.exchange(true, std::memory_order_acq_rel);
    if (first && m_action)
    {
        m_action(message);
    }
}

std::shared_ptr<::aeron::Aeron> connectAeron(
    const AeronClientOptions &options, std::shared_ptr<AeronFatalMonitor> monitor)
{
    if (!monitor)
    {
        throw std::invalid_argument("connectAeron: a fatal monitor is required");
    }
    ::aeron::Context context;
    if (!options.aeronDir.empty())
    {
        context.aeronDir(options.aeronDir);
    }
    context.clientName(options.clientName);
    context.mediaDriverTimeout(static_cast<long>(options.driverTimeoutMs));
    context.errorHandler([monitor](const std::exception &error) { monitor->onError(error); });
    context.closeClientHandler([monitor]() { monitor->onClientClosed(); });
    return ::aeron::Aeron::connect(context);
}

}}}
