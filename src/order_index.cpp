#include "matcher/order_index.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <new>
#include <stdexcept>
#include <utility>

namespace matcher {

namespace {

constexpr std::size_t kMinCapacity = 16;
// 2^64 / golden ratio.
constexpr std::uint64_t kFibonacci = 0x9E3779B97F4A7C15ULL;
// Ids that differ only in their low 2 bits share a 4-slot block: 4 x 16-byte
// slots = one 64-byte cache line.
constexpr unsigned kBlockBits = 2;

std::size_t capacity_for(std::size_t entries) {
    return std::bit_ceil(std::max(entries * 2, kMinCapacity));  // load factor <= 1/2
}

unsigned shift_for(std::size_t capacity) { return 64u - static_cast<unsigned>(std::countr_zero(capacity)); }

}  // namespace

OrderIndex::OrderIndex(std::size_t expected_entries)
    : slots_(capacity_for(expected_entries)), shift_(shift_for(slots_.size())) {}

// Blocked hashing: consecutive (exchange-assigned) ids fill one cache line
// four at a time instead of touching four random lines. Blocks are chosen by
// Fibonacci hashing, which spreads *consecutive* inputs almost perfectly
// evenly (no collisions for sequential ids), after xoring in the input shifted
// right by 12. That pre-xor feeds high bits into the low bits that power-of-two
// strides leave zero, fixing plain Fibonacci's weak strides, while only
// permuting ids within aligned runs, so the sequential property survives.
// Alternatives measured (order-book.md): plain Fibonacci, a multiply-fold-
// multiply mixer, identity, and 8- and 16-id blocks.
// @spec BOOK-OP-010
std::size_t OrderIndex::home_slot(OrderId id) const noexcept {
    std::uint64_t x = id >> kBlockBits;
    x ^= x >> 12;
    const std::uint64_t block = (x * kFibonacci) >> (shift_ + kBlockBits);
    return static_cast<std::size_t>((block << kBlockBits) | (id & ((1U << kBlockBits) - 1)));
}

std::optional<IndexEntry> OrderIndex::find(OrderId id) const noexcept {
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t i = home_slot(id);; i = (i + 1) & mask) {
        const Slot& slot = slots_[i];
        if (slot.key == id) return IndexEntry{slot.node, slot.side};
        if (slot.key == 0) return std::nullopt;
    }
}

void OrderIndex::insert(OrderId id, IndexEntry entry) noexcept {
    assert(id != 0 && (size_ + 1) * 2 <= slots_.size());
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = home_slot(id);
    while (slots_[i].key != 0) i = (i + 1) & mask;
    slots_[i] = Slot{id, entry.node, entry.side};
    ++size_;
}

bool OrderIndex::erase(OrderId id) noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t hole = home_slot(id);
    for (;; hole = (hole + 1) & mask) {
        if (slots_[hole].key == id) break;
        if (slots_[hole].key == 0) return false;
    }

    // Backward-shift deletion: walk the rest of the probe cluster and move
    // back any entry whose home slot is not cyclically in (hole, j], so every
    // remaining key stays reachable from its home slot without tombstones.
    for (std::size_t j = (hole + 1) & mask; slots_[j].key != 0; j = (j + 1) & mask) {
        const std::size_t home = home_slot(slots_[j].key);
        const bool stays = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
        if (stays) continue;
        slots_[hole] = slots_[j];
        hole = j;
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
}

// @spec BOOK-MEM-003, BOOK-MEM-004
bool OrderIndex::reserve_for(std::size_t n) noexcept {
    if ((size_ + n) * 2 <= slots_.size()) [[likely]]
        return true;

    Slots old;
    try {
        Slots grown(capacity_for(size_ + n));
        old = std::exchange(slots_, std::move(grown));
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    shift_ = shift_for(slots_.size());
    size_ = 0;
    for (const Slot& s : old)
        if (s.key != 0) insert(s.key, IndexEntry{s.node, s.side});
    return true;
}

}  // namespace matcher
