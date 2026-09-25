#include <gtest/gtest.h>

#include <map>
#include <vector>

#include "matcher/order_index.h"
#include "support/request_generator.h"

namespace matcher {
namespace {

IndexEntry entry(NodeIndex n, Side s = Side::Buy) { return {n, s}; }

constexpr OrderId kSearchLimit = 1'000'000;  // bounded, so a broken hash fails instead of hanging

// Finds `count` distinct non-zero ids that share one home slot.
std::vector<OrderId> colliding_ids(const OrderIndex& index, std::size_t count) {
    std::map<std::size_t, std::vector<OrderId>> by_slot;
    for (OrderId id = 1; id < kSearchLimit; ++id) {
        auto& ids = by_slot[index.home_slot(id)];
        ids.push_back(id);
        if (ids.size() == count) return ids;
    }
    ADD_FAILURE() << "no " << count << " colliding ids found";
    return {};
}

// @spec BOOK-OP-009
TEST(OrderIndex, InsertFindErase) {
    OrderIndex index(16);
    ASSERT_TRUE(index.reserve_for(1));
    index.insert(123, entry(7, Side::Sell));
    ASSERT_TRUE(index.find(123).has_value());
    EXPECT_EQ(*index.find(123), entry(7, Side::Sell));
    EXPECT_FALSE(index.find(124).has_value());
    EXPECT_EQ(index.size(), 1u);
    EXPECT_TRUE(index.erase(123));
    EXPECT_FALSE(index.find(123).has_value());
    EXPECT_FALSE(index.erase(123));
    EXPECT_EQ(index.size(), 0u);
}

TEST(OrderIndex, CapacityIsAPowerOfTwoWithLoadFactorAtMostHalf) {
    OrderIndex index(100);
    EXPECT_EQ(index.capacity() & (index.capacity() - 1), 0u);
    EXPECT_GE(index.capacity(), 200u);
}

TEST(OrderIndex, HandlesExtremeIds) {
    OrderIndex index(4);
    const OrderId ids[] = {1, 2, std::numeric_limits<OrderId>::max(), std::numeric_limits<OrderId>::max() - 1,
                           std::uint64_t{1} << 63};
    for (OrderId id : ids) {
        ASSERT_TRUE(index.reserve_for(1));
        index.insert(id, entry(static_cast<NodeIndex>(id % 1000)));
    }
    for (OrderId id : ids) EXPECT_TRUE(index.find(id).has_value()) << id;
}

// @spec BOOK-OP-009
TEST(OrderIndex, BackwardShiftDeletionKeepsEveryClusterMemberReachable) {
    OrderIndex index(64);
    const std::vector<OrderId> ids = colliding_ids(index, 6);
    ASSERT_EQ(ids.size(), 6u);
    ASSERT_TRUE(index.reserve_for(ids.size()));
    for (std::size_t i = 0; i < ids.size(); ++i) index.insert(ids[i], entry(static_cast<NodeIndex>(i)));

    // Delete from the front, middle and back of the probe cluster.
    for (std::size_t victim : {std::size_t{0}, std::size_t{3}, std::size_t{5}}) {
        ASSERT_TRUE(index.erase(ids[victim]));
        EXPECT_FALSE(index.find(ids[victim]).has_value());
    }
    for (std::size_t i : {1u, 2u, 4u}) {
        ASSERT_TRUE(index.find(ids[i]).has_value()) << "lost id after backward shift";
        EXPECT_EQ(index.find(ids[i])->node, i);
    }
}

// @spec BOOK-OP-009
TEST(OrderIndex, ClustersThatWrapAroundTheTableEnd) {
    OrderIndex index(8);
    // Ids whose home slot is the last slot, so their cluster wraps to slot 0.
    std::vector<OrderId> ids;
    for (OrderId id = 1; ids.size() < 4 && id < kSearchLimit; ++id)
        if (index.home_slot(id) == index.capacity() - 1) ids.push_back(id);
    ASSERT_EQ(ids.size(), 4u);
    ASSERT_TRUE(index.reserve_for(ids.size()));
    for (std::size_t i = 0; i < ids.size(); ++i) index.insert(ids[i], entry(static_cast<NodeIndex>(i)));
    ASSERT_TRUE(index.erase(ids[0]));
    for (std::size_t i = 1; i < ids.size(); ++i) EXPECT_TRUE(index.find(ids[i]).has_value());
}

// @spec BOOK-MEM-003
TEST(OrderIndex, GrowsAndRehashesUnderLoad) {
    OrderIndex index(2);
    const std::size_t initial = index.capacity();
    for (OrderId id = 1; id <= 10'000; ++id) {
        ASSERT_TRUE(index.reserve_for(1));
        index.insert(id, entry(static_cast<NodeIndex>(id)));
    }
    EXPECT_GT(index.capacity(), initial);
    EXPECT_LE(index.size() * 2, index.capacity());
    for (OrderId id = 1; id <= 10'000; ++id) ASSERT_EQ(index.find(id)->node, id);
}

TEST(OrderIndex, ForEachVisitsExactlyTheLiveEntries) {
    OrderIndex index(16);
    ASSERT_TRUE(index.reserve_for(3));
    index.insert(5, entry(1));
    index.insert(6, entry(2));
    index.insert(7, entry(3));
    (void)index.erase(6);
    std::map<OrderId, NodeIndex> seen;
    index.for_each([&](OrderId id, IndexEntry e) { seen[id] = e.node; });
    EXPECT_EQ(seen, (std::map<OrderId, NodeIndex>{{5, 1}, {7, 3}}));
}

// Randomized churn against std::map: the definitive check for probing and
// backward-shift deletion.
// @spec BOOK-OP-009
TEST(OrderIndex, MatchesStdMapUnderRandomChurn) {
    OrderIndex index(8);
    std::map<OrderId, NodeIndex> model;
    test::Rng rng(7);
    for (int step = 0; step < 300'000; ++step) {
        // Small key space forces heavy collisions and reuse.
        const OrderId id = 1 + rng.below(2'000);
        if (rng.percent(55) && !model.contains(id)) {
            ASSERT_TRUE(index.reserve_for(1));
            const auto node = static_cast<NodeIndex>(step);
            index.insert(id, entry(node));
            model[id] = node;
        } else {
            EXPECT_EQ(index.erase(id), model.erase(id) == 1);
        }
        if (step % 1000 == 0) {
            ASSERT_EQ(index.size(), model.size());
            for (const auto& [k, v] : model) ASSERT_EQ(index.find(k)->node, v);
        }
    }
}

}  // namespace
}  // namespace matcher
