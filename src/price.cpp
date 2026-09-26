#include "matcher/price.h"

#include <array>
#include <charconv>
#include <cstring>

namespace matcher {

namespace {

constexpr std::array<std::int64_t, Price::kDecimals + 1> kPow10 = [] {
    std::array<std::int64_t, Price::kDecimals + 1> table{};
    std::int64_t v = 1;
    for (auto& entry : table) {
        entry = v;
        v *= 10;
    }
    return table;
}();

// Largest integer part that can appear: 92233720368.
constexpr std::uint64_t kMaxIntegerPart = static_cast<std::uint64_t>(Price::kMaxRaw / Price::kScale);

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr unsigned digit(char c) noexcept { return static_cast<unsigned>(c - '0'); }

}  // namespace

std::string_view describe(PriceError error) noexcept {
    switch (error) {
        case PriceError::None: return "none";
        case PriceError::Malformed: return "malformed";
        case PriceError::TooPrecise: return "more than 8 decimal places";
        case PriceError::OutOfRange: return "out of range";
    }
    return "unknown";
}

// @spec PRICE-PARSE-001, PRICE-PARSE-002, PRICE-PARSE-003, PRICE-PARSE-004,
//       PRICE-PARSE-005, PRICE-PARSE-006, PRICE-PARSE-007
std::optional<Price> parse_price(std::string_view text, PriceError& why) noexcept {
    // One pass validates the grammar `'-'? DIGIT+ ('.' DIGIT+)?` while
    // accumulating the value. Range and precision problems are only reported
    // for text that is otherwise well formed.
    std::size_t i = 0;
    const std::size_t n = text.size();

    const bool negative = i < n && text[i] == '-';
    if (negative) ++i;

    const std::size_t integer_start = i;
    std::uint64_t integer = 0;
    bool out_of_range = false;
    for (; i < n && is_digit(text[i]); ++i) {
        if (out_of_range) continue;
        integer = integer * 10 + digit(text[i]);
        if (integer > kMaxIntegerPart) out_of_range = true;
    }
    if (i == integer_start) {
        why = PriceError::Malformed;
        return std::nullopt;
    }

    std::uint64_t fraction = 0;  // the first kDecimals fractional digits
    int fraction_digits = 0;
    bool too_precise = false;
    if (i < n && text[i] == '.') {
        const std::size_t fraction_start = ++i;
        for (; i < n && is_digit(text[i]); ++i) {
            if (fraction_digits < Price::kDecimals) {
                fraction = fraction * 10 + digit(text[i]);
                ++fraction_digits;
            } else if (text[i] != '0') {
                too_precise = true;  // not exactly representable
            }
        }
        if (i == fraction_start) {
            why = PriceError::Malformed;
            return std::nullopt;
        }
    }
    if (i != n) {
        why = PriceError::Malformed;
        return std::nullopt;
    }

    fraction *= static_cast<std::uint64_t>(kPow10[static_cast<std::size_t>(Price::kDecimals - fraction_digits)]);
    // integer <= 92233720368 here, so this cannot overflow uint64.
    const std::uint64_t magnitude = integer * static_cast<std::uint64_t>(Price::kScale) + fraction;
    if (out_of_range || magnitude > static_cast<std::uint64_t>(Price::kMaxRaw)) {
        why = PriceError::OutOfRange;
        return std::nullopt;
    }
    if (too_precise) {
        why = PriceError::TooPrecise;
        return std::nullopt;
    }

    why = PriceError::None;
    const auto raw = static_cast<std::int64_t>(magnitude);
    return Price::from_raw(negative ? -raw : raw);  // "-0" becomes 0
}

// @spec PRICE-FMT-001
char* format_price(Price price, char* out) noexcept {
    const std::int64_t raw = price.raw();
    // |raw| <= INT64_MAX by the Price invariant, so negation is safe.
    const std::uint64_t magnitude = raw < 0 ? static_cast<std::uint64_t>(-raw) : static_cast<std::uint64_t>(raw);
    if (raw < 0) *out++ = '-';

    const auto scale = static_cast<std::uint64_t>(Price::kScale);
    out = std::to_chars(out, out + 20, magnitude / scale).ptr;

    std::uint64_t fraction = magnitude % scale;
    if (fraction != 0) {
        char digits[Price::kDecimals];
        for (int d = Price::kDecimals - 1; d >= 0; --d) {
            digits[d] = static_cast<char>('0' + fraction % 10);
            fraction /= 10;
        }
        std::size_t len = Price::kDecimals;
        while (digits[len - 1] == '0') --len;  // shortest exact form
        *out++ = '.';
        std::memcpy(out, digits, len);
        out += len;
    }
    return out;
}

std::string to_string(Price price) {
    char buf[Price::kMaxFormattedLen];
    return {buf, format_price(price, buf)};
}

}  // namespace matcher
