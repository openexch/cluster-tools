// RC2 S10' contract 7.1 item 2: the frame capture of ct_forwarder_tc_capture (TcCapture.h). The SHA-256 that pins a
// captured frame for the T1-F re-send (review N-11) against the FIPS 180-4 vectors and padding boundaries, and the
// capture files: <n>.bin byte-identical to the offered frame, one sidecar line per frame, a restart continues the
// numbering, FWD_CAPTURE_DIR strict (no default).
#include "doctest/doctest.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TcCapture.h"

using namespace cluster_tools::app::forwarder;

namespace {

std::string shaOf(const std::string &s)
{
    return Sha256::hex(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
}

std::string shaOfPattern(std::size_t n)
{
    std::vector<std::uint8_t> b(n);
    for (std::size_t i = 0; i < n; i++)
    {
        b[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xff);
    }
    return Sha256::hex(b.data(), b.size());
}

std::string readAll(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string tempDir()
{
    char pattern[] = "/tmp/ct-tc-capture-test.XXXXXX";
    const char *dir = ::mkdtemp(pattern);
    REQUIRE(dir != nullptr);
    return dir;
}

}

TEST_CASE("TcCaptureTest.sha256MatchesTheFipsVectors")
{
    CHECK(shaOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(shaOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(shaOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(shaOf(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // the padding boundaries (one or two final blocks), vectors from Python hashlib over bytes (7 i + 3) & 0xff
    CHECK(shaOfPattern(55) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    CHECK(shaOfPattern(56) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
    CHECK(shaOfPattern(63) == "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
    CHECK(shaOfPattern(64) == "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
    CHECK(shaOfPattern(65) == "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e");
    CHECK(shaOfPattern(119) == "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
    CHECK(shaOfPattern(120) == "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5");
    CHECK(shaOfPattern(128) == "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6");
}

TEST_CASE("TcCaptureTest.framesAndSidecarLinesAndRestartNumbering")
{
    const std::string dir = tempDir();
    const std::uint8_t frame9[] = { 80, 0, 9, 0, 1, 0, 12, 0, 0xAA, 0xBB, 0xCC };
    const std::uint8_t frame10[] = { 8, 0, 10, 0, 1, 0, 12, 0, 1, 2, 3, 4 };
    std::string error;
    {
        TcCapture c;
        REQUIRE(c.open(dir.c_str(), error));
        CHECK(c.next() == 1);
        REQUIRE(c.write(frame9, sizeof(frame9), 9, 1050, 4, 0, 17, error));
        REQUIRE(c.write(frame10, sizeof(frame10), 10, 1000, 7, 3, 18, error));
    }
    CHECK(readAll(dir + "/1.bin") == std::string(reinterpret_cast<const char *>(frame9), sizeof(frame9)));
    CHECK(readAll(dir + "/2.bin") == std::string(reinterpret_cast<const char *>(frame10), sizeof(frame10)));
    const std::string sha9 = Sha256::hex(frame9, sizeof(frame9));
    const std::string sha10 = Sha256::hex(frame10, sizeof(frame10));
    CHECK(readAll(dir + "/sidecar.txt") == "1 9 1050 4 0 17 " + sha9 + "\n2 10 1000 7 3 18 " + sha10 + "\n");
    {
        TcCapture c; // a restarted fwdA continues after the highest file and appends to the sidecar
        REQUIRE(c.open(dir.c_str(), error));
        CHECK(c.next() == 3);
        REQUIRE(c.write(frame9, sizeof(frame9), 9, 1050, 4, 0, 17, error));
    }
    CHECK(readAll(dir + "/sidecar.txt") ==
        "1 9 1050 4 0 17 " + sha9 + "\n2 10 1000 7 3 18 " + sha10 + "\n3 9 1050 4 0 17 " + sha9 + "\n");
    CHECK(0 == std::system(("rm -rf '" + dir + "'").c_str()));
}

TEST_CASE("TcCaptureTest.captureDirIsStrict")
{
    std::string error;
    TcCapture missing;
    CHECK_FALSE(missing.open(nullptr, error));
    CHECK(error.find("FWD_CAPTURE_DIR is required") != std::string::npos);
    TcCapture empty;
    CHECK_FALSE(empty.open("", error));
    TcCapture relative;
    CHECK_FALSE(relative.open("capture", error));
    CHECK(error.find("absolute") != std::string::npos);
    TcCapture absent;
    CHECK_FALSE(absent.open("/nonexistent/ct-tc-capture", error));
    CHECK(error.find("not a writable directory") != std::string::npos);
}
