// Price-time priority, trade pricing, sweeps and output sequencing.
#include <gtest/gtest.h>

#include "support/harness.h"

namespace matcher {
namespace {

using test::Harness;
using test::level;
using Lines = std::vector<std::string>;
using Levels = std::vector<OrderBook::LevelSnapshot>;

// @spec MATCH-ADD-002
TEST(Priority, BetterPriceFirstForAnAggressiveBuy) {
    Harness h;
    (void)h.run({"0,1,1,1,12", "0,2,1,1,10", "0,3,1,1,11"});
    EXPECT_EQ(h.run({"0,9,0,3,12"}),
              (Lines{"2,1,10", "4,9,2", "3,2", "2,1,11", "4,9,1", "3,3", "2,1,12", "3,9", "3,1"}));
}

// @spec MATCH-ADD-002
TEST(Priority, BetterPriceFirstForAnAggressiveSell) {
    Harness h;
    (void)h.run({"0,1,0,1,10", "0,2,0,1,12", "0,3,0,1,11"});
    EXPECT_EQ(h.run({"0,9,1,3,10"}),
              (Lines{"2,1,12", "4,9,2", "3,2", "2,1,11", "4,9,1", "3,3", "2,1,10", "3,9", "3,1"}));
}

// @spec MATCH-ADD-002
TEST(Priority, OldestFirstWithinALevel) {
    Harness h;
    (void)h.run({"0,1,0,1,10", "0,2,0,1,10", "0,3,0,1,10"});
    EXPECT_EQ(h.run({"0,9,1,2,10"}), (Lines{"2,1,10", "4,9,1", "3,1", "2,1,10", "3,9", "3,2"}));
}

// @spec MATCH-ADD-007
TEST(Priority, PartialFillKeepsQueuePosition) {
    Harness h;
    (void)h.run({"0,1,1,5,10", "0,2,1,5,10"});
    EXPECT_EQ(h.run({"0,8,0,2,10"}), (Lines{"2,2,10", "3,8", "4,1,3"}));
    // Order 1 is still first despite having been modified.
    EXPECT_EQ(h.run({"0,9,0,4,10"}), (Lines{"2,3,10", "4,9,1", "3,1", "2,1,10", "3,9", "4,2,4"}));
}

// @spec MATCH-ADD-003
TEST(Priority, TradesAtTheRestingPriceGivingPriceImprovement) {
    Harness h;
    (void)h.run({"0,1,1,1,1025"});
    EXPECT_EQ(h.run({"0,2,0,1,1050"}), (Lines{"2,1,1025", "3,2", "3,1"}));
    (void)h.run({"0,3,0,1,1000"});
    EXPECT_EQ(h.run({"0,4,1,1,900"}), (Lines{"2,1,1000", "3,4", "3,3"}));
}

// @spec MATCH-ADD-001
TEST(Priority, EqualPricesCross) {
    Harness h;
    (void)h.run({"0,1,1,1,10.5"});
    EXPECT_EQ(h.run({"0,2,0,1,10.5"}), (Lines{"2,1,10.5", "3,2", "3,1"}));
}

// @spec MATCH-ADD-008
TEST(Priority, OneTickShortDoesNotCross) {
    Harness h;
    (void)h.run({"0,1,1,1,10.00000001", "0,2,0,1,9.99999999"});
    EXPECT_EQ(h.run({"0,3,0,1,10"}), Lines{});
    EXPECT_EQ(h.run({"0,4,1,1,10.00000000"}), (Lines{"2,1,10", "3,4", "3,3"}));
}

// @spec MATCH-ADD-004, MATCH-ADD-005
TEST(Priority, SweepStopsAtTheLimitAndRestsTheRemainder) {
    Harness h;
    (void)h.run({"0,1,1,1,10", "0,2,1,2,11", "0,3,1,3,12", "0,4,1,4,13"});
    EXPECT_EQ(h.run({"0,9,0,10,12"}),
              (Lines{"2,1,10", "4,9,9", "3,1", "2,2,11", "4,9,7", "3,2", "2,3,12", "4,9,4", "3,3"}));
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("12", {{9, 4}})}));
    EXPECT_EQ(h.levels(Side::Sell), (Levels{level("13", {{4, 4}})}));
}

