#include "cluster_tools/domain/bridge/InflightRing.h"

#include "cluster_tools/domain/bridge/TestSeams.h"

#include <stdexcept>

namespace cluster_tools { namespace domain { namespace bridge {

using test_seams::Rule;
using test_seams::disabled;

InflightRing::InflightRing(std::size_t capacity) : m_entries(capacity), m_capacity(capacity)
{
    if (capacity < RING_PUSH_MAX)
    {
        throw std::invalid_argument("InflightRing: capacity below one capped flush (64)");
    }
}

bool InflightRing::full() const noexcept
{
    if (disabled(Rule::RING_UNBOUNDED))
    {
        return false; // RED shape: reading never pauses for the ring
    }
    if (disabled(Rule::RING_FULL_AT_ZERO))
    {
        return 0 == freeSlots(); // RED shape (full at zero)
    }
    return freeSlots() < RING_PUSH_MAX;
}

bool InflightRing::push(const RingEntry &entry) noexcept
{
    if (m_size == m_capacity)
    {
        ++m_dropped; // only reachable with the RING_UNBOUNDED / RING_FULL_AT_ZERO seams
        return false;
    }
    m_entries[(m_head + m_size) % m_capacity] = entry;
    ++m_size;
    return true;
}

std::size_t InflightRing::clear() noexcept
{
    const std::size_t n = m_size;
    m_head = 0;
    m_size = 0;
    return n;
}

KeyTable::KeyTable(std::size_t sources, std::size_t capacityPerSource) :
    m_capacity(capacityPerSource), m_keys(sources, std::vector<archive::RecordingKey>(capacityPerSource)),
    m_used(sources, 0), m_lastHit(sources, 0)
{
    if (0 == capacityPerSource)
    {
        throw std::invalid_argument("KeyTable: capacity 0");
    }
}

bool KeyTable::intern(std::size_t source, const archive::RecordingKey &key, std::uint32_t &index) noexcept
{
    auto &keys = m_keys[source];
    std::size_t &used = m_used[source];
    std::size_t &hit = m_lastHit[source];
    if (hit < used && keys[hit] == key)
    {
        index = static_cast<std::uint32_t>(hit);
        return true;
    }
    for (std::size_t i = used; i-- > 0;)
    {
        if (keys[i] == key)
        {
            hit = i;
            index = static_cast<std::uint32_t>(i);
            return true;
        }
    }
    if (used == m_capacity)
    {
        if (disabled(Rule::KEYTABLE_UNBOUNDED))
        {
            keys[0] = key; // RED shape: an index is reused under the ring's feet
            hit = 0;
            index = 0;
            return true;
        }
        return false;
    }
    keys[used] = key;
    hit = used;
    index = static_cast<std::uint32_t>(used);
    ++used;
    return true;
}

const archive::RecordingKey &KeyTable::key(std::size_t source, std::uint32_t index) const noexcept
{
    return m_keys[source][index];
}

void KeyTable::clear() noexcept
{
    for (auto &u : m_used)
    {
        u = 0;
    }
    for (auto &h : m_lastHit)
    {
        h = 0;
    }
}

}}}
