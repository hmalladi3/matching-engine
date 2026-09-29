#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "matcher/event_sink.h"
#include "matcher/price.h"

namespace matcher::test {

using Event = std::variant<Trade, OrderFullyFilled, OrderPartiallyFilled>;

// Renders an event in the output wire format, independently of EventWriter.
inline std::string to_line(const Event& event) {
    struct Visitor {
        std::string operator()(const Trade& t) const { return "2," + std::to_string(t.qty) + "," + to_string(t.price); }
        std::string operator()(const OrderFullyFilled& f) const { return "3," + std::to_string(f.id); }
        std::string operator()(const OrderPartiallyFilled& p) const {
            return "4," + std::to_string(p.id) + "," + std::to_string(p.remaining);
        }
    };
    return std::visit(Visitor{}, event);
}

inline std::vector<std::string> to_lines(const std::vector<Event>& events) {
    std::vector<std::string> lines;
    lines.reserve(events.size());
    for (const Event& e : events) lines.push_back(to_line(e));
    return lines;
}

// Records engine events in emission order.
class CaptureSink {
public:
    void on_trade(const Trade& t) noexcept { events_.emplace_back(t); }
    void on_fully_filled(const OrderFullyFilled& f) noexcept { events_.emplace_back(f); }
    void on_partially_filled(const OrderPartiallyFilled& p) noexcept { events_.emplace_back(p); }

    const std::vector<Event>& events() const { return events_; }
    std::vector<Event> take() { return std::exchange(events_, {}); }
    std::vector<std::string> take_lines() { return to_lines(take()); }

private:
    std::vector<Event> events_;
};
static_assert(EventSink<CaptureSink>);

}  // namespace matcher::test
