#ifndef CLUSTER_TOOLS_ADAPTERS_SBE_TESTS_ORACLE_SERVE_CLIENT_H
#define CLUSTER_TOOLS_ADAPTERS_SBE_TESTS_ORACLE_SERVE_CLIENT_H

// The client side of the S9h harness `serve` (S9h 6.3): the real behaviour-3 core over stdio, `u32 length, u8 op,
// payload` both ways, little-endian. Entries apply strictly in request order; a QUERY is answered after everything
// before it (the S9a 5.5 ordering the bridge's report rule depends on).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ct_oracle {

using Bytes = std::vector<std::uint8_t>;

class Reader
{
public:
    explicit Reader(const Bytes &b) : m_b(b) {}
    std::int64_t i64()
    {
        need(8);
        std::uint64_t u = 0;
        for (int i = 7; i >= 0; --i)
        {
            u = (u << 8) | m_b[m_at + static_cast<std::size_t>(i)];
        }
        m_at += 8;
        return static_cast<std::int64_t>(u);
    }
    std::uint32_t u32()
    {
        need(4);
        const std::uint32_t v = static_cast<std::uint32_t>(m_b[m_at]) | (static_cast<std::uint32_t>(m_b[m_at + 1]) << 8) |
            (static_cast<std::uint32_t>(m_b[m_at + 2]) << 16) | (static_cast<std::uint32_t>(m_b[m_at + 3]) << 24);
        m_at += 4;
        return v;
    }
    std::uint16_t u16()
    {
        need(2);
        const std::uint16_t v = static_cast<std::uint16_t>(m_b[m_at] | (m_b[m_at + 1] << 8));
        m_at += 2;
        return v;
    }
    std::uint8_t u8()
    {
        need(1);
        return m_b[m_at++];
    }
    Bytes bytes(std::size_t n)
    {
        need(n);
        Bytes out(m_b.begin() + static_cast<long>(m_at), m_b.begin() + static_cast<long>(m_at + n));
        m_at += n;
        return out;
    }
    std::string text(std::size_t n)
    {
        const Bytes b = bytes(n);
        return std::string(b.begin(), b.end());
    }
    std::size_t remaining() const { return m_b.size() - m_at; }

private:
    void need(std::size_t n) const
    {
        if (m_at + n > m_b.size())
        {
            throw std::runtime_error("serve reply truncated");
        }
    }
    const Bytes &m_b;
    std::size_t m_at = 0;
};

inline void put64(Bytes &b, std::int64_t v)
{
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
    {
        b.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
    }
}

