#include "matcher/request_parser.h"

#include <array>
#include <limits>

namespace matcher {

namespace {

// Whitespace: ASCII space and tab, plus the invisible characters that appear
// when text is copied from documents (PDFs often contain U+200B).
constexpr std::array<std::string_view, 5> kWhitespace = {
    " ",
    "\t",
    "\xC2\xA0",      // U+00A0 no-break space
    "\xE2\x80\x8B",  // U+200B zero-width space
    "\xEF\xBB\xBF",  // U+FEFF byte-order mark / zero-width no-break space
};

// Can `c` be the first / last byte of a whitespace sequence? Lets trim()
// return after two byte tests for the overwhelmingly common clean field.
constexpr bool may_start_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\xC2' || c == '\xE2' || c == '\xEF';
}
constexpr bool may_end_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\xA0' || c == '\x8B' || c == '\xBF';
}

std::string_view trim(std::string_view s) noexcept {
    if (s.empty() || (!may_start_whitespace(s.front()) && !may_end_whitespace(s.back()))) [[likely]]
        return s;
    for (bool trimmed = true; trimmed && !s.empty();) {
        trimmed = false;
        for (std::string_view ws : kWhitespace) {
            if (s.starts_with(ws)) {
                s.remove_prefix(ws.size());
                trimmed = true;
            }
            if (s.ends_with(ws)) {
                s.remove_suffix(ws.size());
                trimmed = true;
            }
        }
    }
    return s;
}

enum class IntResult : std::uint8_t { Ok, Malformed, OutOfRange };

// DIGIT+ only: no sign, no decimal point. Leading zeros are allowed.
IntResult parse_uint(std::string_view s, std::uint64_t& value) noexcept {
    if (s.empty()) return IntResult::Malformed;
    for (char c : s)
        if (c < '0' || c > '9') return IntResult::Malformed;
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t v = 0;
    for (char c : s) {
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (kMax - d) / 10) return IntResult::OutOfRange;
        v = v * 10 + d;
    }
    value = v;
    return IntResult::Ok;
}

// Parses a positive integer field; on failure fills `error` and returns false.
bool parse_positive(std::string_view field, ParseError::Kind kind, std::uint64_t& value, ParseError& error) noexcept {
    const IntResult result = parse_uint(field, value);
    if (result == IntResult::Ok && value > 0) return true;
    auto detail = ParseError::IntDetail::NotPositive;  // parsed, but zero
    if (result == IntResult::Malformed) detail = ParseError::IntDetail::Malformed;
    if (result == IntResult::OutOfRange) detail = ParseError::IntDetail::OutOfRange;
    error = ParseError{kind, field, detail};
    return false;
}

}  // namespace

std::string_view clean_line(std::string_view line) noexcept {
    if (const std::size_t comment = line.find("//"); comment != std::string_view::npos) line = line.substr(0, comment);
    return trim(line);
}

ParseResult detail::parse_request_general(std::string_view line) noexcept {
    const std::string_view text = clean_line(line);
    if (text.empty()) return BlankLine{};

    // Split on ',' keeping at most the first five fields; count all of them.
    std::array<std::string_view, 5> fields;
    std::uint32_t count = 0;
    for (std::size_t start = 0;; ++count) {
        const std::size_t comma = text.find(',', start);
        const std::string_view field = text.substr(start, comma == std::string_view::npos ? comma : comma - start);
        if (count < fields.size()) fields[count] = trim(field);
        if (comma == std::string_view::npos) {
            ++count;
            break;
        }
        start = comma + 1;
    }

    // Validation order is the precedence of errors.
    const bool is_add = fields[0] == "0";
    if (!is_add && fields[0] != "1") return ParseError{ParseError::Kind::UnknownMessageType, fields[0]};

    const std::uint32_t expected = is_add ? 5 : 2;
    if (count != expected) {
        ParseError e{ParseError::Kind::WrongFieldCount, {}};
        e.expected_fields = expected;
        e.actual_fields = count;
        e.is_add = is_add;
        return e;
    }

    ParseError error{ParseError::Kind::BadOrderId, {}};
    std::uint64_t id = 0;
    if (!parse_positive(fields[1], ParseError::Kind::BadOrderId, id, error)) return error;
    if (!is_add) return CancelOrder{id};

    if (fields[2] != "0" && fields[2] != "1") return ParseError{ParseError::Kind::BadSide, fields[2]};
    const Side side = fields[2] == "0" ? Side::Buy : Side::Sell;

    std::uint64_t qty = 0;
    if (!parse_positive(fields[3], ParseError::Kind::BadQuantity, qty, error)) return error;

    PriceError why = PriceError::None;
    const std::optional<Price> price = parse_price(fields[4], why);
    if (!price) {
        ParseError e{ParseError::Kind::BadPrice, fields[4]};
        e.price_error = why;
        return e;
    }
    return AddOrder{id, side, qty, *price};
}

