#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "matcher/level_store.h"
#include "matcher/node_pool.h"
#include "matcher/order_index.h"
#include "matcher/types.h"

namespace matcher {

struct BookConfig {
    std::size_t reserve_orders = std::size_t{1} << 20;  // BOOK-MEM-001 default
    std::size_t reserve_levels = 4096;                  // per side
    std::size_t max_nodes = kMaxNodes;                  // hard cap (orders + level sentinels)
    std::size_t max_retained_levels = 256;              // per side; see order-book.md, Retained Empty Levels
};

// Resting orders for one instrument. Storage only: it knows nothing about
// matching rules or messages. MatchingEngine is its only writer.
//
// Every mutation keeps the invariants listed in check_invariants(); the
// operations are intention-level so no caller can leave the book inconsistent.
class OrderBook {
public:
    explicit OrderBook(const BookConfig& config = {});

    // Ensures capacity for one more order on `side`: an order node, a level
    // sentinel, an index slot and a level entry. The only operation that may
    // allocate. Returns false, with the book unchanged, if growth fails.
    [[nodiscard]] bool reserve_for_add(Side side) noexcept;

    [[nodiscard]] bool contains(OrderId id) const noexcept;
    [[nodiscard]] bool empty(Side side) const noexcept;
    // Precondition: !empty(side).
    [[nodiscard]] Price best_price(Side side) const noexcept;

    struct Fill {
        OrderId resting_id;
        Price price;
        Quantity qty;
        Quantity resting_remaining;
    };

    // Fills up to `max_qty` against the oldest order at the best price on
    // `side`: reduces it in place (keeping its queue position) or removes it,
    // and its level, when it reaches zero.
    // Preconditions: !empty(side), max_qty > 0.
    [[nodiscard]] Fill fill_best(Side side, Quantity max_qty) noexcept;

    // Appends a new order to the back of its (possibly new) price level.
    // Preconditions: id absent, qty > 0, reserve_for_add(side) succeeded.
    void rest(Side side, OrderId id, Quantity qty, Price price) noexcept;

    // Removes a live order from anywhere in the book. Returns false if absent.
    bool cancel(OrderId id) noexcept;

    // ---- introspection (tests, invariant checks) --------------------------------

    std::size_t order_count() const noexcept { return index_.size(); }
    // Price levels with resting orders.
    std::size_t level_count(Side side) const noexcept;
    // Empty price levels retained for reuse (see order-book.md).
    std::size_t empty_level_count(Side side) const noexcept;

    struct LevelSnapshot {
        Price price;
        std::vector<std::pair<OrderId, Quantity>> orders;  // oldest first
        friend bool operator==(const LevelSnapshot&, const LevelSnapshot&) = default;
    };
    // Best price first. Allocates; for tests only.
    std::vector<LevelSnapshot> snapshot(Side side) const;

    // Verifies every structural invariant; aborts with a description on
    // violation. O(n); for tests only.
    // @spec BOOK-INV-001
    void check_invariants() const;

private:
    NodeIndex new_sentinel(Price price) noexcept;
    void unlink(NodeIndex index) noexcept;
    template <Side S>
    Fill fill_best_on(LevelStore<S>& levels, Quantity max_qty) noexcept;
    template <Side S>
    void rest_on(LevelStore<S>& levels, OrderId id, Quantity qty, Price price) noexcept;
    template <Side S>
    void drop_empty_best(LevelStore<S>& levels) noexcept;
    template <Side S>
    void release_retained(LevelStore<S>& levels) noexcept;
    template <Side S>
    void level_emptied(LevelStore<S>& levels, NodeIndex sentinel, Price price) noexcept;
    bool is_empty_level(NodeIndex sentinel) const noexcept { return pool_[sentinel].next == sentinel; }
    std::size_t& retained(Side side) noexcept { return empty_levels_[static_cast<std::size_t>(side)]; }
    template <Side S>
    void check_side(const LevelStore<S>& levels, std::size_t& nodes_seen) const;

    NodePool pool_;
    OrderIndex index_;
    LevelStore<Side::Buy> bids_;
    LevelStore<Side::Sell> asks_;
    std::size_t max_retained_;              // per side
    std::size_t empty_levels_[2] = {0, 0};  // retained empty levels, indexed by Side
};

}  // namespace matcher
