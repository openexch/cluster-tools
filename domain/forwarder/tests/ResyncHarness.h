#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_RESYNC_HARNESS_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_TESTS_RESYNC_HARNESS_H

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "doctest/doctest.h"

#include "Fixtures.h"

#include "cluster_tools/domain/forwarder/ResyncMachine.h"

namespace cluster_tools { namespace domain { namespace forwarder { namespace testing {

inline constexpr std::int64_t SECOND = 1'000'000'000LL;

struct Harness
{
    SendRing ring;
    ResyncMachine m{ ring };
    std::int64_t now = 1'000 * SECOND;

    void dispatch(const Dispatch &d)
    {
        m.onCoreFrame(now);
        m.onDispatch(d, classify(d), now);
    }
    void entry(std::int64_t seq) { dispatch(fixtures::create(seq, 100'000 + seq)); }
    void entries(std::initializer_list<std::int64_t> seqs)
    {
        for (std::int64_t s : seqs)
        {
            entry(s);
        }
    }
    PendingRequest send()
    {
        REQUIRE(nullptr != m.requestDue());
        const PendingRequest r = *m.requestDue();
        m.onRequestSent(now);
        return r;
    }
    void end(std::int64_t correlationId, std::int32_t count, std::int64_t head)
    {
        m.onCoreFrame(now);
        m.onPendingEnd(correlationId, count, head, now);
    }
    void advance(std::int64_t ns)
    {
        now += ns;
        m.tick(now);
    }
    // C3 entry, probe {0, head}, then RESYNC(0) answered with an empty final page: LIVE at cursor = head.
    void registerLive(std::int64_t head = 0)
    {
        m.onRegistered(now);
        const PendingRequest probe = send();
        REQUIRE(probe.kind == RequestKind::PROBE);
        end(probe.correlationId, 0, head);
        const PendingRequest page = send();
        REQUIRE(page.kind == RequestKind::PAGE);
        end(page.correlationId, 0, head);
        REQUIRE(m.mode() == Mode::LIVE);
        REQUIRE(m.cursor() == head);
    }
    std::vector<std::int64_t> ringSeqs() const
    {
        std::vector<std::int64_t> out;
        for (std::size_t i = 0; i < ring.depth(); i++)
        {
            out.push_back(ring.at(i).seq);
        }
        return out;
    }
};

}}}}

#endif
