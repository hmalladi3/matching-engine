#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "matcher/types.h"

namespace matcher {

using NodeIndex = std::uint32_t;
inline constexpr NodeIndex kNullNode = std::numeric_limits<NodeIndex>::max();
// Indices 0 .. 2^32-2 are usable; 2^32-1 is kNullNode.
inline constexpr std::size_t kMaxNodes = kNullNode;

// One resting order, or a price level's sentinel (id == 0). `next`/`prev` link
// the level's circular FIFO; `next` doubles as the free-list link.
struct Node {
    OrderId id;
    Quantity qty;
    Price price;
    NodeIndex next;
    NodeIndex prev;
};
static_assert(sizeof(Node) == 32, "two nodes per 64-byte cache line");

// Index-addressed slab of Nodes with a free list.
//
// The backing vector is sized to full capacity up front, so every page is
// touched before trading starts. Only reserve_for() ever allocates; acquire()
// and release() never do.
class NodePool {
public:
    NodePool(std::size_t initial_capacity, std::size_t max_nodes = kMaxNodes);

    // Ensures at least `n` more nodes can be acquired, doubling capacity if
    // needed. Returns false, with the pool unchanged, if that would exceed
    // max_nodes or allocation fails.
    [[nodiscard]] bool reserve_for(std::size_t n) noexcept;

    // Precondition: available() >= 1.
    [[nodiscard]] NodeIndex acquire() noexcept;
    void release(NodeIndex index) noexcept;

    Node& operator[](NodeIndex index) noexcept { return nodes_[index]; }
    const Node& operator[](NodeIndex index) const noexcept { return nodes_[index]; }

    std::size_t capacity() const noexcept { return nodes_.size(); }
    std::size_t live() const noexcept { return used_ - free_count_; }
    std::size_t available() const noexcept { return capacity() - live(); }

private:
    std::vector<Node> nodes_;
    std::size_t max_nodes_;
    std::size_t used_ = 0;        // high-water mark: slots [0, used_) have been handed out
    std::size_t free_count_ = 0;  // length of the free list
    NodeIndex free_head_ = kNullNode;
};

}  // namespace matcher
