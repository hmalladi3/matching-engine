#include "matcher/level_store.h"

#include <algorithm>
#include <cassert>
#include <new>
#include <stdexcept>

namespace matcher {

namespace {

// Orders levels in storage order (worst first): `level` sorts before `price`
// iff `price` is strictly better.
template <Side S>
bool worse_than(const Level& level, Price price) noexcept {
    return LevelStore<S>::is_better(price, level.price);
}

}  // namespace

template <Side S>
LevelStore<S>::LevelStore(std::size_t reserve_levels) {
    levels_.reserve(std::max<std::size_t>(reserve_levels, 1));
}

// @spec BOOK-OP-006, BOOK-OP-007
template <Side S>
Level* LevelStore<S>::find(Price price) noexcept {
    if (levels_.empty()) return nullptr;
    Level& best_level = levels_.back();
    if (best_level.price == price) return &best_level;       // fast path: the best level
    if (is_better(price, best_level.price)) return nullptr;  // would be a new best
    const auto last = levels_.end() - 1;
    const auto it = std::lower_bound(levels_.begin(), last, price, worse_than<S>);
    return it != last && it->price == price ? &*it : nullptr;
}

template <Side S>
Level& LevelStore<S>::insert(Price price, NodeIndex sentinel) noexcept {
    assert(levels_.size() < levels_.capacity() && "reserve_for_one() must precede insert()");
    if (levels_.empty() || is_better(price, levels_.back().price)) {  // fast path: new best
        levels_.push_back(Level{price, sentinel});
        return levels_.back();
    }
    const auto it = std::lower_bound(levels_.begin(), levels_.end(), price, worse_than<S>);
    assert(it == levels_.end() || it->price != price);
    return *levels_.insert(it, Level{price, sentinel});  // memmove of the better levels
}

// @spec BOOK-OP-005
template <Side S>
void LevelStore<S>::erase(Price price) noexcept {
    assert(!levels_.empty());
    if (levels_.back().price == price) {
        levels_.pop_back();
        return;
    }
    const auto it = std::lower_bound(levels_.begin(), levels_.end(), price, worse_than<S>);
    assert(it != levels_.end() && it->price == price);
    levels_.erase(it);
}

// @spec BOOK-MEM-003
template <Side S>
bool LevelStore<S>::reserve_for_one() noexcept {
    if (levels_.size() < levels_.capacity()) [[likely]]
        return true;
    try {
        levels_.reserve(levels_.capacity() * 2);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

template class LevelStore<Side::Buy>;
template class LevelStore<Side::Sell>;

}  // namespace matcher