// @spec MATCH-ADD-004, MATCH-ADD-006
TEST(Priority, SweepThatExactlyExhaustsTheOppositeSide) {
    Harness h;
    (void)h.run({"0,1,0,2,10", "0,2,0,3,9"});
    EXPECT_EQ(h.run({"0,9,1,5,1"}), (Lines{"2,2,10", "4,9,3", "3,1", "2,3,9", "3,9", "3,2"}));
    EXPECT_TRUE(h.book().empty(Side::Buy));
    EXPECT_TRUE(h.book().empty(Side::Sell));
}

// @spec MATCH-ADD-005
TEST(Priority, RemainderRestsWhenTheOppositeSideRunsOut) {
    Harness h;
    (void)h.run({"0,1,1,2,10"});
    EXPECT_EQ(h.run({"0,9,0,5,20"}), (Lines{"2,2,10", "4,9,3", "3,1"}));
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("20", {{9, 3}})}));
}

// An aggressor's remainder can never join a same-side level that already has
// orders (those orders would have matched the same opposite orders first), so
// "behind existing orders" is observable only for non-crossing adds.
// @spec MATCH-ADD-005
TEST(Priority, NonCrossingAddQueuesBehindExistingOrdersAtItsPrice) {
    Harness h;
    (void)h.run({"0,1,0,1,20", "0,2,0,4,20", "0,3,1,1,21"});
    EXPECT_EQ(h.run({"0,4,0,2,20"}), Lines{});
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("20", {{1, 1}, {2, 4}, {4, 2}})}));
}

// @spec MATCH-EVT-003
TEST(Priority, EveryTradeHasAtLeastOneFullAndAtMostOnePartialFill) {
    Harness h;
    (void)h.run({"0,1,1,3,10"});
    EXPECT_EQ(h.run({"0,2,0,3,10"}), (Lines{"2,3,10", "3,2", "3,1"}));  // both full
    (void)h.run({"0,3,1,5,10"});
    EXPECT_EQ(h.run({"0,4,0,2,10"}), (Lines{"2,2,10", "3,4", "4,3,3"}));  // aggressor full
    EXPECT_EQ(h.run({"0,5,0,7,10"}), (Lines{"2,3,10", "4,5,4", "3,3"}));  // resting full
}

// @spec MATCH-CXL-001
TEST(Priority, CancelledOrdersNeverTrade) {
    Harness h;
    (void)h.run({"0,1,1,1,10", "0,2,1,1,10", "1,1"});
    EXPECT_EQ(h.run({"0,3,0,2,10"}), (Lines{"2,1,10", "4,3,1", "3,2"}));
}

// @spec MATCH-CXL-001
TEST(Priority, CancellingTheLastOrderAtBestMovesTheBestPrice) {
    Harness h;
    (void)h.run({"0,1,1,1,10", "0,2,1,1,11", "1,1"});
    EXPECT_EQ(h.book().best_price(Side::Sell), test::px("11"));
    EXPECT_EQ(h.run({"0,3,0,1,10"}), Lines{});  // no longer crosses
}

// @spec MATCH-CXL-001
TEST(Priority, CancelDeepInTheBookAndMidQueue) {
    Harness h;
    (void)h.run({"0,1,0,1,10", "0,2,0,1,10", "0,3,0,1,10", "0,4,0,1,5", "0,5,0,1,1"});
    (void)h.run({"1,2", "1,4"});
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("10", {{1, 1}, {3, 1}}), level("1", {{5, 1}})}));
}

}  // namespace
}  // namespace matcher
