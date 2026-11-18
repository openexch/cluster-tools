// ct_aeron_driver_tests (WITH_AERON, CT_BUILD_MEDIA_DRIVER): the Aeron C media driver as this repository builds it, the
// verified tarball plus the patches dependencies.lock names (aeron_patches), driven in-process over real UDP on loopback.
//
//   ElicitSetupRetry.failedSendStillRegistersThePendingSetup
//       A subscription in a real driver; the test is the remote source of one session, a source that already had a
//       connection: it sends heartbeats and answers only a setup-eliciting status message (SM, S flag) with a setup frame,
//       as a connected Aeron publication does. The driver's first setup-eliciting SM fails in its send path with EPERM
//       (an outgoing UDP interceptor, the seam: the shape a local firewall DROP gives). The session must be back in the
//       initial state within the pending-setup timeout, the next heartbeat must elicit a setup again, and the image
//       must form. The failed send must be in the driver's error log. Without the patch (the RED control,
//       ct_aeron_driver_unpatched_tests: the dispatcher compiled from the pristine tarball file) the driver returns
//       before it registers the pending setup, the session stays PENDING_SETUP_FRAME, no SM is sent again and no image
//       forms: "no image formed".
//
// Settings (environment, each with a default; a malformed value fails the test, it is never replaced by the default):
//   CT_ELICIT_TEST_HEARTBEAT_MS     100    the fake source's heartbeat interval (an Aeron publication's is 100 ms)
//   CT_ELICIT_TEST_IMAGE_WAIT_MS    10000  how long the image may take after the subscription is up
//   CT_ELICIT_TEST_RETRY_BOUND_MS   2500   the bound from the failed send to the next setup-eliciting SM: Aeron's pending
//                                          setup timeout (1000 ms, a constant of the C driver) plus heartbeat and receiver
//                                          duty-cycle slack
//   CT_ELICIT_TEST_AERON_PARENT     /dev/shm  the parent of the driver's per-run directory
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

// the driver headers are Aeron's own: their unused parameters are not this test's warnings
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
extern "C"
{
#include "aeronc.h"
#include "aeronmd.h"
#include "aeron_alloc.h"
#include "media/aeron_udp_channel_transport_bindings.h"
#include "protocol/aeron_udp_protocol.h"
#include "util/aeron_error.h"
}
#pragma GCC diagnostic pop

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t setting(const char *name, std::int64_t fallback)
{
    const char *raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0')
    {
        return fallback;
    }
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value <= 0)
    {
        FAIL(name << "=" << raw << " is not a positive integer");
    }
    return value;
}

std::string textSetting(const char *name, const char *fallback)
{
    const char *raw = std::getenv(name);
    return (raw == nullptr || *raw == '\0') ? std::string(fallback) : std::string(raw);
}

// ---- the seam: an outgoing UDP interceptor that fails the next N setup-eliciting SMs with EPERM

std::atomic<int> g_failuresLeft{0};
std::atomic<int> g_failuresInjected{0};
std::atomic<std::int64_t> g_failedAtNs{0};
constexpr const char *INJECTED_ERROR = "failed to sendmmsg (injected by ct_aeron_driver_tests)";

std::int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

bool isSetupElicitingSm(const std::uint8_t *frame, std::size_t length)
{
    if (length < sizeof(aeron_status_message_header_t))
    {
        return false;
    }
    aeron_frame_header_t header;
    std::memcpy(&header, frame, sizeof(header));
    return header.type == AERON_HDR_TYPE_SM && (header.flags & AERON_STATUS_MESSAGE_HEADER_SEND_SETUP_FLAG) != 0;
}

extern "C" int ctFailOnceInit(void **state, aeron_driver_context_t *, aeron_udp_channel_transport_affinity_t)
{
    *state = nullptr;
    return 0;
}

