#ifndef CLUSTER_TOOLS_APP_BRIDGE_EMBEDDED_DRIVER_H
#define CLUSTER_TOOLS_APP_BRIDGE_EMBEDDED_DRIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

// EmbeddedDriver - BRIDGE_DRIVER_MODE=embedded only (dev and demo; the prod default is the host aeronmd, checklist
// C13): the Aeron C media driver of the verified tree, in this process (RC2 S9c 3.1, 3.3 step 4, 3.4, W4).
//   - dir <BRIDGE_DRIVER_DIR>/ct-bridge-<BRIDGE_INSTANCE_ID>: per INSTANCE, not per pid (review FIX-1, D-S9c-17), so
//     a restart reuses and cleans the same dir and no dead sibling can leak on the tmpfs; a LIVE driver already in it
//     (aeron_is_driver_active: a second instance with the same id on this host) refuses the start (78);
//   - the instance lock (coordinator decision Q3, 2026-11-01): flock(LOCK_EX | LOCK_NB) on <dir>.lock, next to the dir,
//     taken before the dir is looked at and held for the process lifetime (the kernel drops it at any exit, kill -9
//     included; the file is never deleted, so every instance locks the same inode): a second same-id instance exits
//     78 without touching the dir, also when both start in the same instant over a stale dir;
//   - SHARED threading (one driver thread), delete the dir on start and on shutdown;
//   - the termination hook is installed so that a validated TerminateDriver request is LOUD (F2, exit 70); it is NOT a
//     death detector (review FIX-4): a dead driver thread is seen by the client conductor's CnC driver timeout (F1);
//   - any driver context / init / start error refuses the start (78, review FIX-12: host configuration a restart
//     cannot fix: socket buffer limits, a tmpfs too small for the CnC);
//   - close() runs on a helper thread with a timed wait (3.4: the stop must not hang on it).
// manualMainLoop (the ct_bridge_tc_driverstall build only): the driver's duty cycle runs on a thread this class owns,
// and stopDriving() stops it (the driver thread dead, the client alive: the exact 2026-08-12 shape, R-K13 (b)).

struct aeron_driver_context_stct;
struct aeron_driver_stct;

namespace cluster_tools { namespace app { namespace bridge {

class EmbeddedDriver
{
public:
    using HookAction = std::function<void(const std::string &why)>;

    // The per-instance dir (EMBEDDED_DIR_PER_PID RED: per pid).
    static std::string dirFor(const std::string &parent, std::int64_t instanceId);

    explicit EmbeddedDriver(HookAction onTerminate);
    ~EmbeddedDriver();

    EmbeddedDriver(const EmbeddedDriver &) = delete;
    EmbeddedDriver &operator=(const EmbeddedDriver &) = delete;

    // False (with `error`) = refuse to start (78). reclaimed() says whether a stale dir was reused and cleaned.
    bool start(const std::string &dir, bool manualMainLoop, std::string &error);
    bool reclaimed() const noexcept { return m_reclaimed; }
    const std::string &dir() const noexcept { return m_dir; }
    bool running() const noexcept { return nullptr != m_driver; }

    // tc_driverstall: the manual duty-cycle thread stops calling the driver (the driver is dead, the client alive).
    void stopDriving() noexcept { m_driving.store(false, std::memory_order_release); }

    // Closes the driver on a helper thread; true when it finished within `boundMs`.
    bool close(std::int64_t boundMs);

    // The termination hook's C entry (public for the C callback).
    void onTerminationHook();

private:
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
};

}}}

#endif
