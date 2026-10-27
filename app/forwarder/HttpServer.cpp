#include "HttpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace cluster_tools { namespace app { namespace forwarder {

std::string statusText(int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

HttpServer::~HttpServer()
{
    shutdown();
    if (m_listenFd >= 0)
    {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
}

bool HttpServer::bind(const std::string &host, std::uint16_t port, std::string &error)
{
    m_listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0)
    {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int one = 1;
    ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string h = ("localhost" == host) ? "127.0.0.1" : host;
    if (1 != ::inet_pton(AF_INET, h.c_str(), &addr.sin_addr))
    {
        error = "FWD_HTTP_BIND host '" + host + "' is not an IPv4 address";
        return false;
    }
    if (0 != ::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        error = "bind " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
        return false;
    }
    if (0 != ::listen(m_listenFd, 16))
    {
        error = std::string("listen: ") + std::strerror(errno);
        return false;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(m_listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    return true;
}

void HttpServer::run()
{
    while (!m_shutdown.load(std::memory_order_acquire))
    {
        const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
        {
            if (EINTR == errno && !m_shutdown.load(std::memory_order_acquire))
            {
                continue;
            }
            break; // shutdown() (EINVAL) or a dead listener: the thread ends
        }
        try
        {
            serve(fd);
        }
        catch (...)
        {
            ::close(fd); // the client sees EOF; the caller maps the exception to exit 70 (NOTE-5)
            m_finished.store(true, std::memory_order_release);
            throw;
        }
        ::close(fd);
    }
    m_finished.store(true, std::memory_order_release);
}

void HttpServer::shutdown() noexcept
{
    if (!m_shutdown.exchange(true) && m_listenFd >= 0)
    {
        ::shutdown(m_listenFd, SHUT_RDWR); // a blocked accept() returns
    }
}

namespace {
std::int64_t monotonicMs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}
}

void HttpServer::serve(int fd)
{
    timeval tv{};
    tv.tv_sec = SOCKET_TIMEOUT_MS / 1000;
    tv.tv_usec = (SOCKET_TIMEOUT_MS % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Review FIX-2: ONE deadline for the whole request, not a per-recv timeout: a client trickling a byte every 1.5 s
    // must not hold the only fwd-http thread (and with it /health and /ready).
    const std::int64_t deadline = monotonicMs() + SOCKET_TIMEOUT_MS;
    char buf[4096];
    std::size_t used = 0;
    while (used < sizeof(buf) - 1)
    {
        const std::int64_t remaining = deadline - monotonicMs();
        if (remaining <= 0)
        {
            m_timedOut.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pollfd p{ fd, POLLIN, 0 };
        const int ready = ::poll(&p, 1, static_cast<int>(remaining));
        if (ready <= 0)
        {
            if (ready == 0)
            {
                m_timedOut.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        const ssize_t n = ::recv(fd, buf + used, sizeof(buf) - 1 - used, MSG_DONTWAIT);
        if (n <= 0)
        {
            return; // timeout, reset or EOF before a request line: nothing served
        }
        used += static_cast<std::size_t>(n);
        buf[used] = '\0';
        if (nullptr != std::strstr(buf, "\r\n\r\n") || nullptr != std::strstr(buf, "\n\n"))
        {
            break;
        }
    }
    buf[used] = '\0';
    HttpRequest request;
    const char *sp1 = std::strchr(buf, ' ');
    const char *sp2 = nullptr == sp1 ? nullptr : std::strchr(sp1 + 1, ' ');
    HttpResponse response;
    if (nullptr == sp1 || nullptr == sp2)
    {
        response.status = 400;
        response.body = "bad request\n";
    }
    else
    {
        request.method.assign(buf, static_cast<std::size_t>(sp1 - buf));
        request.path.assign(sp1 + 1, static_cast<std::size_t>(sp2 - sp1 - 1));
        response = m_handler(request);
    }
    std::string out = "HTTP/1.0 " + std::to_string(response.status) + " " + statusText(response.status) +
        "\r\nContent-Type: " + response.contentType + "\r\nContent-Length: " + std::to_string(response.body.size()) +
        "\r\nConnection: close\r\n\r\n" + response.body;
    std::size_t sent = 0;
    while (sent < out.size())
    {
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
    m_served.fetch_add(1, std::memory_order_relaxed);
}

}}}