namespace {

// Parses a positive decimal integer of 1 to 19 digits at `p`, which always fits
// in uint64 (20-digit values, leading-zero-only fields and zero go to the
// general parser, which decides range and diagnostics).
bool fast_positive(const char*& p, const char* end, std::uint64_t& value) noexcept {
    const char* const start = p;
    std::uint64_t v = 0;
    while (p != end) {
        const auto digit = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
        if (digit > 9) break;
        v = v * 10 + digit;
        ++p;
    }
    const auto digits = p - start;
    if (digits == 0 || digits > 19 || v == 0) return false;
    value = v;
    return true;
}

// Parses the rest of the line as a price of the common form
// '-'? DIGIT{1,10} ('.' DIGIT{1,8})? with nothing after it. Ten integer digits
// stay far below the ±92233720368.54775807 limit, so no range check is needed;
// anything longer, finer or malformed goes to the general parser. Digits are
// accumulated unsigned: an over-long run wraps harmlessly (defined behavior)
// before the digit count rejects it.
bool fast_price(const char* p, const char* end, Price& price) noexcept {
    static constexpr std::int64_t kScaleFor[] = {100'000'000, 10'000'000, 1'000'000, 100'000, 10'000,
                                                 1'000,       100,        10,        1};
    const bool negative = p != end && *p == '-';
    if (negative) ++p;
    std::uint64_t units = 0;
    const char* const integer_start = p;
    while (p != end) {
        const auto digit = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
        if (digit > 9) break;
        units = units * 10 + digit;
        ++p;
    }
    const auto integer_digits = p - integer_start;
    if (integer_digits == 0 || integer_digits > 10) return false;

    std::uint64_t fraction = 0;
    std::ptrdiff_t fraction_digits = 0;
    if (p != end && *p == '.') {
        const char* const fraction_start = ++p;
        while (p != end) {
            const auto digit = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
            if (digit > 9) break;
            fraction = fraction * 10 + digit;
            ++p;
        }
        fraction_digits = p - fraction_start;
        if (fraction_digits == 0 || fraction_digits > Price::kDecimals) return false;
    }
    if (p != end) return false;

    const auto raw = static_cast<std::int64_t>(units) * Price::kScale +
                     static_cast<std::int64_t>(fraction) * kScaleFor[fraction_digits];
    price = Price::from_raw(negative ? -raw : raw);
    return true;
}

// One pass over the byte forms that nearly every real line takes:
//   0,<id>,<side>,<qty>,<price>     1,<id>
// with no whitespace, comment or error. Returns false for anything else, and
// the general parser then produces the (identical) result or the diagnostic.
bool parse_clean(std::string_view line, ParseResult& out) noexcept {
    const char* p = line.data();
    const char* const end = p + line.size();
    if (line.size() < 3 || p[1] != ',') return false;
    const char type = p[0];
    p += 2;

    std::uint64_t id = 0;
    if (!fast_positive(p, end, id)) return false;
    if (type == '1') {
        if (p != end) return false;
        out = CancelOrder{id};
        return true;
    }
    if (type != '0' || end - p < 4 || p[0] != ',' || (p[1] != '0' && p[1] != '1') || p[2] != ',') return false;
    const Side side = p[1] == '0' ? Side::Buy : Side::Sell;
    p += 3;

    std::uint64_t qty = 0;
    if (!fast_positive(p, end, qty) || p == end || *p != ',') return false;
    ++p;

    Price price;
    if (!fast_price(p, end, price)) return false;
    out = AddOrder{id, side, qty, price};
    return true;
}

}  // namespace

ParseResult parse_request(std::string_view line) noexcept {
    ParseResult result;
    if (parse_clean(line, result)) [[likely]]
        return result;
    return detail::parse_request_general(line);
}

}  // namespace matcher
