#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace matcher {

namespace detail {
consteval std::int64_t pow10(int n) {
    std::int64_t v = 1;
    for (int i = 0; i < n; ++i) v *= 10;
    return v;
}
}  // namespace detail

// Exact fixed-point price: value × 10^8 stored in a signed 64-bit integer.
// Any value in ±92233720368.54775807 is valid, including zero and negatives.
class Price {
public:
    static constexpr int kDecimals = 8;
    static constexpr std::int64_t kScale = detail::pow10(kDecimals);
    static constexpr std::int64_t kMaxRaw = std::numeric_limits<std::int64_t>::max();
    // "-92233720368.54775807" is the longest possible text.
    static constexpr std::size_t kMaxFormattedLen = 21;

    constexpr Price() noexcept = default;

    // Precondition: |raw| <= kMaxRaw (i.e. raw != INT64_MIN).
    static constexpr Price from_raw(std::int64_t raw) noexcept { return Price(raw); }

    // Convenience for whole-number prices, e.g. Price::from_units(1025).
    static constexpr Price from_units(std::int64_t units) noexcept { return Price(units * kScale); }

    constexpr std::int64_t raw() const noexcept { return raw_; }

    friend constexpr auto operator<=>(const Price&, const Price&) = default;

private:
    constexpr explicit Price(std::int64_t raw) noexcept : raw_(raw) {}
    std::int64_t raw_ = 0;
};

static_assert(sizeof(Price) == 8);
static_assert(std::is_trivially_copyable_v<Price>);

enum class PriceError : std::uint8_t { None, Malformed, TooPrecise, OutOfRange };

// Human-readable reason, e.g. "more than 8 decimal places".
std::string_view describe(PriceError) noexcept;

// Parses `'-'? DIGIT+ ('.' DIGIT+)?`. Exact or rejected, never rounded.
// On failure returns nullopt and sets `why`.
// @spec PRICE-PARSE-001
std::optional<Price> parse_price(std::string_view text, PriceError& why) noexcept;

// Writes the shortest exact decimal form (at most kMaxFormattedLen bytes) and
// returns one past the last byte written.
// @spec PRICE-FMT-001
char* format_price(Price price, char* out) noexcept;

// Allocating convenience for tests and diagnostics; never used on the hot path.
std::string to_string(Price price);

}  // namespace matcher
