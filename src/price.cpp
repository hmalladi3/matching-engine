#include "matcher/price.h"

namespace matcher {

std::string_view describe(PriceError) noexcept { return "stub"; }

std::optional<Price> parse_price(std::string_view, PriceError& why) noexcept {
    why = PriceError::Malformed;  // Phase 5 stub
    return std::nullopt;
}

char* format_price(Price, char* out) noexcept { return out; }  // Phase 5 stub

std::string to_string(Price price) {
    char buf[Price::kMaxFormattedLen];
    return std::string(buf, format_price(price, buf));
}

}  // namespace matcher
