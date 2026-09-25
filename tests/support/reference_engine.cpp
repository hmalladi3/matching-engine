#include "support/reference_engine.h"

#include <algorithm>

namespace matcher::test {

bool ReferenceEngine::is_live(OrderId id) const {
    auto has = [id](const Order& o) { return o.id == id; };
    return std::any_of(bids_.begin(), bids_.end(), has) || std::any_of(asks_.begin(), asks_.end(), has);
}

Reject ReferenceEngine::add(const AddOrder& request, std::vector<Event>& events) {
    if (request.id == 0) return Reject::InvalidOrderId;
    if (request.qty == 0) return Reject::InvalidQuantity;
    if (is_live(request.id)) return Reject::DuplicateOrderId;

    std::vector<Order>& opposite_orders = side_orders(opposite(request.side));
    Quantity remaining = request.qty;

    while (remaining > 0 && !opposite_orders.empty()) {
        // Best resting order: best price for the aggressor, then earliest arrival.
        auto better = [&](const Order& a, const Order& b) {
            if (a.price != b.price)
                return request.side == Side::Buy ? a.price < b.price : a.price > b.price;
            return a.seq < b.seq;
        };
        auto best = std::min_element(opposite_orders.begin(), opposite_orders.end(), better);

        const bool crosses =
            request.side == Side::Buy ? request.price >= best->price : request.price <= best->price;
        if (!crosses) break;

        const Quantity traded = std::min(remaining, best->qty);
        remaining -= traded;
        best->qty -= traded;

        events.emplace_back(Trade{traded, best->price});
        if (remaining == 0) events.emplace_back(OrderFullyFilled{request.id});
        else events.emplace_back(OrderPartiallyFilled{request.id, remaining});
        if (best->qty == 0) {
            events.emplace_back(OrderFullyFilled{best->id});
            opposite_orders.erase(best);
        } else {
            events.emplace_back(OrderPartiallyFilled{best->id, best->qty});
        }
    }

    if (remaining > 0)
        side_orders(request.side).push_back({request.id, remaining, request.price, next_seq_++});
    return Reject::None;
}

Reject ReferenceEngine::cancel(const CancelOrder& request, Quantity& cancelled_qty) {
    for (std::vector<Order>* orders : {&bids_, &asks_}) {
        auto it = std::find_if(orders->begin(), orders->end(),
                               [&](const Order& o) { return o.id == request.id; });
        if (it != orders->end()) {
            cancelled_qty = it->qty;
            orders->erase(it);
            return Reject::None;
        }
    }
    return Reject::UnknownOrderId;
}

std::vector<OrderBook::LevelSnapshot> ReferenceEngine::snapshot(Side side) const {
    std::vector<Order> orders = side_orders(side);
    std::sort(orders.begin(), orders.end(), [side](const Order& a, const Order& b) {
        if (a.price != b.price) return side == Side::Buy ? a.price > b.price : a.price < b.price;
        return a.seq < b.seq;
    });
    std::vector<OrderBook::LevelSnapshot> levels;
    for (const Order& o : orders) {
        if (levels.empty() || levels.back().price != o.price) levels.push_back({o.price, {}});
        levels.back().orders.emplace_back(o.id, o.qty);
    }
    return levels;
}

}  // namespace matcher::test
