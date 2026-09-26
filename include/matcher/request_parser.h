#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "matcher/price.h"
#include "matcher/types.h"

namespace matcher {

// A line with no content after comment removal and trimming.
struct BlankLine {
    friend bool operator==(const BlankLine&, const BlankLine&) = default;
};

struct ParseError {
    enum class Kind : std::uint8_t {
        UnknownMessageType,
        WrongFieldCount,
        BadOrderId,
        BadSide,
        BadQuantity,
        BadPrice,
    };
    // Why an integer field was rejected.
    enum class IntDetail : std::uint8_t { Malformed, NotPositive, OutOfRange };

    Kind kind;
    std::string_view field;  // the offending field (trimmed), if any
    IntDetail int_detail = IntDetail::Malformed;
    PriceError price_error = PriceError::None;
    std::uint32_t expected_fields = 0;  // WrongFieldCount
    std::uint32_t actual_fields = 0;    // WrongFieldCount
    bool is_add = true;                 // WrongFieldCount: which message was expected

    friend bool operator==(const ParseError&, const ParseError&) = default;
};

using ParseResult = std::variant<AddOrder, CancelOrder, BlankLine, ParseError>;

// Removes a `//` comment and trims whitespace (ASCII space/tab, U+00A0,
// U+200B, U+FEFF) from both ends. The result is a view into `line`.
// @spec PROTO-PARSE-001, PROTO-PARSE-002
std::string_view clean_line(std::string_view line) noexcept;

// Parses one input line. Never throws; any byte sequence yields a result.
// Views inside a ParseError point into `line`.
// @spec PROTO-PARSE-003, PROTO-PARSE-004, PROTO-PARSE-005, PROTO-PARSE-006,
//       PROTO-PARSE-007, PROTO-PARSE-008, PROTO-PARSE-009, PROTO-PARSE-010,
//       PROTO-PARSE-011
ParseResult parse_request(std::string_view line) noexcept;

namespace detail {
// The general parser: handles every form (whitespace, comments, errors) and is
// the authority on diagnostics. parse_request() tries a single-pass fast path
// for clean lines first and falls back to this; the two must agree on every
// input (tested). Exposed for that test.
ParseResult parse_request_general(std::string_view line) noexcept;
}  // namespace detail

}  // namespace matcher
