#ifndef CLUSTER_TOOLS_APP_FORWARDER_TC_CAPTURE_H
#define CLUSTER_TOOLS_APP_FORWARDER_TC_CAPTURE_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

// TcCapture - the frame capture of ct_forwarder_tc_capture (T1-F re-sends; never
// published: publish.yml refuses every ct_forwarder_tc_* name). ForwarderAgent includes this header and calls it only
// under CT_FORWARDER_TC_CAPTURE, so the production ct-forwarder's drain path is unchanged.
//
// Every template 9 / 10 frame the agent offered to the ME with a positive result is written byte for byte to
// <dir>/<n>.bin (n = 1, 2, ...; a restarted process continues after the highest existing file), and one line is
// appended to <dir>/sidecar.txt:
//   n template commandIdHigh commandIdLow kind dispatchSeq sha256
// template 10: the ids of the frame's FIRST entry (commandIdLow = its idLowFloor), kind 3, dispatchSeq = the frame's
// releaseSeq; the frame's other entries are in the .bin. sha256 is the hex SHA-256 of the .bin (the
// re-send input is pinned by it). The .bin is complete and closed before its sidecar line is written and flushed, so
// a SIGKILL never leaves a line naming a partial frame. Duty-thread blocking I/O: acceptable in a test build only.

namespace cluster_tools { namespace app { namespace forwarder {

// FIPS 180-4 SHA-256, the one-shot form the capture needs (no allocation).
class Sha256
{
public:
    static constexpr std::size_t DIGEST = 32;

    static void digest(const std::uint8_t *data, std::size_t length, std::uint8_t out[DIGEST]) noexcept
    {
        std::uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu,
            0x5be0cd19u };
        std::size_t done = 0;
        for (; done + 64 <= length; done += 64)
        {
            block(h, data + done);
        }
        std::uint8_t tail[128] = {};
        const std::size_t rest = length - done;
        if (rest > 0)
        {
            std::memcpy(tail, data + done, rest);
        }
        tail[rest] = 0x80;
        const std::size_t padded = rest + 1 + 8 <= 64 ? 64 : 128;
        const std::uint64_t bits = static_cast<std::uint64_t>(length) * 8u;
        for (int i = 0; i < 8; i++)
        {
            tail[padded - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
        }
        block(h, tail);
        if (128 == padded)
        {
            block(h, tail + 64);
        }
        for (int i = 0; i < 8; i++)
        {
            out[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
            out[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
        }
    }

    static std::string hex(const std::uint8_t *data, std::size_t length)
    {
        std::uint8_t d[DIGEST];
        digest(data, length, d);
        static const char *digits = "0123456789abcdef";
        std::string s(2 * DIGEST, '0');
        for (std::size_t i = 0; i < DIGEST; i++)
        {
            s[2 * i] = digits[d[i] >> 4];
            s[2 * i + 1] = digits[d[i] & 0x0f];
        }
        return s;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

    static void block(std::uint32_t h[8], const std::uint8_t *p) noexcept
    {
        static constexpr std::uint32_t K[64] = { 0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u,
            0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu,
            0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u };
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++)
        {
            w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) |
                (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) | static_cast<std::uint32_t>(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; i++)
        {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++)
        {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = k + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            k = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += k;
    }
};

class TcCapture
{
public:
    TcCapture() = default;
    TcCapture(const TcCapture &) = delete;
    TcCapture &operator=(const TcCapture &) = delete;
    ~TcCapture()
    {
        if (nullptr != m_sidecar)
        {
            std::fclose(m_sidecar);
        }
    }

    // dir = FWD_CAPTURE_DIR (strict: missing, empty, relative or not a writable directory is refused; no default).
    bool open(const char *dir, std::string &error)
    {
        if (nullptr == dir || '\0' == dir[0])
        {
            error = "FWD_CAPTURE_DIR is required by ct_forwarder_tc_capture (no default)";
            return false;
        }
        if ('/' != dir[0])
        {
            error = std::string("FWD_CAPTURE_DIR must be an absolute path, got '") + dir + "'";
            return false;
        }
        struct stat st{};
        if (0 != ::stat(dir, &st) || !S_ISDIR(st.st_mode) || 0 != ::access(dir, W_OK))
        {
            error = std::string("FWD_CAPTURE_DIR '") + dir + "' is not a writable directory";
            return false;
        }
        m_dir = dir;
        // continue after the highest existing <n>.bin (a restarted fwdA keeps the earlier frames)
        m_next = 1;
        while (0 == ::access(binPath(m_next).c_str(), F_OK))
        {
            m_next++;
        }
        const std::string sidecar = m_dir + "/sidecar.txt";
        m_sidecar = std::fopen(sidecar.c_str(), "a");
        if (nullptr == m_sidecar)
        {
            error = "cannot open " + sidecar + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }

    const std::string &dir() const noexcept { return m_dir; }
    std::uint64_t next() const noexcept { return m_next; }

    bool write(const std::uint8_t *frame, std::size_t length, unsigned templateId, std::int64_t commandIdHigh,
        std::int64_t commandIdLow, unsigned kind, std::int64_t dispatchSeq, std::string &error)
    {
        const std::uint64_t n = m_next++;
        const std::string path = binPath(n);
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (nullptr == f)
        {
            error = "cannot create " + path + ": " + std::strerror(errno);
            return false;
        }
        const bool written = length == std::fwrite(frame, 1, length, f);
        if (0 != std::fclose(f) || !written)
        {
            error = "short write of " + path;
            return false;
        }
        const std::string sha = Sha256::hex(frame, length);
        if (std::fprintf(m_sidecar, "%llu %u %lld %lld %u %lld %s\n", static_cast<unsigned long long>(n), templateId,
                static_cast<long long>(commandIdHigh), static_cast<long long>(commandIdLow), kind,
                static_cast<long long>(dispatchSeq), sha.c_str()) < 0 ||
            0 != std::fflush(m_sidecar))
        {
            error = "cannot append to " + m_dir + "/sidecar.txt";
            return false;
        }
        return true;
    }

private:
    std::string binPath(std::uint64_t n) const { return m_dir + "/" + std::to_string(n) + ".bin"; }

    std::string m_dir;
    std::uint64_t m_next = 1;
    std::FILE *m_sidecar = nullptr;
};

}}}

#endif
