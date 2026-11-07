#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_SEND_RING_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_SEND_RING_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "cluster_tools/domain/forwarder/DispatchRules.h"

// SendRing - the fixed 65,536-slot FIFO of classified dispatches in their 5.5 form, between the
// resync machine (producer) and the ME offers (consumer), and its drain plan (RC2 S12 5.3, 7).
//
// Drain plan: the frame at the ring head is
//   - one template 9 frame for a COMMAND slot;
//   - one template 10 frame of n = 1 for a RELEASE_ISOLATED (RX) slot;
//   - one template 10 frame of the maximal run of RELEASE (P/RA) slots at the head, n <= 48; a COMMAND
//     or RELEASE_ISOLATED slot ends the run; nothing waits for more entries.
// Slots are popped only after a positive offer (pop is the caller's, after OK). One allocation, at
// construction; push/peek/pop/clear allocate nothing.

namespace cluster_tools { namespace domain { namespace forwarder {

enum class FrameType : std::uint8_t
{
    NONE,
    COMMAND,  // template 9, count 1
    RELEASES, // template 10, count 1..48
};

struct FramePlan
{
    FrameType type = FrameType::NONE;
    std::uint32_t count = 0;
    std::int64_t firstSeq = 0; // the frame's releaseSeq (template 10)
};

template<typename T, std::size_t N>
class FixedRing
{
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

public:
    static constexpr std::size_t CAPACITY = N;

    FixedRing() : m_slots(new T[N]) {}

    bool push(const T &value) noexcept
    {
        if (full())
        {
            return false;
        }
        m_slots[m_tail & (N - 1)] = value;
        m_tail++;
        return true;
    }
    std::size_t size() const noexcept { return static_cast<std::size_t>(m_tail - m_head); }
    bool empty() const noexcept { return m_tail == m_head; }
    bool full() const noexcept { return size() == N; }
    // i = 0 is the oldest element.
    const T &at(std::size_t i) const noexcept { return m_slots[(m_head + i) & (N - 1)]; }
    T &at(std::size_t i) noexcept { return m_slots[(m_head + i) & (N - 1)]; }
    void popFront(std::size_t n) noexcept { m_head += (n > size() ? size() : n); }
    void popBack(std::size_t n) noexcept { m_tail -= (n > size() ? size() : n); }
    std::size_t clear() noexcept
    {
        const std::size_t n = size();
        m_head = m_tail;
        return n;
    }

private:
    std::unique_ptr<T[]> m_slots;
    std::uint64_t m_head = 0;
    std::uint64_t m_tail = 0;
};

// Test-only observation of every push and clear (the property test's push log); null in production.
struct SendRingObserver
{
    virtual void onPush(const RingSlot &slot) = 0;
    virtual void onClear(std::size_t entries) = 0;

protected:
    ~SendRingObserver() = default;
};

class SendRing
{
public:
    static constexpr std::size_t CAPACITY = 65'536;
    static constexpr std::uint32_t RELEASE_FRAME_CAP = 48;

    bool push(const RingSlot &slot) noexcept
    {
        if (!m_ring.push(slot))
        {
            m_overflows++;
            return false;
        }
        if (nullptr != m_observer)
        {
            m_observer->onPush(slot);
        }
        return true;
    }
    std::size_t depth() const noexcept { return m_ring.size(); }
    bool empty() const noexcept { return m_ring.empty(); }
    bool full() const noexcept { return m_ring.full(); }
    const RingSlot &at(std::size_t i) const noexcept { return m_ring.at(i); }

    FramePlan nextFrame() const noexcept;
    void pop(std::size_t n) noexcept { m_ring.popFront(n); }
    std::size_t clear() noexcept
    {
        const std::size_t n = m_ring.clear();
        if (nullptr != m_observer)
        {
            m_observer->onClear(n);
        }
        return n;
    }
    std::uint64_t overflows() const noexcept { return m_overflows; }
    void setObserverForTest(SendRingObserver *observer) noexcept { m_observer = observer; }

private:
    SendRingObserver *m_observer = nullptr;
    FixedRing<RingSlot, CAPACITY> m_ring;
    std::uint64_t m_overflows = 0;
};

}}}

#endif
