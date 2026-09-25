#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>

#if defined(__x86_64__)
#include <x86intrin.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace matcher::bench {

// The cheapest trustworthy timestamp on each platform:
//   x86-64:  rdtscp (waits for earlier instructions) + lfence (stops later
//            ones starting early). Invariant TSC on any modern x86.
//   macOS:   mach_absolute_time(). Reading cntvct_el0 directly from user
//            space returns unusable values on macOS; this is Apple's supported
//            counter (24 MHz on Apple silicon, ~42 ns per tick).
//   aarch64 Linux: the generic timer's virtual count (cntvct_el0), after isb.
//   other:   std::chrono::steady_clock.
// Coarse counters quantize per-operation latencies, so the harness reports
// the measured resolution alongside results.
class Timer {
public:
    static std::uint64_t now() noexcept {
#if defined(__x86_64__)
        unsigned aux;
        const std::uint64_t t = __rdtscp(&aux);
        _mm_lfence();
        return t;
#elif defined(__APPLE__)
        return mach_absolute_time();
#elif defined(__aarch64__)
        std::uint64_t t;
        asm volatile("isb; mrs %0, cntvct_el0" : "=r"(t)::"memory");
        return t;
#else
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    static std::string_view source() noexcept {
#if defined(__x86_64__)
        return "rdtscp+lfence";
#elif defined(__APPLE__)
        return "mach_absolute_time";
#elif defined(__aarch64__)
        return "cntvct_el0";
#else
        return "steady_clock";
#endif
    }

    // Calibrates ticks against steady_clock over ~200 ms.
    Timer() {
        using clock = std::chrono::steady_clock;
        const auto wall0 = clock::now();
        const std::uint64_t t0 = now();
        while (clock::now() - wall0 < std::chrono::milliseconds(200)) {
        }
        const std::uint64_t t1 = now();
        const auto wall_ns = std::chrono::duration<double, std::nano>(clock::now() - wall0).count();
        ns_per_tick_ = wall_ns / static_cast<double>(t1 - t0);

        // Overhead: the smallest nonzero back-to-back difference, and the
        // typical cost of one timing pair.
        std::uint64_t smallest = UINT64_MAX;
        for (int i = 0; i < 100'000; ++i) {
            const std::uint64_t a = now();
            const std::uint64_t b = now();
            if (b > a) smallest = std::min(smallest, b - a);
        }
        resolution_ns_ = std::max(ns_per_tick_, static_cast<double>(smallest) * ns_per_tick_);
    }

    double to_ns(std::uint64_t ticks) const noexcept { return static_cast<double>(ticks) * ns_per_tick_; }
    double ns_per_tick() const noexcept { return ns_per_tick_; }
    double resolution_ns() const noexcept { return resolution_ns_; }

private:
    double ns_per_tick_ = 1.0;
    double resolution_ns_ = 1.0;
};

}  // namespace matcher::bench
