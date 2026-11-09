// TranslatorDifferentialTest: the harness writes `random <seed> <count>`
// (in.fjr, and out.ffr from the Java reference translator), and the C++ translate of in.fjr must equal out.ffr byte
// for byte. The seed is CT_DIFF_SEED, or the CI run id, else a fresh one; it is always printed. 10^6 records per CI
// run, 10^5 on the dev box (CT_DIFF_COUNT).
#include "doctest/doctest.h"

#include "BridgeFeedFiles.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <sys/wait.h>

using namespace ct_bridge_codec_test;

TEST_CASE("TranslatorDifferentialTest.theCppTranslateEqualsTheJavaReferenceOnSeededRandomJournals")
{
    namespace fs = std::filesystem;
    std::string seed;
    if (const char *s = std::getenv("CT_DIFF_SEED"))
    {
        seed = s;
    }
    else if (const char *s = std::getenv("GITHUB_RUN_ID"))
    {
        seed = s;
    }
    else
    {
        seed = std::to_string(std::random_device{}() & 0x7fffffff);
    }
    const std::string count = std::getenv("CT_DIFF_COUNT") ? std::getenv("CT_DIFF_COUNT") : "100000";
    const fs::path dir = fs::temp_directory_path() / ("ct-bridge-diff-" + seed + "-" + count);
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string cmd = std::string("'") + CT_JAVA + "' -jar '" + CT_FEED_HARNESS_JAR + "' random " + seed + " " + count +
        " '" + dir.string() + "' 2> '" + (dir / "harness.err").string() + "'";
    MESSAGE("E2 differential: seed " << seed << ", " << count << " records");
    const int rc = std::system(cmd.c_str());
    const bool harnessOk = WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
    REQUIRE_MESSAGE(harnessOk, "the harness failed: " << cmd);
    const std::vector<FjrEntry> in = decodeFjr(readFile((dir / "in.fjr").string()));
    const std::vector<Bytes> want = decodeFfr(readFile((dir / "out.ffr").string()));
    const std::vector<Bytes> got = translate(in);
    std::string diff = firstDifference(got, want);
    if (!diff.empty())
    {
        // name the input record behind the first differing frame
        std::size_t frame = std::stoul(diff.substr(6));
        std::vector<FjrEntry> prefix;
        for (const FjrEntry &e : in)
        {
            prefix.push_back(e);
            if (!e.flush && translate(prefix).size() > frame)
            {
                diff += " :: input record " + hex(e.record);
                break;
            }
        }
    }
    INFO("seed " << seed << ": " << diff);
    CHECK(diff.empty());
    MESSAGE("E2 differential: " << in.size() << " entries, " << want.size() << " frames compared");
    if (diff.empty())
    {
        fs::remove_all(dir);
    }
}
