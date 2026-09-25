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
    explicit MatchingEngine(Sink& sink, const BookConfig& config = {})
        : sink_(sink), book_(config) {}

    [[nodiscard]] Reject add(const AddOrder& request) noexcept;
    [[nodiscard]] Reject cancel(const CancelOrder& request) noexcept;

    const OrderBook& book() const noexcept { return book_; }

private:
    Sink& sink_;
    OrderBook book_;
};

// ---- implementation -----------------------------------------------------------

template <EventSink Sink>
Reject MatchingEngine<Sink>::add(const AddOrder& request) noexcept {
    (void)request;
    return Reject::None;  // Phase 5 stub
}

template <EventSink Sink>
Reject MatchingEngine<Sink>::cancel(const CancelOrder& request) noexcept {
    (void)request;
    return Reject::None;  // Phase 5 stub
}

}  // namespace matcher
