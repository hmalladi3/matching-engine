#include "matcher/order_index.h"

namespace matcher {

OrderIndex::OrderIndex(std::size_t) {}  // Phase 5 stub
bool OrderIndex::reserve_for(std::size_t) noexcept { return false; }
std::optional<IndexEntry> OrderIndex::find(OrderId) const noexcept { return std::nullopt; }
void OrderIndex::insert(OrderId, IndexEntry) noexcept {}
bool OrderIndex::erase(OrderId) noexcept { return false; }
std::size_t OrderIndex::home_slot(OrderId) const noexcept { return 0; }

}  // namespace matcher
