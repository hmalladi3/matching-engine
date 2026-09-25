#include <gtest/gtest.h>

#include <set>

#include "matcher/node_pool.h"

namespace matcher {
namespace {

// @spec BOOK-MEM-001
TEST(NodePool, StartsWithFullReservedCapacity) {
    NodePool pool(100);
    EXPECT_EQ(pool.capacity(), 100u);
    EXPECT_EQ(pool.live(), 0u);
    EXPECT_EQ(pool.available(), 100u);
}

TEST(NodePool, AcquireHandsOutDistinctIndices) {
    NodePool pool(8);
    std::set<NodeIndex> seen;
    for (int i = 0; i < 8; ++i) EXPECT_TRUE(seen.insert(pool.acquire()).second);
    EXPECT_EQ(pool.live(), 8u);
    EXPECT_EQ(pool.available(), 0u);
}

// @spec BOOK-MEM-005
TEST(NodePool, ReleasedNodesAreReusedBeforeFreshOnes) {
    NodePool pool(8);
    const NodeIndex a = pool.acquire();
    const NodeIndex b = pool.acquire();
    pool.release(a);
    EXPECT_EQ(pool.acquire(), a);  // LIFO reuse keeps hot nodes in cache
    pool.release(b);
    pool.release(a);
    EXPECT_EQ(pool.acquire(), a);
    EXPECT_EQ(pool.acquire(), b);
    EXPECT_EQ(pool.capacity(), 8u);  // never shrinks
}

// @spec BOOK-MEM-003
TEST(NodePool, ReserveForGrowsByDoublingAndKeepsContents) {
    NodePool pool(2);
    const NodeIndex a = pool.acquire();
    pool[a].id = 42;
    pool[a].qty = 7;
    ASSERT_TRUE(pool.reserve_for(5));
    EXPECT_GE(pool.available(), 5u);
    EXPECT_EQ(pool.capacity() & (pool.capacity() - 1), 0u) << "doubling from 2 stays a power of two";
    EXPECT_EQ(pool[a].id, 42u);  // indices stay valid across growth
    EXPECT_EQ(pool[a].qty, 7u);
}

TEST(NodePool, ReserveForIsANoOpWhenCapacitySuffices) {
    NodePool pool(16);
    ASSERT_TRUE(pool.reserve_for(16));
    EXPECT_EQ(pool.capacity(), 16u);
}

// @spec BOOK-MEM-004
TEST(NodePool, ReserveForFailsWithoutSideEffectsAtTheNodeLimit) {
    NodePool pool(2, /*max_nodes=*/5);
    ASSERT_TRUE(pool.reserve_for(5));
    for (int i = 0; i < 5; ++i) (void)pool.acquire();
    const std::size_t capacity = pool.capacity();
    EXPECT_FALSE(pool.reserve_for(1));
    EXPECT_EQ(pool.capacity(), capacity);
    EXPECT_EQ(pool.live(), 5u);
}

TEST(NodePool, GrowthIsClampedToTheNodeLimit) {
    NodePool pool(4, /*max_nodes=*/6);
    ASSERT_TRUE(pool.reserve_for(6));  // doubling to 8 would exceed the limit
    EXPECT_EQ(pool.capacity(), 6u);
}

TEST(NodePool, ZeroInitialCapacityStillWorks) {
    NodePool pool(0);
    ASSERT_TRUE(pool.reserve_for(1));
    (void)pool.acquire();
    EXPECT_EQ(pool.live(), 1u);
}

}  // namespace
}  // namespace matcher
