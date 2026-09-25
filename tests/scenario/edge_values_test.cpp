// Boundary prices and quantities through the whole engine.
#include <gtest/gtest.h>

#include "support/harness.h"

namespace matcher {
namespace {

using test::Harness;
using test::level;
using Lines = std::vector<std::string>;
using Levels = std::vector<OrderBook::LevelSnapshot>;

// @spec PRICE-PARSE-007, MATCH-ADD-002
TEST(EdgeValues, NegativeAndZeroPricesTradeAndOrderCorrectly) {
    Harness h;
    (void)h.run({"0,1,1,1,0.25", "0,2,1,1,-0.5", "0,3,1,1,0"});
    EXPECT_EQ(h.run({"0,9,0,3,0.25"}),
              (Lines{"2,1,-0.5", "4,9,2", "3,2", "2,1,0", "4,9,1", "3,3", "2,1,0.25", "3,9", "3,1"}));
}

TEST(EdgeValues, WtiStyleNegativeSettlement) {
    Harness h;
    (void)h.run({"0,1,0,5,-37.63", "0,2,0,5,-40"});
    EXPECT_EQ(h.run({"0,3,1,7,-40"}), (Lines{"2,5,-37.63", "4,3,2", "3,1", "2,2,-40", "3,3", "4,2,3"}));
}

TEST(EdgeValues, ExtremePrices) {
    Harness h;
    (void)h.run({"0,1,1,1,92233720368.54775807", "0,2,0,1,-92233720368.54775807"});
    EXPECT_EQ(h.levels(Side::Sell), (Levels{level("92233720368.54775807", {{1, 1}})}));
    EXPECT_EQ(h.run({"0,3,0,1,92233720368.54775807"}), (Lines{"2,1,92233720368.54775807", "3,3", "3,1"}));
    EXPECT_EQ(h.run({"0,4,1,1,-92233720368.54775807"}), (Lines{"2,1,-92233720368.54775807", "3,4", "3,2"}));
}

TEST(EdgeValues, MaximumQuantitiesNeverOverflow) {
    Harness h;
    (void)h.run({"0,1,1,18446744073709551615,10", "0,2,1,18446744073709551615,11"});
    EXPECT_EQ(h.run({"0,3,0,18446744073709551615,11"}), (Lines{"2,18446744073709551615,10", "3,3", "3,1"}));
    EXPECT_EQ(h.run({"0,4,0,1,11"}), (Lines{"2,1,11", "3,4", "4,2,18446744073709551614"}));
}

TEST(EdgeValues, MaximumOrderId) {
    Harness h;
    (void)h.run({"0,18446744073709551615,1,1,10"});
    EXPECT_EQ(h.run({"0,1,0,1,10"}), (Lines{"2,1,10", "3,1", "3,18446744073709551615"}));
}

TEST(EdgeValues, SmallestPriceStep) {
    Harness h;
    (void)h.run({"0,1,1,1,0.00000002", "0,2,1,1,0.00000001"});
    EXPECT_EQ(h.run({"0,3,0,1,0.00000001"}), (Lines{"2,1,0.00000001", "3,3", "3,2"}));
}

TEST(EdgeValues, ManyLevelsAndOrdersGrowFromTinyReservations) {
    Harness h(BookConfig{1, 1, kMaxNodes});
    for (int i = 1; i <= 2000; ++i)
        (void)h.send("0," + std::to_string(i) + ",1,1," + std::to_string(1000 + (i * 37) % 997));
    EXPECT_EQ(h.book().order_count(), 2000u);
    EXPECT_EQ(h.book().level_count(Side::Sell), 997u);
    (void)h.send("0,99999,0,2000,999999");
    EXPECT_TRUE(h.book().empty(Side::Sell));
    EXPECT_EQ(h.take_output().size(), 3u * 2000u);
}

}  // namespace
}  // namespace matcher
