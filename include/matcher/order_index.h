#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "matcher/node_pool.h"
#include "matcher/types.h"

namespace matcher {

struct IndexEntry {
    NodeIndex node;
    Side side;
    friend bool operator==(const IndexEntry&, const IndexEntry&) = default;
};

// OrderId -> (node, side). Open addressing with linear probing, power-of-two
// capacity, load factor <= 1/2, Fibonacci hashing, and backward-shift deletion
// (no tombstones, so probe lengths stay short under add/cancel churn).
// Key 0 marks an empty slot, which is safe because order ids are positive.
// @spec BOOK-OP-009
class OrderIndex {
public:
    explicit OrderIndex(std::size_t expected_entries);

    // Ensures `n` more entries fit within the load factor, doubling and
    // rehashing if needed. Returns false, with the index unchanged, on
    // allocation failure.
    [[nodiscard]] bool reserve_for(std::size_t n) noexcept;

    [[nodiscard]] std::optional<IndexEntry> find(OrderId id) const noexcept;

    // Preconditions: id != 0, id absent, capacity reserved.
    void insert(OrderId id, IndexEntry entry) noexcept;

    // Returns false if id was absent.
    bool erase(OrderId id) noexcept;

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    // The slot an id hashes to before probing. Lets tests construct collisions.
    std::size_t home_slot(OrderId id) const noexcept;

    template <class F>
    void for_each(F&& f) const {
        for (const Slot& s : slots_)
            if (s.key != 0) f(s.key, IndexEntry{s.node, s.side});
    }

private:
    struct Slot {
        OrderId key = 0;
        NodeIndex node = kNullNode;
        Side side = Side::Buy;
    };
    static_assert(sizeof(Slot) == 16);

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
    unsigned shift_ = 0;  // 64 - log2(capacity)
};

}  // namespace matcher
