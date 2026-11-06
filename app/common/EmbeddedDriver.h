#ifndef CLUSTER_TOOLS_APP_COMMON_EMBEDDED_DRIVER_H
#define CLUSTER_TOOLS_APP_COMMON_EMBEDDED_DRIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "common/StorageStarvation.h"

// EmbeddedDriver - <PROCESS>_DRIVER_MODE=embedded only (dev and demo; the prod mode is the host aeronmd that admin and
// cloud-console manage, checklist C13): the Aeron C media driver of the verified tree, in this process. One copy for
// ct-bridge (RC2 S9c 3.1, 3.3 step 4, 3.4, W4) and ct-forwarder (RC2 S10' section 8, rule 4): the process name, the dir
// prefix and the instance key are constructor inputs (EmbeddedDriverOptions), the rules are the same.
//   - dir <parent>/<dirPrefix><instance id>: per INSTANCE, not per pid (S9c review FIX-1, D-S9c-17), so a restart reuses
//     and cleans the same dir and no dead sibling can leak on the tmpfs; a LIVE driver already in it
//     (aeron_is_driver_active: a second instance with the same id on this host) refuses the start (78);
//   - the instance lock (coordinator decision Q3, 2026-11-01): flock(LOCK_EX | LOCK_NB) on <dir>.lock, next to the dir,
//     taken before the dir is looked at and held for the object's lifetime (the process lifetime in both processes; the
//     kernel drops it at any exit, kill -9 included; the file is never deleted, so every instance locks the same inode):
//     a second same-id instance refuses without touching the dir, also when both start in the same instant over a
//     stale dir. A LIVE driver dir is never deleted;
//   - SHARED threading (one driver thread), delete the dir on start and on shutdown;
//   - the termination hook is installed so that a validated TerminateDriver request is LOUD (exit 70); it is NOT a
//     death detector (S9c review FIX-4): a dead driver thread is seen by the client conductor's CnC driver timeout;
//   - any driver context / init / start error refuses the start (78, S9c review FIX-12: host configuration a restart
//     cannot fix: socket buffer limits), EXCEPT an init / start that failed for storage space (ENOSPC or Aeron's
//     STORAGE_SPACE: the CnC cannot be allocated on a full tmpfs; RC2 S10' review C13-6): startFailedForStorage() says
//     so; ct-forwarder retries it under the Q1 rule (StorageStarvation.h: 70 after 30 s, never 78, which would keep it
//     down for good after a passing tmpfs squeeze); ct-bridge's main still maps every start failure to 78;
//   - close() runs on a helper thread with a timed wait (the stop must not hang on it).
// manualMainLoop (the *_tc_driverstall test builds only): the driver's duty cycle runs on a thread this class owns, and
// stopDriving() stops it (the driver thread dead, the client alive: the 2026-08-12 shape, R-K13 (b)).

struct aeron_driver_context_stct;
struct aeron_driver_stct;

namespace cluster_tools { namespace app { namespace common {

struct EmbeddedDriverOptions
{
    std::string processName = "ct-process";  // "ct-bridge", "ct-forwarder": the refusal texts
    std::string dirPrefix = "ct-process-";   // "ct-bridge-", "ct-forwarder-": <parent>/<dirPrefix><id>
    std::string instanceKey = "INSTANCE_ID"; // "BRIDGE_INSTANCE_ID", "FWD_INSTANCE_ID": named in the refusal texts
    // Negative-control shapes, set only by the seamed builds of the owner (the owner's app seams decide):
    bool dirPerPid = false;    // EMBEDDED_DIR_PER_PID: the dir is per pid (a stale sibling is left behind)
    bool instanceLock = true;  // EMBEDDED_INSTANCE_LOCK off: no lock, two same-id starts both reclaim the dir
    bool liveCheck = true;     // EMBEDDED_LIVE_CHECK off: an existing dir is read as stale without the live-driver check
};

class EmbeddedDriver
{
public:
    using HookAction = std::function<void(const std::string &why)>;

    // The per-instance dir (per pid in the EMBEDDED_DIR_PER_PID RED shape).
    static std::string dirFor(const std::string &parent, std::int64_t instanceId, const EmbeddedDriverOptions &options);

    EmbeddedDriver(EmbeddedDriverOptions options, HookAction onTerminate);
    ~EmbeddedDriver();

    EmbeddedDriver(const EmbeddedDriver &) = delete;
    EmbeddedDriver &operator=(const EmbeddedDriver &) = delete;

    // False (with `error`) = refuse to start (78), or, when startFailedForStorage(), a storage-starved start the owner
    // retries with a NEW object (this one keeps the instance lock until it goes). reclaimed() says whether a stale dir
    // was reused and cleaned.
    bool start(const std::string &dir, bool manualMainLoop, std::string &error);
    bool startFailedForStorage() const noexcept { return isStorageSpaceErrcode(m_failureErrcode); }
    int failureErrcode() const noexcept { return m_failureErrcode; }
    bool reclaimed() const noexcept { return m_reclaimed; }
    const std::string &dir() const noexcept { return m_dir; }
    bool running() const noexcept { return nullptr != m_driver; }
    const EmbeddedDriverOptions &options() const noexcept { return m_options; }

    // tc_driverstall: the manual duty-cycle thread stops calling the driver (the driver is dead, the client alive).
    void stopDriving() noexcept { m_driving.store(false, std::memory_order_release); }

    // Closes the driver on a helper thread; true when it finished within `boundMs`.
    bool close(std::int64_t boundMs);

    // The termination hook's C entry (public for the C callback).
    void onTerminationHook();

private:
    EmbeddedDriverOptions m_options;
    HookAction m_onTerminate;
    std::string m_dir;
    aeron_driver_context_stct *m_context = nullptr;
    aeron_driver_stct *m_driver = nullptr;
    bool m_reclaimed = false;
    bool m_manual = false;
    std::atomic<bool> m_driving{ false };
    std::atomic<bool> m_stopLoop{ false };
    std::thread m_loop;
    int m_lockFd = -1;
    int m_failureErrcode = 0; // aeron_errcode() of a failed aeron_driver_init / aeron_driver_start (C13-6)
};

}}}

#endif
