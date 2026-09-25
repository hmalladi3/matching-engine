#pragma once

#include <cstdint>
#include <string_view>

#include "matcher/price.h"

namespace matcher {

using OrderId = std::uint64_t;   // always > 0; 0 is reserved as the index's empty marker
using Quantity = std::uint64_t;  // always > 0 for a live order

// Numeric values match the wire protocol.
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side side) noexcept { return side == Side::Buy ? Side::Sell : Side::Buy; }

// ---- requests (input) ---------------------------------------------------------

struct AddOrder {
    OrderId id;
    Side side;
    Quantity qty;
    Price price;
    friend bool operator==(const AddOrder&, const AddOrder&) = default;
};

struct CancelOrder {
    OrderId id;
    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

// ---- events (output) ----------------------------------------------------------

struct Trade {
    Quantity qty;
    Price price;
    friend bool operator==(const Trade&, const Trade&) = default;
};

struct OrderFullyFilled {
    OrderId id;
    friend bool operator==(const OrderFullyFilled&, const OrderFullyFilled&) = default;
};

struct OrderPartiallyFilled {
    OrderId id;
    Quantity remaining;
    friend bool operator==(const OrderPartiallyFilled&, const OrderPartiallyFilled&) = default;
};

// ---- rejections ---------------------------------------------------------------

// Why the engine refused a request. Checked in this order for adds (MATCH-REJ-006).
enum class Reject : std::uint8_t {
    None = 0,
    InvalidOrderId,    // id == 0 (defense in depth; the parser also rejects)
    InvalidQuantity,   // qty == 0 (defense in depth; the parser also rejects)
    DuplicateOrderId,  // an order with this id is live
    UnknownOrderId,    // cancel of an id that is not live
    CapacityExceeded,  // the book could not grow
};

std::string_view to_string(Reject) noexcept;

}  // namespace matcher
