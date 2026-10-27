#ifndef CLUSTER_TOOLS_APP_FORWARDER_HTTP_SERVER_H
#define CLUSTER_TOOLS_APP_FORWARDER_HTTP_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// HttpServer - fwd-http (RC2 S12 contract section 3, FIX-8): a minimal blocking HTTP/1.0 server for GET
// /health, /ready, /metrics and POST /resync. It owns no liveness decision and never touches a session, the
// ring or the domain: the handler reads the StatusBoard and at most sets the resync atomic. Every accepted
// socket gets SO_RCVTIMEO / SO_SNDTIMEO of 2 s, so a client that connects and sends nothing cannot hold the
// thread; shutdown() shuts the listening socket down so a blocked accept() returns (the stop path, F14).

namespace cluster_tools { namespace app { namespace forwarder {

struct HttpRequest
{
    std::string method;
    std::string path;
};

struct HttpResponse
{
    int status = 404;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
};

class HttpServer
{
public:
    static constexpr int SOCKET_TIMEOUT_MS = 2000; // the whole request (read) has this long; the send too

    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    explicit HttpServer(Handler handler) : m_handler(std::move(handler)) {}
    ~HttpServer();

    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // Bind and listen (port 0 = ephemeral; boundPort() says which). False with error on failure.
    bool bind(const std::string &host, std::uint16_t port, std::string &error);
    std::uint16_t boundPort() const noexcept { return m_port; }

    // The accept loop (the fwd-http thread); returns after shutdown(). An exception from the handler propagates to the
    // caller (the Supervisor maps it to exit 70, review NOTE-5).
    void run();
    void shutdown() noexcept;
    bool finished() const noexcept { return m_finished.load(std::memory_order_acquire); }
    std::uint64_t served() const noexcept { return m_served.load(std::memory_order_relaxed); }
    // Requests abandoned at the whole-request deadline (review FIX-2).
    std::uint64_t timedOut() const noexcept { return m_timedOut.load(std::memory_order_relaxed); }

private:
    void serve(int fd);

    Handler m_handler;
    int m_listenFd = -1;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_shutdown{ false };
    std::atomic<bool> m_finished{ false };
    std::atomic<std::uint64_t> m_served{ 0 };
    std::atomic<std::uint64_t> m_timedOut{ 0 };
};

std::string statusText(int status);

}}}

#endif
