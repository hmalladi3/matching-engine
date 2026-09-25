#include "matcher/error_reporter.h"

#include <algorithm>
#include <charconv>
#include <cstring>

#include "matcher/line_reader.h"

namespace matcher {

namespace {
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026 "…"
}  // namespace

std::string_view to_string(Reject reject) noexcept {
    switch (reject) {
        case Reject::None: return "none";
        case Reject::InvalidOrderId: return "invalid orderid";
        case Reject::InvalidQuantity: return "invalid quantity";
        case Reject::DuplicateOrderId: return "duplicate orderid";
        case Reject::UnknownOrderId: return "unknown orderid";
        case Reject::CapacityExceeded: return "capacity exceeded";
    }
    return "unknown";
}

void ErrorReporter::number(std::uint64_t value) noexcept {
    char buf[20];
    err_.append({buf, static_cast<std::size_t>(std::to_chars(buf, buf + sizeof buf, value).ptr - buf)});
}

void ErrorReporter::line_prefix(std::uint64_t line) noexcept {
    err_.append("line ");
    number(line);
    err_.append(": ");
}

// Printable ASCII passes through; every other byte becomes \xHH, so binary
// input cannot corrupt a terminal.
void ErrorReporter::escaped(std::string_view text) noexcept {
    static constexpr char kHex[] = "0123456789ABCDEF";
    while (!text.empty()) {
        const auto run = static_cast<std::size_t>(
            std::find_if(text.begin(), text.end(), [](char c) { return c < 0x20 || c > 0x7E; }) - text.begin());
        err_.append(text.substr(0, run));
        text.remove_prefix(run);
        if (text.empty()) break;
        const auto byte = static_cast<unsigned char>(text.front());
        const char hex[4] = {'\\', 'x', kHex[byte >> 4], kHex[byte & 0xF]};
        err_.append({hex, sizeof hex});
        text.remove_prefix(1);
    }
}

void ErrorReporter::excerpt(std::string_view text, bool always_ellipsis) noexcept {
    escaped(text.substr(0, kExcerptBytes));
    if (always_ellipsis || text.size() > kExcerptBytes) err_.append(kEllipsis);
}

void ErrorReporter::int_field_reason(std::string_view name, const ParseError& error) noexcept {
    switch (error.int_detail) {
        case ParseError::IntDetail::Malformed:
            err_.append("invalid ");
            err_.append(name);
            err_.append(" '");
            excerpt(error.field, false);
            err_.append("'");
            return;
        case ParseError::IntDetail::NotPositive:
            err_.append(name);
            err_.append(" must be positive");
            return;
        case ParseError::IntDetail::OutOfRange:
            err_.append(name);
            err_.append(" out of range");
            return;
    }
}

// @spec PROTO-APP-001, PROTO-APP-002
void ErrorReporter::parse_error(std::uint64_t line, const ParseError& error, std::string_view cleaned_line) noexcept {
    line_prefix(line);
    switch (error.kind) {
        case ParseError::Kind::UnknownMessageType:
            err_.append("Unknown message type");  // the brief's wording
            break;
        case ParseError::Kind::WrongFieldCount:
            err_.append(error.is_add ? "AddOrderRequest expects " : "CancelOrderRequest expects ");
            number(error.expected_fields);
            err_.append(" fields, got ");
            number(error.actual_fields);
            break;
        case ParseError::Kind::BadOrderId: int_field_reason("orderid", error); break;
        case ParseError::Kind::BadSide:
            err_.append("invalid side '");
            excerpt(error.field, false);
            err_.append("' (expected 0=Buy or 1=Sell)");
            break;
        case ParseError::Kind::BadQuantity: int_field_reason("quantity", error); break;
        case ParseError::Kind::BadPrice:
            err_.append("invalid price '");
            excerpt(error.field, false);
            err_.append("': ");
            err_.append(describe(error.price_error));
            break;
    }
    err_.append(": ");
    excerpt(cleaned_line, false);
    err_.append("\n");
    ++count_;
}

// @spec OUT-DIAG-001
void ErrorReporter::reject(std::uint64_t line, Reject reason, OrderId id, std::string_view cleaned_line) noexcept {
    line_prefix(line);
    switch (reason) {
        case Reject::DuplicateOrderId:
            err_.append("duplicate orderid ");
            number(id);
            err_.append(" (an order with this id is still resting)");
            break;
        case Reject::UnknownOrderId:
            err_.append("cannot cancel orderid ");
            number(id);
            err_.append(": no resting order with this id");
            break;
        case Reject::CapacityExceeded: err_.append("order rejected: order book capacity exhausted"); break;
        default:  // unreachable behind the parser
            err_.append("internal error: ");
            err_.append(to_string(reason));
            break;
    }
    err_.append(": ");
    excerpt(cleaned_line, false);
    err_.append("\n");
    ++count_;
}

// @spec PROTO-READ-003
void ErrorReporter::line_too_long(std::uint64_t line, std::string_view raw_prefix) noexcept {
    line_prefix(line);
    err_.append("line exceeds ");
    number(LineReader::kMaxLineBytes);
    err_.append(" bytes: ");
    excerpt(raw_prefix, true);
    err_.append("\n");
    ++count_;
}

void ErrorReporter::message(std::string_view text) noexcept {
    err_.append(text);
    err_.append("\n");
    ++count_;
}

void ErrorReporter::system_error(std::string_view what, int errnum) noexcept {
    err_.append(what);
    err_.append(": ");
    err_.append(std::strerror(errnum));
    err_.append("\n");
    ++count_;
}

}  // namespace matcher
