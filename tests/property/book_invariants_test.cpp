// Random operations applied directly to OrderBook (no engine), checking every
// structural invariant after every operation and comparing against a simple
// model. Exercises the book's own contract independently of matching rules.
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <vector>

#include "matcher/order_book.h"
#include "support/request_generator.h"

namespace matcher {
namespace {

struct ModelOrder {
    OrderId id;
    Quantity qty;
};

// Per side: price -> FIFO of orders. Best is highest bid / lowest ask.
using SideModel = std::map<std::int64_t, std::vector<ModelOrder>>;

std::vector<OrderBook::LevelSnapshot> snapshot_of(const SideModel& model, Side side) {
    std::vector<OrderBook::LevelSnapshot> out;
    auto emit = [&](const auto& entry) {
        OrderBook::LevelSnapshot l{Price::from_raw(entry.first), {}};
        for (const ModelOrder& o : entry.second) l.orders.emplace_back(o.id, o.qty);
        out.push_back(std::move(l));
    };
    if (side == Side::Buy)
        std::for_each(model.rbegin(), model.rend(), emit);
    else
        std::for_each(model.begin(), model.end(), emit);
    return out;
}

// @spec BOOK-INV-001, BOOK-OP-002, BOOK-OP-003, BOOK-OP-004, BOOK-OP-005, BOOK-OP-008, BOOK-MEM-005
TEST(BookInvariants, HoldAfterEveryRandomOperation) {
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        OrderBook book(BookConfig{2, 1, kMaxNodes});
        SideModel model[2];
        std::map<OrderId, std::pair<Side, std::int64_t>> where;
        test::Rng rng(seed);
        OrderId next_id = 1;

        for (int step = 0; step < 100'000; ++step) {
            const Side side = rng.percent(50) ? Side::Buy : Side::Sell;
            SideModel& m = model[static_cast<int>(side)];
            const unsigned op = static_cast<unsigned>(rng.below(100));

            if (op < 50 && where.size() < 3000) {  // rest (sides kept apart so the book never crosses)
                const std::int64_t price = (side == Side::Buy ? -1 : 1) * rng.between(1, 60);
                const Quantity qty = 1 + rng.below(9);
                ASSERT_TRUE(book.reserve_for_add(side));
                book.rest(side, next_id, qty, Price::from_raw(price));
                m[price].push_back({next_id, qty});
                where[next_id] = {side, price};
                ++next_id;
            } else if (op < 75 && !m.empty()) {  // fill at best
                auto best = side == Side::Buy ? std::prev(m.end()) : m.begin();
                ModelOrder& oldest = best->second.front();
                const Quantity take = 1 + rng.below(oldest.qty + 2);  // sometimes more than resting
                const OrderBook::Fill f = book.fill_best(side, take);
                ASSERT_EQ(f.resting_id, oldest.id);
                ASSERT_EQ(f.price.raw(), best->first);
                ASSERT_EQ(f.qty, std::min(take, oldest.qty));
                oldest.qty -= f.qty;
                ASSERT_EQ(f.resting_remaining, oldest.qty);
                if (oldest.qty == 0) {
                    where.erase(oldest.id);
                    best->second.erase(best->second.begin());
                    if (best->second.empty()) m.erase(best);
                }
            } else if (!where.empty()) {  // cancel anywhere
                auto it = where.begin();
                std::advance(it, static_cast<long>(rng.below(std::min<std::size_t>(where.size(), 64))));
                const auto [id, loc] = *it;
                ASSERT_TRUE(book.cancel(id));
                auto& fifo = model[static_cast<int>(loc.first)][loc.second];
                fifo.erase(std::find_if(fifo.begin(), fifo.end(), [id](const ModelOrder& o) { return o.id == id; }));
                if (fifo.empty()) model[static_cast<int>(loc.first)].erase(loc.second);
                where.erase(it);
            }

            book.check_invariants();
            ASSERT_EQ(book.order_count(), where.size());
            if (step % 97 == 0) {
                ASSERT_EQ(book.snapshot(Side::Buy), snapshot_of(model[0], Side::Buy)) << "step " << step;
                ASSERT_EQ(book.snapshot(Side::Sell), snapshot_of(model[1], Side::Sell)) << "step " << step;
            }
        }
    }
}

}  // namespace
}  // namespace matcher
