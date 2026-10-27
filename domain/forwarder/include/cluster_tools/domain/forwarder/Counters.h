#ifndef CLUSTER_TOOLS_DOMAIN_FORWARDER_COUNTERS_H
#define CLUSTER_TOOLS_DOMAIN_FORWARDER_COUNTERS_H

#include <cstdint>

#include "cluster_tools/domain/forwarder/DispatchRules.h"

// Plain counters the forwarder agent owns (duty thread only) and exports through the StatusBoard
// snapshot (RC2 S12 contract section 10). Fixed-size, trivially copyable: no allocation, no locks.

namespace cluster_tools { namespace domain { namespace forwarder {

// forwarder_invalid_dispatch_total{kind, rule, action}: kind buckets 0..3 and 4 = any raw kind above 3.
inline constexpr unsigned KIND_BUCKETS = 5;

inline unsigned kindBucket(std::uint8_t kind) noexcept
{
    return kind <= KIND_RELEASE_ID ? kind : 4u;
}

struct InvalidDispatchCounts
{
    std::uint64_t counts[KIND_BUCKETS][RULE_COUNT][CLASS_COUNT] = {};

    void add(std::uint8_t kind, const Classification &c) noexcept
    {
        counts[kindBucket(kind)][static_cast<unsigned>(c.rule)][static_cast<unsigned>(c.cls)]++;
    }
    std::uint64_t total() const noexcept
    {
        std::uint64_t sum = 0;
        for (const auto &k : counts)
            for (const auto &r : k)
                for (unsigned c = 1; c < CLASS_COUNT; c++) // class 0 = PASS, never counted
                    sum += r[c];
        return sum;
    }
    std::uint64_t byClass(DispatchClass cls) const noexcept
    {
        std::uint64_t sum = 0;
        for (const auto &k : counts)
            for (const auto &r : k)
                sum += r[static_cast<unsigned>(cls)];
        return sum;
    }
};

inline constexpr unsigned CLUSTER_CORE = 0;
inline constexpr unsigned CLUSTER_ME = 1;
inline constexpr unsigned TEMPLATE_BUCKETS = 256; // every engine template id is below 256; 255 = "other"

struct ForwarderCounters
{
    InvalidDispatchCounts invalid;

    std::uint64_t sentTemplate9 = 0;
    std::uint64_t sentTemplate10 = 0;
    std::uint64_t releaseEntriesSent = 0;
    std::uint64_t meBackPressure = 0;
    std::uint64_t meOfferFailures = 0;
    std::uint64_t mePollSaturated = 0;

    std::uint64_t coreStaleTerm = 0;
    std::uint64_t coreFutureTerm = 0;
    std::uint64_t coreMalformed = 0;
    std::uint64_t dispatchSeqBelowOne = 0; // review NOTE-2: 104/105 frames refused for an entry with dispatchSeq < 1
    std::uint64_t unknownTemplate[2][TEMPLATE_BUCKETS] = {};
    std::uint64_t coreControlFrames = 0; // OrderFault / ConfigApplied on CH_CONTROL: counted, dropped (Q9)

    std::uint64_t connectClusterErrors[2] = {};
    std::uint64_t meEpochReconnects = 0;
    std::uint64_t registrationLost = 0; // UNREGISTERED_SESSION / UNAUTHORIZED_CLASS naming 65
    std::uint64_t requestOfferBackPressured = 0;
    std::uint64_t admissionOrigin = 0;
    std::uint64_t admissionBroadcast = 0;
    std::uint64_t pendingEnds = 0;

    void unknown(unsigned cluster, std::uint16_t templateId) noexcept
    {
        unknownTemplate[cluster][templateId < TEMPLATE_BUCKETS ? templateId : TEMPLATE_BUCKETS - 1]++;
    }
};

}}}

#endif
