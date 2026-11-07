// E1GateTest (RC2 S9b 13.14, gate half; the translation half is adapters/sbe TranslatorE1Test): on every member
// stream of the E1 corpus captured from J-ME3 (S9h section 8), the C++ gate (S9a FramePeek::admitFrame) admits
// exactly the records the Java ReferenceFrameGate admitted (the member's FJR) and refuses exactly the frames of its
// refused.txt with the same reason. The files are S9h's; the C++ side never writes them.
#include "doctest/doctest.h"

#include "TestSupport.h"

#include "cluster_tools/archive/FramePeek.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace cluster_tools::archive;
using ct_archive_test::Bytes;

namespace {

struct Fjr
{
    std::vector<Bytes> records; // FLUSH markers dropped
    int flushes = 0;
};

std::uint32_t le32(const Bytes &b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
        (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

Fjr readFjr(const std::string &path)
{
    const Bytes b = ct_archive_test::readFile(path);
    Fjr out;
    REQUIRE(b.size() >= 8);
    REQUIRE(b[0] == 0x46);
    REQUIRE(b[1] == 0x4A);
    REQUIRE(b[2] == 0x52);
    REQUIRE(b[3] == 0x31);
    const std::uint32_t n = le32(b, 4);
    std::size_t at = 8;
    for (std::uint32_t i = 0; i < n; ++i)
    {
        REQUIRE(at < b.size());
        const std::uint8_t type = b[at++];
        if (2 == type)
        {
            ++out.flushes;
            continue;
        }
        REQUIRE(1 == type);
        const std::size_t len = static_cast<std::size_t>(b[at] | (b[at + 1] << 8));
        at += 2;
        REQUIRE(at + len <= b.size());
        out.records.emplace_back(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + len));
        at += len;
    }
    CHECK(at == b.size());
    return out;
}

Bytes unhex(const std::string &hex)
{
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string upper(const char *s)
{
    std::string u(s);
    for (char &c : u)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return u;
}

}

TEST_CASE("E1GateTest.theCppGateAgreesWithTheJavaReferenceGateOnEveryCapturedFrame")
{
    namespace fs = std::filesystem;
    const fs::path root(CT_E1_FIXTURE_DIR);
    int scenarios = 0, members = 0, admitted = 0, refused = 0;
    std::vector<fs::path> dirs;
    for (const auto &entry : fs::directory_iterator(root))
    {
        if (entry.is_directory())
        {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path &dir : dirs)
    {
        ++scenarios;
        for (int m = 0; m < 3; ++m)
        {
            const std::string base = (dir / ("member" + std::to_string(m))).string();
            if (!fs::exists(base + ".fjr"))
            {
                continue;
            }
            ++members;
            CAPTURE(base);
            const Fjr fjr = readFjr(base + ".fjr");
            CHECK(fjr.flushes >= 1); // a FLUSH at each recording STOP and at the end (S9h e1)
            for (const Bytes &r : fjr.records)
            {
                const Admission a = admitFrame(FRAME_FLAGS_UNFRAGMENTED, r.data(), r.size());
                CHECK(a.accepted());
                ++admitted;
            }
            std::ifstream in(base + ".refused.txt");
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty())
                {
                    continue;
                }
                std::istringstream fields(line);
                std::string index, reason, length, hex;
                fields >> index >> reason >> length >> hex;
                const Bytes frame = unhex(hex);
                REQUIRE(frame.size() == static_cast<std::size_t>(std::stoul(length)));
                const Admission a = admitFrame(FRAME_FLAGS_UNFRAGMENTED, frame.data(), frame.size());
                CHECK_FALSE(a.accepted());
                CHECK(upper(refusalLabel(a.refusal)) == reason);
                ++refused;
            }
        }
    }
    MESSAGE("E1: " << scenarios << " scenarios, " << members << " member streams, " << admitted << " records admitted, "
            << refused << " refused");
    CHECK(scenarios == 15);
    CHECK(members == 45);
    CHECK(refused >= 1); // schema3_record carries the refused schema 3 record
    CHECK(admitted > 100);
}
