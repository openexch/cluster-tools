// RC2 S10' review C13-6 (ct_forwarder_config_tests, pure): the verdict on a failed embedded driver start.
//   storageStarvedStartIsRetriedThen70  a start that failed for storage space (aeron_errcode ENOSPC, the CnC of a full
//                           tmpfs; or Aeron's -12 STORAGE_SPACE) is retried while the starvation lasts and is 70
//                           F_STORAGE_STARVED 30 s after the first such failure, never 78 (RED DRIVER_START_STORAGE_78:
//                           78 at the first failure, the S9c FIX-12 shape that RestartPreventExitStatus=78 keeps down)
//   otherStartFailuresAre78  every other failure is a 78 at once, also after a storage episode (which it ends)
// The attempt cadence is the real one: a retry finds the failed attempt's dir and waits out the live-driver check
// (about 3.1 s) before its 1 s pause, so attempts land about 4.1 s apart.
#include "doctest/doctest.h"

#include <cstdint>
#include <string>

#include "ForwarderDriver.h"

using cluster_tools::app::common::StorageStarvation;
using cluster_tools::app::common::isStorageSpaceErrcode;
using cluster_tools::app::forwarder::DriverStartVerdict;
using cluster_tools::app::forwarder::driverStartVerdict;

namespace {

constexpr std::int64_t MS = 1'000'000;
const std::string WHY = "aeron_driver_init: (28) No space left on device [aeron_create_file, aeron_fileutil.c:986] Failed "
                        "to allocate file space: /dev/shm/fwd-tmpfs-32/ct-forwarder-101/cnc.dat";

}

TEST_CASE("DriverStartTest.storageStarvedStartIsRetriedThen70")
{
    // the classifier: ENOSPC (28) and -AERON_ERROR_CODE_STORAGE_SPACE; +12 is ENOMEM, 13 EACCES, 22 EINVAL
    CHECK(isStorageSpaceErrcode(28));
    CHECK(isStorageSpaceErrcode(-12));
    for (const int other : { 0, 12, 13, 22, -1, -6 })
    {
        CAPTURE(other);
        CHECK_FALSE(isStorageSpaceErrcode(other));
    }

    StorageStarvation episode;
    const std::int64_t t0 = 5'000 * MS;
    const DriverStartVerdict first = driverStartVerdict(true, WHY, t0, episode);
    const bool retriedAtTheFirstStorageFailure = DriverStartVerdict::RETRY_STORAGE == first;
    CHECK_MESSAGE(retriedAtTheFirstStorageFailure, "a storage-starved start refused 78 at once"); // DRIVER_START_STORAGE_78 RED
    REQUIRE(retriedAtTheFirstStorageFailure);
    CHECK(episode.active());
    CHECK(std::string(episode.why()).find("No space left on device") != std::string::npos);
    // attempts about 4.1 s apart: retried up to 29.9 s after the first failure
    std::int64_t t = t0;
    int retries = 0;
    while (t + 4'100 * MS < t0 + 30'000 * MS)
    {
        t += 4'100 * MS;
        CHECK(DriverStartVerdict::RETRY_STORAGE == driverStartVerdict(true, WHY, t, episode));
        retries++;
    }
    CHECK(retries == 7);
    CHECK(DriverStartVerdict::RETRY_STORAGE == driverStartVerdict(true, WHY, t0 + 29'999 * MS, episode));
    // 30 s after the FIRST failure (not the last): 70, restartable
    CHECK(DriverStartVerdict::EXIT_70_STORAGE == driverStartVerdict(true, WHY, t0 + 30'000 * MS, episode));
    CHECK(episode.heldNs(t0 + 30'000 * MS) == 30'000 * MS);
}

TEST_CASE("DriverStartTest.otherStartFailuresAre78")
{
    StorageStarvation episode;
    // host configuration at the first attempt: 78
    CHECK(DriverStartVerdict::REFUSE_78 == driverStartVerdict(false, "another ct-forwarder holds the instance lock", 0, episode));
    CHECK_FALSE(episode.active());
    // a storage episode, then another failure (the lock lost to a second same-id instance between attempts): 78, and the
    // episode ends
    CHECK(DriverStartVerdict::RETRY_STORAGE == driverStartVerdict(true, WHY, 1'000 * MS, episode));
    CHECK(DriverStartVerdict::REFUSE_78 ==
        driverStartVerdict(false, "a LIVE media driver is already in /dev/shm/x/ct-forwarder-1", 5'100 * MS, episode));
    CHECK_FALSE(episode.active());
    // a new storage episode counts from its own first failure
    CHECK(DriverStartVerdict::RETRY_STORAGE == driverStartVerdict(true, WHY, 40'000 * MS, episode));
    CHECK(DriverStartVerdict::RETRY_STORAGE == driverStartVerdict(true, WHY, 69'999 * MS, episode));
    CHECK(DriverStartVerdict::EXIT_70_STORAGE == driverStartVerdict(true, WHY, 70'000 * MS, episode));
}
