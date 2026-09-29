#include <gtest/gtest.h>

#include <vector>

#include "matcher/level_store.h"

namespace matcher {
namespace {

Price p(std::int64_t units) { return Price::from_units(units); }

template <Side S>
std::vector<std::int64_t> prices(const LevelStore<S>& store) {
    std::vector<std::int64_t> out;
    for (const Level& l : store.levels()) out.push_back(l.price.raw() / Price::kScale);
    return out;
}

template <Side S>
void insert(LevelStore<S>& store, std::int64_t units) {
    ASSERT_TRUE(store.reserve_for_one());
    store.insert(p(units), static_cast<NodeIndex>(units));
}

TEST(LevelStore, BetterMeansHigherForBidsAndLowerForAsks) {
    EXPECT_TRUE(LevelStore<Side::Buy>::is_better(p(101), p(100)));
    EXPECT_FALSE(LevelStore<Side::Buy>::is_better(p(100), p(100)));
    EXPECT_TRUE(LevelStore<Side::Sell>::is_better(p(99), p(100)));
    EXPECT_FALSE(LevelStore<Side::Sell>::is_better(p(100), p(100)));
}

TEST(LevelStore, BidsKeepBestAtTheBackWhateverTheInsertionOrder) {
    LevelStore<Side::Buy> bids(2);
    for (std::int64_t u : {100, 102, 98, 101, 99, 103, 97}) insert(bids, u);
    EXPECT_EQ(prices(bids), (std::vector<std::int64_t>{97, 98, 99, 100, 101, 102, 103}));
    EXPECT_EQ(bids.best().price, p(103));
}

TEST(LevelStore, AsksKeepBestAtTheBackWhateverTheInsertionOrder) {
    LevelStore<Side::Sell> asks(2);
    for (std::int64_t u : {100, 102, 98, 101, 99, 103, 97}) insert(asks, u);
    EXPECT_EQ(prices(asks), (std::vector<std::int64_t>{103, 102, 101, 100, 99, 98, 97}));
    EXPECT_EQ(asks.best().price, p(97));
}

TEST(LevelStore, FindReturnsTheLevelOrNull) {
    LevelStore<Side::Buy> bids(4);
    for (std::int64_t u : {100, 105, 95}) insert(bids, u);
    ASSERT_NE(bids.find(p(105)), nullptr);  // fast path: the best level
    EXPECT_EQ(bids.find(p(105))->sentinel, 105u);
    ASSERT_NE(bids.find(p(95)), nullptr);  // binary-search path
    EXPECT_EQ(bids.find(p(95))->sentinel, 95u);
    EXPECT_EQ(bids.find(p(110)), nullptr);  // better than best
    EXPECT_EQ(bids.find(p(99)), nullptr);   // gap
    EXPECT_EQ(bids.find(p(90)), nullptr);   // worse than worst
    LevelStore<Side::Sell> empty(1);
    EXPECT_EQ(empty.find(p(1)), nullptr);
}

TEST(LevelStore, EraseAnywhereKeepsOrder) {
    LevelStore<Side::Sell> asks(8);
    for (std::int64_t u : {10, 11, 12, 13, 14}) insert(asks, u);
    asks.erase(p(12));  // middle
    asks.erase(p(10));  // best
    asks.erase(p(14));  // worst
    EXPECT_EQ(prices(asks), (std::vector<std::int64_t>{13, 11}));
    asks.pop_best();
    EXPECT_EQ(prices(asks), (std::vector<std::int64_t>{13}));
}

TEST(LevelStore, ReserveForOneGrowsSoInsertNeverReallocates) {
    LevelStore<Side::Buy> bids(1);
    for (std::int64_t u = 0; u < 100; ++u) {
        ASSERT_TRUE(bids.reserve_for_one());
        const Level* before = bids.levels().data();
        const std::size_t size = bids.size();
        bids.insert(p(u), 0);
        if (size > 0) {
            EXPECT_EQ(bids.levels().data(), before) << "insert reallocated";
        }
    }
}

TEST(LevelStore, NegativeAndZeroPricesOrderCorrectly) {
    LevelStore<Side::Buy> bids(4);
    for (std::int64_t u : {0, -5, 3, -1}) insert(bids, u);
    EXPECT_EQ(prices(bids), (std::vector<std::int64_t>{-5, -1, 0, 3}));
}

}  // namespace
}  // namespace matcher
