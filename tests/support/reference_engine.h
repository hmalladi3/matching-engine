#pragma once

#include <cstdint>
#include <vector>

#include "matcher/order_book.h"
#include "matcher/types.h"
#include "support/capture_sink.h"

namespace matcher::test {

// A deliberately naive matching engine used as a test oracle.
//
// Each side is a plain vector of orders; every operation scans linearly. It
// shares no code with the real engine beyond the plain types, and is short
// enough to check against the matching rules by reading. Capacity is unlimited, so it
// never produces CapacityExceeded.
class ReferenceEngine {
public:
    Reject add(const AddOrder& request, std::vector<Event>& events);
    // On success, `cancelled_qty` receives the quantity removed from the book.
    Reject cancel(const CancelOrder& request, Quantity& cancelled_qty);

    // Same shape as OrderBook::snapshot: best price first, oldest order first.
    std::vector<OrderBook::LevelSnapshot> snapshot(Side side) const;

private:
    struct Order {
        OrderId id;
        Quantity qty;
        Price price;
        std::uint64_t seq;  // arrival order
    };

    std::vector<Order>& side_orders(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const std::vector<Order>& side_orders(Side side) const { return side == Side::Buy ? bids_ : asks_; }
    bool is_live(OrderId id) const;

    std::vector<Order> bids_;
    std::vector<Order> asks_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace matcher::test
