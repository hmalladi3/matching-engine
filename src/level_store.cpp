#include "matcher/level_store.h"

namespace matcher {

template <Side S>
LevelStore<S>::LevelStore(std::size_t) {}  // Phase 5 stub
template <Side S>
Level* LevelStore<S>::find(Price) noexcept { return nullptr; }
template <Side S>
Level& LevelStore<S>::insert(Price price, NodeIndex sentinel) noexcept {
    levels_.push_back({price, sentinel});
    return levels_.back();
}
template <Side S>
void LevelStore<S>::erase(Price) noexcept {}
template <Side S>
bool LevelStore<S>::reserve_for_one() noexcept { return false; }

template class LevelStore<Side::Buy>;
template class LevelStore<Side::Sell>;

}  // namespace matcher
