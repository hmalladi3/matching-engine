#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace matcher {

inline constexpr std::size_t kHugePageBytes = std::size_t{2} << 20;

// Allocator for the book's two large arrays (node pool, order index).
//
// Requests of 2 MiB or more are 2 MiB-aligned and, on Linux, advised with
// MADV_HUGEPAGE before anything touches them, so the kernel can back them with
// 2 MB pages from the first fault even where transparent hugepages are opt-in
// (the "madvise" policy, Ubuntu's default). At ~32 MB per array for 10^6
// orders, 4 KB pages would make TLB misses as costly as cache misses.
// Smaller requests use the ordinary heap.
template <class T>
struct HugePageAllocator {
    using value_type = T;

    HugePageAllocator() noexcept = default;
    template <class U>
    HugePageAllocator(const HugePageAllocator<U>& /*other*/) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        const std::size_t bytes = n * sizeof(T);
        if (bytes < kHugePageBytes) return static_cast<T*>(::operator new(bytes));

        const std::size_t rounded = (bytes + kHugePageBytes - 1) & ~(kHugePageBytes - 1);
        void* memory = std::aligned_alloc(kHugePageBytes, rounded);
        if (memory == nullptr) throw std::bad_alloc();
#if defined(__linux__)
        (void)madvise(memory, rounded, MADV_HUGEPAGE);  // a hint; failure only costs performance
#endif
        return static_cast<T*>(memory);
    }

    void deallocate(T* memory, std::size_t n) noexcept {
        if (n * sizeof(T) < kHugePageBytes)
            ::operator delete(memory);
        else
            std::free(memory);
    }

    template <class U>
    friend bool operator==(const HugePageAllocator& /*lhs*/, const HugePageAllocator<U>& /*rhs*/) noexcept {
        return true;
    }
};

}  // namespace matcher