extern "C" int ctFailOnceSend(
    void *, aeron_udp_channel_outgoing_interceptor_t *delegate, aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *address, struct iovec *iov, size_t iovLength, int64_t *bytesSent)
{
    if (iovLength >= 1 && isSetupElicitingSm(static_cast<const std::uint8_t *>(iov[0].iov_base), iov[0].iov_len))
    {
        int left = g_failuresLeft.load();
        while (left > 0 && !g_failuresLeft.compare_exchange_weak(left, left - 1))
        {
        }
        if (left > 0)
        {
            g_failedAtNs.store(nowNs());
            g_failuresInjected.fetch_add(1);
            AERON_SET_ERR(EPERM, "%s", INJECTED_ERROR);
            return -1;
        }
    }
    return delegate->outgoing_send_func(
        delegate->interceptor_state, delegate->next_interceptor, transport, address, iov, iovLength, bytesSent);
}

aeron_udp_channel_interceptor_bindings_t *failOnceBindings()
{
    aeron_udp_channel_interceptor_bindings_t *bindings = nullptr;
    // freed by aeron_driver_context_close (it frees every interceptor binding with aeron_free)
    if (aeron_alloc(reinterpret_cast<void **>(&bindings), sizeof(*bindings)) < 0)
    {
        return nullptr;
    }
    bindings->outgoing_init_func = ctFailOnceInit;
    bindings->outgoing_send_func = ctFailOnceSend;
    bindings->meta_info.name = "ct-fail-setup-eliciting-sm";
    bindings->meta_info.type = "interceptor";
    bindings->meta_info.next_interceptor_bindings = nullptr;
    return bindings;
}

// ---- the remote source: one session, heartbeats, a setup frame only when asked

constexpr std::int32_t STREAM_ID = 1001;
constexpr std::int32_t SESSION_ID = 0x5e7u;
constexpr std::int32_t INITIAL_TERM_ID = 7;
constexpr std::int32_t TERM_LENGTH = 64 * 1024;
constexpr std::int32_t MTU = 1408;

struct FakeSource
{
    int fd = -1;
    sockaddr_in driver{};

    ~FakeSource()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    void sendFrame(const void *frame, std::size_t length) const
    {
        const ssize_t sent =
            ::sendto(fd, frame, length, 0, reinterpret_cast<const sockaddr *>(&driver), sizeof(driver));
        REQUIRE_MESSAGE(sent == static_cast<ssize_t>(length), "fake source sendto: " << std::strerror(errno));
    }

    void heartbeat() const
    {
        aeron_data_header_t frame{};
        frame.frame_header.frame_length = 0;  // a heartbeat: a data header with no payload
        frame.frame_header.version = AERON_FRAME_HEADER_VERSION;
        frame.frame_header.flags = AERON_DATA_HEADER_BEGIN_FLAG | AERON_DATA_HEADER_END_FLAG;
        frame.frame_header.type = AERON_HDR_TYPE_DATA;
        frame.term_offset = 0;
        frame.session_id = SESSION_ID;
        frame.stream_id = STREAM_ID;
        frame.term_id = INITIAL_TERM_ID;
        sendFrame(&frame, sizeof(frame));
    }

    void setup() const
    {
        aeron_setup_header_t frame{};
        frame.frame_header.frame_length = sizeof(aeron_setup_header_t);
        frame.frame_header.version = AERON_FRAME_HEADER_VERSION;
        frame.frame_header.type = AERON_HDR_TYPE_SETUP;
        frame.term_offset = 0;
        frame.session_id = SESSION_ID;
        frame.stream_id = STREAM_ID;
        frame.initial_term_id = INITIAL_TERM_ID;
        frame.active_term_id = INITIAL_TERM_ID;
        frame.term_length = TERM_LENGTH;
        frame.mtu = MTU;
        frame.ttl = 0;
        sendFrame(&frame, sizeof(frame));
    }

    // Drains what the driver sent here; returns how many setup-eliciting SMs for our session arrived.
    int drainElicitingSms() const
    {
        int eliciting = 0;
        std::uint8_t buffer[2048];
        for (;;)
        {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n < 0)
            {
                break;
            }
            if (isSetupElicitingSm(buffer, static_cast<std::size_t>(n)))
            {
                aeron_status_message_header_t sm;
                std::memcpy(&sm, buffer, sizeof(sm));
                if (sm.session_id == SESSION_ID && sm.stream_id == STREAM_ID)
                {
                    ++eliciting;
                }
            }
        }
        return eliciting;
    }
};

