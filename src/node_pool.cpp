#include "matcher/node_pool.h"

#include <algorithm>
#include <cassert>
#include <new>
#include <stdexcept>

namespace matcher {

NodePool::NodePool(std::size_t initial_capacity, std::size_t max_nodes)
    : nodes_(std::min(initial_capacity, max_nodes)),  // value-initialized: every page touched now
      max_nodes_(max_nodes) {}

bool NodePool::reserve_for(std::size_t n) noexcept {
    if (available() >= n) [[likely]]
        return true;

    const std::size_t needed = live() + n;
    if (needed > max_nodes_) return false;
    std::size_t capacity = std::max<std::size_t>(nodes_.size(), 1);
    while (capacity < needed) capacity *= 2;
    capacity = std::min(capacity, max_nodes_);
    try {
        nodes_.resize(capacity);  // strong guarantee: unchanged if this throws
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

NodeIndex NodePool::acquire() noexcept {
    assert(available() >= 1);
    if (free_head_ != kNullNode) {  // reuse the most recently freed (cache-warm) node
        const NodeIndex index = free_head_;
        free_head_ = nodes_[index].next;
        --free_count_;
        return index;
    }
    return static_cast<NodeIndex>(used_++);
}

void NodePool::release(NodeIndex index) noexcept {
    nodes_[index].next = free_head_;
    free_head_ = index;
    ++free_count_;
}

}  // namespace matcher
