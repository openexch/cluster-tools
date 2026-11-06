#include "cluster_tools/domain/forwarder/SendRing.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

namespace cluster_tools { namespace domain { namespace forwarder {

FramePlan SendRing::nextFrame() const noexcept
{
    FramePlan plan;
    if (m_ring.empty())
    {
        return plan;
    }
    const RingSlot &head = m_ring.at(0);
    plan.firstSeq = head.seq;
    plan.count = 1;
    switch (head.form)
    {
        case SlotForm::COMMAND:
            plan.type = FrameType::COMMAND;
            return plan;
        case SlotForm::RELEASE_ISOLATED:
            plan.type = FrameType::RELEASES;
            return plan;
        case SlotForm::RELEASE:
            break;
    }
    plan.type = FrameType::RELEASES;
    std::uint32_t cap = RELEASE_FRAME_CAP;
    if (test_seams::disabled(test_seams::Rule::RING_GROUP_CAP_48))
    {
        cap = static_cast<std::uint32_t>(CAPACITY);
    }
    if (test_seams::disabled(test_seams::Rule::RING_GROUP_RELEASES))
    {
        cap = 1;
    }
    const std::size_t depth = m_ring.size();
    while (plan.count < cap && plan.count < depth && SlotForm::RELEASE == m_ring.at(plan.count).form)
    {
        plan.count++;
    }
    return plan;
}

}}}
