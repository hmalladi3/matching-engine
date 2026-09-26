#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "matcher/huge_page_allocator.h"

namespace matcher {
namespace {

constexpr std::size_t k2MiB = std::size_t{2} << 20;

// @spec BOOK-MEM-006
TEST(HugePageAllocator, LargeAllocationsAre2MiBAligned) {
    std::vector<std::uint64_t, HugePageAllocator<std::uint64_t>> big(3 * k2MiB / sizeof(std::uint64_t));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(big.data()) % k2MiB, 0u);
    big.back() = 42;  // usable to the end
    EXPECT_EQ(big.back(), 42u);
}

TEST(HugePageAllocator, SmallAllocationsUseTheOrdinaryHeap) {
    std::vector<int, HugePageAllocator<int>> small(10, 7);
    small.push_back(8);
    EXPECT_EQ(small.size(), 11u);
    EXPECT_EQ(small.back(), 8);
}

TEST(HugePageAllocator, GrowthAcrossTheThresholdPreservesContents) {
    std::vector<std::uint32_t, HugePageAllocator<std::uint32_t>> v;
    for (std::uint32_t i = 0; i < 2'000'000; ++i) v.push_back(i);  // crosses 2 MiB and reallocates
    for (std::uint32_t i = 0; i < 2'000'000; i += 99'991) ASSERT_EQ(v[i], i);
}

#if defined(__linux__)
// Reads the kernel's transparent-hugepage policy, e.g. "always [madvise] never".
std::string thp_policy() {
    std::ifstream f("/sys/kernel/mm/transparent_hugepage/enabled");
    std::string line;
    std::getline(f, line);
    return line;
}

// Sums AnonHugePages (KiB) over the mappings that contain [begin, end).
long anon_huge_kib(std::uintptr_t begin, std::uintptr_t end) {
    std::ifstream smaps("/proc/self/smaps");
    long total = 0;
    bool inside = false;
    for (std::string line; std::getline(smaps, line);) {
        std::uintptr_t lo = 0, hi = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx", &lo, &hi) == 2 && line.find(' ') != std::string::npos &&
            line.find('-') < line.find(' ')) {
            inside = lo < end && hi > begin;
        } else if (inside && line.rfind("AnonHugePages:", 0) == 0) {
            total += std::stol(line.substr(14));
        }
    }
    return total;
}

// With the kernel's policy at "madvise" (the Ubuntu default) memory is backed by
// huge pages only if it asks; this proves the allocator asks.
// @spec BOOK-MEM-006
TEST(HugePageAllocator, LinuxBacksLargeAllocationsWithHugePages) {
    const std::string policy = thp_policy();
    if (policy.empty() || policy.find("[never]") != std::string::npos)
        GTEST_SKIP() << "transparent hugepages disabled on this kernel: '" << policy << "'";
    std::vector<char, HugePageAllocator<char>> big(8 * k2MiB);
    std::memset(big.data(), 1, big.size());  // first touch
    const auto begin = reinterpret_cast<std::uintptr_t>(big.data());
    EXPECT_GT(anon_huge_kib(begin, begin + big.size()), 0) << "policy: " << policy;
}
#endif

}  // namespace
}  // namespace matcher
