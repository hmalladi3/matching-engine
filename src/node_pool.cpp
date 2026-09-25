#include "matcher/node_pool.h"

namespace matcher {

NodePool::NodePool(std::size_t initial_capacity, std::size_t max_nodes) : max_nodes_(max_nodes) {
    (void)initial_capacity;  // Phase 5 stub
}
bool NodePool::reserve_for(std::size_t) noexcept { return false; }
NodeIndex NodePool::acquire() noexcept { return kNullNode; }
void NodePool::release(NodeIndex) noexcept {}

}  // namespace matcher
