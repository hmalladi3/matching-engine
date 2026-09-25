#pragma once

#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

#include "matcher/node_pool.h"
#include "matcher/types.h"

namespace matcher {

// A price level: its price and the sentinel node of its circular FIFO.
struct Level {
    Price price;
    NodeIndex sentinel;
};
static_assert(sizeof(Level) == 16, "four levels per cache line");
static_assert(std::is_trivially_copyable_v<Level>, "vector insert/erase become memmove");

// One side's price levels in a vector sorted so that back() is the best price
// (bids ascending, asks descending). Activity concentrates at the best price,
// which is the cheap end of a vector.
template <Side S>
class LevelStore {
public:
    explicit LevelStore(std::size_t reserve_levels);

    // True if `a` is a strictly better price than `b` for this side.
    static constexpr bool is_better(Price a, Price b) noexcept {
        if constexpr (S == Side::Buy)
            return a > b;
        else
            return a < b;
    }

    bool empty() const noexcept { return levels_.empty(); }
    std::size_t size() const noexcept { return levels_.size(); }

    // Preconditions: !empty().
    Level& best() noexcept { return levels_.back(); }
    const Level& best() const noexcept { return levels_.back(); }
    void pop_best() noexcept { levels_.pop_back(); }

    // The level at `price`, or nullptr. O(1) at or better than the best price.
    Level* find(Price price) noexcept;

    // Preconditions: no level at `price`; reserve_for_one() succeeded.
    Level& insert(Price price, NodeIndex sentinel) noexcept;

    // Precondition: a level exists at `price`.
    void erase(Price price) noexcept;

    // Ensures one more level can be inserted without reallocation. Returns
    // false, with the store unchanged, on allocation failure.
    [[nodiscard]] bool reserve_for_one() noexcept;

    // Storage order: worst price first, best price last.
    std::span<const Level> levels() const noexcept { return levels_; }

private:
    std::vector<Level> levels_;
};

extern template class LevelStore<Side::Buy>;
extern template class LevelStore<Side::Sell>;

}  // namespace matcher
