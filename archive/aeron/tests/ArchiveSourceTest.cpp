// ArchiveSource without an archive (RC2 S9a contract 3.3, W3): the re-entrancy guard is checked
// before any C call and ends the process with exit 70 (section 7 row F5); an exception escaping the
// owner's invoker ends it with exit 70 too (F6, never 134); archive error codes map to the classes
// the follow policy acts on (4.3 step 6).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "cluster_tools/archive/aeron/ArchiveSource.h"

#include "aeron_archive_client_error.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

using namespace cluster_tools::archive;
using namespace cluster_tools::archive::aeron_archive;
using cluster_tools::client::aeron_transport::AeronFatalMonitor;

namespace {

ArchiveSourceConfig config()
{
    ArchiveSourceConfig c;
    c.identity = SourceIdentity{ "127.0.0.1:9010", 4010, 4001, "aeron:ipc" };
    c.localHost = "127.0.0.1";
    c.replayStreamId = 9101;
    return c;
}

int exitStatusOf(const std::function<void()> &child)
{
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (0 == pid)
    {
        child();
        std::_Exit(0); // the guard did not end the process
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

}

TEST_CASE("ArchiveSourceTest.reentryFromInvokerExits70")
{
    SUBCASE("injected exit action: the guard fires before any C call")
    {
        std::string exitMessage;
        ArchiveMetrics metrics;
        auto monitor = std::make_shared<AeronFatalMonitor>([](const std::string &) {});
        ArchiveSource source(config(), nullptr, monitor, nullptr, &metrics, {},
            [&](const std::string &message) { exitMessage = message; });
        // An archive call from inside an archive call (what an owner invoker calling the archive does).
        CHECK_THROWS_AS(source.runAsArchiveCallForTest([&] { source.list(); }), std::logic_error);
        CHECK(exitMessage.find("re-entered") != std::string::npos);
        exitMessage.clear();
        CHECK_THROWS_AS(source.runAsArchiveCallForTest([&] { source.probe(1); }), std::logic_error);
        CHECK_FALSE(exitMessage.empty());
        exitMessage.clear();
        source.runAsArchiveCallForTest([&] { source.stopReplay(1); });
        CHECK_FALSE(exitMessage.empty());
        // outside a call the same methods are fine (no archive: a NOT_CONNECTED answer)
        exitMessage.clear();
        CHECK(source.list().error.kind == ArchiveErrorKind::NOT_CONNECTED);
        CHECK(exitMessage.empty());
    }
    SUBCASE("the real exit action: exit status 70")
    {
        const int status = exitStatusOf([] {
            auto monitor = std::make_shared<AeronFatalMonitor>([](const std::string &) {});
            ArchiveSource source(config(), nullptr, monitor, nullptr, nullptr);
            source.runAsArchiveCallForTest([&] { source.list(); });
        });
        CHECK(status == 70);
    }
}

TEST_CASE("ArchiveSourceTest.invokerExceptionExits70NotAbort")
{
    const int status = exitStatusOf([] {
        auto monitor = std::make_shared<AeronFatalMonitor>([](const std::string &) {});
        ArchiveSource source(config(), nullptr, monitor, [] { throw std::runtime_error("owner bug"); }, nullptr);
        source.invokeOwnerForTest(); // exactly as aeron_archive_context_invoke_aeron_client calls it
    });
    CHECK(status == 70);
}

TEST_CASE("ArchiveSourceTest.archiveErrorCodesMapToPolicyClasses")
{
    using aeron::util::ArchiveException;
    auto kind = [](int code, const std::string &message = "x") {
        return classifyArchiveError(ArchiveException(message, SOURCEINFO, code), false).kind;
    };
    CHECK(kind(-AERON_ARCHIVE_ERROR_CODE_INVALID_POSITION) == ArchiveErrorKind::INVALID_POSITION);
    CHECK(kind(-AERON_ARCHIVE_ERROR_CODE_UNKNOWN_RECORDING) == ArchiveErrorKind::UNKNOWN_RECORDING);
    CHECK(kind(-AERON_ARCHIVE_ERROR_CODE_MAX_REPLAYS) == ArchiveErrorKind::MAX_REPLAYS);
    CHECK(kind(-AERON_ARCHIVE_ERROR_CODE_STORAGE_SPACE) == ArchiveErrorKind::STORAGE_SPACE);
    CHECK(kind(ETIMEDOUT) == ArchiveErrorKind::TIMEOUT);
    CHECK(kind(-11, "subscription to archive is not connected") == ArchiveErrorKind::NOT_CONNECTED);
    CHECK(kind(-AERON_ARCHIVE_ERROR_CODE_GENERIC) == ArchiveErrorKind::GENERIC);
    // the fatal monitor wins over any code
    CHECK(classifyArchiveError(ArchiveException("x", SOURCEINFO, ETIMEDOUT), true).kind == ArchiveErrorKind::FATAL);
}

TEST_CASE("ArchiveSourceTest.channelsCarryTheLocalHost")
{
    CHECK(udpChannel("10.0.0.9", "0") == "aeron:udp?endpoint=10.0.0.9:0");
    CHECK(udpChannel("fd00::9", "0") == "aeron:udp?endpoint=[fd00::9]:0");
}
