#ifndef CLUSTER_TOOLS_APP_FORWARDER_TESTS_ALLOCATION_COUNTER_H
#define CLUSTER_TOOLS_APP_FORWARDER_TESTS_ALLOCATION_COUNTER_H

#include <atomic>
#include <cstdint>

namespace cluster_tools { namespace app { namespace forwarder { namespace test {
extern std::atomic<bool> g_countAllocations;
extern std::atomic<std::uint64_t> g_allocations;
}}}}

#endif
