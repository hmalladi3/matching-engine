// The brief's worked example, step by step.
#include <gtest/gtest.h>

#include "support/harness.h"

namespace matcher {
namespace {

using test::Harness;
using test::level;
using Levels = std::vector<OrderBook::LevelSnapshot>;

// Builds the example's standing book (orders 1000000-1000007 and the cancel),
// none of which trade. This harness drives the engine directly, so the
// example's BADMESSAGE line is left out here; the golden end-to-end test
// (data/golden/brief_example.*) runs the whole input, bad line included, through
// the real binary and checks its stderr.
void build_standing_book(Harness& h) {
    EXPECT_EQ(
        h.run({"0,1000000,1,1,1075", "0,1000001,0,9,1000", "0,1000002,0,30,975", "0,1000003,1,10,1050",
               "0,1000004,0,10,950", "0,1000005,1,2,1025", "0,1000006,0,1,1000", "1,1000004", "0,1000007,1,5,1025"}),
        std::vector<std::string>{});
}

// @spec MATCH-ADD-008, MATCH-ADD-005, MATCH-EVT-004
TEST(BriefExample, StandingBookDoesNotTrade) {
    Harness h;
    build_standing_book(h);
    EXPECT_EQ(h.levels(Side::Sell), (Levels{level("1025", {{1000005, 2}, {1000007, 5}}), level("1050", {{1000003, 10}}),
                                            level("1075", {{1000000, 1}})}));
    EXPECT_EQ(h.levels(Side::Buy),
              (Levels{level("1000", {{1000001, 9}, {1000006, 1}}), level("975", {{1000002, 30}})}));
}

// @spec MATCH-ADD-001, MATCH-ADD-002, MATCH-ADD-003, MATCH-EVT-001, MATCH-EVT-002, MATCH-EVT-003
TEST(BriefExample, AggressiveBuyProducesTheBriefsExactOutput) {
    Harness h;
    build_standing_book(h);
    EXPECT_EQ(h.run({"0,1000008,0,3,1050"}), (std::vector<std::string>{"2,2,1025", "4,1000008,1", "3,1000005",
                                                                       "2,1,1025", "3,1000008", "4,1000007,4"}));
}

// @spec MATCH-ADD-006, MATCH-ADD-007
TEST(BriefExample, BookAfterTheMatch) {
    Harness h;
    build_standing_book(h);
    (void)h.run({"0,1000008,0,3,1050"});
    EXPECT_EQ(h.levels(Side::Sell),
              (Levels{level("1025", {{1000007, 4}}), level("1050", {{1000003, 10}}), level("1075", {{1000000, 1}})}));
    EXPECT_EQ(h.levels(Side::Buy),
              (Levels{level("1000", {{1000001, 9}, {1000006, 1}}), level("975", {{1000002, 30}})}));
    EXPECT_FALSE(h.book().contains(1000008)) << "a fully filled aggressive order never rests";
}

// "If a new sell order arrives at a price of 1025, it will be added behind the S4 in the queue."
// @spec MATCH-ADD-005, MATCH-ADD-007
TEST(BriefExample, NewSellAt1025QueuesBehindS4) {
    Harness h;
    build_standing_book(h);
    (void)h.run({"0,1000008,0,3,1050"});
    EXPECT_EQ(h.run({"0,1000009,1,6,1025"}), std::vector<std::string>{});
    EXPECT_EQ(h.levels(Side::Sell).front(), level("1025", {{1000007, 4}, {1000009, 6}}));
    // And a buy for 5 fills S4 first, then 1 from the newcomer.
    EXPECT_EQ(h.run({"0,1000010,0,5,1025"}), (std::vector<std::string>{"2,4,1025", "4,1000010,1", "3,1000007",
                                                                       "2,1,1025", "3,1000010", "4,1000009,5"}));
}

}  // namespace
}  // namespace matcher
