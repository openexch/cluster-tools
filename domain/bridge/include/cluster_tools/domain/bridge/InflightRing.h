#ifndef CLUSTER_TOOLS_DOMAIN_BRIDGE_INFLIGHT_RING_H
#define CLUSTER_TOOLS_DOMAIN_BRIDGE_INFLIGHT_RING_H

// The in-flight ring and the recording key table (RC2 S9b 6.2, cpp 2.5 as amended by S9a 5.2 rule 2): every
// record whose offer returned OK, ascending in seq (G1), until a report with C' >= seq pops it and proves it on
// the memo of the source it was read from (never another source). Preallocated; nothing allocates after
// construction.

#include "cluster_tools/archive/Chain.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cluster_tools { namespace domain { namespace bridge {

// 24 bytes (6.2). `moneyTemplate` is the contract's reserved u16: the frame kind the record travelled in
// (61..68), kept for the proof seam and diagnostics.
struct RingEntry
{
    std::int64_t seq = 0;
    std::int64_t endPosition = 0;
    std::uint16_t sourceIndex = 0;
    std::uint16_t moneyTemplate = 0;
    std::uint32_t recordingKeyIndex = 0;
};
static_assert(sizeof(RingEntry) == 24);

// The largest single push (a 62 at the cap): "full" means fewer free slots than this (review N-4).
inline constexpr std::size_t RING_PUSH_MAX = 64;

class InflightRing
{
public:
    explicit InflightRing(std::size_t capacity);

    std::size_t capacity() const noexcept { return m_capacity; }
    std::size_t size() const noexcept { return m_size; }
    bool empty() const noexcept { return 0 == m_size; }
    std::size_t freeSlots() const noexcept { return m_capacity - m_size; }
    bool full() const noexcept;
    bool push(const RingEntry &entry) noexcept; // false when no slot is left (the RING_UNBOUNDED seam only)
    const RingEntry &back() const noexcept { return at(m_size - 1); }
    const RingEntry &front() const noexcept { return at(0); }
    const RingEntry &at(std::size_t i) const noexcept { return m_entries[(m_head + i) % m_capacity]; }
    std::uint64_t dropped() const noexcept { return m_dropped; }

    // Pops every entry with seq <= cursor; for each run of consecutive entries with the same (source, key) the
    // visitor is called ONCE with the run's last (largest) entry. Returns the number popped.
    template<typename Visitor>
    std::size_t popThrough(std::int64_t cursor, Visitor &&visit) noexcept
    {
        std::size_t popped = 0;
        bool haveRun = false;
        RingEntry run{};
        while (m_size > 0 && at(0).seq <= cursor)
        {
            const RingEntry e = at(0);
            if (haveRun && (e.sourceIndex != run.sourceIndex || e.recordingKeyIndex != run.recordingKeyIndex))
            {
                visit(run);
            }
            run = e;
            haveRun = true;
            m_head = (m_head + 1) % m_capacity;
            --m_size;
            ++popped;
        }
        if (haveRun)
        {
            visit(run);
        }
        return popped;
    }
    // Drops every entry (at a sync start, after the pop: the rest is possibly lost and re-read).
    std::size_t clear() noexcept;

private:
    std::vector<RingEntry> m_entries;
    std::size_t m_capacity;
    std::size_t m_head = 0;
    std::size_t m_size = 0;
    std::uint64_t m_dropped = 0;
};

// Per source, the RecordingKeys of offered records interned to u32 (6.2): ring entries and staged trades carry
// the index, not the 40-byte key.
class KeyTable
{
public:
    KeyTable(std::size_t sources, std::size_t capacityPerSource);

    // The index of `key` for `source`, interning it when new; false when the table has no free index for it.
    bool intern(std::size_t source, const archive::RecordingKey &key, std::uint32_t &index) noexcept;
    const archive::RecordingKey &key(std::size_t source, std::uint32_t index) const noexcept;
    void clear() noexcept;
    std::size_t size(std::size_t source) const noexcept { return m_used[source]; }
    std::size_t capacity() const noexcept { return m_capacity; }

private:
    std::size_t m_capacity;
    std::vector<std::vector<archive::RecordingKey>> m_keys;
    std::vector<std::size_t> m_used;
    std::vector<std::size_t> m_lastHit; // the index of the last lookup (the common case: the same recording)
};

}}}

#endif
