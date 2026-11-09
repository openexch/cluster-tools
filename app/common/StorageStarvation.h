#ifndef CLUSTER_TOOLS_APP_COMMON_STORAGE_STARVATION_H
#define CLUSTER_TOOLS_APP_COMMON_STORAGE_STARVATION_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string_view>

// StorageStarvation - "our own resource is refused for storage space" held CONTINUOUSLY ends the process.
// A process whose publication (or replay image) the media driver refuses for insufficient storage stays
// alive and retries forever otherwise: alive, not ready, never progressing (the false-green shape of the 2026-07-25
// /dev/shm starvation that left the settlement bridge silent for 17 h). The rule, one copy for ct-forwarder and
// ct-bridge:
//   - every attempt that failed for storage space marks the episode (its start time is the first such failure);
//   - an attempt that succeeded, or that failed for ANOTHER reason, ends it (the starvation is no longer continuous);
//   - while it lasts /ready is 503 (the owner's not-ready bit) and <process>_storage_starved_seconds_total grows;
//   - STORAGE_STARVED_EXIT_NS after its start the owner exits 70: restartable (systemd restarts it, loud in the restart
//     log and the alert), never 78, which would stop it for good after a passing tmpfs squeeze.
// The 30 s is a constant in code, not a configuration key. Pure: no Aeron, no clock of its own (the owner's ns clock).

namespace cluster_tools { namespace app { namespace common {

inline constexpr std::int64_t STORAGE_STARVED_EXIT_NS = 30'000'000'000LL;

// Aeron's STORAGE_SPACE (AERON_ERROR_CODE_STORAGE_SPACE 12; the C client renders it "(-12) insufficient storage space",
// the driver "insufficient usable storage for new log ..."; the archive's own code is classified by its adapter).
inline bool isStorageSpaceError(std::string_view detail) noexcept
{
    return std::string_view::npos != detail.find("insufficient storage space") ||
        std::string_view::npos != detail.find("insufficient usable storage") ||
        std::string_view::npos != detail.find("(-12) ");
}

// The same class for an aeron_errcode(): ENOSPC, the errno a file allocation in the driver dir
// fails with (aeron_driver_init on a full tmpfs: "(28) No space left on device ... cnc.dat", measured by tmpfs-min), or
// -AERON_ERROR_CODE_STORAGE_SPACE (Aeron's own codes are negative; +12 is ENOMEM, not storage).
inline constexpr int AERON_STORAGE_SPACE_CODE = -12; // -AERON_ERROR_CODE_STORAGE_SPACE
inline constexpr bool isStorageSpaceErrcode(int aeronErrcode) noexcept
{
    return ENOSPC == aeronErrcode || AERON_STORAGE_SPACE_CODE == aeronErrcode;
}

class StorageStarvation
{
public:
    // An attempt failed for storage space at `nowNs`; `why` names it (copied, truncated; the onset only).
    // Returns true at the onset of an episode (the owner logs it once).
    bool onStarved(std::int64_t nowNs, std::string_view why) noexcept
    {
        if (m_active)
        {
            return false;
        }
        m_active = true;
        m_sinceNs = nowNs;
        const std::size_t n = why.size() < sizeof(m_why) - 1 ? why.size() : sizeof(m_why) - 1;
        for (std::size_t i = 0; i < n; ++i)
        {
            m_why[i] = why[i];
        }
        m_why[n] = '\0';
        m_episodes++;
        return true;
    }

    // An attempt succeeded, or failed for another reason: the starvation is not continuous any more.
    void onRelieved(std::int64_t nowNs) noexcept
    {
        if (m_active)
        {
            m_closedNs += nowNs - m_sinceNs;
            m_active = false;
        }
    }

    bool active() const noexcept { return m_active; }
    std::int64_t heldNs(std::int64_t nowNs) const noexcept { return m_active ? nowNs - m_sinceNs : 0; }
    bool expired(std::int64_t nowNs) const noexcept { return m_active && nowNs - m_sinceNs >= STORAGE_STARVED_EXIT_NS; }
    std::uint64_t episodes() const noexcept { return m_episodes; }
    const char *why() const noexcept { return m_why; }

    // <process>_storage_starved_seconds_total: every starved second so far (closed episodes plus the current one).
    std::uint64_t starvedSeconds(std::int64_t nowNs) const noexcept
    {
        const std::int64_t total = m_closedNs + heldNs(nowNs);
        return total > 0 ? static_cast<std::uint64_t>(total / 1'000'000'000LL) : 0;
    }

private:
    bool m_active = false;
    std::int64_t m_sinceNs = 0;
    std::int64_t m_closedNs = 0;
    std::uint64_t m_episodes = 0;
    char m_why[200] = {};
};

// The per-attempt feed from a ClusterClient (counters().connectAttemptsFailed, lastDetail(), state()): call once per
// duty iteration. A newly failed attempt is starved or relieving by its detail; a connected client relieves.
template<typename Client>
class ClientStorageFeed
{
public:
    // Returns true at the onset of an episode.
    bool update(const Client &client, bool connected, std::int64_t nowNs, StorageStarvation &watch) noexcept
    {
        bool onset = false;
        const std::uint64_t failed = client.counters().connectAttemptsFailed;
        if (failed != m_seenFailed)
        {
            m_seenFailed = failed;
            const std::string_view detail(client.lastDetail());
            if (isStorageSpaceError(detail))
            {
                onset = watch.onStarved(nowNs, detail);
            }
            else
            {
                watch.onRelieved(nowNs);
            }
        }
        if (connected)
        {
            watch.onRelieved(nowNs);
        }
        return onset;
    }

private:
    std::uint64_t m_seenFailed = 0;
};

}}}

#endif
