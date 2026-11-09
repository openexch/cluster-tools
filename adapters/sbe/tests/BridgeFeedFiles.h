#ifndef CLUSTER_TOOLS_ADAPTERS_SBE_TESTS_BRIDGE_FEED_FILES_H
#define CLUSTER_TOOLS_ADAPTERS_SBE_TESTS_BRIDGE_FEED_FILES_H

// The harness file formats (all little-endian, every reader strict) and the C++ `translate`:
// NOT a second implementation: it drives the domain's own Stager (domain/bridge, the staging the live bridge runs)
// with BridgeCodec as its encoder and a collecting FrameOutput: RECORD -> Stager::forward, FLUSH -> Stager::flush,
// end -> flush. The expected bytes these are compared with come only from the Java harness.

#include "cluster_tools/adapters/sbe/BridgeCodec.h"
#include "cluster_tools/archive/FramePeek.h"
#include "cluster_tools/domain/bridge/Stager.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ct_bridge_codec_test {

using Bytes = std::vector<std::uint8_t>;

struct FjrEntry
{
    bool flush = false;
    Bytes record;
};

inline std::uint32_t le32(const Bytes &b, std::size_t at)
{
    return static_cast<std::uint32_t>(b.at(at)) | (static_cast<std::uint32_t>(b.at(at + 1)) << 8) |
        (static_cast<std::uint32_t>(b.at(at + 2)) << 16) | (static_cast<std::uint32_t>(b.at(at + 3)) << 24);
}

inline std::uint16_t le16(const Bytes &b, std::size_t at)
{
    return static_cast<std::uint16_t>(b.at(at) | (b.at(at + 1) << 8));
}

inline void putLe32(Bytes &b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

inline void putLe16(Bytes &b, std::uint16_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xff));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

inline Bytes readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot read " + path);
    }
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline bool magic(const Bytes &b, const char *m)
{
    return b.size() >= 8 && b[0] == static_cast<std::uint8_t>(m[0]) && b[1] == static_cast<std::uint8_t>(m[1]) &&
        b[2] == static_cast<std::uint8_t>(m[2]) && b[3] == static_cast<std::uint8_t>(m[3]);
}

inline std::vector<FjrEntry> decodeFjr(const Bytes &b)
{
    if (!magic(b, "FJR1"))
    {
        throw std::runtime_error("not an FJR1 file");
    }
    const std::uint32_t n = le32(b, 4);
    std::vector<FjrEntry> out;
    std::size_t at = 8;
    for (std::uint32_t i = 0; i < n; ++i)
    {
        const std::uint8_t type = b.at(at++);
        FjrEntry e;
        if (2 == type)
        {
            e.flush = true;
        }
        else if (1 == type)
        {
            const std::size_t len = le16(b, at);
            at += 2;
            if (len < 8 || at + len > b.size())
            {
                throw std::runtime_error("FJR1 record " + std::to_string(i) + " truncated");
            }
            e.record.assign(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + len));
            at += len;
        }
        else
        {
            throw std::runtime_error("FJR1 entry " + std::to_string(i) + ": unknown type");
        }
        out.push_back(std::move(e));
    }
    if (at != b.size())
    {
        throw std::runtime_error("FJR1 trailing bytes");
    }
    return out;
}

inline std::vector<Bytes> decodeFfr(const Bytes &b)
{
    if (!magic(b, "FFR1"))
    {
        throw std::runtime_error("not an FFR1 file");
    }
    const std::uint32_t n = le32(b, 4);
    std::vector<Bytes> out;
    std::size_t at = 8;
    for (std::uint32_t i = 0; i < n; ++i)
    {
        const std::size_t len = le16(b, at);
        at += 2;
        if (at + len > b.size())
        {
            throw std::runtime_error("FFR1 frame " + std::to_string(i) + " truncated");
        }
        out.emplace_back(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + len));
        at += len;
    }
    if (at != b.size())
    {
        throw std::runtime_error("FFR1 trailing bytes");
    }
    return out;
}

inline Bytes encodeFfr(const std::vector<Bytes> &frames)
{
    Bytes out = { 0x46, 0x46, 0x52, 0x31 };
    putLe32(out, static_cast<std::uint32_t>(frames.size()));
    for (const Bytes &f : frames)
    {
        putLe16(out, static_cast<std::uint16_t>(f.size()));
        out.insert(out.end(), f.begin(), f.end());
    }
    return out;
}

// Collects every emitted frame; never back-pressured.
struct Collect final : cluster_tools::domain::bridge::FrameOutput
{
    std::vector<Bytes> frames;
    cluster_tools::domain::bridge::StepResult emit(const std::uint8_t *frame, std::size_t length, std::uint16_t,
        const cluster_tools::domain::bridge::OfferedRecord *, std::size_t) noexcept override
    {
        frames.emplace_back(frame, frame + length);
        return cluster_tools::domain::bridge::StepResult::OK;
    }
};

// The live staging (Stager) + the generated-codec translation (BridgeCodec).
inline std::vector<Bytes> translate(const std::vector<FjrEntry> &input)
{
    using namespace cluster_tools::domain::bridge;
    cluster_tools::sbe_adapter::bridge_codec::BridgeCodec codec;
    auto stager = std::make_unique<Stager>(codec);
    Collect out;
    std::int64_t index = 0;
    for (const FjrEntry &e : input)
    {
        if (e.flush)
        {
            if (StepResult::OK != stager->flush(out))
            {
                throw std::runtime_error("translate: a flush failed");
            }
            continue;
        }
        OfferedRecord meta;
        meta.seq = ++index;
        const std::uint16_t t = cluster_tools::archive::readLe16(e.record.data() + 2);
        const ForwardResult r = stager->forward(e.record.data(), e.record.size(), t, meta, out);
        if (StepResult::OK != r.step || StepResult::OK != r.flush)
        {
            throw std::runtime_error("translate: record " + std::to_string(index) + " was not encoded");
        }
    }
    if (StepResult::OK != stager->flush(out))
    {
        throw std::runtime_error("translate: the end flush failed");
    }
    return out.frames;
}

inline std::string hex(const Bytes &b, std::size_t max = 4096)
{
    std::string s;
    char buf[3];
    for (std::size_t i = 0; i < b.size() && i < max; ++i)
    {
        std::snprintf(buf, sizeof(buf), "%02x", b[i]);
        s += buf;
    }
    return s;
}

// "" when equal, else the first differing frame index and offset
inline std::string firstDifference(const std::vector<Bytes> &got, const std::vector<Bytes> &want)
{
    for (std::size_t i = 0; i < got.size() && i < want.size(); ++i)
    {
        if (got[i] != want[i])
        {
            std::size_t off = 0;
            while (off < got[i].size() && off < want[i].size() && got[i][off] == want[i][off])
            {
                ++off;
            }
            return "frame " + std::to_string(i) + " differs at offset " + std::to_string(off) + " (lengths " +
                std::to_string(got[i].size()) + " / " + std::to_string(want[i].size()) + "): got " + hex(got[i]) +
                " want " + hex(want[i]);
        }
    }
    if (got.size() != want.size())
    {
        return "frame counts differ: got " + std::to_string(got.size()) + ", want " + std::to_string(want.size());
    }
    return "";
}

}

#endif