int boundUdpSocket(std::uint16_t port, std::uint16_t *boundPort)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    socklen_t len = sizeof(addr);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0)
    {
        ::close(fd);
        return -1;
    }
    *boundPort = ntohs(addr.sin_port);
    return fd;
}

std::atomic<int> g_imagesAvailable{0};

extern "C" void onAvailableImage(void *, aeron_subscription_t *, aeron_image_t *image)
{
    aeron_image_constants_t constants;
    if (aeron_image_constants(image, &constants) == 0 && constants.session_id == SESSION_ID)
    {
        g_imagesAvailable.fetch_add(1);
    }
}

struct ErrorLogScan
{
    bool injected = false;
    std::string records;
};

extern "C" void onErrorRecord(int32_t, int64_t, int64_t, const char *error, size_t length, void *clientd)
{
    auto *scan = static_cast<ErrorLogScan *>(clientd);
    const std::string record(error, length);
    scan->records += record + "\n";
    if (record.find(INJECTED_ERROR) != std::string::npos)
    {
        scan->injected = true;
    }
}

// The driver and the client in this process, torn down in reverse order whatever the test outcome.
struct DriverAndClient
{
    aeron_driver_context_t *driverContext = nullptr;
    aeron_driver_t *driver = nullptr;
    aeron_context_t *clientContext = nullptr;
    aeron_t *client = nullptr;
    std::string dir;

    ~DriverAndClient()
    {
        if (client != nullptr)
        {
            aeron_close(client);
        }
        if (clientContext != nullptr)
        {
            aeron_context_close(clientContext);
        }
        if (driver != nullptr)
        {
            aeron_driver_close(driver);
        }
        if (driverContext != nullptr)
        {
            aeron_driver_context_close(driverContext);
        }
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
};

}  // namespace

