#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_VERDICTS_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_VERDICTS_H

// Failover: the per-source verdicts at the current e (8.1) and the decision when every source
// has one (8.3, rows 1-6) as a pure function. Rotation, the dwell, the timers and the confirming report live in
// BridgeDomain, which owns the clock and the commands.

#include "cluster_tools/archive/ArchiveMetrics.h"
#include "cluster_tools/domain/bridge/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cluster_tools { namespace domain { namespace bridge {

struct Verdict
{
    VerdictKind kind = VerdictKind::UNKNOWN;
    BadReason bad = BadReason::COUNT;
    std::int64_t seq = 0;   // HOLED: seen; BAD, FOREIGN_EPOCH: the frame's seq (0 when none)
    std::int64_t epoch = 0; // FOREIGN_EPOCH
    archive::FaultKind fault = archive::FaultKind::NONE;
    TimePoint since{};
};

class VerdictTable
{
public:
    explicit VerdictTable(std::size_t sources) noexcept : m_sources(sources) {}

    std::size_t sources() const noexcept { return m_sources; }
    std::int64_t e() const noexcept { return m_e; }
    bool hasFirst() const noexcept { return m_hasFirst; }
    TimePoint firstAt() const noexcept { return m_firstAt; }

    // Moves the table to `e`: every verdict but REFUSED_STATIC clears when e changes (8.1).
    void atE(std::int64_t e) noexcept;
    // Sets a verdict at the current e; true when it changed (the event is logged once).
    bool set(std::size_t source, const Verdict &v, TimePoint now) noexcept;
    const Verdict &at(std::size_t source) const noexcept { return m_v[source]; }
    void clearSource(std::size_t source) noexcept { m_v[source] = Verdict{}; }
    // Every verdict but REFUSED_STATIC (a forward, a sync start).
    void clearDynamic() noexcept;
    bool anyUnknown() const noexcept;
    bool all(VerdictKind kind) const noexcept;

private:
    std::size_t m_sources;
    std::array<Verdict, archive::MAX_SOURCES> m_v{};
    std::int64_t m_e = 0;
    bool m_hasFirst = false;
    TimePoint m_firstAt{};
};

enum class DecisionRow : std::uint8_t
{
    UNDECIDED,    // some source has no verdict yet
    NO_REACHABLE, // row 1: no halt, degraded NO_REACHABLE_SOURCE
    ALL_BEHIND,   // row 2: no halt, park on the last live source
    HOLE_GRACE,   // row 3: no halt yet, degraded HOLE_WITH_LAGGING_SOURCE until holeGrace
    GAP,          // row 4
    EPOCH,        // row 5
    SCHEMA        // row 6
};
const char *decisionRowName(DecisionRow row) noexcept;

inline constexpr std::size_t NO_SOURCE = std::numeric_limits<std::size_t>::max();

struct Decision
{
    DecisionRow row = DecisionRow::UNDECIDED;
    std::size_t target = NO_SOURCE; // rows 1-3: the source to stay on (NO_SOURCE: stay where the bridge is)
    std::int64_t seen = 0;          // row 4: min seen over the real HOLED sources; row 5: the foreign record's seq
    std::int64_t epochSeen = 0;     // row 5: the foreign epoch
};

// 8.3. `lastForwardSource` is the source that made the last forward (NO_SOURCE when nothing was forwarded yet).
Decision decide(const VerdictTable &table, bool graceExpired, std::size_t lastForwardSource) noexcept;

}}}

#endif
