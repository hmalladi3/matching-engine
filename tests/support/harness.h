#pragma once

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "matcher/matching_engine.h"
#include "matcher/request_parser.h"
#include "support/capture_sink.h"

namespace matcher::test {

// Tiny reservations, so ordinary tests also exercise every growth path.
inline BookConfig tiny_config() { return BookConfig{4, 2, kMaxNodes}; }

inline Price px(std::string_view text) {
    PriceError why = PriceError::None;
    auto price = parse_price(text, why);
    EXPECT_TRUE(price.has_value()) << "bad test price: " << text;
    return price.value_or(Price{});
}

inline OrderBook::LevelSnapshot level(std::string_view price,
                                      std::vector<std::pair<OrderId, Quantity>> orders) {
    return {px(price), std::move(orders)};
}

// Drives a MatchingEngine with brief-format CSV lines, so tests read like the
// brief's own examples. Lines go through the real request parser.
class Harness {
public:
    explicit Harness(const BookConfig& config = tiny_config()) : engine_(sink_, config) {}

    // Sends one request; returns the engine's verdict. Unparseable lines fail the test.
    Reject send(std::string_view line) {
        const ParseResult parsed = parse_request(line);
        if (const auto* add = std::get_if<AddOrder>(&parsed)) return check(engine_.add(*add));
        if (const auto* cancel = std::get_if<CancelOrder>(&parsed)) return check(engine_.cancel(*cancel));
        ADD_FAILURE() << "test line did not parse as a request: " << line;
        return Reject::None;
    }

    // Sends each line and returns every output line produced, in order.
    std::vector<std::string> run(std::initializer_list<std::string_view> lines) {
        for (std::string_view line : lines) (void)send(line);
        return sink_.take_lines();
    }

    std::vector<std::string> take_output() { return sink_.take_lines(); }
    std::vector<OrderBook::LevelSnapshot> levels(Side side) const { return engine_.book().snapshot(side); }
    const OrderBook& book() const { return engine_.book(); }

private:
    Reject check(Reject r) {
        engine_.book().check_invariants();
        return r;
    }

    CaptureSink sink_;
    MatchingEngine<CaptureSink> engine_;
};

}  // namespace matcher::test
