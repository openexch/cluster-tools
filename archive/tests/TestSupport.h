#ifndef CLUSTER_TOOLS_ARCHIVE_TESTS_TEST_SUPPORT_H
#define CLUSTER_TOOLS_ARCHIVE_TESTS_TEST_SUPPORT_H

// Shared builders for the pure archive tests: schema 4 frames, recordings, fixture reads.

#include "cluster_tools/archive/Chain.h"
#include "cluster_tools/archive/FramePeek.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace ct_archive_test {

using Bytes = std::vector<std::uint8_t>;

inline void putLe16(Bytes &b, std::size_t at, std::uint16_t v)
{
    b[at] = static_cast<std::uint8_t>(v & 0xff);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline void putLe64(Bytes &b, std::size_t at, std::int64_t v)
{
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
    {
        b[at + i] = static_cast<std::uint8_t>(u >> (8 * i));
    }
}

// A frame of template 1, 2, 3 or 5 with the schema 4 header and the common head.
inline Bytes makeFrame(std::uint16_t templateId, std::int64_t seq, std::int64_t epoch, std::int64_t egress = 64)
{
    using namespace cluster_tools::archive;
    const std::uint16_t block = blockLengthFor(templateId);
    Bytes b(MESSAGE_HEADER_LENGTH + block, 0);
    putLe16(b, 0, block);
    putLe16(b, 2, templateId);
    putLe16(b, 4, JOURNAL_SCHEMA_ID);
    putLe16(b, 6, JOURNAL_SCHEMA_VERSION);
    putLe64(b, FRAME_OFFSET_JOURNAL_SEQ, seq);
    putLe64(b, FRAME_OFFSET_ME_EPOCH, epoch);
    putLe64(b, FRAME_OFFSET_EGRESS_SEQ, egress);
    return b;
}

// Record 4 with n release entries.
inline Bytes makeRelease(std::int64_t seq, std::int64_t epoch, std::uint16_t entries, std::uint16_t entryBlock = 22)
{
    using namespace cluster_tools::archive;
    Bytes b(RELEASE_MIN_FRAME + static_cast<std::size_t>(entryBlock) * entries, 0);
    putLe16(b, 0, BLOCK_RELEASE);
    putLe16(b, 2, TEMPLATE_RELEASE);
    putLe16(b, 4, JOURNAL_SCHEMA_ID);
    putLe16(b, 6, JOURNAL_SCHEMA_VERSION);
    putLe64(b, FRAME_OFFSET_JOURNAL_SEQ, seq);
    putLe64(b, FRAME_OFFSET_ME_EPOCH, epoch);
    putLe64(b, FRAME_OFFSET_EGRESS_SEQ, 64);
    putLe16(b, MESSAGE_HEADER_LENGTH + BLOCK_RELEASE, entryBlock);
    putLe16(b, MESSAGE_HEADER_LENGTH + BLOCK_RELEASE + 2, entries);
    return b;
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

inline Bytes fixture(const std::string &name)
{
    return readFile(std::string(CT_MEJOURNAL_FIXTURE_DIR) + "/" + name);
}

inline cluster_tools::archive::RecordingKey key(std::int64_t id, std::int32_t mtu = 1408, std::int32_t stream = 4001,
    std::int64_t startTimestamp = 1000)
{
    cluster_tools::archive::RecordingKey k;
    k.recordingId = id;
    k.startTimestamp = startTimestamp + id;
    k.initialTermId = 42;
    k.sessionId = 7;
    k.termBufferLength = 65536;
    k.mtuLength = mtu;
    k.streamId = stream;
    return k;
}

inline cluster_tools::archive::Recording stopped(std::int64_t id, std::int64_t start, std::int64_t stop, std::int32_t mtu = 1408)
{
    return cluster_tools::archive::Recording{ key(id, mtu), start, stop };
}

inline cluster_tools::archive::Recording active(std::int64_t id, std::int64_t start, std::int32_t mtu = 1408)
{
    return cluster_tools::archive::Recording{ key(id, mtu), start, cluster_tools::archive::NULL_POSITION };
}

// The aligned Aeron frame length of a message of `length` bytes (32-byte header, 32 alignment).
inline std::int64_t alignedFrame(std::size_t length)
{
    return static_cast<std::int64_t>((length + 32 + 31) / 32 * 32);
}

}

#endif