TEST_CASE("ElicitSetupRetry.failedSendStillRegistersThePendingSetup")
{
    const std::int64_t heartbeatMs = setting("CT_ELICIT_TEST_HEARTBEAT_MS", 100);
    const std::int64_t imageWaitMs = setting("CT_ELICIT_TEST_IMAGE_WAIT_MS", 10000);
    const std::int64_t retryBoundMs = setting("CT_ELICIT_TEST_RETRY_BOUND_MS", 2500);
    const std::string parent = textSetting("CT_ELICIT_TEST_AERON_PARENT", "/dev/shm");

    g_failuresLeft.store(1);
    g_failuresInjected.store(0);
    g_failedAtNs.store(0);
    g_imagesAvailable.store(0);

    // The driver's subscription port, reserved by binding it and handed over (closed just before the driver binds).
    std::uint16_t driverPort = 0;
    const int reserve = boundUdpSocket(0, &driverPort);
    REQUIRE_MESSAGE(reserve >= 0, "no free UDP port on loopback: " << std::strerror(errno));

    FakeSource source;
    std::uint16_t sourcePort = 0;
    source.fd = boundUdpSocket(0, &sourcePort);
    REQUIRE_MESSAGE(source.fd >= 0, "fake source socket: " << std::strerror(errno));
    source.driver.sin_family = AF_INET;
    source.driver.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    source.driver.sin_port = htons(driverPort);

    DriverAndClient run;
    run.dir = parent + "/ct-aeron-elicit-" + std::to_string(::getpid());

    REQUIRE_MESSAGE(aeron_driver_context_init(&run.driverContext) == 0, aeron_errmsg());
    aeron_driver_context_set_dir(run.driverContext, run.dir.c_str());
    aeron_driver_context_set_dir_delete_on_start(run.driverContext, true);
    aeron_driver_context_set_dir_delete_on_shutdown(run.driverContext, true);
    aeron_driver_context_set_threading_mode(run.driverContext, AERON_THREADING_MODE_SHARED);
    aeron_driver_context_set_term_buffer_sparse_file(run.driverContext, true);
    aeron_udp_channel_interceptor_bindings_t *bindings = failOnceBindings();
    REQUIRE_MESSAGE(bindings != nullptr, "interceptor bindings: " << aeron_errmsg());
    REQUIRE(aeron_driver_context_set_udp_channel_outgoing_interceptors(run.driverContext, bindings) == 0);
    REQUIRE_MESSAGE(aeron_driver_init(&run.driver, run.driverContext) == 0, "driver init: " << aeron_errmsg());
    REQUIRE_MESSAGE(aeron_driver_start(run.driver, false) == 0, "driver start: " << aeron_errmsg());

    REQUIRE(aeron_context_init(&run.clientContext) == 0);
    aeron_context_set_dir(run.clientContext, run.dir.c_str());
    REQUIRE_MESSAGE(aeron_init(&run.client, run.clientContext) == 0, "client init: " << aeron_errmsg());
    REQUIRE_MESSAGE(aeron_start(run.client) == 0, "client start: " << aeron_errmsg());

    ::close(reserve);
    const std::string channel = "aeron:udp?endpoint=127.0.0.1:" + std::to_string(driverPort);
    aeron_async_add_subscription_t *pending = nullptr;
    REQUIRE(aeron_async_add_subscription(&pending, run.client, channel.c_str(), STREAM_ID, onAvailableImage, nullptr,
                nullptr, nullptr) == 0);
    aeron_subscription_t *subscription = nullptr;
    const auto addDeadline = Clock::now() + std::chrono::milliseconds(imageWaitMs);
    int polled = 0;
    while ((polled = aeron_async_add_subscription_poll(&subscription, pending)) == 0 && Clock::now() < addDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_MESSAGE(polled == 1, "subscription " << channel << " not added: " << aeron_errmsg());

    // The source runs: a heartbeat every interval, a setup frame for each setup-eliciting SM.
    int elicitingSms = 0;
    int elicitingAfterFailure = 0;
    std::optional<std::int64_t> retryDelayMs;
    const auto deadline = Clock::now() + std::chrono::milliseconds(imageWaitMs);
    auto nextHeartbeat = Clock::now();
    while (g_imagesAvailable.load() == 0 && Clock::now() < deadline)
    {
        if (Clock::now() >= nextHeartbeat)
        {
            source.heartbeat();
            nextHeartbeat += std::chrono::milliseconds(heartbeatMs);
        }
        const int arrived = source.drainElicitingSms();
        if (arrived > 0)
        {
            elicitingSms += arrived;
            const std::int64_t failedAt = g_failedAtNs.load();
            if (failedAt != 0)
            {
                elicitingAfterFailure += arrived;
                if (!retryDelayMs)
                {
                    retryDelayMs = (nowNs() - failedAt) / 1000000;
                }
            }
            source.setup();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    MESSAGE("injected send failures " << g_failuresInjected.load() << ", setup-eliciting SMs received " << elicitingSms
            << " (after the failure " << elicitingAfterFailure << "), retry after "
            << (retryDelayMs ? std::to_string(*retryDelayMs) + " ms" : std::string("never")) << ", images "
            << g_imagesAvailable.load());

    // The seam fired: without it the test would prove nothing.
    REQUIRE_MESSAGE(g_failuresInjected.load() == 1,
        "the injected send failure never fired: the driver sent no setup-eliciting SM through the interceptor");
    REQUIRE_MESSAGE(g_imagesAvailable.load() == 1,
        "no image formed within " << imageWaitMs << " ms after the injected send_sm failure: the session was not "
        "returned to the initial state and no later heartbeat elicited a setup (setup-eliciting SMs after the "
        "failure: " << elicitingAfterFailure << ")");
    REQUIRE(retryDelayMs.has_value());
    CHECK_MESSAGE(*retryDelayMs <= retryBoundMs,
        "the setup was elicited again " << *retryDelayMs << " ms after the failed send, bound " << retryBoundMs << " ms");

    // The failed send is in the driver's error log, as the receiver's other send errors are.
    aeron_cnc_t *cnc = nullptr;
    REQUIRE_MESSAGE(aeron_cnc_init(&cnc, run.dir.c_str(), 1000) == 0, "cnc: " << aeron_errmsg());
    ErrorLogScan scan;
    aeron_cnc_error_log_read(cnc, onErrorRecord, &scan, 0);
    aeron_cnc_close(cnc);
    CHECK_MESSAGE(scan.injected, "the failed send is not in the driver error log; records:\n" << scan.records);
}
