// doctest main for ct_forwarder_app_tests (RC2 S12 contract 11.3) with a counting global operator new: the
// zeroAllocDutyCycle test arms it around 100,000 dispatches through decode, ring and offer.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <atomic>
#include <cstdlib>
#include <new>

#include "AllocationCounter.h"

namespace cluster_tools { namespace app { namespace forwarder { namespace test {
std::atomic<bool> g_countAllocations{ false };
std::atomic<std::uint64_t> g_allocations{ 0 };
}}}}

namespace {
void *allocate(std::size_t size)
{
    if (cluster_tools::app::forwarder::test::g_countAllocations.load(std::memory_order_relaxed))
    {
        cluster_tools::app::forwarder::test::g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void *p = std::malloc(0 == size ? 1 : size);
    if (nullptr == p)
    {
        throw std::bad_alloc();
    }
    return p;
}
}

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
