// Shared test-only allocation counter.
//
// The technique is the one ticket 28 used in `tests/dsp/voice_test.cpp` and ticket 30 reused for the
// plugin's audio path: **replace the global allocation functions with a counter**. A tracking allocator
// observes every allocation from every translation unit in this binary, which is what catches an
// allocation in code nobody suspected — a weaker instrument (a counting allocator passed to one class)
// only sees the code that was told about it.
//
// It lives in its own translation unit because the operators may be defined exactly once per binary:
// the real-time invariant and the interface's own no-interface-on-the-audio-path check both measure with
// it, and both must measure the same hook.

#pragma once

#include <atomic>
#include <cstddef>

namespace xerum::plugin::tests
{

/// Inert until a test turns tracking on, so Catch2's and JUCE's own allocations are not counted.
extern std::atomic<long> g_allocations;
extern std::atomic<bool> g_tracking;

inline void resetAllocations() { g_allocations.store (0, std::memory_order_relaxed); }
inline void setTracking (const bool on) { g_tracking.store (on, std::memory_order_relaxed); }
inline long allocations() { return g_allocations.load (std::memory_order_relaxed); }

/// Prove the hook is installed and counting: one allocation the optimiser cannot elide, and whether the
/// counter saw it. Turns tracking on and off around the probe, so it must not be called inside a measured
/// window.
///
/// The allocation goes through a `volatile` function pointer on purpose. A plain `std::make_unique<int>`
/// in a test is a new/delete pair the optimiser may remove entirely — measured here: once the operators
/// moved out of the test's own translation unit, clang elided it, and a control that has been optimised
/// away proves nothing while looking like it does.
bool hookIsLive();

} // namespace xerum::plugin::tests
