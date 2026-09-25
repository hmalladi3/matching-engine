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

// @spec MATCH-SAFE-001
TEST(MatchingEngine, SinkCallbacksMustBeNoexcept) {
    static_assert(EventSink<CaptureSink>);
    static_assert(!EventSink<ThrowingSink>);
}

// @spec MATCH-SAFE-002
TEST(MatchingEngine, RequestsAreNoexcept) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, test::tiny_config());
    static_assert(noexcept(engine.add(std::declval<const AddOrder&>())));
    static_assert(noexcept(engine.cancel(std::declval<const CancelOrder&>())));
}

// @spec MATCH-REJ-003
TEST(MatchingEngine, RejectsZeroIdAndZeroQuantity) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, test::tiny_config());
    EXPECT_EQ(engine.add(add(0, Side::Buy, 1, "10")), Reject::InvalidOrderId);
    EXPECT_EQ(engine.add(add(1, Side::Buy, 0, "10")), Reject::InvalidQuantity);
    EXPECT_EQ(engine.book().order_count(), 0u);
    EXPECT_TRUE(sink.events().empty());
}

// @spec MATCH-REJ-006
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

// @spec MATCH-REJ-004
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

// @spec MATCH-EVT-004
TEST(MatchingEngine, CancelEmitsNoEvents) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, test::tiny_config());
    ASSERT_EQ(engine.add(add(1, Side::Buy, 5, "10")), Reject::None);
    ASSERT_EQ(engine.cancel({1}), Reject::None);
    EXPECT_TRUE(sink.events().empty());
    EXPECT_EQ(engine.cancel({1}), Reject::UnknownOrderId);
}

}  // namespace
}  // namespace matcher
