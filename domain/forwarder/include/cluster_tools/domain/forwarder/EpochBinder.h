#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_EPOCH_BINDER_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_EPOCH_BINDER_H

#include <cstdint>

// EpochBinder - the ME epoch the forwarder learns and then insists on (RC2 S12 contract 4.2).
//
// The process epoch E is set ONCE, by the first ClusterHeartbeat with meEpoch != 0 on any ME session,
// and never changes (it is the sourceEpoch of every RegisterSession). Each ME session starts UNVERIFIED
// (M2: nothing may be offered to it) and becomes VERIFIED (M3) on a heartbeat whose meEpoch equals E.
//   M2: meEpoch 0 ignored (the ME is not FRESH yet); first non-zero binds; equal verifies; different = fatal 78
//   M3: a heartbeat with meEpoch != E, 0 included (the ME lost its identity), = fatal 78
//   M2 for EPOCH_WAIT_NS while connected without a verifying heartbeat = reconnect (the W9 hello of the next
//   session delivers the epoch).

namespace cluster_tools { namespace domain { namespace forwarder {

enum class EpochVerdict : std::uint8_t
{
    NONE,
    BOUND,          // E was bound by this heartbeat (and the session verified)
    VERIFIED,       // the session reached M3
    FATAL_MISMATCH, // meEpoch != E (exit 78, F8)
    FATAL_ZERO,     // meEpoch 0 after the session was verified (exit 78, F8)
    RECONNECT,      // no verifying heartbeat within EPOCH_WAIT_NS
};

const char *nameOf(EpochVerdict verdict) noexcept;

class EpochBinder
{
public:
    static constexpr std::int64_t EPOCH_WAIT_NS = 5'000'000'000LL;

    // A new ME session is CONNECTED (M2).
    void onSessionStart(std::int64_t nowNs) noexcept;
    // The ME session ended (M0): nothing verified any more.
    void onSessionEnd() noexcept;
    EpochVerdict onHeartbeat(std::int64_t meEpoch, std::int64_t nowNs) noexcept;
    // RECONNECT when the current session has waited EPOCH_WAIT_NS unverified (once per session).
    EpochVerdict tick(std::int64_t nowNs) noexcept;

    bool bound() const noexcept { return 0 != m_epoch; }
    std::int64_t epoch() const noexcept { return m_epoch; }
    bool verified() const noexcept { return m_active && m_verified; }
    bool sessionActive() const noexcept { return m_active; }
    std::int64_t lastSeenEpoch() const noexcept { return m_lastSeen; }

private:
    std::int64_t m_epoch = 0;
    std::int64_t m_lastSeen = 0;
    bool m_active = false;
    bool m_verified = false;
    bool m_reconnectReported = false;
    std::int64_t m_sessionStartNs = 0;
};

}}}

#endif
