#include "matcher/order_book.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace matcher {

// Every level owns a sentinel node, so the pool holds orders plus sentinels.
OrderBook::OrderBook(const BookConfig& config)
    : pool_(config.reserve_orders + 2 * config.reserve_levels, config.max_nodes),
      index_(config.reserve_orders),
      bids_(config.reserve_levels),
      asks_(config.reserve_levels) {}

// @spec BOOK-MEM-003, BOOK-MEM-004
bool OrderBook::reserve_for_add(Side side) noexcept {
    // Worst case for one add: an order node and a new level (sentinel node +
    // level entry) on its own side, plus one index slot. Each step leaves its
    // structure unchanged on failure; spare capacity from an earlier step is
    // harmless.
    const bool levels_ok = side == Side::Buy ? bids_.reserve_for_one() : asks_.reserve_for_one();
    return levels_ok && pool_.reserve_for(2) && index_.reserve_for(1);
}

bool OrderBook::contains(OrderId id) const noexcept { return index_.find(id).has_value(); }

bool OrderBook::empty(Side side) const noexcept {
    return side == Side::Buy ? bids_.empty() : asks_.empty();
}

// @spec BOOK-OP-001
Price OrderBook::best_price(Side side) const noexcept {
    assert(!empty(side));
    return side == Side::Buy ? bids_.best().price : asks_.best().price;
}

std::size_t OrderBook::level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

NodeIndex OrderBook::new_sentinel(Price price) noexcept {
    const NodeIndex sentinel = pool_.acquire();
    pool_[sentinel] = Node{0, 0, price, sentinel, sentinel};  // empty circular list
    return sentinel;
}

void OrderBook::unlink(NodeIndex index) noexcept {
    const Node& node = pool_[index];
    pool_[node.prev].next = node.next;
    pool_[node.next].prev = node.prev;
}

template <Side S>
void OrderBook::erase_level_if_empty(LevelStore<S>& levels, NodeIndex sentinel, Price price) noexcept {
    if (pool_[sentinel].next != sentinel) return;
    levels.erase(price);  // O(1) at the best price; O(log L + d) deeper
    pool_.release(sentinel);
}

// @spec BOOK-OP-002, BOOK-OP-003
template <Side S>
OrderBook::Fill OrderBook::fill_best_on(LevelStore<S>& levels, Quantity max_qty) noexcept {
    const Level& level = levels.best();
    const NodeIndex sentinel = level.sentinel;
    const NodeIndex oldest = pool_[sentinel].next;
    Node& order = pool_[oldest];

    const Quantity qty = std::min(max_qty, order.qty);
    order.qty -= qty;  // in place: a partially filled order keeps its queue position
    const Fill fill{order.id, level.price, qty, order.qty};

    if (order.qty == 0) {
        unlink(oldest);
        index_.erase(fill.resting_id);
        pool_.release(oldest);
        erase_level_if_empty(levels, sentinel, fill.price);
    }
    return fill;
}

OrderBook::Fill OrderBook::fill_best(Side side, Quantity max_qty) noexcept {
    assert(!empty(side) && max_qty > 0);
    return side == Side::Buy ? fill_best_on(bids_, max_qty) : fill_best_on(asks_, max_qty);
}

// @spec BOOK-OP-006, BOOK-OP-007, BOOK-OP-008
template <Side S>
void OrderBook::rest_on(LevelStore<S>& levels, OrderId id, Quantity qty, Price price) noexcept {
    NodeIndex sentinel;
    if (const Level* level = levels.find(price)) {
        sentinel = level->sentinel;
    } else {
        sentinel = new_sentinel(price);
        levels.insert(price, sentinel);
    }

    // Append at the tail (sentinel.prev): newest order, lowest time priority.
    const NodeIndex index = pool_.acquire();
    const NodeIndex tail = pool_[sentinel].prev;
    pool_[index] = Node{id, qty, price, sentinel, tail};
    pool_[tail].next = index;
    pool_[sentinel].prev = index;
    index_.insert(id, IndexEntry{index, S});
}

