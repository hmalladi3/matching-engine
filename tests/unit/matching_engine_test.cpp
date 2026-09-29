#include <gtest/gtest.h>

#include <type_traits>

#include "matcher/matching_engine.h"
#include "support/capture_sink.h"
#include "support/harness.h"

namespace matcher {
namespace {

using test::CaptureSink;
using test::px;

AddOrder add(OrderId id, Side side, Quantity qty, std::string_view price) { return {id, side, qty, px(price)}; }

// A sink whose callbacks may throw must not satisfy the concept.
struct ThrowingSink {
    void on_trade(const Trade&) {}
    void on_fully_filled(const OrderFullyFilled&) noexcept {}
    void on_partially_filled(const OrderPartiallyFilled&) noexcept {}
};

TEST(MatchingEngine, SinkCallbacksMustBeNoexcept) {
    static_assert(EventSink<CaptureSink>);
    static_assert(!EventSink<ThrowingSink>);
}

TEST(MatchingEngine, RequestsAreNoexcept) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, test::tiny_config());
    static_assert(noexcept(engine.add(std::declval<const AddOrder&>())));
    static_assert(noexcept(engine.cancel(std::declval<const CancelOrder&>())));
}

TEST(MatchingEngine, RejectsZeroIdAndZeroQuantity) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, test::tiny_config());
    EXPECT_EQ(engine.add(add(0, Side::Buy, 1, "10")), Reject::InvalidOrderId);
    EXPECT_EQ(engine.add(add(1, Side::Buy, 0, "10")), Reject::InvalidQuantity);
    EXPECT_EQ(engine.book().order_count(), 0u);
    EXPECT_TRUE(sink.events().empty());
}

TEST(MatchingEngine, RejectionPrecedence) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, BookConfig{1, 1, /*max_nodes=*/2});
    ASSERT_EQ(engine.add(add(1, Side::Buy, 1, "10")), Reject::None);  // uses both nodes
    // id 0 and qty 0: id wins.
    EXPECT_EQ(engine.add(add(0, Side::Buy, 0, "10")), Reject::InvalidOrderId);
    // duplicate id with qty 0: quantity wins.
    EXPECT_EQ(engine.add(add(1, Side::Buy, 0, "10")), Reject::InvalidQuantity);
    // duplicate id with the book full: duplicate wins.
    EXPECT_EQ(engine.add(add(1, Side::Buy, 1, "9")), Reject::DuplicateOrderId);
    // fresh id with the book full: capacity.
    EXPECT_EQ(engine.add(add(2, Side::Buy, 1, "9")), Reject::CapacityExceeded);
}

TEST(MatchingEngine, CapacityExceededLeavesBookAndOutputUntouched) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, BookConfig{1, 1, /*max_nodes=*/2});
    ASSERT_EQ(engine.add(add(1, Side::Sell, 5, "10")), Reject::None);
    const auto before = engine.book().snapshot(Side::Sell);
    // Would match (and could fully fill order 1) but capacity is checked first.
    EXPECT_EQ(engine.add(add(2, Side::Buy, 9, "10")), Reject::CapacityExceeded);
    EXPECT_EQ(engine.book().snapshot(Side::Sell), before);
    EXPECT_TRUE(sink.events().empty());
    engine.book().check_invariants();
}

TEST(MatchingEngine, CancelEmitsNoEvents) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, test::tiny_config());
    ASSERT_EQ(engine.add(add(1, Side::Buy, 5, "10")), Reject::None);
    ASSERT_EQ(engine.cancel({1}), Reject::None);
    EXPECT_TRUE(sink.events().empty());
    EXPECT_EQ(engine.cancel({1}), Reject::UnknownOrderId);
}

// Value types compare every field; tests depend on these operators.
TEST(ValueTypes, EqualityComparesEveryField) {
    const AddOrder a{1, Side::Buy, 2, px("3")};
    EXPECT_EQ(a, (AddOrder{1, Side::Buy, 2, px("3")}));
    EXPECT_NE(a, (AddOrder{9, Side::Buy, 2, px("3")}));
    EXPECT_NE(a, (AddOrder{1, Side::Sell, 2, px("3")}));
    EXPECT_NE(a, (AddOrder{1, Side::Buy, 9, px("3")}));
    EXPECT_NE(a, (AddOrder{1, Side::Buy, 2, px("9")}));
    EXPECT_NE((CancelOrder{1}), (CancelOrder{2}));
    EXPECT_NE((Trade{1, px("1")}), (Trade{2, px("1")}));
    EXPECT_NE((Trade{1, px("1")}), (Trade{1, px("2")}));
    EXPECT_NE((OrderFullyFilled{1}), (OrderFullyFilled{2}));
    EXPECT_NE((OrderPartiallyFilled{1, 1}), (OrderPartiallyFilled{2, 1}));
    EXPECT_NE((OrderPartiallyFilled{1, 1}), (OrderPartiallyFilled{1, 2}));
    EXPECT_NE((IndexEntry{1, Side::Buy}), (IndexEntry{2, Side::Buy}));
    EXPECT_NE((IndexEntry{1, Side::Buy}), (IndexEntry{1, Side::Sell}));
    using L = OrderBook::LevelSnapshot;
    EXPECT_NE((L{px("1"), {{1, 1}}}), (L{px("2"), {{1, 1}}}));
    EXPECT_NE((L{px("1"), {{1, 1}}}), (L{px("1"), {{1, 2}}}));
}

}  // namespace
}  // namespace matcher
