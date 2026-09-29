// Rejections leave no trace; ids can be reused once dead.
#include <gtest/gtest.h>

#include "support/harness.h"

namespace matcher {
namespace {

using test::Harness;
using test::level;
using Lines = std::vector<std::string>;
using Levels = std::vector<OrderBook::LevelSnapshot>;

TEST(Rejection, DuplicateLiveIdOnTheSameSide) {
    Harness h;
    (void)h.run({"0,1,0,5,10"});
    EXPECT_EQ(h.send("0,1,0,7,11"), Reject::DuplicateOrderId);
    EXPECT_EQ(h.take_output(), Lines{});
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("10", {{1, 5}})}));
}

TEST(Rejection, DuplicateLiveIdThatWouldHaveCrossed) {
    Harness h;
    (void)h.run({"0,1,0,5,10", "0,2,0,1,9"});
    // Same id as a live buy, on the sell side, priced to cross it: rejected
    // before any matching, so order 1 is untouched.
    EXPECT_EQ(h.send("0,1,1,5,9"), Reject::DuplicateOrderId);
    EXPECT_EQ(h.take_output(), Lines{});
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("10", {{1, 5}}), level("9", {{2, 1}})}));
    EXPECT_TRUE(h.book().empty(Side::Sell));
}

TEST(Rejection, CancelOfUnknownFilledOrCancelledIds) {
    Harness h;
    EXPECT_EQ(h.send("1,42"), Reject::UnknownOrderId);  // never seen
    EXPECT_EQ(h.run({"0,1,1,1,10", "0,2,0,1,10"}), (Lines{"2,1,10", "3,2", "3,1"}));
    EXPECT_EQ(h.send("1,1"), Reject::UnknownOrderId);  // filled (resting side)
    EXPECT_EQ(h.send("1,2"), Reject::UnknownOrderId);  // filled (aggressive side, never rested)
    (void)h.run({"0,3,0,1,5"});
    EXPECT_EQ(h.send("1,3"), Reject::None);
    EXPECT_EQ(h.send("1,3"), Reject::UnknownOrderId);  // already cancelled
    EXPECT_EQ(h.take_output(), Lines{}) << "rejections and cancels emit nothing";
}

TEST(Rejection, IdReuseAfterFillIsANewOrderAtTheBackOfTheQueue) {
    Harness h;
    (void)h.run({"0,1,1,1,10", "0,2,1,1,10", "0,7,0,1,10"});  // fills 1
    EXPECT_EQ(h.send("0,1,1,1,10"), Reject::None);            // 1 reused, now behind 2
    (void)h.take_output();
    EXPECT_EQ(h.levels(Side::Sell), (Levels{level("10", {{2, 1}, {1, 1}})}));
    EXPECT_EQ(h.run({"0,8,0,1,10"}), (Lines{"2,1,10", "3,8", "3,2"}));
}

TEST(Rejection, IdReuseAfterCancelIsANewOrderAtTheBackOfTheQueue) {
    Harness h;
    (void)h.run({"0,1,0,1,10", "0,2,0,1,10", "1,1"});
    EXPECT_EQ(h.send("0,1,0,3,10"), Reject::None);
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("10", {{2, 1}, {1, 3}})}));
}

TEST(Rejection, AggressorIdCanBeReusedAfterItFilledWithoutResting) {
    Harness h;
    (void)h.run({"0,1,1,1,10", "0,2,0,1,10"});
    EXPECT_EQ(h.send("0,2,0,1,9"), Reject::None);
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("9", {{2, 1}})}));
}

}  // namespace
}  // namespace matcher
