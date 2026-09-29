#pragma once

#include "matcher/event_sink.h"
#include "matcher/order_book.h"
#include "matcher/types.h"

namespace matcher {

// Applies the matching rules: price-time priority, trades at the resting
// order's price, remainders rest. Emits typed events to `Sink`; never touches
// I/O. A request is applied completely or rejected with no effect.
template <EventSink Sink>
class MatchingEngine {
public:
    explicit MatchingEngine(Sink& sink, const BookConfig& config = {}) : sink_(sink), book_(config) {}

    [[nodiscard]] Reject add(const AddOrder& request) noexcept;
    [[nodiscard]] Reject cancel(const CancelOrder& request) noexcept;

    const OrderBook& book() const noexcept { return book_; }

private:
    Sink& sink_;
    OrderBook book_;
};

// ---- implementation -----------------------------------------------------------

namespace detail {
// A buy crosses when its limit is at or above the best ask; a sell when its
// limit is at or below the best bid.
constexpr bool crosses(Side aggressor, Price limit, Price best_opposite) noexcept {
    return aggressor == Side::Buy ? limit >= best_opposite : limit <= best_opposite;
}
}  // namespace detail

template <EventSink Sink>
Reject MatchingEngine<Sink>::add(const AddOrder& request) noexcept {
    // 1. Validate and reserve before touching anything: a rejected request has
    //    no effect. The order of these checks is the rejection precedence.
    if (request.id == 0) [[unlikely]]
        return Reject::InvalidOrderId;
    if (request.qty == 0) [[unlikely]]
        return Reject::InvalidQuantity;
    if (book_.contains(request.id)) [[unlikely]]
        return Reject::DuplicateOrderId;
    if (!book_.reserve_for_add(request.side)) [[unlikely]]
        return Reject::CapacityExceeded;

    // 2. Match against the best opposite price, oldest order first, until
    //    filled or the prices no longer cross. Each trade is at the resting
    //    order's price for the smaller of the two quantities.
    const Side opposite_side = opposite(request.side);
    Quantity remaining = request.qty;
    while (remaining > 0 && !book_.empty(opposite_side) &&
           detail::crosses(request.side, request.price, book_.best_price(opposite_side))) {
        const OrderBook::Fill fill = book_.fill_best(opposite_side, remaining);
        remaining -= fill.qty;

        sink_.on_trade(Trade{fill.qty, fill.price});
        if (remaining == 0)
            sink_.on_fully_filled(OrderFullyFilled{request.id});
        else
            sink_.on_partially_filled(OrderPartiallyFilled{request.id, remaining});
        if (fill.resting_remaining == 0)
            sink_.on_fully_filled(OrderFullyFilled{fill.resting_id});
        else
            sink_.on_partially_filled(OrderPartiallyFilled{fill.resting_id, fill.resting_remaining});
    }

    // 3. Whatever is left rests at the limit price, behind existing orders.
    if (remaining > 0) book_.rest(request.side, request.id, remaining, request.price);
    return Reject::None;
}

template <EventSink Sink>
Reject MatchingEngine<Sink>::cancel(const CancelOrder& request) noexcept {
    return book_.cancel(request.id) ? Reject::None : Reject::UnknownOrderId;
}

}  // namespace matcher
