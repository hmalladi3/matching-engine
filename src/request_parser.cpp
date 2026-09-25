#include "matcher/request_parser.h"

#include <array>
#include <limits>

namespace matcher {

namespace {

// Whitespace: ASCII space and tab, plus the invisible characters that appear
// when text is copied from documents (the brief's own PDF contains U+200B).
constexpr std::array<std::string_view, 5> kWhitespace = {
    " ", "\t",
    "\xC2\xA0",      // U+00A0 no-break space
    "\xE2\x80\x8B",  // U+200B zero-width space
    "\xEF\xBB\xBF",  // U+FEFF byte-order mark / zero-width no-break space
};

std::string_view trim(std::string_view s) noexcept {
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

enum class IntResult { Ok, Malformed, OutOfRange };

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
bool parse_positive(std::string_view field, ParseError::Kind kind, std::uint64_t& value,
                    ParseError& error) noexcept {
    switch (parse_uint(field, value)) {
        case IntResult::Ok:
            if (value > 0) return true;
            error = ParseError{kind, field, ParseError::IntDetail::NotPositive};
            return false;
        case IntResult::Malformed:
            error = ParseError{kind, field, ParseError::IntDetail::Malformed};
            return false;
        case IntResult::OutOfRange:
            error = ParseError{kind, field, ParseError::IntDetail::OutOfRange};
            return false;
    }
    return false;
}

}  // namespace

// @spec PROTO-PARSE-001, PROTO-PARSE-002
std::string_view clean_line(std::string_view line) noexcept {
    if (const std::size_t comment = line.find("//"); comment != std::string_view::npos)
        line = line.substr(0, comment);
    return trim(line);
}

// @spec PROTO-PARSE-003, PROTO-PARSE-004, PROTO-PARSE-005, PROTO-PARSE-006,
//       PROTO-PARSE-007, PROTO-PARSE-008, PROTO-PARSE-009, PROTO-PARSE-010,
//       PROTO-PARSE-011
ParseResult parse_request(std::string_view line) noexcept {
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

    // Validation order is the precedence of errors (PROTO-PARSE-010).
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

}  // namespace matcher
