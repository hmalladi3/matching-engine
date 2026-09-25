#include <gtest/gtest.h>

#include "matcher/order_book.h"
#include "support/harness.h"

namespace matcher {
namespace {

using test::level;
using test::px;
using test::tiny_config;
using Levels = std::vector<OrderBook::LevelSnapshot>;

void rest(OrderBook& book, Side side, OrderId id, Quantity qty, std::string_view price) {
    ASSERT_TRUE(book.reserve_for_add(side));
    book.rest(side, id, qty, px(price));
    book.check_invariants();
}

TEST(OrderBook, StartsEmpty) {
    OrderBook book(tiny_config());
    EXPECT_TRUE(book.empty(Side::Buy));
    EXPECT_TRUE(book.empty(Side::Sell));
    EXPECT_EQ(book.order_count(), 0u);
    book.check_invariants();
}

// @spec BOOK-OP-001, BOOK-OP-008
TEST(OrderBook, RestAppendsToTheBackOfItsLevel) {
    OrderBook book(tiny_config());
    rest(book, Side::Buy, 1, 9, "1000");
    rest(book, Side::Buy, 2, 1, "1000");
    rest(book, Side::Buy, 3, 30, "975");
    rest(book, Side::Buy, 4, 5, "1000");
    EXPECT_EQ(book.best_price(Side::Buy), px("1000"));
    EXPECT_EQ(book.snapshot(Side::Buy),
              (Levels{level("1000", {{1, 9}, {2, 1}, {4, 5}}), level("975", {{3, 30}})}));
    EXPECT_TRUE(book.contains(4));
    EXPECT_EQ(book.order_count(), 4u);
    EXPECT_EQ(book.level_count(Side::Buy), 2u);
}

// @spec BOOK-OP-006, BOOK-OP-007
TEST(OrderBook, RestCreatesLevelsAtBestMiddleAndWorst) {
    OrderBook book(tiny_config());
    rest(book, Side::Sell, 1, 1, "1050");
    rest(book, Side::Sell, 2, 1, "1025");  // new best
    rest(book, Side::Sell, 3, 1, "1075");  // new worst
    rest(book, Side::Sell, 4, 1, "1060");  // middle
    EXPECT_EQ(book.best_price(Side::Sell), px("1025"));
    EXPECT_EQ(book.snapshot(Side::Sell),
              (Levels{level("1025", {{2, 1}}), level("1050", {{1, 1}}), level("1060", {{4, 1}}),
                      level("1075", {{3, 1}})}));
}

// @spec BOOK-OP-003
TEST(OrderBook, PartialFillReducesInPlaceAndKeepsPriority) {
    OrderBook book(tiny_config());
    rest(book, Side::Sell, 5, 5, "1025");
    rest(book, Side::Sell, 6, 3, "1025");
    const OrderBook::Fill f = book.fill_best(Side::Sell, 1);
    book.check_invariants();
    EXPECT_EQ(f.resting_id, 5u);
    EXPECT_EQ(f.price, px("1025"));
    EXPECT_EQ(f.qty, 1u);
    EXPECT_EQ(f.resting_remaining, 4u);
    EXPECT_EQ(book.snapshot(Side::Sell), (Levels{level("1025", {{5, 4}, {6, 3}})}));
}

// @spec BOOK-OP-002
TEST(OrderBook, FullFillRemovesTheOrderAndItsEmptyLevel) {
    OrderBook book(tiny_config());
    rest(book, Side::Sell, 5, 2, "1025");
    rest(book, Side::Sell, 6, 10, "1050");
    const OrderBook::Fill f = book.fill_best(Side::Sell, 7);  // more than resting
    book.check_invariants();
    EXPECT_EQ(f.resting_id, 5u);
    EXPECT_EQ(f.qty, 2u);
    EXPECT_EQ(f.resting_remaining, 0u);
    EXPECT_FALSE(book.contains(5));
    EXPECT_EQ(book.best_price(Side::Sell), px("1050"));
    EXPECT_EQ(book.level_count(Side::Sell), 1u);
}

// @spec BOOK-OP-002
TEST(OrderBook, FullFillOfTheOnlyOrderEmptiesTheSide) {
    OrderBook book(tiny_config());
    rest(book, Side::Buy, 1, 3, "10");
    (void)book.fill_best(Side::Buy, 3);
    book.check_invariants();
    EXPECT_TRUE(book.empty(Side::Buy));
    EXPECT_EQ(book.order_count(), 0u);
}

// @spec BOOK-OP-004
TEST(OrderBook, CancelHeadMiddleAndTailOfAFifo) {
    OrderBook book(tiny_config());
    for (OrderId id = 1; id <= 5; ++id) rest(book, Side::Buy, id, id, "100");
    ASSERT_TRUE(book.cancel(3));  // middle
    book.check_invariants();
    ASSERT_TRUE(book.cancel(1));  // head
    book.check_invariants();
    ASSERT_TRUE(book.cancel(5));  // tail
    book.check_invariants();
    EXPECT_EQ(book.snapshot(Side::Buy), (Levels{level("100", {{2, 2}, {4, 4}})}));
    // FIFO order still correct for subsequent fills.
    EXPECT_EQ(book.fill_best(Side::Buy, 1).resting_id, 2u);
}

// @spec BOOK-OP-005
TEST(OrderBook, CancelOfTheLastOrderRemovesItsLevelAnywhere) {
    OrderBook book(tiny_config());
    rest(book, Side::Sell, 1, 1, "10");
    rest(book, Side::Sell, 2, 1, "11");
    rest(book, Side::Sell, 3, 1, "12");
    ASSERT_TRUE(book.cancel(2));  // middle level
    book.check_invariants();
    ASSERT_TRUE(book.cancel(1));  // best level: best price moves
    book.check_invariants();
    EXPECT_EQ(book.best_price(Side::Sell), px("12"));
    ASSERT_TRUE(book.cancel(3));
    EXPECT_TRUE(book.empty(Side::Sell));
    book.check_invariants();
}

TEST(OrderBook, CancelOfAnUnknownIdReturnsFalse) {
    OrderBook book(tiny_config());
    rest(book, Side::Buy, 1, 1, "10");
    EXPECT_FALSE(book.cancel(2));
    ASSERT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.cancel(1));
}

// @spec BOOK-MEM-005
TEST(OrderBook, FreedNodesAreReusedWithoutGrowth) {
    OrderBook book(BookConfig{8, 4, kMaxNodes});
    for (int round = 0; round < 1000; ++round) {
        rest(book, Side::Buy, 1, 1, "100");
        rest(book, Side::Sell, 2, 1, "101");
        ASSERT_TRUE(book.cancel(1));
        ASSERT_TRUE(book.cancel(2));
    }
    EXPECT_EQ(book.order_count(), 0u);
}

// @spec BOOK-MEM-003
TEST(OrderBook, GrowsPastTinyReservationsTransparently) {
    OrderBook book(BookConfig{1, 1, kMaxNodes});
    for (OrderId id = 1; id <= 500; ++id)
        rest(book, id % 2 ? Side::Buy : Side::Sell, id, 1,
             std::to_string(id % 2 ? 1000 - static_cast<int>(id) : 2000 + static_cast<int>(id)));
    EXPECT_EQ(book.order_count(), 500u);
    EXPECT_EQ(book.level_count(Side::Buy), 250u);
}

// @spec BOOK-MEM-004
TEST(OrderBook, ReserveFailsAtTheNodeLimitWithoutSideEffects) {
    // Each resting order at a new price needs two nodes (order + sentinel).
    OrderBook book(BookConfig{1, 1, /*max_nodes=*/4});
    rest(book, Side::Buy, 1, 1, "10");
    rest(book, Side::Buy, 2, 1, "11");
    EXPECT_FALSE(book.reserve_for_add(Side::Buy));
    book.check_invariants();
    EXPECT_EQ(book.order_count(), 2u);
    ASSERT_TRUE(book.cancel(1));
    EXPECT_TRUE(book.reserve_for_add(Side::Buy));  // capacity freed by the cancel
}

TEST(OrderBook, SidesAreIndependent) {
    OrderBook book(tiny_config());
    rest(book, Side::Buy, 1, 1, "100");
    rest(book, Side::Sell, 2, 1, "101");
    EXPECT_EQ(book.best_price(Side::Buy), px("100"));
    EXPECT_EQ(book.best_price(Side::Sell), px("101"));
    ASSERT_TRUE(book.cancel(2));
    EXPECT_TRUE(book.empty(Side::Sell));
    EXPECT_FALSE(book.empty(Side::Buy));
}

}  // namespace
}  // namespace matcher