void OrderBook::rest(Side side, OrderId id, Quantity qty, Price price) noexcept {
    assert(id != 0 && qty > 0 && !contains(id));
    if (side == Side::Buy) rest_on(bids_, id, qty, price);
    else rest_on(asks_, id, qty, price);
}

// @spec BOOK-OP-004, BOOK-OP-005
bool OrderBook::cancel(OrderId id) noexcept {
    const std::optional<IndexEntry> entry = index_.find(id);
    if (!entry) return false;

    const NodeIndex index = entry->node;
    const Node node = pool_[index];
    unlink(index);  // O(1): touches only the two neighbours
    index_.erase(id);
    pool_.release(index);

    // The list is circular through the sentinel, so if this was the level's
    // only order both neighbours are the sentinel. Only then is the level
    // (found by price) erased.
    if (node.next == node.prev) {
        if (entry->side == Side::Buy) erase_level_if_empty(bids_, node.next, node.price);
        else erase_level_if_empty(asks_, node.next, node.price);
    }
    return true;
}

std::vector<OrderBook::LevelSnapshot> OrderBook::snapshot(Side side) const {
    std::vector<LevelSnapshot> out;
    const auto collect = [&](std::span<const Level> levels) {
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) {  // best first
            LevelSnapshot snap{it->price, {}};
            for (NodeIndex i = pool_[it->sentinel].next; i != it->sentinel; i = pool_[i].next)
                snap.orders.emplace_back(pool_[i].id, pool_[i].qty);
            out.push_back(std::move(snap));
        }
    };
    collect(side == Side::Buy ? bids_.levels() : asks_.levels());
    return out;
}

// ---- invariant checking --------------------------------------------------------

namespace {

[[noreturn]] void invariant_failed(const char* what, unsigned long long detail) {
    std::fprintf(stderr, "OrderBook invariant violated: %s (%llu)\n", what, detail);
    std::abort();
}

void require(bool condition, const char* what, unsigned long long detail = 0) {
    if (!condition) invariant_failed(what, detail);
}

}  // namespace

template <Side S>
void OrderBook::check_side(const LevelStore<S>& levels, std::size_t& nodes_seen) const {
    const std::span<const Level> all = levels.levels();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Level& level = all[i];
        if (i > 0)
            require(LevelStore<S>::is_better(level.price, all[i - 1].price),
                    "levels not strictly ordered best-at-back", i);

        const Node& sentinel = pool_[level.sentinel];
        require(sentinel.id == 0, "level sentinel has an order id", level.sentinel);
        require(sentinel.next != level.sentinel, "empty level present", i);

        std::size_t steps = 0;
        NodeIndex prev = level.sentinel;
        for (NodeIndex n = sentinel.next; n != level.sentinel; prev = n, n = pool_[n].next) {
            require(++steps <= pool_.capacity(), "FIFO does not return to its sentinel", i);
            const Node& order = pool_[n];
            require(order.prev == prev, "FIFO back-link broken", n);
            require(order.id != 0, "order with id 0", n);
            require(order.qty > 0, "order with zero quantity", order.id);
            require(order.price == level.price, "order price differs from its level", order.id);
            const std::optional<IndexEntry> entry = index_.find(order.id);
            require(entry.has_value(), "resting order missing from index", order.id);
            require(entry->node == n, "index points at the wrong node", order.id);
            require(entry->side == S, "index records the wrong side", order.id);
        }
        require(sentinel.prev == prev, "sentinel back-link broken", level.sentinel);
        nodes_seen += steps + 1;  // orders plus the sentinel
    }
}

// @spec BOOK-INV-001
void OrderBook::check_invariants() const {
    std::size_t nodes_seen = 0;
    check_side(bids_, nodes_seen);
    check_side(asks_, nodes_seen);
    const std::size_t orders = nodes_seen - bids_.size() - asks_.size();
    // Each resting order was found in the index; equal counts make it a bijection.
    require(orders == index_.size(), "index size differs from resting orders", index_.size());
    require(nodes_seen == pool_.live(), "pool live count differs from nodes in the book", pool_.live());
    if (!bids_.empty() && !asks_.empty())
        require(bids_.best().price < asks_.best().price, "book is crossed");
}

}  // namespace matcher