inline void put32(Bytes &b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

struct Ready
{
    std::int64_t n = 0;
    std::vector<std::int64_t> barriers;
    std::string cleanSha;
    std::int64_t bridgeSession = 0, identity = 0, sourceEpoch = 0, genesisId = 0, t0 = 0;
};

class ServeClient
{
public:
    enum Op : std::uint8_t { BOOT = 1, OPEN_BRIDGE = 2, FRAME = 3, QUERY = 4, ROUND = 5, FINISH = 6, QUIT = 7, DUMP = 8,
        ADMIN_OPEN = 9, ERR = 0xFF };

    ServeClient(const std::string &java, const std::string &jar, const std::string &errLog)
    {
        int in[2], out[2];
        if (pipe(in) != 0 || pipe(out) != 0)
        {
            throw std::runtime_error("pipe");
        }
        m_pid = fork();
        if (m_pid < 0)
        {
            throw std::runtime_error("fork");
        }
        if (0 == m_pid)
        {
            dup2(in[0], 0);
            dup2(out[1], 1);
            const std::string redirect = errLog;
            FILE *err = std::fopen(redirect.c_str(), "w");
            if (err)
            {
                dup2(fileno(err), 2);
            }
            close(in[1]);
            close(out[0]);
            execl(java.c_str(), java.c_str(), "-jar", jar.c_str(), "serve", static_cast<char *>(nullptr));
            _exit(127);
        }
        close(in[0]);
        close(out[1]);
        m_to = in[1];
        m_from = out[0];
    }

    ~ServeClient()
    {
        try
        {
            if (m_pid > 0)
            {
                request(QUIT, {});
            }
        }
        catch (...)
        {
        }
        if (m_to >= 0)
        {
            close(m_to);
        }
        if (m_from >= 0)
        {
            close(m_from);
        }
        if (m_pid > 0)
        {
            int status = 0;
            waitpid(m_pid, &status, 0);
        }
    }

    // one request, its reply payload; throws on ERR
    Bytes request(std::uint8_t op, const Bytes &payload, std::uint8_t *replyOp = nullptr)
    {
        Bytes msg;
        put32(msg, static_cast<std::uint32_t>(1 + payload.size()));
        msg.push_back(op);
        msg.insert(msg.end(), payload.begin(), payload.end());
        writeAll(msg.data(), msg.size());
        std::uint8_t head[5];
        readAll(head, 5);
        const std::uint32_t length = static_cast<std::uint32_t>(head[0]) | (static_cast<std::uint32_t>(head[1]) << 8) |
            (static_cast<std::uint32_t>(head[2]) << 16) | (static_cast<std::uint32_t>(head[3]) << 24);
        Bytes body(length - 1);
        if (!body.empty())
        {
            readAll(body.data(), body.size());
        }
        if (replyOp)
        {
            *replyOp = head[4];
        }
        if (ERR == head[4])
        {
            throw std::runtime_error("serve ERR op " + std::to_string(op) + ": " + std::string(body.begin(), body.end()));
        }
        if (QUIT == op)
        {
            m_pid = -m_pid; // BYE: the process exits
            int status = 0;
            waitpid(-m_pid, &status, 0);
            m_pid = 0;
        }
        return body;
    }

    Ready boot(const std::string &storyDir)
    {
        const Bytes b = request(BOOT, Bytes(storyDir.begin(), storyDir.end()));
        Reader r(b);
        Ready ready;
        ready.n = r.i64();
        const std::uint32_t rounds = r.u32();
        for (std::uint32_t i = 0; i < rounds; ++i)
        {
            ready.barriers.push_back(r.i64());
        }
        ready.cleanSha = r.text(64);
        ready.bridgeSession = r.i64();
        ready.identity = r.i64();
        (void)r.u8();
        (void)r.u8();
        (void)r.u8();
        ready.sourceEpoch = r.i64();
        ready.genesisId = r.i64();
        ready.t0 = r.i64();
        return ready;
    }

    Bytes openBridge(std::int64_t session, std::int64_t identity)
    {
        Bytes p;
        put64(p, session);
        put64(p, identity);
        const Bytes b = request(OPEN_BRIDGE, p);
        Reader r(b);
        return r.bytes(r.u16());
    }

    // the ORIGIN egress frames the entry produced for the session
    std::vector<Bytes> frame(std::int64_t session, const std::uint8_t *f, std::size_t n)
    {
        Bytes p;
        put64(p, session);
        p.insert(p.end(), f, f + n);
        const Bytes b = request(FRAME, p);
        Reader r(b);
        const std::uint32_t count = r.u32();
        std::vector<Bytes> out;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            out.push_back(r.bytes(r.u16()));
        }
        return out;
    }

    // the FeedPositionReport (17) frame
    Bytes query(std::int64_t session, std::int64_t corr)
    {
        Bytes p;
        put64(p, session);
        put64(p, corr);
        const Bytes b = request(QUERY, p);
        Reader r(b);
        for (int i = 0; i < 3; ++i)
        {
            (void)r.i64();
        }
        (void)r.u8();
        (void)r.u8();
        (void)r.i64();
        (void)r.i64();
        return r.bytes(r.u16());
    }

    std::int64_t round(std::uint32_t round)
    {
        Bytes p;
        put32(p, round);
        const Bytes b = request(ROUND, p);
        Reader r(b);
        (void)r.u32();
        (void)r.u32();
        return r.i64();
    }

    std::string finish()
    {
        const Bytes b = request(FINISH, {});
        return std::string(b.begin(), b.end());
    }

    struct Dump
    {
        std::string liveSha, modelSha;
        std::vector<std::string> lines;
        std::string latchLine() const
        {
            for (const auto &l : lines)
            {
                if (l.rfind("L ", 0) == 0)
                {
                    return l;
                }
            }
            return "";
        }
    };

    Dump dump(std::int64_t upto = -1)
    {
        Bytes p;
        if (upto >= 0)
        {
            put64(p, upto);
        }
        const Bytes b = request(DUMP, p);
        Reader r(b);
        Dump d;
        d.liveSha = r.text(64);
        d.modelSha = r.text(64);
        const std::uint32_t n = r.u32();
        const std::string text = r.text(r.remaining());
        std::size_t start = 0;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            const std::size_t end = text.find('\n', start);
            d.lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return d;
    }

    Bytes adminOpen()
    {
        const Bytes b = request(ADMIN_OPEN, {});
        Reader r(b);
        return r.bytes(r.u16());
    }

private:
    void writeAll(const std::uint8_t *p, std::size_t n)
    {
        while (n > 0)
        {
            const ssize_t w = ::write(m_to, p, n);
            if (w <= 0)
            {
                throw std::runtime_error("serve pipe closed (write)");
            }
            p += w;
            n -= static_cast<std::size_t>(w);
        }
    }
    void readAll(std::uint8_t *p, std::size_t n)
    {
        while (n > 0)
        {
            const ssize_t r = ::read(m_from, p, n);
            if (r <= 0)
            {
                throw std::runtime_error("serve pipe closed (read)");
            }
            p += r;
            n -= static_cast<std::size_t>(r);
        }
    }

    pid_t m_pid = 0;
    int m_to = -1;
    int m_from = -1;
};

// a JSON number or string field of the flat Result
inline std::string jsonField(const std::string &json, const std::string &key)
{
    const std::string k = "\"" + key + "\":";
    const std::size_t at = json.find(k);
    if (at == std::string::npos)
    {
        return "";
    }
    std::size_t s = at + k.size();
    if (json[s] == '"')
    {
        const std::size_t e = json.find('"', s + 1);
        return json.substr(s + 1, e - s - 1);
    }
    std::size_t e = s;
    while (e < json.size() && json[e] != ',' && json[e] != '}')
    {
        ++e;
    }
    return json.substr(s, e - s);
}

inline std::int64_t duplicateCount(const std::string &json)
{
    std::int64_t total = 0;
    for (int t = 61; t <= 68; ++t)
    {
        const std::string v = jsonField(json, "feedDuplicateCount" + std::to_string(t));
        if (!v.empty())
        {
            total += std::stoll(v);
        }
    }
    return total;
}

}

#endif
