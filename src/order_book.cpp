#include "matcher/order_book.h"

namespace matcher {

OrderBook::OrderBook(const BookConfig& config)
    : pool_(config.reserve_orders, config.max_nodes),
      index_(config.reserve_orders),
      bids_(config.reserve_levels),
      asks_(config.reserve_levels) {}  // Phase 5 stub

bool OrderBook::reserve_for_add(Side) noexcept { return false; }
bool OrderBook::contains(OrderId) const noexcept { return false; }
bool OrderBook::empty(Side) const noexcept { return true; }
Price OrderBook::best_price(Side) const noexcept { return Price{}; }
OrderBook::Fill OrderBook::fill_best(Side, Quantity) noexcept { return {}; }
void OrderBook::rest(Side, OrderId, Quantity, Price) noexcept {}
bool OrderBook::cancel(OrderId) noexcept { return false; }
std::size_t OrderBook::level_count(Side) const noexcept { return 0; }
std::vector<OrderBook::LevelSnapshot> OrderBook::snapshot(Side) const { return {}; }
void OrderBook::check_invariants() const {}

}  // namespace matcher
