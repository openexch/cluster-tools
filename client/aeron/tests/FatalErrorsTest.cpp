// Fatal Aeron error classification and the monitor's once-only action. The real driver-death
// exit is proven by the it/fatal-exit.sh integration test.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <Aeron.h>
#include "aeron_client_error.h"

#include "cluster_tools/client/aeron/FatalErrors.h"

using namespace cluster_tools::client::aeron_transport;
using namespace aeron::util;

TEST_CASE("driver, client and conductor timeouts are fatal; ordinary errors are not")
{
    CHECK(isFatalAeronError(DriverTimeoutException("driver gone", SOURCEINFO, 0)));
    CHECK(isFatalAeronError(ClientTimeoutException("client timed out", SOURCEINFO, 0)));
    CHECK(isFatalAeronError(ConductorServiceTimeoutException("service interval", SOURCEINFO, 0)));
    CHECK_FALSE(isFatalAeronError(IllegalArgumentException("bad channel", SOURCEINFO, 0)));
    CHECK_FALSE(isFatalAeronError(std::runtime_error("other")));
}

TEST_CASE("the conductor's real driver-timeout report (positive code, plain AeronException) is fatal")
{
    // What Context::errorHandlerCallback builds for aeron_client_conductor.c's
    // error_handler(..., AERON_CLIENT_ERROR_DRIVER_TIMEOUT, "MediaDriver keepalive: age=...").
    bool mapped = false;
    try
    {
        AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(AERON_CLIENT_ERROR_DRIVER_TIMEOUT, "MediaDriver keepalive: age=1012ms");
    }
    catch (const SourcedException &e)
    {
        mapped = true;
        CHECK((nullptr == dynamic_cast<const DriverTimeoutException *>(&e))); // the wrapper quirk itself
        CHECK(isFatalAeronError(e));
    }
    CHECK(mapped);
    try
    {
        AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(AERON_CLIENT_ERROR_CLIENT_TIMEOUT, "client timeout");
    }
    catch (const SourcedException &e)
    {
        CHECK(isFatalAeronError(e));
    }
    try
    {
        AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(EINVAL, "bad uri");
    }
    catch (const SourcedException &e)
    {
        CHECK_FALSE(isFatalAeronError(e));
    }
}

TEST_CASE("the monitor runs its action exactly once and keeps the first message")
{
    std::vector<std::string> fired;
    AeronFatalMonitor monitor([&](const std::string &m) { fired.push_back(m); });
    monitor.onError(IllegalStateException("not fatal", SOURCEINFO, 0));
    CHECK_FALSE(monitor.isFatal());
    monitor.onError(DriverTimeoutException("driver has been inactive", SOURCEINFO, 0));
    monitor.onError(ClientTimeoutException("second", SOURCEINFO, 0));
    CHECK(monitor.isFatal());
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].find("driver has been inactive") != std::string::npos);
    CHECK(monitor.errorCount() == 3);
}

TEST_CASE("an unexpected client close is fatal; a deliberate one is not")
{
    int fired = 0;
    AeronFatalMonitor unexpected([&](const std::string &) { fired++; });
    unexpected.onClientClosed();
    CHECK(fired == 1);

    AeronFatalMonitor deliberate([&](const std::string &) { fired++; });
    deliberate.beginDeliberateClose();
    deliberate.onClientClosed();
    CHECK(fired == 1);
    CHECK_FALSE(deliberate.isFatal());
}

// ---- added by the S11 independent review (notebook .../RC2-me-link/S11/review.md) ----

TEST_CASE("review: every terminating conductor report is fatal in the shape the C conductor sends it; buffer-full is not")
{
    // aeron_client_conductor.c reports driver timeout, the heartbeat-counter loss (also code
    // DRIVER_TIMEOUT), client timeout and the conductor service-interval timeout with POSITIVE
    // codes via Context::errorHandlerCallback -> AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW.
    const int fatalCodes[] = { AERON_CLIENT_ERROR_DRIVER_TIMEOUT, AERON_CLIENT_ERROR_CLIENT_TIMEOUT,
        AERON_CLIENT_ERROR_CONDUCTOR_SERVICE_TIMEOUT, -AERON_CLIENT_ERROR_DRIVER_TIMEOUT,
        -AERON_CLIENT_ERROR_CLIENT_TIMEOUT, -AERON_CLIENT_ERROR_CONDUCTOR_SERVICE_TIMEOUT };
    for (int code : fatalCodes)
    {
        bool caught = false;
        try
        {
            AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(code, "terminating");
        }
        catch (const SourcedException &e)
        {
            caught = true;
            CHECK_MESSAGE(isFatalAeronError(e), "code " << code);
        }
        CHECK(caught);
    }
    const int nonFatalCodes[] = { AERON_CLIENT_ERROR_BUFFER_FULL, AERON_CLIENT_ERROR_DRIVER_BUFFER_FULL };
    for (int code : nonFatalCodes)
    {
        try
        {
            AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(code, "transient");
        }
        catch (const SourcedException &e)
        {
            CHECK_FALSE_MESSAGE(isFatalAeronError(e), "code " << code);
        }
    }

    // The monitor fires on the conductor's service-interval report too.
    int fired = 0;
    AeronFatalMonitor monitor([&](const std::string &) { fired++; });
    try
    {
        AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(AERON_CLIENT_ERROR_CONDUCTOR_SERVICE_TIMEOUT, "service interval exceeded");
    }
    catch (const SourcedException &e)
    {
        monitor.onError(e);
    }
    CHECK(fired == 1);
    CHECK(monitor.isFatal());
}

TEST_CASE("F1: RESOURCE_TEMPORARILY_UNAVAILABLE from the driver is a retry, any other registration error is not")
{
    for (const int code : { AERON_ERROR_CODE_RESOURCE_TEMPORARILY_UNAVAILABLE, -AERON_ERROR_CODE_RESOURCE_TEMPORARILY_UNAVAILABLE })
    {
        try
        {
            AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(code, "send channel endpoint is closing");
        }
        catch (const SourcedException &e)
        {
            CHECK(isTransientRegistrationError(e));
            CHECK_FALSE(isFatalAeronError(e));
        }
    }
    try
    {
        AERON_MAP_TO_SOURCED_EXCEPTION_AND_THROW(EINVAL, "invalid channel");
    }
    catch (const SourcedException &e)
    {
        CHECK_FALSE(isTransientRegistrationError(e));
    }
    CHECK_FALSE(isTransientRegistrationError(std::runtime_error("x")));
}

TEST_CASE("F4: connectAeron refuses to run without a monitor it can own")
{
    AeronClientOptions options;
    CHECK_THROWS_AS(connectAeron(options, nullptr), std::invalid_argument);
}
