// The global allocation hook behind `AllocationCounter.h`. Test-only.

#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

namespace xerum::plugin::tests
{
std::atomic<long> g_allocations { 0 };
std::atomic<bool> g_tracking { false };
} // namespace xerum::plugin::tests

namespace
{
using xerum::plugin::tests::g_allocations;
using xerum::plugin::tests::g_tracking;

void noteAllocation()
{
    if (g_tracking.load (std::memory_order_relaxed))
        g_allocations.fetch_add (1, std::memory_order_relaxed);
}

void* trackedAllocate (const std::size_t size)
{
    noteAllocation();

    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;

    throw std::bad_alloc();
}
} // namespace

namespace xerum::plugin::tests
{

bool hookIsLive()
{
    // Through a `volatile` function pointer: the optimiser cannot see which allocation function is called,
    // so it cannot fold the pair away. See the note in the header.
    void* (*volatile allocate) (std::size_t) = &::operator new;
    void (*volatile release) (void*) noexcept = &::operator delete;

    setTracking (true);
    const auto before = allocations();

    release (allocate (64));

    const auto counted = allocations() > before;
    setTracking (false);

    return counted;
}

} // namespace xerum::plugin::tests

void* operator new (std::size_t size) { return trackedAllocate (size); }
void* operator new[] (std::size_t size) { return trackedAllocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    noteAllocation();
    return std::malloc (size == 0 ? 1 : size);
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    noteAllocation();
    return std::malloc (size == 0 ? 1 : size);
}
void* operator new (std::size_t size, const std::align_val_t alignment)
{
    noteAllocation();

    void* p = nullptr;

    if (posix_memalign (&p, static_cast<std::size_t> (alignment), size == 0 ? 1 : size) != 0)
        throw std::bad_alloc();

    return p;
}
void* operator new[] (std::size_t size, const std::align_val_t alignment) { return operator new (size, alignment); }

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete (void*, std::size_t, std::align_val_t) noexcept {}
void operator delete[] (void*, std::size_t, std::align_val_t) noexcept {}
