// The README's worked example, step by step.
#include <gtest/gtest.h>

#include "support/harness.h"

namespace matcher {
namespace {

using test::Harness;
using test::level;
using Levels = std::vector<OrderBook::LevelSnapshot>;

// Builds the example's standing book (orders 101-105 and the cancel of 103),
// none of which trade. This harness drives the engine directly, so the
// example's HELLO line is left out here; the golden end-to-end test
// (data/golden/worked_example.*) runs the whole input, bad line included,
// through the real binary and checks its stderr.
void build_standing_book(Harness& h) {
    EXPECT_EQ(h.run({"0,101,1,5,100.25", "0,102,1,3,100.50", "0,103,0,4,99.75", "0,104,1,2,100.25", "0,105,0,6,99.50",
                     "1,103"}),
              std::vector<std::string>{});
}

TEST(WorkedExample, StandingBookDoesNotTrade) {
    Harness h;
    build_standing_book(h);
    EXPECT_EQ(h.levels(Side::Sell), (Levels{level("100.25", {{101, 5}, {104, 2}}), level("100.50", {{102, 3}})}));
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("99.50", {{105, 6}})}));
}

// Buy 6 at 100.25: fills all 5 of 101 (oldest at the best price), then 1 of 104.
TEST(WorkedExample, AggressiveBuyFillsInTimePriority) {
    Harness h;
    build_standing_book(h);
    EXPECT_EQ(h.run({"0,106,0,6,100.25"}),
              (std::vector<std::string>{"2,5,100.25", "4,106,1", "3,101", "2,1,100.25", "3,106", "4,104,1"}));
}

TEST(WorkedExample, BookAfterTheMatch) {
    Harness h;
    build_standing_book(h);
    (void)h.run({"0,106,0,6,100.25"});
    EXPECT_EQ(h.levels(Side::Sell), (Levels{level("100.25", {{104, 1}}), level("100.50", {{102, 3}})}));
    EXPECT_EQ(h.levels(Side::Buy), (Levels{level("99.50", {{105, 6}})}));
    EXPECT_FALSE(h.book().contains(106)) << "a fully filled aggressive order never rests";
}

// A partially filled resting order keeps its place: a new sell at 100.25
// queues behind 104, and the next buy fills 104 first.
TEST(WorkedExample, NewSellQueuesBehindThePartiallyFilledOrder) {
    Harness h;
    build_standing_book(h);
    (void)h.run({"0,106,0,6,100.25"});
    EXPECT_EQ(h.run({"0,107,1,4,100.25"}), std::vector<std::string>{});
    EXPECT_EQ(h.levels(Side::Sell).front(), level("100.25", {{104, 1}, {107, 4}}));
    EXPECT_EQ(h.run({"0,108,0,3,100.25"}),
              (std::vector<std::string>{"2,1,100.25", "4,108,2", "3,104", "2,2,100.25", "3,108", "4,107,2"}));
}

}  // namespace
}  // namespace matcher
